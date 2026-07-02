// applications/earth_explorer/ai_media.cpp
// 快照(抓帧)+ 生图管线实现。设计与 ai_chat.cpp 的 worker 模式一致:耗时的网络调用
// 放独立线程,主线程只在 update() 里轮询状态、绝不阻塞。
#include "ai_media.h"
#include "ai_cards.h"
#include "3rdparty/libhv/all/client/requests.h"
#include "3rdparty/libhv/all/base64.h"
#include <osgViewer/ViewerEventHandlers>
#include <osgDB/FileUtils>
#include <osg/Notify>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <memory>
#include <sstream>
#include <cstdlib>
#include <ctime>

namespace earthai
{
    // 错误摘要截断,与 ai_chat.cpp::truncate200 同规则(避免长 base64/大段文本刷屏日志)。
    static std::string truncate200(const std::string& s)
    {
        if (s.size() <= 200) return s;
        std::string r = s.substr(0, 200);
        while (!r.empty() && ((unsigned char)r.back() & 0xC0) == 0x80) r.pop_back();
        if (!r.empty() && (unsigned char)r.back() >= 0xC0) r.pop_back();
        return r;
    }

    static bool readFileBytes(const std::string& path, std::string& out)
    {
        std::ifstream ifs(path.c_str(), std::ios::binary);
        if (!ifs.is_open()) return false;
        std::stringstream ss; ss << ifs.rdbuf();
        out = ss.str();
        return true;
    }

    // grab(pngPath) 的实际落盘文件:去掉 ".png" 再拼 ScreenCaptureHandler 的 "_0.png" 后缀。
    // 此逻辑曾在 grab/ready/照片提交三处各写一份,视频提交第四份拷贝时把后缀直接追加到
    // ".png" 之后酿成真机 bug(离线 FAKE 路径跳过读快照,测不到)—— 统一收敛到这里。
    static std::string capturedPath(const std::string& pngPath)
    {
        std::string prefix = pngPath;
        const std::string ext = ".png";
        if (prefix.size() >= ext.size() && prefix.compare(prefix.size() - ext.size(), ext.size(), ext) == 0)
            prefix.resize(prefix.size() - ext.size());
        return prefix + "_0.png";
    }

    static bool writeFileBytes(const std::string& path, const std::string& bytes)
    {
        std::ofstream ofs(path.c_str(), std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return false;
        ofs.write(bytes.data(), (std::streamsize)bytes.size());
        return ofs.good();
    }

    // ---------------- SnapshotGrabber ----------------

    SnapshotGrabber::SnapshotGrabber(osgViewer::Viewer* viewer) : _viewer(viewer), _lastSize(0)
    {
        // 唯一一个 ScreenCaptureHandler,构造时创建并 addEventHandler 一次;grab() 只更新
        // 它内部 WriteToFile 的捕获目标(setCaptureOperation),不再重复 addEventHandler ——
        // 修复此前"每次 grab() 都 new 一个 handler 且从不摘除"导致 viewer 上 handler 无限
        // 累积的问题(N 次 grab 后 viewer 挂了 N 个 handler)。
        // WriteToFile 的 filename 是"不含扩展名的前缀",实际路径由 EARTH_AUTOCAP 同款规则
        // 拼成 "<prefix>_0.<ext>"(单 context、OVERWRITE 策略下固定后缀 "_0")。
        osg::ref_ptr<osgViewer::ScreenCaptureHandler::WriteToFile> writer =
            new osgViewer::ScreenCaptureHandler::WriteToFile(
                "", "png", osgViewer::ScreenCaptureHandler::WriteToFile::OVERWRITE);
        _capturer = new osgViewer::ScreenCaptureHandler(writer.get(), 1);
        // ScreenCaptureHandler 默认响应键盘 'c' 触发截屏(见 handle() 里 _keyEventTakeScreenShot);
        // 这会导致用户在 AI 拍照流程之外按一次 'c' 就写出一份意料之外的文件(用当前 WriteToFile
        // 前缀,即最近一次 grab() 的目标)。setKeyEventTakeScreenShot(0) 存在于此版本 OSG
        // (build/sdk_core/include/osgViewer/ViewerEventHandlers 已确认),0 不是合法 GUIEventAdapter
        // 键值,等效关闭该热键。
        _capturer->setKeyEventTakeScreenShot(0);
        _viewer->addEventHandler(_capturer.get());
    }

    void SnapshotGrabber::grab(const std::string& pngPath)
    {
        // pngPath 末尾去掉 ".png" 作为 WriteToFile 的前缀(它自己会拼回 "_0.png")。
        std::string prefix = pngPath;
        const std::string ext = ".png";
        if (prefix.size() >= ext.size() && prefix.compare(prefix.size() - ext.size(), ext.size(), ext) == 0)
            prefix.resize(prefix.size() - ext.size());

        // 只重设捕获目标(新 WriteToFile 覆盖旧的 CaptureOperation),复用同一个已挂载的
        // handler,不再 addEventHandler。
        osg::ref_ptr<osgViewer::ScreenCaptureHandler::WriteToFile> writer =
            new osgViewer::ScreenCaptureHandler::WriteToFile(
                prefix, "png", osgViewer::ScreenCaptureHandler::WriteToFile::OVERWRITE);
        _capturer->setCaptureOperation(writer.get());
        _capturer->setFramesToCapture(1);
        _capturer->captureNextFrame(*_viewer);
        _lastSize = 0;   // 新一轮抓帧:清掉上一轮遗留的大小记录,避免 ready() 首次调用就误判稳定
        OSG_NOTICE << "[AIChat] snapshot grab -> " << pngPath << std::endl;
    }

    bool SnapshotGrabber::ready(const std::string& pngPath)
    {
        // WriteToFile 用 "_0" 后缀(单 GraphicsContext、OVERWRITE 策略)拼实际文件名,
        // 统一经 capturedPath() 计算,与提交侧读取路径保持一致。
        std::string actual = capturedPath(pngPath);

        // 写盘是渲染线程在捕获回调里做的,文件可能"存在但还没写完"——真正的跨帧稳定性:
        // 本次测到的大小要与"上一次 ready() 调用"记录的大小相同才算稳定,而不是同一次调用
        // 内部读两次(那样两次 stat 间隔仅几微秒,写到一半也几乎总能读到同一个字节数,
        // 起不到防御作用)。调用方(MediaManager::update())每帧调一次 ready(),两次真实的
        // update() tick 之间隔了一整帧的时间,足够覆盖磁盘写入延迟。
        if (!osgDB::fileExists(actual)) { _lastSize = 0; return false; }
        std::ifstream ifs(actual.c_str(), std::ios::binary | std::ios::ate);
        if (!ifs.is_open()) { _lastSize = 0; return false; }
        std::streamsize sz = ifs.tellg();
        ifs.close();
        if (sz <= 0) { _lastSize = 0; return false; }

        bool stable = (sz == _lastSize);
        _lastSize = sz;
        return stable;
    }

    // ---------------- GeminiMediaProvider ----------------

    GeminiMediaProvider::GeminiMediaProvider(const std::string& apiKey) : _apiKey(apiKey) {}

    // 防御式解析 candidates[0].content.parts[] 找 inlineData.data(base64),风格与
    // ai_chat.cpp::parseGeminiResponse 一致:任何字段缺失/类型不符都落到 err,不抛异常。
    static bool parseImageResponse(const std::string& body, std::string& outPngBytes, std::string& err)
    {
        picojson::value v;
        std::string perr = picojson::parse(v, body);
        if (!perr.empty() || !v.is<picojson::object>())
        { err = "bad json: " + truncate200(perr.empty() ? body : perr); return false; }

        if (!v.contains("candidates") || !v.get("candidates").is<picojson::array>()
            || v.get("candidates").get<picojson::array>().empty())
        {
            if (v.contains("promptFeedback") && v.get("promptFeedback").is<picojson::object>()
                && v.get("promptFeedback").contains("blockReason"))
                err = "blocked: " + truncate200(v.get("promptFeedback").get("blockReason").to_str());
            else if (v.contains("error") && v.get("error").is<picojson::object>()
                     && v.get("error").contains("message"))
                err = "api error: " + truncate200(v.get("error").get("message").to_str());
            else err = "no candidates: " + truncate200(body);
            return false;
        }

        const picojson::value& cand0 = v.get("candidates").get<picojson::array>()[0];
        if (!cand0.is<picojson::object>() || !cand0.contains("content")
            || !cand0.get("content").is<picojson::object>()
            || !cand0.get("content").contains("parts")
            || !cand0.get("content").get("parts").is<picojson::array>())
        {
            if (cand0.is<picojson::object>() && cand0.contains("finishReason"))
                err = "no content: finishReason=" + truncate200(cand0.get("finishReason").to_str());
            else err = "no content parts: " + truncate200(body);
            return false;
        }

        const picojson::array& parts = cand0.get("content").get("parts").get<picojson::array>();
        for (size_t i = 0; i < parts.size(); ++i)
        {
            const picojson::value& part = parts[i];
            if (!part.is<picojson::object>()) continue;
            if (part.contains("inlineData") && part.get("inlineData").is<picojson::object>())
            {
                const picojson::value& id = part.get("inlineData");
                if (id.contains("data") && id.get("data").is<std::string>())
                {
                    const std::string& b64 = id.get("data").get<std::string>();
                    outPngBytes = hv::Base64Decode(b64.c_str(), (unsigned int)b64.size());
                    if (outPngBytes.empty()) { err = "empty decoded image"; return false; }
                    return true;
                }
            }
        }
        err = "no inlineData in response parts: " + truncate200(body);
        return false;
    }

    std::string GeminiMediaProvider::generateImage(const std::string& pngBytes,
                                                    const std::string& prompt, std::string& err)
    {
        std::string b64 = hv::Base64Encode((const unsigned char*)pngBytes.data(), (unsigned int)pngBytes.size());

        picojson::object textPart; textPart["text"] = picojson::value(prompt);
        picojson::object inlineData;
        inlineData["mime_type"] = picojson::value(std::string("image/png"));
        inlineData["data"] = picojson::value(b64);
        picojson::object imagePart; imagePart["inline_data"] = picojson::value(inlineData);

        picojson::array parts;
        parts.push_back(picojson::value(textPart));
        parts.push_back(picojson::value(imagePart));

        picojson::object content; content["parts"] = picojson::value(parts);
        picojson::array contents; contents.push_back(picojson::value(content));

        picojson::array modalities;
        modalities.push_back(picojson::value(std::string("TEXT")));
        modalities.push_back(picojson::value(std::string("IMAGE")));
        picojson::object genConfig; genConfig["responseModalities"] = picojson::value(modalities);

        picojson::object body;
        body["contents"] = picojson::value(contents);
        body["generationConfig"] = picojson::value(genConfig);

        requests::Request req(new HttpRequest);
        req->method = HTTP_POST;
        req->timeout = 60;   // 生图比对话慢,给足 60s(spec 要求)
        // 注意:key 拼在 URL 里,下面任何日志/错误信息都不得把 req->url 整串打印出来
        req->url = "https://generativelanguage.googleapis.com/v1beta/models/"
                   "gemini-2.5-flash-image:generateContent?key=" + _apiKey;
        req->headers["Content-Type"] = "application/json";
        req->body = picojson::value(body).serialize();

        requests::Response resp = requests::request(req);
        if (!resp || resp->status_code != 200)
        {
            err = "HTTP " + std::to_string(resp ? (int)resp->status_code : -1) +
                  (resp ? (": " + truncate200(resp->body)) : "");
            return std::string();
        }

        std::string outBytes;
        if (!parseImageResponse(resp->body, outBytes, err)) return std::string();
        return outBytes;
    }

    // ---------------- VeoVideoProvider(Task 9)----------------

    VeoVideoProvider::VeoVideoProvider(const std::string& apiKey, const std::string& model)
        : _apiKey(apiKey), _model(model) {}

    // 防御式解析 predictLongRunning 响应,取 "name" 字段(operation 名字)。
    static bool parseOperationName(const std::string& body, std::string& outName, std::string& err)
    {
        picojson::value v;
        std::string perr = picojson::parse(v, body);
        if (!perr.empty() || !v.is<picojson::object>())
        { err = "bad json: " + truncate200(perr.empty() ? body : perr); return false; }
        if (v.contains("name") && v.get("name").is<std::string>())
        { outName = v.get("name").get<std::string>(); return true; }
        if (v.contains("error") && v.get("error").is<picojson::object>()
            && v.get("error").contains("message"))
        { err = "api error: " + truncate200(v.get("error").get("message").to_str()); return false; }
        err = "no operation name: " + truncate200(body);
        return false;
    }

    std::string VeoVideoProvider::submit(const std::string& pngBytesA, const std::string& pngBytesB,
                                         const std::string& motionPrompt, std::string& err)
    {
        std::string b64A = hv::Base64Encode((const unsigned char*)pngBytesA.data(), (unsigned int)pngBytesA.size());
        std::string b64B = hv::Base64Encode((const unsigned char*)pngBytesB.data(), (unsigned int)pngBytesB.size());

        picojson::object image;
        image["bytesBase64Encoded"] = picojson::value(b64A);
        image["mimeType"] = picojson::value(std::string("image/png"));

        picojson::object lastFrame;
        lastFrame["bytesBase64Encoded"] = picojson::value(b64B);
        lastFrame["mimeType"] = picojson::value(std::string("image/png"));

        picojson::object instance;
        instance["prompt"] = picojson::value(motionPrompt);
        instance["image"] = picojson::value(image);
        instance["lastFrame"] = picojson::value(lastFrame);

        picojson::array instances; instances.push_back(picojson::value(instance));
        picojson::object body; body["instances"] = picojson::value(instances);

        requests::Request req(new HttpRequest);
        req->method = HTTP_POST;
        req->timeout = 60;
        // 注意:key 拼在 URL 里,下面任何日志/错误信息都不得把 req->url 整串打印出来
        req->url = "https://generativelanguage.googleapis.com/v1beta/models/" + _model +
                   ":predictLongRunning?key=" + _apiKey;
        req->headers["Content-Type"] = "application/json";
        req->body = picojson::value(body).serialize();

        requests::Response resp = requests::request(req);
        if (!resp || resp->status_code != 200)
        {
            err = "HTTP " + std::to_string(resp ? (int)resp->status_code : -1) +
                  (resp ? (": " + truncate200(resp->body)) : "");
            return std::string();
        }

        std::string opName;
        if (!parseOperationName(resp->body, opName, err)) return std::string();
        return opName;
    }

    void VeoVideoProvider::poll(const std::string& operationName, bool& done,
                                std::string& mp4Bytes, std::string& err)
    {
        done = false; mp4Bytes.clear(); err.clear();

        requests::Request req(new HttpRequest);
        req->method = HTTP_GET;
        req->timeout = 30;
        // operationName 形如 "models/veo-3.1-generate-001/operations/xxxx",GET v1beta/<name>。
        req->url = "https://generativelanguage.googleapis.com/v1beta/" + operationName + "?key=" + _apiKey;

        requests::Response resp = requests::request(req);
        if (!resp || resp->status_code != 200)
        {
            err = "HTTP " + std::to_string(resp ? (int)resp->status_code : -1) +
                  (resp ? (": " + truncate200(resp->body)) : "");
            done = true;   // HTTP 层面出错视为终态失败,不再重试(调用方按 err 非空判失败)
            return;
        }

        picojson::value v;
        std::string perr = picojson::parse(v, resp->body);
        if (!perr.empty() || !v.is<picojson::object>())
        { err = "bad json: " + truncate200(perr.empty() ? resp->body : perr); done = true; return; }

        if (!v.contains("done") || !v.get("done").is<bool>() || !v.get("done").get<bool>())
        { done = false; return; }   // 仍在跑

        done = true;
        if (v.contains("error") && v.get("error").is<picojson::object>())
        {
            std::string msg = v.get("error").contains("message")
                ? v.get("error").get("message").to_str() : resp->body;
            err = "operation error: " + truncate200(msg);
            return;
        }
        if (!v.contains("response") || !v.get("response").is<picojson::object>())
        { err = "done but no response field: " + truncate200(resp->body); return; }

        const picojson::value& response = v.get("response");
        if (!response.contains("generateVideoResponse")
            || !response.get("generateVideoResponse").is<picojson::object>())
        { err = "no generateVideoResponse: " + truncate200(resp->body); return; }

        const picojson::value& gvr = response.get("generateVideoResponse");
        if (!gvr.contains("generatedSamples") || !gvr.get("generatedSamples").is<picojson::array>()
            || gvr.get("generatedSamples").get<picojson::array>().empty())
        { err = "no generatedSamples: " + truncate200(resp->body); return; }

        const picojson::value& sample0 = gvr.get("generatedSamples").get<picojson::array>()[0];
        if (!sample0.is<picojson::object>() || !sample0.contains("video")
            || !sample0.get("video").is<picojson::object>())
        { err = "no video in sample: " + truncate200(resp->body); return; }

        const picojson::value& video = sample0.get("video");
        // 防御两种形状:bytesBase64Encoded(内联字节)或 uri(需要再发一次 GET 下载,
        // uri 可能没带 key 查询参数,这里统一补上——已带 "?" 的话改用 "&" 拼接)。
        if (video.contains("bytesBase64Encoded") && video.get("bytesBase64Encoded").is<std::string>())
        {
            const std::string& b64 = video.get("bytesBase64Encoded").get<std::string>();
            mp4Bytes = hv::Base64Decode(b64.c_str(), (unsigned int)b64.size());
            if (mp4Bytes.empty()) err = "empty decoded video bytes";
            return;
        }
        if (video.contains("uri") && video.get("uri").is<std::string>())
        {
            std::string uri = video.get("uri").get<std::string>();
            std::string sep = (uri.find('?') != std::string::npos) ? "&" : "?";
            std::string dlUrl = uri + sep + "key=" + _apiKey;

            requests::Request dlReq(new HttpRequest);
            dlReq->method = HTTP_GET;
            dlReq->timeout = 60;
            dlReq->url = dlUrl;
            requests::Response dlResp = requests::request(dlReq);
            if (!dlResp || dlResp->status_code != 200)
            {
                err = "video download HTTP " + std::to_string(dlResp ? (int)dlResp->status_code : -1);
                return;
            }
            mp4Bytes = dlResp->body;
            if (mp4Bytes.empty()) err = "empty downloaded video";
            return;
        }
        err = "video object has neither bytesBase64Encoded nor uri: " + truncate200(resp->body);
    }

    // ---------------- MediaManager ----------------

    // 输出目录:EARTH_AI_OUTDIR 覆盖,默认 $HOME/Pictures/EarthExplorer(仅算一次,
    // 与 EARTH_AI_FAKE_IMG 一样用 static 局部量作"进程期只读一次"的缓存)。
    static std::string outDir()
    {
        static std::string dir;
        static bool inited = false;
        if (!inited)
        {
            inited = true;
            const char* env = getenv("EARTH_AI_OUTDIR");
            if (env && *env) dir = env;
            else
            {
                const char* home = getenv("HOME");
                dir = (home && *home) ? (std::string(home) + "/Pictures/EarthExplorer") : "./EarthExplorer_out";
            }
            osgDB::makeDirectory(dir);
        }
        return dir;
    }

    // EARTH_AI_FAKE_IMG=<png路径>:离线 E2E 用,generateImage 步骤替换成拷贝该文件字节
    // (不联网、不需要 key)。只读一次环境变量(static),与 EARTH_AI_FAKE 的约定一致。
    static bool fakeImgPath(std::string& path)
    {
        static std::string cached;
        static bool inited = false, has = false;
        if (!inited)
        {
            inited = true;
            const char* env = getenv("EARTH_AI_FAKE_IMG");
            if (env && *env) { cached = env; has = true; }
        }
        if (has) path = cached;
        return has;
    }

    // EARTH_AI_FAKE_MP4=<mp4路径>:离线 E2E 用,confirmVideo 的"提交+轮询+下载"整段替换成
    // 拷贝该文件字节(不联网、不需要 key),模拟一个短暂延迟(见 VideoJob::Phase 注释)后
    // 直接进入完成态。只读一次环境变量(static),与 fakeImgPath 的约定一致。
    static bool fakeMp4Path(std::string& path)
    {
        static std::string cached;
        static bool inited = false, has = false;
        if (!inited)
        {
            inited = true;
            const char* env = getenv("EARTH_AI_FAKE_MP4");
            if (env && *env) { cached = env; has = true; }
        }
        if (has) path = cached;
        return has;
    }

    // Veo 模型名:EARTH_AI_VIDEO_MODEL 覆盖。默认 fast 变体(真机 key 实测 2026-07:该 key 可见
    // veo-3.1-{generate,fast-generate,lite-generate}-preview,无 -001 GA 名;fast 约省一半费用,
    // 追求画质可 env 换 veo-3.1-generate-preview)。(与 EARTH_AI_MODEL
    // 是两个独立配置项——对话模型与视频模型通常不是同一个)。
    static std::string videoModel()
    {
        const char* env = getenv("EARTH_AI_VIDEO_MODEL");
        return (env && *env) ? std::string(env) : std::string("veo-3.1-fast-generate-preview");
    }

    // WAITING_SNAPSHOT 状态下 update() 的超时阈值(见类头注释与 update() 里的判定)。
    // 600 次≈600 帧,正常帧率下几秒钟就该等到快照文件;真出现"文件永不出现"的极端情况
    // (例如磁盘写失败但没报错、或 CaptureOperation 目标被意外替换),超过这个次数就判超时,
    // 避免 job 永远卡在 WAITING_SNAPSHOT、后续所有 generate_photo 都被"already running"拒绝。
    static const int kWaitSnapshotTimeoutTicks = 600;

    // ---------------- 视频状态机(Task 9)----------------
    // review 建议:与照片流程字段雷同的地方一旦重复三次以上就该提取——视频流程比照片多一个
    // "两点采集 + 确认 Modal"前置阶段,以及提交/轮询两段网络交互,字段数明显更多,硬塞进
    // MediaManager 本体会让类过胖;单独一个 VideoJob 结构体把"一次视频任务从头到尾"的所有
    // 状态收在一起,MediaManager 只持有一个指针(_video,构造时 new,析构时 delete)。
    struct MediaManager::VideoJob
    {
        enum Phase
        {
            IDLE = 0,           // 空闲
            WAIT_A,             // A 点快照抓取中(还没稳定)
            WAIT_B,             // A 点已就绪,等待用户触发 B 点采集(beginVideoCapture 已完成)
            CAPTURING_B,        // 已触发 B 点采集,B 点快照抓取中
            AWAIT_CONFIRM,      // A/B 都就绪,等待用户在确认 Modal 里点「确认」
            SUBMITTING,         // 已确认,worker 线程正在提交 Veo predictLongRunning 请求
            POLLING,            // 已拿到 operation 名字,worker 线程在轮询直到 done
            DOWNLOADING_FAKE    // EARTH_AI_FAKE_MP4 路径的"模拟延迟"阶段(见 update() 里的 tick 判断)
            // review:曾有 DONE_HANDLED 收尾态,但 phase 从未被赋成它——DONE/FAILED/超时
            // 三条路径都是"处理完直接 resetVideo() 回 IDLE"(见 POLLING 分支与
            // DOWNLOADING_FAKE 分支),不存在"下一帧再回 IDLE"这一步,枚举值和对应分支都是
            // 死代码,已删除。
        };

        Phase phase = IDLE;
        int jobId = 0;
        osg::Vec3d llaA, llaB;
        std::string snapPathA, snapPathB;   // 传给 SnapshotGrabber::grab() 的路径(不含 "_0" 后缀)
        std::string motionPrompt;
        std::string mp4Path;                // 最终保存路径(worker 完成后写入)
        std::string operationName;          // Veo predictLongRunning 返回的 operation 名字

        std::thread worker;
        bool workerJoinable = false;

        int waitSnapshotTicks = 0;   // WAIT_A/CAPTURING_B 等待快照稳定的计数,超时判定复用 kWaitSnapshotTimeoutTicks
        int fakeDelayTicks = 0;      // DOWNLOADING_FAKE 阶段的模拟延迟计数(约 100 ticks,见 spec)
        int pollTicks = 0;           // POLLING 阶段累计经过的 tick 数(用于超时判定与进度爬升)
        int ticksSinceLastPoll = 0;  // 距上一次发起轮询过去的 tick 数,达到 kPollIntervalTicks 才发下一次 GET
        bool pollInFlight = false;   // 当前是否有一次轮询 worker 正在跑(避免同时发出两个 GET)

        // bug fix:一个"已经跑完但还没被主线程 join"的 std::thread 仍然 joinable()——
        // 之前用 "!v.worker.joinable()" 当"worker 已完成"的信号是错的,join() 之前它永远
        // 是 true,导致 pollInFlight 永远不清零、后续轮询被 :994 的守卫永久拦住(真实 Veo
        // 任务必然撞 10 分钟超时)。改用 worker 主动写的完成标志位 pollDone 来判断。
        // std::atomic<bool> 本身不可移动,而 resetVideo() 里 "*_video = VideoJob()" 是
        // move-assign(std::thread 成员要求),两者冲突;这里选"共享指针包一层"的方案——
        // 侵入最小:VideoJob 依旧可移动/可默认重置,resetVideo() 的"先 join 再整体赋值"
        // 语义不用改。每次起一次新的轮询 worker 前重新 make_shared 一个新 flag(而不是复用
        // 旧的),避免上一轮遗留的 flag 对象被下一轮误读。
        std::shared_ptr<std::atomic<bool>> pollDone = std::make_shared<std::atomic<bool>>(false);
    };

    MediaManager::MediaManager(osgViewer::Viewer* viewer, AICardPanel* cards, const std::string& apiKeyOrEmpty)
        : _viewer(viewer), _cards(cards), _apiKey(apiKeyOrEmpty), _grabber(viewer),
          _state(IDLE), _jobId(0), _workerJoinable(false), _waitSnapshotTicks(0),
          _video(new VideoJob), _videoGrabber(viewer)
    {}

    MediaManager::~MediaManager()
    {
        joinWorkerIfAny();
        if (_video)
        {
            if (_video->workerJoinable && _video->worker.joinable()) _video->worker.join();
            delete _video;
        }
    }

    void MediaManager::joinWorkerIfAny()
    {
        if (_workerJoinable && _worker.joinable())
        {
            _worker.join();
            _workerJoinable = false;
        }
    }

    picojson::value MediaManager::startPhotoJob(const std::string& stylePrompt, double lat, double lon,
                                                double altKm, bool haveView)
    {
        if (_state != IDLE)
        {
            picojson::object err; err["error"] = picojson::value(std::string("photo job already running"));
            return picojson::value(err);
        }

        std::string fakeImg;
        bool hasFake = fakeImgPath(fakeImg);
        if (_apiKey.empty() && !hasFake)
        {
            picojson::object err;
            err["error"] = picojson::value(std::string("no EARTH_AI_KEY and no EARTH_AI_FAKE_IMG configured"));
            return picojson::value(err);
        }

        long long epoch = (long long)time(nullptr);
        std::string dir = outDir();
        _snapPath = dir + "/snap_" + std::to_string(epoch) + ".png";
        _genPath = dir + "/gen_" + std::to_string(epoch) + ".png";

        // Prompt:三维地球渲染图为构图参考 + 视角元数据(拿不到就省略,不硬凑)+ 用户风格词。
        std::string p = u8"以这张三维地球渲染图为构图参考,生成同一地点同一视角的真实照片。";
        if (haveView)
        {
            char buf[256];
            snprintf(buf, sizeof(buf), u8"位置:纬度%.4f 经度%.4f 高度%.1f千米。", lat, lon, altKm);
            p += buf;
        }
        p += u8"真实光影、真实材质。";
        if (!stylePrompt.empty()) p += stylePrompt;
        _prompt = p;

        _jobId = _jobs.create("photo", u8"生成实景照片");
        _jobs.update(_jobId, AIJob::RUNNING, 0.1f, "", "");
        if (_cards) _cards->pushJob(&_jobs, _jobId, u8"生成实景照片");
        _grabber.grab(_snapPath);
        _state = WAITING_SNAPSHOT;
        _waitSnapshotTicks = 0;  // 重新计数,供 update() 判断等待超时

        OSG_NOTICE << "[AIChat] generate_photo job=" << _jobId << " snap=" << _snapPath << std::endl;

        picojson::object r;
        r["status"] = picojson::value(std::string("started"));
        r["job_id"] = picojson::value((double)_jobId);
        return picojson::value(r);
    }

    void MediaManager::update()
    {
        // 视频状态机与照片状态机相互独立(两条流程可以同时进行,互不阻塞),照片状态机
        // 下面各分支里散布着多个提前 return——先驱动一次视频状态机,避免视频推进逻辑被
        // 挂在某个 return 之后而"post-photo-return 才执行"从而实际上跑不到。
        updateVideoInternal();

        if (_state == WAITING_SNAPSHOT)
        {
            if (!_grabber.ready(_snapPath))
            {
                // 快照文件迟迟不出现(例如捕获回调没被触发/磁盘异常):计数超阈值就判超时,
                // 收尾 job 为 FAILED 并把状态机拉回 IDLE,避免永久卡住导致后续 generate_photo
                // 全部被 startPhotoJob 的 "photo job already running" 拒绝。
                if (++_waitSnapshotTicks > kWaitSnapshotTimeoutTicks)
                {
                    _jobs.update(_jobId, AIJob::FAILED, 1.0f, "", "snapshot timeout");
                    if (_cards) _cards->removeJob(_jobId);
                    OSG_WARN << "[AIChat] photo job timeout waiting snapshot" << std::endl;
                    _state = IDLE;
                }
                return;
            }

            std::string snapBytes;
            std::string actual = capturedPath(_snapPath);

            if (!readFileBytes(actual, snapBytes) || snapBytes.empty())
            {
                _jobs.update(_jobId, AIJob::FAILED, 1.0f, "", "failed to read snapshot file");
                if (_cards) _cards->removeJob(_jobId);
                OSG_WARN << "[AIChat] photo job " << _jobId << " failed: cannot read " << actual << std::endl;
                _state = DONE_HANDLED;
                return;
            }

            _jobs.update(_jobId, AIJob::RUNNING, 0.4f, "", "");
            _state = GENERATING;

            joinWorkerIfAny();
            std::string apiKey = _apiKey;
            std::string prompt = _prompt;
            std::string genPath = _genPath;
            int jobId = _jobId;
            JobManager* jobsPtr = &_jobs;
            std::string fakeImg; bool hasFake = fakeImgPath(fakeImg);

            _worker = std::thread([snapBytes, prompt, genPath, jobId, jobsPtr, apiKey, hasFake, fakeImg]()
            {
                std::string outBytes, err;
                if (hasFake)
                {
                    // 离线 E2E:直接拷贝指定 PNG 的字节作为"生成结果",不联网不需要 key。
                    if (!readFileBytes(fakeImg, outBytes) || outBytes.empty())
                        err = "EARTH_AI_FAKE_IMG unreadable: " + fakeImg;
                }
                else
                {
                    GeminiMediaProvider provider(apiKey);
                    try { outBytes = provider.generateImage(snapBytes, prompt, err); }
                    catch (const std::exception& e)
                    { err = std::string("provider exception: ") + e.what(); }
                }

                if (outBytes.empty())
                {
                    jobsPtr->update(jobId, AIJob::FAILED, 1.0f, "", err.empty() ? "unknown error" : err);
                    return;
                }
                if (!writeFileBytes(genPath, outBytes))
                {
                    jobsPtr->update(jobId, AIJob::FAILED, 1.0f, "", "failed to write " + genPath);
                    return;
                }
                jobsPtr->update(jobId, AIJob::DONE, 1.0f, genPath, "");
            });
            _workerJoinable = true;
            return;
        }

        if (_state == GENERATING)
        {
            AIJob snap;
            if (!_jobs.get(_jobId, snap)) { _state = DONE_HANDLED; return; }
            if (snap.status == AIJob::DONE)
            {
                joinWorkerIfAny();
                if (_cards) { _cards->removeJob(_jobId); _cards->pushPhoto(snap.resultPath, u8"实景照片"); }
                OSG_NOTICE << "[AIChat] photo job done -> " << snap.resultPath << std::endl;
                _state = DONE_HANDLED;
            }
            else if (snap.status == AIJob::FAILED)
            {
                joinWorkerIfAny();
                if (_cards) _cards->removeJob(_jobId);
                OSG_WARN << "[AIChat] photo job " << _jobId << " failed: " << snap.error << std::endl;
                _state = DONE_HANDLED;
            }
            return;
        }

        if (_state == DONE_HANDLED)
        {
            // 一次性收尾:回到 IDLE,允许下一次 generate_photo。
            _state = IDLE;
        }
    }

    // ---------------- 视频两点巡航流程实现(Task 9)----------------

    VideoPhaseKindPublic MediaManager::videoPhase() const
    {
        switch (_video->phase)
        {
        case VideoJob::IDLE:            return VIDEO_IDLE;
        case VideoJob::WAIT_A:          return VIDEO_WAIT_A;
        case VideoJob::WAIT_B:          return VIDEO_WAIT_B;
        case VideoJob::CAPTURING_B:     return VIDEO_CAPTURING_B;
        case VideoJob::AWAIT_CONFIRM:   return VIDEO_AWAIT_CONFIRM;
        default:                        return VIDEO_RUNNING;   // SUBMITTING/POLLING/DOWNLOADING_FAKE
        }
    }

    bool MediaManager::beginVideoCapture(const osg::Vec3d& llaA)
    {
        if (_video->phase != VideoJob::IDLE) return false;

        long long epoch = (long long)time(nullptr);
        std::string dir = outDir();
        _video->llaA = llaA;
        _video->snapPathA = dir + "/tourA_" + std::to_string(epoch) + ".png";
        _videoGrabber.grab(_video->snapPathA);
        _video->phase = VideoJob::WAIT_A;
        _video->waitSnapshotTicks = 0;

        OSG_NOTICE << "[AIChat] generate_video phase=A snap=" << _video->snapPathA << std::endl;
        return true;
    }

    bool MediaManager::captureVideoEnd(const osg::Vec3d& llaB)
    {
        // 只有"A 点已经稳定就绪、且还没触发过 B 点采集"这一个阶段允许调用——WAIT_B 是
        // beginVideoCapture 完成后 update() 里自动进入的稳态(见 update()/updateVideo() 里
        // WAIT_A→WAIT_B 的转换),不是"WAIT_A 尚在等待快照稳定"那个瞬态。
        if (_video->phase != VideoJob::WAIT_B) return false;

        long long epoch = (long long)time(nullptr);
        std::string dir = outDir();
        _video->llaB = llaB;
        _video->snapPathB = dir + "/tourB_" + std::to_string(epoch) + ".png";
        _videoGrabber.grab(_video->snapPathB);
        _video->phase = VideoJob::CAPTURING_B;
        _video->waitSnapshotTicks = 0;

        OSG_NOTICE << "[AIChat] generate_video phase=B snap=" << _video->snapPathB << std::endl;
        return true;
    }

    MediaManager::PendingVideoInfo MediaManager::pendingVideoInfo() const
    {
        PendingVideoInfo info;
        info.llaA = _video->llaA; info.llaB = _video->llaB;
        info.ready = (_video->phase == VideoJob::AWAIT_CONFIRM);
        if (info.ready) info.motionPrompt = _video->motionPrompt;
        return info;
    }

    picojson::value MediaManager::confirmVideo()
    {
        if (_video->phase != VideoJob::AWAIT_CONFIRM)
        {
            picojson::object err;
            err["error"] = picojson::value(std::string("no video pending confirmation"));
            return picojson::value(err);
        }

        std::string fakeMp4;
        bool hasFake = fakeMp4Path(fakeMp4);
        if (_apiKey.empty() && !hasFake)
        {
            picojson::object err;
            err["error"] = picojson::value(std::string("no EARTH_AI_KEY and no EARTH_AI_FAKE_MP4 configured"));
            return picojson::value(err);
        }

        long long epoch = (long long)time(nullptr);
        _video->mp4Path = outDir() + "/tour_" + std::to_string(epoch) + ".mp4";
        _video->jobId = _jobs.create("video", u8"生成巡航视频");
        _jobs.update(_video->jobId, AIJob::RUNNING, 0.2f, "", "");
        if (_cards) _cards->pushJob(&_jobs, _video->jobId, u8"生成巡航视频");

        if (hasFake)
        {
            // 离线 E2E:跳过网络,模拟一个短延迟(~100 ticks,见类头 spec)后直接"完成"。
            _video->fakeDelayTicks = 0;
            _video->phase = VideoJob::DOWNLOADING_FAKE;
            OSG_NOTICE << "[AIChat] video autotest confirm (fake mp4=" << fakeMp4 << ")" << std::endl;
        }
        else
        {
            // 真网络:起 worker 线程读两张快照 PNG + 提交 predictLongRunning,拿到 operation
            // 名字后写回 job 的 resultPath 字段(借用它临时存 operationName,DONE 之前
            // resultPath 语义上还没定成"最终产物路径",复用它避免再加一个字段);主线程在
            // POLLING 阶段从 job.resultPath 读出 operationName 发起轮询。
            // 注意:视频用 _video->worker 这个独立线程句柄,与照片流程的 _worker 完全分开
            // (两条流程可能同时在跑),因此这里 join 的是 _video->worker 而非 _worker。
            std::string snapA = _video->snapPathA, snapB = _video->snapPathB;
            std::string prompt = _video->motionPrompt;
            std::string apiKey = _apiKey;
            std::string model = videoModel();
            int jobId = _video->jobId;
            JobManager* jobsPtr = &_jobs;

            if (_video->workerJoinable && _video->worker.joinable()) _video->worker.join();
            _video->worker = std::thread([snapA, snapB, prompt, apiKey, model, jobId, jobsPtr]()
            {
                std::string actualA = capturedPath(snapA), actualB = capturedPath(snapB);
                std::string bytesA, bytesB;
                if (!readFileBytes(actualA, bytesA) || bytesA.empty()
                    || !readFileBytes(actualB, bytesB) || bytesB.empty())
                {
                    jobsPtr->update(jobId, AIJob::FAILED, 1.0f, "", "failed to read A/B snapshot files");
                    return;
                }

                VeoVideoProvider provider(apiKey, model);
                std::string err, opName;
                try { opName = provider.submit(bytesA, bytesB, prompt, err); }
                catch (const std::exception& e) { err = std::string("provider exception: ") + e.what(); }

                if (opName.empty())
                {
                    jobsPtr->update(jobId, AIJob::FAILED, 1.0f, "", err.empty() ? "submit failed" : err);
                    return;
                }
                // resultPath 字段借用来传递 operationName(见上方注释),progress 0.3 标志
                // "已提交、开始轮询"。
                jobsPtr->update(jobId, AIJob::RUNNING, 0.3f, opName, "");
            });
            _video->workerJoinable = true;
            _video->phase = VideoJob::SUBMITTING;
        }

        OSG_NOTICE << "[AIChat] generate_video confirmed job=" << _video->jobId << std::endl;
        picojson::object r;
        r["status"] = picojson::value(std::string("started"));
        r["job_id"] = picojson::value((double)_video->jobId);
        return picojson::value(r);
    }

    void MediaManager::cancelVideo()
    {
        if (_video->phase == VideoJob::IDLE) return;
        OSG_NOTICE << "[AIChat] generate_video cancelled (phase="
                   << (int)_video->phase << ")" << std::endl;
        // 取消发生在"确认之前"(WAIT_A/WAIT_B/CAPTURING_B/AWAIT_CONFIRM),此时还没建 Job、
        // 没花任何网络请求成本,直接清零状态即可,无需处理 job(它此时还不存在)——
        // resetVideo() 仍会先检查 worker 是否 joinable(理论上不会,防御一下)。
        // 若未来允许在 SUBMITTING/POLLING 阶段取消(已花钱的请求),需要额外处理 job 收尾——
        // 当前 UI 设计(见 ai_ui.cpp)确认后按钮直接消失,不提供"生成中取消"入口,故不实现。
        resetVideo();
    }

    // std::thread 的 move 赋值要求目标对象此刻不 joinable,否则直接 std::terminate——
    // 所有"重置视频状态回 VideoJob() 初值"的地方统一走这个函数,先 join 掉可能还
    // joinable 的 worker(正常路径上各调用点在此之前已经 join 过,这里是最后一道防线,
    // 尤其是 SUBMITTING/POLLING 里"job 不存在"这类理论上不会发生的防御分支)。
    void MediaManager::resetVideo()
    {
        if (_video->workerJoinable && _video->worker.joinable()) _video->worker.join();
        *_video = VideoJob();
    }

    // 每帧驱动视频状态机:被 update() 在末尾调用(主线程,与照片状态机同一 update() tick 内)。
    void MediaManager::updateVideoInternal()
    {
        VideoJob& v = *_video;

        if (v.phase == VideoJob::WAIT_A)
        {
            if (!_videoGrabber.ready(v.snapPathA))
            {
                if (++v.waitSnapshotTicks > kWaitSnapshotTimeoutTicks)
                {
                    OSG_WARN << "[AIChat] video A-point snapshot timeout" << std::endl;
                    resetVideo();
                }
                return;
            }
            // A 点快照已稳定 -> 进入"等待用户触发 B 点"的稳态,不做任何自动跳转
            // (beginVideoCapture 只负责起个头,真正的下一步由用户移动相机后再次调用
            // captureVideoEnd 触发——这里只是把"抓帧中"过渡到"抓帧已就绪"这两个瞬态/稳态分开)。
            v.phase = VideoJob::WAIT_B;
            return;
        }

        if (v.phase == VideoJob::CAPTURING_B)
        {
            if (!_videoGrabber.ready(v.snapPathB))
            {
                if (++v.waitSnapshotTicks > kWaitSnapshotTimeoutTicks)
                {
                    OSG_WARN << "[AIChat] video B-point snapshot timeout" << std::endl;
                    resetVideo();
                }
                return;
            }
            // B 点快照就绪 -> 生成运动提示词预览,进入等待确认。
            v.motionPrompt = buildMotionPrompt(v.llaA, v.llaB);
            v.phase = VideoJob::AWAIT_CONFIRM;
            OSG_NOTICE << "[AIChat] generate_video awaiting confirm, prompt=" << v.motionPrompt << std::endl;
            return;
        }

        if (v.phase == VideoJob::DOWNLOADING_FAKE)
        {
            // EARTH_AI_FAKE_MP4 路径:模拟约 100 ticks 的"生成延迟"(spec 要求),
            // 让 E2E 断言能观察到 JOB 卡的进行中状态,而不是同一帧就瞬间完成。
            ++v.fakeDelayTicks;
            float prog = 0.3f + 0.6f * std::min(1.0f, (float)v.fakeDelayTicks / 100.0f);
            _jobs.update(v.jobId, AIJob::RUNNING, prog, "", "");
            if (v.fakeDelayTicks < 100) return;

            std::string fakeMp4; fakeMp4Path(fakeMp4);
            std::string bytes;
            if (!readFileBytes(fakeMp4, bytes) || bytes.empty())
            {
                _jobs.update(v.jobId, AIJob::FAILED, 1.0f, "", "EARTH_AI_FAKE_MP4 unreadable: " + fakeMp4);
                if (_cards) _cards->removeJob(v.jobId);
                OSG_WARN << "[AIChat] video job " << v.jobId << " failed: cannot read " << fakeMp4 << std::endl;
                resetVideo();
                return;
            }
            if (!writeFileBytes(v.mp4Path, bytes))
            {
                _jobs.update(v.jobId, AIJob::FAILED, 1.0f, "", "failed to write " + v.mp4Path);
                if (_cards) _cards->removeJob(v.jobId);
                OSG_WARN << "[AIChat] video job " << v.jobId << " failed: cannot write " << v.mp4Path << std::endl;
                resetVideo();
                return;
            }
            _jobs.update(v.jobId, AIJob::DONE, 1.0f, v.mp4Path, "");
            if (_cards) { _cards->removeJob(v.jobId); _cards->pushPhoto(v.mp4Path, u8"巡航视频", true); }
            OSG_NOTICE << "[AIChat] video job done -> " << v.mp4Path << std::endl;
            resetVideo();
            return;
        }

        if (v.phase == VideoJob::SUBMITTING)
        {
            AIJob snap;
            if (!_jobs.get(v.jobId, snap)) { resetVideo(); return; }
            if (snap.status == AIJob::FAILED)
            {
                if (v.workerJoinable && v.worker.joinable()) v.worker.join();
                if (_cards) _cards->removeJob(v.jobId);
                OSG_WARN << "[AIChat] video job " << v.jobId << " submit failed: " << snap.error << std::endl;
                resetVideo();
                return;
            }
            if (snap.status == AIJob::RUNNING && snap.progress >= 0.3f && !snap.resultPath.empty())
            {
                // worker 已把 operationName 写进 resultPath(见 confirmVideo 里的注释),
                // 提交阶段的 worker 线程本身已经跑完(返回了),可以安全 join 回收。
                if (v.workerJoinable && v.worker.joinable()) v.worker.join();
                v.workerJoinable = false;
                v.operationName = snap.resultPath;
                v.pollTicks = 0;
                v.phase = VideoJob::POLLING;
                OSG_NOTICE << "[AIChat] generate_video operation=" << v.operationName << std::endl;
            }
            return;
        }

        if (v.phase == VideoJob::POLLING)
        {
            // 每约 300 ticks(≈5-10s,取决于帧率)发起一次轮询,而不是每帧都发 HTTP 请求。
            const int kPollIntervalTicks = 300;
            const int kPollTimeoutTicks = 300 * 60;   // 300 次轮询上限(约 10 分钟),超时判失败
            ++v.pollTicks;
            ++v.ticksSinceLastPoll;

            // 若上一次轮询 worker 还没跑完,先看它是否已经把结果写进 job 了
            // (DONE/FAILED 都代表 worker 已返回,可以安全 join 回收线程)。
            if (v.pollInFlight)
            {
                AIJob snap;
                if (!_jobs.get(v.jobId, snap)) { resetVideo(); return; }
                if (snap.status == AIJob::DONE)
                {
                    if (v.workerJoinable && v.worker.joinable()) v.worker.join();
                    v.workerJoinable = false; v.pollInFlight = false;
                    if (_cards) { _cards->removeJob(v.jobId); _cards->pushPhoto(snap.resultPath, u8"巡航视频", true); }
                    OSG_NOTICE << "[AIChat] video job done -> " << snap.resultPath << std::endl;
                    resetVideo();
                    return;
                }
                if (snap.status == AIJob::FAILED)
                {
                    if (v.workerJoinable && v.worker.joinable()) v.worker.join();
                    v.workerJoinable = false; v.pollInFlight = false;
                    if (_cards) _cards->removeJob(v.jobId);
                    OSG_WARN << "[AIChat] video job " << v.jobId << " poll failed: " << snap.error << std::endl;
                    resetVideo();
                    return;
                }
                // status 仍是 RUNNING 且没有失败/完成信号 -> 本轮 poll worker 要么还在飞
                // (还没 GET 完成),要么已经返回但判定"operation 仍在跑"(done:false,worker
                // lambda 里那条 "!done 直接 return" 分支,不触碰 job)。
                //
                // bug fix:这里不能再用 "!v.worker.joinable()" 当"worker 已经跑完"的信号——
                // std::thread 只要执行体还没被 join() 过,joinable() 就一直是 true,跟它的
                // 函数体是否已经跑完毫无关系(线程结束运行 ≠ 变得不可 join,那要等 std::thread
                // 对象被 join() 或 detach())。旧代码这里读到的永远是 true,pollInFlight 永远
                // 清不掉,下面 :1007 的 "if (v.pollInFlight || ...) return;" 就会永久拦住后续
                // 轮询——真实网络下第一次 GET 返回 done:false 之后,整个任务就再也不会发起
                // 第二次轮询,只能干等到 10 分钟超时(此时 Veo 那边任务其实可能已经生成完毕,
                // 白花钱)。
                //
                // 改用 worker 主动写的完成标志 pollDone(std::atomic<bool>,worker lambda 的
                // 最后一步就是把它置 true,见下方新 worker 里的 doneFlag->store(true))——
                // 无论 poll() 判定 done 还是仍在跑,只要 lambda 本身执行完毕就会置位,这才是
                // "worker 已返回、可以安全 join"的真实信号。置位后 join 回收线程、清零
                // pollInFlight 和 pollDone,下一个轮询间隔到了自然会再起一个新 worker——
                // 也就是本次修复之后轮询变成真正可重复的了(spawn → 置位 → join+清 →
                // 下一个 interval 再 spawn)。
                if (v.pollDone->load())
                {
                    if (v.workerJoinable && v.worker.joinable()) v.worker.join();
                    v.workerJoinable = false;
                    v.pollInFlight = false;
                    v.pollDone->store(false);
                    // review:收割完当前这一轮 worker 后立刻 return,不让本 tick 继续往下走
                    // 到"到达轮询间隔就再 spawn 一个新 worker"那段——pollInFlight 刚清成
                    // false、ticksSinceLastPoll 也可能恰好已经 >= kPollIntervalTicks,不加
                    // 这个 return 就会在同一 tick 内清零又立刻重新置位,产生一个极窄的竞态
                    // 窗口。下一 tick 才重读 job 状态、决定是否该发起下一轮轮询,只是多等
                    // 一帧(可忽略的延迟),换来这个竞态彻底消失。
                    return;
                }
            }

            // 进度条在等待轮询期间缓慢爬升(0.3~0.8,阶梯式,给用户"仍在进行"的观感)。
            // bug fix:这里原先直接 _jobs.update(v.jobId, RUNNING, ..., "", "") 会把 status
            // 强行写成 RUNNING——如果恰好在这次 update() tick 与上面 pollInFlight 分支之间,
            // 后台 poll worker 刚把 job 写成 DONE/FAILED(比如轮询间隔到了、worker 已经拿到
            // 结果,但这次 tick 走到这里时上面 pollInFlight 判断还没来得及看到新状态——两次
            // 独立的加锁操作之间不是原子的),这行 creep 就会把已经完成/失败的 status 又
            // 覆盖回 RUNNING,导致 UI 卡片"完成后又变回进行中"甚至掩盖失败原因。
            // JobManager::creepProgress() 把"读 status 是否 RUNNING"和"写 progress"放进
            // 同一次加锁,对 RUNNING 之外的状态直接跳过、不做任何修改,消除这个竞态。
            float creepFrac = std::min(1.0f, (float)v.pollTicks / (float)kPollTimeoutTicks);
            _jobs.creepProgress(v.jobId, 0.3f + 0.5f * creepFrac);

            if (v.pollTicks > kPollTimeoutTicks)
            {
                // review:这里的 join() 有可能阻塞主线程——但最多只会阻塞"一个 HTTP 往返"
                // 的时长(worker 里单次 poll() 顶多是一次 30s 轮询 GET 或一次 60s 下载 GET,
                // 见 VeoVideoProvider::poll() 的实现),不是无界等待。10 分钟超时本身极小
                // 概率触发(正常任务远早于此完成),这里为了状态机简单直接同步 join,
                // 权衡后可接受,不做成异步收尾。
                if (v.workerJoinable && v.worker.joinable()) v.worker.join();
                _jobs.update(v.jobId, AIJob::FAILED, 1.0f, "", "polling timeout (10min)");
                if (_cards) _cards->removeJob(v.jobId);
                OSG_WARN << "[AIChat] video job " << v.jobId << " polling timeout" << std::endl;
                resetVideo();
                return;
            }

            if (v.pollInFlight || v.ticksSinceLastPoll < kPollIntervalTicks) return;

            // 到达轮询间隔且没有上一次轮询还在飞行中:起一个一次性 worker 线程做本次
            // GET(不阻塞主线程),结果写回 job(DONE/FAILED);"仍在跑"(done:false)则
            // 不触碰 job 状态,只是让 worker 自然返回,下一轮再发起。
            if (v.workerJoinable && v.worker.joinable()) v.worker.join();
            v.ticksSinceLastPoll = 0;
            v.pollInFlight = true;
            // 新开一轮轮询就换一个新的 pollDone(而不是复用/清零旧对象):shared_ptr 本身
            // 保证了"worker 捕获的是这一轮专属的 flag",即便未来某处逻辑变化导致上一轮的
            // flag 还有其它持有者在读,也不会跟这一轮混淆。当前 worker lambda 结束时把它
            // 置 true 就是本节点新增的"worker 已返回"信号(见上面 POLLING 分支里的用法)。
            v.pollDone = std::make_shared<std::atomic<bool>>(false);
            std::string apiKey = _apiKey;
            std::string model = videoModel();
            std::string opName = v.operationName;
            std::string mp4Path = v.mp4Path;
            int jobId = v.jobId;
            JobManager* jobsPtr = &_jobs;
            std::shared_ptr<std::atomic<bool>> doneFlag = v.pollDone;
            v.worker = std::thread([apiKey, model, opName, mp4Path, jobId, jobsPtr, doneFlag]()
            {
                // RAII 收尾:无论下面哪条分支 return,都保证最后一步是把 doneFlag 置位——
                // 这是主线程判断"这次轮询 worker 已经跑完、可以安全 join"的唯一依据
                // (std::thread::joinable() 不能用来判断线程函数体是否执行完毕,见上方
                // POLLING 分支里的详细注释)。
                struct DoneSetter
                {
                    std::shared_ptr<std::atomic<bool>> flag;
                    ~DoneSetter() { flag->store(true); }
                } doneSetter{doneFlag};

                VeoVideoProvider provider(apiKey, model);
                bool done = false; std::string mp4Bytes, err;
                try { provider.poll(opName, done, mp4Bytes, err); }
                catch (const std::exception& e) { done = true; err = std::string("provider exception: ") + e.what(); }

                if (!done) return;   // 仍在跑,job 状态不变,下次轮询再试
                if (mp4Bytes.empty())
                {
                    jobsPtr->update(jobId, AIJob::FAILED, 1.0f, "", err.empty() ? "unknown poll error" : err);
                    return;
                }
                if (!writeFileBytes(mp4Path, mp4Bytes))
                {
                    jobsPtr->update(jobId, AIJob::FAILED, 1.0f, "", "failed to write " + mp4Path);
                    return;
                }
                jobsPtr->update(jobId, AIJob::DONE, 1.0f, mp4Path, "");
            });
            v.workerJoinable = true;
            return;
        }
    }
}
