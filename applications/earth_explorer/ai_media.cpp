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
#include <fstream>
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

    static bool writeFileBytes(const std::string& path, const std::string& bytes)
    {
        std::ofstream ofs(path.c_str(), std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return false;
        ofs.write(bytes.data(), (std::streamsize)bytes.size());
        return ofs.good();
    }

    // ---------------- SnapshotGrabber ----------------

    SnapshotGrabber::SnapshotGrabber(osgViewer::Viewer* viewer) : _viewer(viewer)
    {
        // WriteToFile 的 filename 是"不含扩展名的前缀",实际路径由 EARTH_AUTOCAP 同款规则
        // 拼成 "<prefix>_0.<ext>"(单 context、OVERWRITE 策略下固定后缀 "_0")。
        // grab() 每次重建 WriteToFile(不同前缀),避免多次调用互相覆盖对方的目标文件。
    }

    void SnapshotGrabber::grab(const std::string& pngPath)
    {
        // pngPath 末尾去掉 ".png" 作为 WriteToFile 的前缀(它自己会拼回 "_0.png")。
        std::string prefix = pngPath;
        const std::string ext = ".png";
        if (prefix.size() >= ext.size() && prefix.compare(prefix.size() - ext.size(), ext.size(), ext) == 0)
            prefix.resize(prefix.size() - ext.size());

        osg::ref_ptr<osgViewer::ScreenCaptureHandler::WriteToFile> writer =
            new osgViewer::ScreenCaptureHandler::WriteToFile(
                prefix, "png", osgViewer::ScreenCaptureHandler::WriteToFile::OVERWRITE);
        _capturer = new osgViewer::ScreenCaptureHandler(writer.get(), 1);
        _viewer->addEventHandler(_capturer.get());
        _capturer->setFramesToCapture(1);
        _capturer->captureNextFrame(*_viewer);
        OSG_NOTICE << "[AIChat] snapshot grab -> " << pngPath << std::endl;
    }

    bool SnapshotGrabber::ready(const std::string& pngPath) const
    {
        // WriteToFile 用 "_0" 后缀(单 GraphicsContext、OVERWRITE 策略)拼实际文件名,
        // 与 grab() 里去掉 ".png" 再拼前缀的逻辑对应。
        std::string prefix = pngPath;
        const std::string ext = ".png";
        if (prefix.size() >= ext.size() && prefix.compare(prefix.size() - ext.size(), ext.size(), ext) == 0)
            prefix.resize(prefix.size() - ext.size());
        std::string actual = prefix + "_0.png";

        // 写盘是渲染线程在捕获回调里做的,文件可能"存在但还没写完"——用两次 stat 之间
        // 文件大小是否稳定来判断真正写完(简单但足够:两次查询间隔由调用方的轮询节奏决定,
        // 通常是相邻两帧,写盘早已在这之前完成,稳定性判断只是防御极端慢速磁盘)。
        if (!osgDB::fileExists(actual)) return false;
        std::ifstream ifs(actual.c_str(), std::ios::binary | std::ios::ate);
        if (!ifs.is_open()) return false;
        std::streamsize sz1 = ifs.tellg();
        ifs.close();
        if (sz1 <= 0) return false;

        std::ifstream ifs2(actual.c_str(), std::ios::binary | std::ios::ate);
        if (!ifs2.is_open()) return false;
        std::streamsize sz2 = ifs2.tellg();
        return sz1 == sz2 && sz2 > 0;
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

    MediaManager::MediaManager(osgViewer::Viewer* viewer, AICardPanel* cards, const std::string& apiKeyOrEmpty)
        : _viewer(viewer), _cards(cards), _apiKey(apiKeyOrEmpty), _grabber(viewer),
          _state(IDLE), _jobId(0), _workerJoinable(false)
    {}

    MediaManager::~MediaManager()
    {
        joinWorkerIfAny();
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

        OSG_NOTICE << "[AIChat] generate_photo job=" << _jobId << " snap=" << _snapPath << std::endl;

        picojson::object r;
        r["status"] = picojson::value(std::string("started"));
        r["job_id"] = picojson::value((double)_jobId);
        return picojson::value(r);
    }

    void MediaManager::update()
    {
        if (_state == WAITING_SNAPSHOT)
        {
            if (!_grabber.ready(_snapPath)) return;

            std::string snapBytes;
            // 实际磁盘文件名带 "_0" 后缀(见 SnapshotGrabber::ready 注释)。
            std::string actual = _snapPath;
            const std::string ext = ".png";
            if (actual.size() >= ext.size() && actual.compare(actual.size() - ext.size(), ext.size(), ext) == 0)
                actual.resize(actual.size() - ext.size());
            actual += "_0.png";

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
}
