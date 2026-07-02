#include "ai_setup.h"
#include "quake_data.h"
#include "flight_data.h"
#include "ai_ui.h"
#include "ai_media.h"
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
//
// 测试专用:EARTH_AI_VIDEO_AUTOTEST=1(见 update() 里的 _videoAutotest 分支)——headless
// E2E 没法真的点 UI 按钮/在确认 Modal 里点「确认」，用这个钩子在固定帧数程序化地走一遍
// beginVideoCapture → captureVideoEnd → confirmVideo，绕开 UI 但复用完全相同的
// MediaManager 状态机（因此仍然验证了真实状态机，只是跳过了鼠标点击这一步）。
// 只在环境变量设置时激活，不影响正常运行路径。
class AIFrameHandler : public osgGA::GUIEventHandler
{
    earthai::AIChatCore* _core;
    earthai::MediaManager* _media;   // 可为 null(无 key 且无 EARTH_AI_FAKE_IMG/EARTH_AI_FAKE)
    osgVerse::EarthManipulator* _mani;
    std::string _autoSubmitText; int _delayFrames; int _frameCount; bool _fired;

    bool _videoAutotest;             // EARTH_AI_VIDEO_AUTOTEST=1
    int _videoFrameCount;
    bool _videoBeganA, _videoCapturedB, _videoConfirmed;
public:
    AIFrameHandler(earthai::AIChatCore* c, earthai::MediaManager* media,
                   osgVerse::EarthManipulator* mani,
                   const std::string& autoSubmitText, int delayFrames)
        : _core(c), _media(media), _mani(mani), _autoSubmitText(autoSubmitText), _delayFrames(delayFrames),
          _frameCount(0), _fired(false),
          _videoAutotest(false), _videoFrameCount(0),
          _videoBeganA(false), _videoCapturedB(false), _videoConfirmed(false)
    {
        const char* env = getenv("EARTH_AI_VIDEO_AUTOTEST");
        _videoAutotest = (env && *env && std::string(env) != "0");
    }
    virtual bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter&)
    {
        if (ea.getEventType() == osgGA::GUIEventAdapter::FRAME)
        {
            _core->drainMainThread();
            if (_media) _media->update();
            if (!_fired && !_autoSubmitText.empty() && _frameCount >= _delayFrames)
            { _core->submit(_autoSubmitText); _fired = true; }
            if (!_fired) ++_frameCount;

            // ---- EARTH_AI_VIDEO_AUTOTEST 钩子(测试专用,仅当环境变量设置时激活)----
            if (_videoAutotest && _media && _mani)
            {
                ++_videoFrameCount;
                if (!_videoBeganA && _videoFrameCount >= 60)
                {
                    osg::Vec3d llaA = _mani->computeEyeLatLonHeight();
                    _media->beginVideoCapture(llaA);
                    _videoBeganA = true;
                }
                else if (_videoBeganA && !_videoCapturedB && _videoFrameCount >= 120)
                {
                    osg::Vec3d llaB = _mani->computeEyeLatLonHeight();
                    if (_media->captureVideoEnd(llaB)) _videoCapturedB = true;
                    // captureVideoEnd 要求 phase==WAIT_B(A 点快照已稳定);若 A 点还没稳定
                    // (WAIT_A),这里会返回 false,下一帧再试——不推进 _videoCapturedB。
                }
                else if (_videoCapturedB && !_videoConfirmed
                         && _media->videoPhase() == earthai::VIDEO_AWAIT_CONFIRM)
                {
                    // 绕过确认 Modal(headless 无法点击),直接调 confirmVideo() ——
                    // 与真实 UI 按钮调用的是同一个函数,状态机本身没有被绕过。
                    picojson::value r = _media->confirmVideo();
                    OSG_NOTICE << "[AIChat] video autotest confirm -> " << r.serialize() << std::endl;
                    _videoConfirmed = true;
                }
            }
        }
        return false;
    }
};

AIChatRuntime configureAIChat(const AIChatDeps& deps)
{
    osgViewer::Viewer& viewer = *deps.viewer;
    osgVerse::EarthManipulator* mani = deps.mani;
    LayerManager* layerMgr = deps.layers;
    QuakeLayer* quakeLayer = deps.quakes;
    FlightLayer* flightLayer = deps.flights;
    AIChatUI* ui = deps.ui;
    AIChatRuntime runtime;
    // ---- AI Chat（spec: docs/superpowers/specs/2026-07-02-earth-ai-chat-design.md）----
    // 无 EARTH_AI_KEY 且无 EARTH_AI_FAKE → aiCore 为空，后续全部跳过，零影响。
    // registry/aiCore 用裸指针、进程生命周期存活，与本文件其余单例/裸 new 风格一致。
    earthai::ToolRegistry* aiRegistry = new earthai::ToolRegistry;
    earthai::AIChatCore* aiCore = nullptr;

    // MediaManager(Task 8 快照+生图管线)先于 aiRegistry 构造完:generate_photo/generate_video
    // 工具的 execute 需要捕获它的指针。EARTH_AI_KEY 在这里先读一次(下面 aiCore 构造再读一次
    // 不冲突,getenv 本身可重复调用);EARTH_AI_FAKE_IMG 的读取封装在 MediaManager 内部
    // (ai_media.cpp::fakeImgPath),此处只需知道"要不要 new"。
    // PART A 审查修复:原条件只看 (有 key || 有 FAKE_IMG),没考虑 aiCore 是否真的会被创建——
    // 若只设了 EARTH_AI_FAKE_IMG 而没设 EARTH_AI_KEY/EARTH_AI_FAKE,aiCore 下面仍会保持
    // null(FakeProvider 需要 EARTH_AI_FAKE 脚本,不是 FAKE_IMG),导致 mediaMgr 被 new 出来
    // 却从未挂上 AIFrameHandler(update() 永远不被调用)、也没有 generate_photo/video 工具
    // 可触发它——一个"活不起来"的死对象外加一根不会触发的 UI 引用。这里改成与"aiCore 是否
    // 会被创建"同一条件(有 EARTH_AI_KEY 或 EARTH_AI_FAKE),否则不构造 mediaMgr,即便设了
    // EARTH_AI_FAKE_IMG 也不例外——FAKE_IMG 只是"生图/生视频这一步跳过网络",离线 E2E 时
    // 仍需搭配 EARTH_AI_FAKE 脚本才能把 generate_photo/generate_video 工具调用触发起来。
    const char* aiKeyForMedia = getenv("EARTH_AI_KEY");
    const char* aiFakeForMedia = getenv("EARTH_AI_FAKE");
    earthai::MediaManager* mediaMgr = nullptr;
    if ((aiKeyForMedia && *aiKeyForMedia) || (aiFakeForMedia && *aiFakeForMedia))
    {
        mediaMgr = new earthai::MediaManager(&viewer, ui ? ui->cards() : nullptr,
            (aiKeyForMedia && *aiKeyForMedia) ? std::string(aiKeyForMedia) : std::string());
    }
    runtime.media = mediaMgr;

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

        // show_chart:模型把统计数据整理成 spec,推给右上角图表卡(AIChatUI::pushChart)。
        // execute 就在 AIChatCore::drainMainThread 里跑(主线程),与 UI draw() 同线程,
        // pushChart 内部因此不需要加锁(见 ai_ui.cpp 头注释)。
        earthai::Tool chart; chart.name = "show_chart";
        chart.description = u8"把数据画成图表卡片,显示在屏幕右上角。"
            u8"type 取值 bar(横向条形图)/donut(环形图)/line(折线图)/stat(单个大数字);"
            u8"labels 与 values 两个数组需等长,分别是每项的标签与数值(stat 图只用第一项:"
            u8"values[0] 是大数字,labels[0] 可选,作为副标题)。"
            u8"适合在用户要求“画个图”“统计一下”“可视化”之类需求,或你自己觉得图表比文字更清楚时调用。";
        chart.parametersJson = "{\"type\":\"object\",\"properties\":{"
            "\"type\":{\"type\":\"string\",\"enum\":[\"bar\",\"donut\",\"line\",\"stat\"]},"
            "\"title\":{\"type\":\"string\"},"
            "\"labels\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
            "\"values\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}}},"
            "\"required\":[\"type\",\"values\"]}";
        chart.execute = [ui](const picojson::value& args) {
            if (!ui)
            {
                picojson::object err; err["error"] = picojson::value("ui unavailable");
                return picojson::value(err);
            }
            if (!args.is<picojson::object>() || !args.contains("type")
                || !args.get("type").is<std::string>() || !args.contains("values")
                || !args.get("values").is<picojson::array>())
            {
                picojson::object err;
                err["error"] = picojson::value("spec needs string \"type\" and array \"values\"");
                return picojson::value(err);
            }
            std::string t = args.get("type").get<std::string>();
            size_t n = args.get("values").get<picojson::array>().size();
            ui->pushChart(args);
            OSG_NOTICE << "[AIChat] show_chart type=" << t << " items=" << n << std::endl;
            picojson::object r; r["ok"] = picojson::value(true); return picojson::value(r);
        };
        aiRegistry->add(chart);

        // generate_photo:抓当前渲染帧 → 喂给 Gemini 图像模型 → 生成实拍照片,异步 Job,
        // 结果出现在右上角照片卡(见 ai_media.cpp/ai_cards.cpp)。mediaMgr 为空(无 key 也
        // 无 EARTH_AI_FAKE_IMG)时直接报错,不注册也可以,但注册后模型能看到"为什么不行"
        // 比工具压根不存在更利于它跟用户解释。
        earthai::Tool photo; photo.name = "generate_photo";
        photo.description = u8"截取当前三维地球视角并生成一张同地点同视角的写实照片,"
            u8"异步执行(不会阻塞对话),结果出现在屏幕右上角的照片卡片里。"
            u8"style 可选:风格/时代/天气描述(如“黄昏”“雪天”“1900 年老照片风格”)。";
        photo.parametersJson = "{\"type\":\"object\",\"properties\":{"
            "\"style\":{\"type\":\"string\",\"description\":\"可选:风格/时代/天气描述\"}}}";
        earthai::MediaManager* mediaPtr = mediaMgr;
        osgVerse::EarthManipulator* maniPhoto = mani;
        photo.execute = [mediaPtr, maniPhoto](const picojson::value& args) {
            if (!mediaPtr)
            {
                picojson::object err;
                err["error"] = picojson::value(std::string(
                    "media pipeline unavailable (no EARTH_AI_KEY / EARTH_AI_FAKE_IMG)"));
                return picojson::value(err);
            }
            std::string style;
            if (args.is<picojson::object>() && args.contains("style")
                && args.get("style").is<std::string>())
                style = args.get("style").get<std::string>();

            bool haveView = false; double lat = 0.0, lon = 0.0, altKm = 0.0;
            if (maniPhoto)
            {
                osg::Vec3d lla = maniPhoto->computeEyeLatLonHeight();
                lat = osg::RadiansToDegrees(lla[0]);
                lon = osg::RadiansToDegrees(lla[1]);
                altKm = lla[2] / 1000.0;
                haveView = true;
            }
            picojson::value r = mediaPtr->startPhotoJob(style, lat, lon, altKm, haveView);
            OSG_NOTICE << "[AIChat] generate_photo -> " << r.serialize() << std::endl;
            return r;
        };
        aiRegistry->add(photo);

        // generate_video(Task 9):自然语言路径与 🎬 按钮共用同一套两点采集 + 确认 Modal
        // 状态机(MediaManager 的 videoPhase()/beginVideoCapture()/captureVideoEnd()),
        // 工具本身绝不跳过确认弹窗——即使是模型自主调用,也只推进"记录 A 点"/"记录 B 点"
        // 两步,真正提交(花钱)那一步永远要用户在 Modal 里点「确认」(confirmVideo() 由
        // ai_ui.cpp 的 Modal 按钮调用,这里不调用)。
        earthai::Tool video; video.name = "generate_video";
        video.description = u8"生成一段首尾帧巡航视频(Veo):记录当前相机位置为起点 A,"
            u8"提示用户移动相机到目标位置后再次调用本工具记录终点 B,"
            u8"两点都记录后会弹出确认框(涉及费用,约 $2-6),用户确认后才真正开始生成。"
            u8"本工具从不跳过确认框。";
        video.parametersJson = "{\"type\":\"object\",\"properties\":{"
            "\"style\":{\"type\":\"string\",\"description\":\"预留:风格描述(暂未使用)\"}}}";
        earthai::MediaManager* mediaVideoPtr = mediaMgr;
        osgVerse::EarthManipulator* maniVideo = mani;
        video.execute = [mediaVideoPtr, maniVideo](const picojson::value&) {
            if (!mediaVideoPtr)
            {
                picojson::object err;
                err["error"] = picojson::value(std::string(
                    "media pipeline unavailable (no EARTH_AI_KEY / EARTH_AI_FAKE_MP4)"));
                return picojson::value(err);
            }
            osg::Vec3d lla = maniVideo ? maniVideo->computeEyeLatLonHeight() : osg::Vec3d();

            earthai::VideoPhaseKindPublic phase = mediaVideoPtr->videoPhase();
            if (phase == earthai::VIDEO_IDLE)
            {
                bool ok = mediaVideoPtr->beginVideoCapture(lla);
                picojson::object r;
                if (!ok) { r["error"] = picojson::value(std::string("failed to start video capture")); return picojson::value(r); }
                r["status"] = picojson::value(std::string("awaiting_second_point"));
                r["note"] = picojson::value(std::string(
                    u8"已记录起点,请移动相机到目标点后点击视频按钮完成,或再次调用本工具"));
                OSG_NOTICE << "[AIChat] generate_video tool -> awaiting_second_point" << std::endl;
                return picojson::value(r);
            }
            if (phase == earthai::VIDEO_WAIT_B)
            {
                bool ok = mediaVideoPtr->captureVideoEnd(lla);
                picojson::object r;
                if (!ok) { r["error"] = picojson::value(std::string("failed to capture second point")); return picojson::value(r); }
                r["status"] = picojson::value(std::string("awaiting_confirmation"));
                r["note"] = picojson::value(std::string(
                    u8"两点已记录,请在弹出的确认框中确认(涉及费用)"));
                OSG_NOTICE << "[AIChat] generate_video tool -> awaiting_confirmation" << std::endl;
                return picojson::value(r);
            }
            // 其它阶段(A 点/B 点还在抓帧稳定中、已等待确认、或已确认在生成中):
            // 明确告知模型当前状态,不做任何状态转换——避免工具在瞬态阶段被重复调用时
            // 产生意外跳转(例如正在等确认时又把 B 点覆盖掉)。
            picojson::object r;
            const char* phaseName =
                (phase == earthai::VIDEO_WAIT_A) ? "capturing_first_point" :
                (phase == earthai::VIDEO_CAPTURING_B) ? "capturing_second_point" :
                (phase == earthai::VIDEO_AWAIT_CONFIRM) ? "awaiting_confirmation" : "running";
            r["status"] = picojson::value(std::string(phaseName));
            r["note"] = picojson::value(std::string(u8"视频流程正在进行中,请稍候或在界面上操作"));
            return picojson::value(r);
        };
        aiRegistry->add(video);
    }
    const char* aiKey = getenv("EARTH_AI_KEY");
    const char* aiFake = getenv("EARTH_AI_FAKE");
    if (aiFake && *aiFake)
    {
        // 注意:app 启动早期会把 cwd 切到可执行文件目录(见启动日志 "Working directory"),
        // EARTH_AI_FAKE 用相对路径会相对该目录解析而非命令行所在目录——fixture 建议一律
        // 传绝对路径。loadFromFile 失败时下面的 OSG_WARN 把传入路径原样打出来,便于排查。
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
        viewer.addEventHandler(new AIFrameHandler(aiCore, mediaMgr, mani,
            (autoSubmit && *autoSubmit) ? autoSubmit : "", delayFrames));
    }
    runtime.core = aiCore;
    return runtime;
}
