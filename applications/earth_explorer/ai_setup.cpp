#include "ai_setup.h"
#include "quake_data.h"
#include "flight_data.h"
#include <readerwriter/EarthManipulator.h>
#include <modeling/Math.h>
#include <osg/Notify>
#include <osg/Math>

// 汇总类工具（get_quakes_summary / get_flights_summary）高度雷同：懒开启图层 → 取
// summaryJson → 解析 → count<=0 时补 loadingNote → 打日志。抽成一个通用小工厂，
// 两处注册只需传各自的 layerId/描述/isEnabled+summaryJson 回调。
static earthai::Tool makeSummaryTool(const std::string& name, const std::string& descCn,
                                     const std::string& layerId, LayerManager* layers,
                                     std::function<std::string()> summaryFn,
                                     std::function<bool()> isEnabledFn)
{
    earthai::Tool t; t.name = name; t.description = descCn;
    t.parametersJson = "{\"type\":\"object\",\"properties\":{}}";
    t.execute = [name, layerId, layers, summaryFn, isEnabledFn](const picojson::value&) {
        if (!isEnabledFn())   // 懒开启,触发抓取
            layers->setEnabled(layerId, true);
        std::string json = summaryFn();
        picojson::value v; std::string perr = picojson::parse(v, json);
        if (!perr.empty() || !v.is<picojson::object>())
        {
            picojson::object err; err["error"] = picojson::value("bad summary json: " + perr);
            return picojson::value(err);
        }
        picojson::object& obj = v.get<picojson::object>();
        double n = obj.count("count") ? obj["count"].get<double>() : 0.0;
        if (n <= 0.0)
            obj["loadingNote"] = picojson::value(
                std::string(u8"图层已自动开启，数据抓取中，请稍后再查询一次"));
        OSG_NOTICE << "[AIChat] " << name << " count=" << (long long)n << std::endl;
        return picojson::value(obj);
    };
    return t;
}

// FRAME drain（工具必须主线程执行；与降水层 FRAME handler 同模式）。
// 同时承担 EARTH_AI_AUTOSUBMIT 的延迟提交：数据类图层（地震/航班）的抓取线程
// 在另一个线程跑，若开局第 0 帧就 submit，AI 工具可能在 fixture/网络数据落地
// 之前就查询到空结果——用 EARTH_AI_AUTOSUBMIT_DELAY_FRAMES（默认 0，立即提交）
// 推迟到第 N 帧再提交，给数据同步（syncIfDirty，主线程 update 遍历）留出时间。
class AIFrameHandler : public osgGA::GUIEventHandler
{
    earthai::AIChatCore* _core;
    std::string _autoSubmitText; int _delayFrames; int _frameCount; bool _fired;
public:
    AIFrameHandler(earthai::AIChatCore* c, const std::string& autoSubmitText, int delayFrames)
        : _core(c), _autoSubmitText(autoSubmitText), _delayFrames(delayFrames),
          _frameCount(0), _fired(false) {}
    virtual bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter&)
    {
        if (ea.getEventType() == osgGA::GUIEventAdapter::FRAME)
        {
            _core->drainMainThread();
            if (!_fired && !_autoSubmitText.empty() && _frameCount >= _delayFrames)
            { _core->submit(_autoSubmitText); _fired = true; }
            if (!_fired) ++_frameCount;
        }
        return false;
    }
};

earthai::AIChatCore* configureAIChat(osgViewer::Viewer& viewer,
                                     osgVerse::EarthManipulator* mani,
                                     LayerManager* layerMgr,
                                     QuakeLayer* quakeLayer, FlightLayer* flightLayer)
{
    // ---- AI Chat（spec: docs/superpowers/specs/2026-07-02-earth-ai-chat-design.md）----
    // 无 EARTH_AI_KEY 且无 EARTH_AI_FAKE → aiCore 为空，后续全部跳过，零影响。
    // registry/aiCore 用裸指针、进程生命周期存活，与本文件其余单例/裸 new 风格一致。
    earthai::ToolRegistry* aiRegistry = new earthai::ToolRegistry;
    earthai::AIChatCore* aiCore = nullptr;
    {
        earthai::Tool fly; fly.name = "fly_to";
        fly.description = u8"把相机飞到指定经纬度与高度。用户说地名时你自己换算经纬度。";
        fly.parametersJson = "{\"type\":\"object\",\"properties\":{"
            "\"lat\":{\"type\":\"number\"},\"lon\":{\"type\":\"number\"},"
            "\"alt_km\":{\"type\":\"number\",\"description\":\"default 50\"}},"
            "\"required\":[\"lat\",\"lon\"]}";
        fly.execute = [mani](const picojson::value& a) {
            double lat = a.get("lat").get<double>(), lon = a.get("lon").get<double>();
            double alt = a.contains("alt_km") ? a.get("alt_km").get<double>() : 50.0;
            if (lat < -90 || lat > 90 || lon < -180 || lon > 180 || alt <= 0.0)
            {
                picojson::object err;
                err["error"] = picojson::value("out of range: lat[-90,90] lon[-180,180] alt_km>0");
                return picojson::value(err);
            }
            mani->setByEye(osg::inDegrees(lat), osg::inDegrees(lon), alt * 1000.0);
            OSG_NOTICE << "[AIChat] fly_to " << lat << "," << lon << "," << alt << "km" << std::endl;
            picojson::object r; r["ok"] = picojson::value(true); return picojson::value(r);
        };
        aiRegistry->add(fly);

        earthai::Tool setLayer; setLayer.name = "set_layer";
        setLayer.description = u8"开关或调整一个数据图层。可用 id："
            u8"labels(路网·地名)、clouds(GIBS 影像/云图)、precip(降水雷达)、"
            u8"quakes(实时地震)、flights(实时航班)、hk3d(香港实景三维)。"
            u8"opacity 仅对支持透明度的图层有效（可选，0-1）。";
        setLayer.parametersJson = "{\"type\":\"object\",\"properties\":{"
            "\"id\":{\"type\":\"string\"},\"enabled\":{\"type\":\"boolean\"},"
            "\"opacity\":{\"type\":\"number\"}},\"required\":[\"id\",\"enabled\"]}";
        LayerManager* lmptr = layerMgr;
        setLayer.execute = [lmptr](const picojson::value& a) {
            std::string id = a.get("id").get<std::string>();
            bool enabled = a.get("enabled").get<bool>();
            OverlayLayer* l = lmptr->find(id);
            if (!l)
            {
                std::string avail;
                std::vector<OverlayLayer>& all = lmptr->layers();
                for (size_t i = 0; i < all.size(); ++i) avail += (i ? "," : "") + all[i].id;
                picojson::object err;
                err["error"] = picojson::value("unknown layer id " + id + ", available: " + avail);
                return picojson::value(err);
            }
            if (!l->apply)   // "base" 底图常开、无开关动作
            {
                picojson::object err; err["error"] = picojson::value("layer not togglable");
                return picojson::value(err);
            }
            lmptr->setEnabled(id, enabled);
            if (a.contains("opacity") && l->hasOpacity)
            {
                float op = osg::clampBetween((float)a.get("opacity").get<double>(), 0.0f, 1.0f);
                lmptr->setOpacity(id, op);
            }
            OSG_NOTICE << "[AIChat] set_layer " << id << " " << (enabled ? "on" : "off") << std::endl;
            picojson::object r;
            r["ok"] = picojson::value(true); r["layer"] = picojson::value(id);
            r["enabled"] = picojson::value(enabled);
            r["opacity"] = picojson::value((double)l->opacity);
            return picojson::value(r);
        };
        aiRegistry->add(setLayer);

        earthai::Tool viewState; viewState.name = "get_view_state";
        viewState.description = u8"读取当前相机的经纬度/高度，以及各图层开关状态。"
            u8"单位：lat/lon 为度，alt_km 为千米。";
        viewState.parametersJson = "{\"type\":\"object\",\"properties\":{}}";
        osgVerse::EarthManipulator* maniV = mani;
        viewState.execute = [maniV, lmptr](const picojson::value&) {
            // 与 EarthControlUI「相机 Camera」小节同一取值方式：computeEyeLatLonHeight()
            // 返回 (纬度弧度, 经度弧度, 高度米)。
            osg::Vec3d lla = maniV->computeEyeLatLonHeight();
            picojson::object r;
            r["lat"] = picojson::value(osg::RadiansToDegrees(lla[0]));
            r["lon"] = picojson::value(osg::RadiansToDegrees(lla[1]));
            r["alt_km"] = picojson::value(lla[2] / 1000.0);
            picojson::object layersObj;
            std::vector<OverlayLayer>& all = lmptr->layers();
            for (size_t i = 0; i < all.size(); ++i)
                layersObj[all[i].id] = picojson::value(all[i].enabled);
            r["layers"] = picojson::value(layersObj);
            OSG_NOTICE << "[AIChat] get_view_state" << std::endl;
            return picojson::value(r);
        };
        aiRegistry->add(viewState);

        // get_quakes_summary / get_flights_summary:懒开启图层 + 汇总 JSON，两者 ~95% 重复，
        // 抽成 makeSummaryTool（见上）。
        QuakeLayer* qSummaryPtr = quakeLayer;
        aiRegistry->add(makeSummaryTool("get_quakes_summary",
            u8"查询当前地震数据汇总：总数、最大震级(及地点/时间)、"
            u8"震级直方图(<3/3-4/4-5/5-6/>=6)、过去24小时内数量。"
            u8"数据来自 USGS 实时地震流；若地震层尚未开启会自动开启并开始抓取，"
            u8"此时返回的 count 可能为 0，代表数据仍在加载中，可稍后再查询一次。",
            "quakes", lmptr,
            [qSummaryPtr]() { return qSummaryPtr ? qSummaryPtr->summaryJson() : std::string("{}"); },
            [qSummaryPtr]() { return qSummaryPtr && qSummaryPtr->isEnabled(); }));

        FlightLayer* fSummaryPtr = flightLayer;
        aiRegistry->add(makeSummaryTool("get_flights_summary",
            u8"查询当前视口内航班数据汇总：航班数量、高度分桶"
            u8"(<2km/2-8km/>=8km)、最快航班(呼号+速度)。"
            u8"数据来自 OpenSky 实时航班流，覆盖范围随相机视口变化（非全球总数）；"
            u8"若航班层尚未开启会自动开启并开始抓取，此时返回的 count 可能为 0，"
            u8"代表数据仍在加载中，可稍后再查询一次。",
            "flights", lmptr,
            [fSummaryPtr]() { return fSummaryPtr ? fSummaryPtr->summaryJson() : std::string("{}"); },
            [fSummaryPtr]() { return fSummaryPtr && fSummaryPtr->isEnabled(); }));
    }
    const char* aiKey = getenv("EARTH_AI_KEY");
    const char* aiFake = getenv("EARTH_AI_FAKE");
    if (aiFake && *aiFake)
    {
        earthai::FakeProvider* fp = new earthai::FakeProvider;
        if (!fp->loadFromFile(aiFake)) OSG_WARN << "[AIChat] bad fake script: " << aiFake << std::endl;
        aiCore = new earthai::AIChatCore(fp, aiRegistry);
    }
    else if (aiKey && *aiKey)
    {
        const char* m = getenv("EARTH_AI_MODEL");
        earthai::GeminiProvider* gp = new earthai::GeminiProvider(
            aiKey, (m && *m) ? m : "gemini-2.5-flash");
        gp->setSystemPrompt(u8"你是 EarthExplorer 三维地球应用的中文助手。优先使用提供的工具完成用户请求；"
                            u8"用户提到地名时自行换算经纬度；回答保持简洁；不要编造工具没有返回的数据。");
        aiCore = new earthai::AIChatCore(gp, aiRegistry);
    }
    if (aiCore)
    {
        const char* autoSubmit = getenv("EARTH_AI_AUTOSUBMIT");   // headless E2E 用
        const char* delayEnv = getenv("EARTH_AI_AUTOSUBMIT_DELAY_FRAMES");
        int delayFrames = (delayEnv && *delayEnv) ? atoi(delayEnv) : 0;
        viewer.addEventHandler(new AIFrameHandler(aiCore,
            (autoSubmit && *autoSubmit) ? autoSubmit : "", delayFrames));
    }
    return aiCore;
}
