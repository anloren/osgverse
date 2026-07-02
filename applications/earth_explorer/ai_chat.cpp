// applications/earth_explorer/ai_chat.cpp
// AIChatCore 代理循环实现 + FakeProvider(离线可测)。
// 设计要点:worker 线程只读 provider->chat() 的输入(contents/decls 字符串快照),
// 产出结果通过 _mutex 保护的 _pending* 交给主线程;worker 绝不直接碰 registry/transcript。
#include "ai_chat.h"
#include "3rdparty/libhv/all/client/requests.h"
#include <fstream>
#include <sstream>
#include <iostream>

namespace earthai
{
    // ---------------- FakeProvider ----------------

    static LLMTurn parseScriptItem(const picojson::value& item)
    {
        LLMTurn turn;
        if (item.contains("error"))
        {
            turn.error = item.get("error").to_str();
            return turn;
        }
        if (item.contains("text"))
            turn.text = item.get("text").to_str();
        if (item.contains("calls") && item.get("calls").is<picojson::array>())
        {
            const picojson::array& calls = item.get("calls").get<picojson::array>();
            for (size_t i = 0; i < calls.size(); ++i)
            {
                FunctionCall fc;
                fc.name = calls[i].get("name").to_str();
                if (calls[i].contains("args")) fc.args = calls[i].get("args");
                turn.calls.push_back(fc);
            }
        }
        return turn;
    }

    bool FakeProvider::loadFromString(const std::string& json)
    {
        picojson::value v; std::string err = picojson::parse(v, json);
        if (!err.empty() || !v.is<picojson::array>()) return false;

        std::lock_guard<std::mutex> g(_mutex);
        _script.clear(); _cursor = 0;
        const picojson::array& arr = v.get<picojson::array>();
        for (size_t i = 0; i < arr.size(); ++i)
            _script.push_back(parseScriptItem(arr[i]));
        return true;
    }

    bool FakeProvider::loadFromFile(const std::string& path)
    {
        std::ifstream ifs(path.c_str());
        if (!ifs.is_open()) return false;
        std::stringstream ss; ss << ifs.rdbuf();
        return loadFromString(ss.str());
    }

    LLMTurn FakeProvider::chat(const std::string& /*contentsJson*/, const std::string& /*declsJson*/)
    {
        std::lock_guard<std::mutex> g(_mutex);
        if (_cursor >= _script.size())
        {
            LLMTurn turn; turn.error = "fake script exhausted"; return turn;
        }
        return _script[_cursor++];
    }

    // ---------------- contents JSON 序列化 ----------------

    std::string buildContentsJson(const std::vector<HistoryItem>& history)
    {
        std::string s = "[";
        for (size_t i = 0; i < history.size(); ++i)
        {
            const HistoryItem& h = history[i];
            picojson::object entry;
            picojson::object part;
            switch (h.role)
            {
            case HistoryItem::USER_TEXT:
                entry["role"] = picojson::value("user");
                part["text"] = picojson::value(h.text);
                break;
            case HistoryItem::MODEL_TEXT:
                entry["role"] = picojson::value("model");
                part["text"] = picojson::value(h.text);
                break;
            case HistoryItem::MODEL_CALL:
            {
                entry["role"] = picojson::value("model");
                picojson::object fc;
                fc["name"] = picojson::value(h.callName);
                fc["args"] = h.callArgs.is<picojson::null>() ? picojson::value(picojson::object()) : h.callArgs;
                part["functionCall"] = picojson::value(fc);
                break;
            }
            case HistoryItem::TOOL_RESPONSE:
            {
                entry["role"] = picojson::value("user");
                picojson::object fr;
                fr["name"] = picojson::value(h.callName);
                fr["response"] = h.toolResponse.is<picojson::null>() ? picojson::value(picojson::object()) : h.toolResponse;
                part["functionResponse"] = picojson::value(fr);
                break;
            }
            }
            picojson::array parts; parts.push_back(picojson::value(part));
            entry["parts"] = picojson::value(parts);
            s += picojson::value(entry).serialize();
            if (i + 1 < history.size()) s += ",";
        }
        return s + "]";
    }

    // ---------------- AIChatCore ----------------

    AIChatCore::AIChatCore(LLMProvider* p, ToolRegistry* reg)
        : _provider(p), _registry(reg)
    {}

    AIChatCore::~AIChatCore()
    {
        // 析构时若还有活着的 worker,必须 join,避免进程退出时崩溃
        joinWorkerIfAny();
    }

    void AIChatCore::joinWorkerIfAny()
    {
        if (_workerJoinable && _worker.joinable())
        {
            _worker.join();
            _workerJoinable = false;
        }
    }

    void AIChatCore::submit(const std::string& userText)
    {
        {
            std::lock_guard<std::mutex> g(_mutex);
            if (_busy)
            {
                ChatEntry e; e.kind = ChatEntry::TOOL_NOTE; e.text = u8"上一条还在处理中";
                _transcript.push_back(e);
                return;
            }
            ChatEntry ue; ue.kind = ChatEntry::USER; ue.text = userText;
            _transcript.push_back(ue);

            HistoryItem hi; hi.role = HistoryItem::USER_TEXT; hi.text = userText;
            _history.push_back(hi);

            // 历史裁剪:按条目数粗略限长(约 10 轮 user+assistant 交换,含工具调用/结果条目)。
            // 切点必须落在用户文本边界:硬切可能停在 MODEL_CALL 与 TOOL_RESPONSE 之间,
            // 或让历史以非 user 角色开头——两种情况真实 Gemini API 都会 400。
            // 因此算出目标切点后向后推进到下一个 USER_TEXT 才 erase。
            const size_t kMaxHistoryItems = 40;
            if (_history.size() > kMaxHistoryItems)
            {
                size_t cut = _history.size() - kMaxHistoryItems;
                while (cut < _history.size() && _history[cut].role != HistoryItem::USER_TEXT) ++cut;
                _history.erase(_history.begin(), _history.begin() + cut);
            }

            _busy = true;
            _round = 0;
        }
        startWorkerRound();
    }

    void AIChatCore::startWorkerRound()
    {
        std::string contentsJson;
        std::string declsJson;
        {
            std::lock_guard<std::mutex> g(_mutex);
            contentsJson = buildContentsJson(_history);
            declsJson = _registry->buildDeclarationsJson();
        }

        // 回收上一轮 worker(调用此函数时,前一轮 worker 一定已经把结果写回 _pending* 并退出,
        // 因为 startWorkerRound 只在 submit()/drainMainThread() 里、确认拿到上一轮结果之后才被调用)
        LLMProvider* provider = _provider;
        joinWorkerIfAny();
        _worker = std::thread([this, provider, contentsJson, declsJson]()
        {
            // worker 线程:只调用 provider->chat,绝不碰 registry/transcript/_history 之外的东西,
            // 结果一律通过加锁的 _pending* 交回主线程处理。
            // 注意:_busy 一律由主线程的 drainMainThread 清除(消费完 pending 之后),
            // worker 不清 busy——否则"busy 已清但回复还没上屏"的窗口里 submit 会插队打乱会话。
            LLMTurn turn;
            try { turn = provider->chat(contentsJson, declsJson); }
            catch (const std::exception& e)
            {
                // Provider 内部异常(如 picojson::get 类型不符)转成错误结果,避免异常逃逸 std::terminate
                turn = LLMTurn(); turn.error = std::string("provider exception: ") + e.what();
            }

            std::lock_guard<std::mutex> g(_mutex);
            if (!turn.error.empty())
            {
                ChatEntry e; e.kind = ChatEntry::ERR; e.text = turn.error;
                _pendingEntries.push_back(e);
            }
            else if (!turn.calls.empty())
            {
                _pendingCalls = turn.calls; // 等 drainMainThread 在主线程执行工具
            }
            else
            {
                ChatEntry e; e.kind = ChatEntry::ASSISTANT; e.text = turn.text;
                _pendingEntries.push_back(e);
                _pendingAssistantText = turn.text;
                _hasPendingAssistantText = true;
            }
            _hasPendingResult = true;
        });
        _workerJoinable = true;
    }

    bool AIChatCore::busy() const
    {
        std::lock_guard<std::mutex> g(_mutex);
        return _busy;
    }

    void AIChatCore::drainMainThread()
    {
        std::vector<FunctionCall> callsToRun;
        bool haveCalls = false;

        {
            std::lock_guard<std::mutex> g(_mutex);
            if (!_hasPendingResult) return;
            _hasPendingResult = false;

            // 把 worker 产出的文本/错误条目搬进 transcript
            for (size_t i = 0; i < _pendingEntries.size(); ++i)
                _transcript.push_back(_pendingEntries[i]);
            _pendingEntries.clear();

            if (_hasPendingAssistantText)
            {
                // 纯文本回复也要计入历史,否则下一次 submit 时模型看不到自己上一轮说了什么
                HistoryItem hi; hi.role = HistoryItem::MODEL_TEXT; hi.text = _pendingAssistantText;
                _history.push_back(hi);
                _hasPendingAssistantText = false;
                _pendingAssistantText.clear();
            }

            if (!_pendingCalls.empty())
            {
                // 记录 model 的 functionCall 条目到历史(用于下一轮 contents)
                for (size_t i = 0; i < _pendingCalls.size(); ++i)
                {
                    HistoryItem hi; hi.role = HistoryItem::MODEL_CALL;
                    hi.callName = _pendingCalls[i].name; hi.callArgs = _pendingCalls[i].args;
                    _history.push_back(hi);
                }
                callsToRun = _pendingCalls;
                _pendingCalls.clear();
                haveCalls = true;
            }

            // busy 只在这里(主线程、pending 已消费、回复已上屏)清除,
            // 关闭"worker 清 busy 但 drain 还没搬运回复"期间 submit 插队的窗口
            if (!haveCalls) _busy = false;
        }

        if (!haveCalls) return; // 纯文本或错误结果,这一轮代理循环到此结束

        // 代理循环轮次上限检查(在主线程执行工具前检查,避免无限循环)
        {
            std::lock_guard<std::mutex> g(_mutex);
            ++_round;
            if (_round > kMaxRounds)
            {
                // 上面已把这批 functionCall 记入 _history,若就此返回会留下"裸 functionCall",
                // 真实 Gemini API 要求 call/response 严格配对,不配对会让之后每次请求都 400。
                // 因此给每个未执行的调用补一条合成 functionResponse,保持历史合法。
                for (size_t i = 0; i < callsToRun.size(); ++i)
                {
                    picojson::object errObj;
                    errObj["error"] = picojson::value("tool loop limit reached");
                    HistoryItem hi; hi.role = HistoryItem::TOOL_RESPONSE;
                    hi.callName = callsToRun[i].name; hi.toolResponse = picojson::value(errObj);
                    _history.push_back(hi);
                }
                ChatEntry e; e.kind = ChatEntry::ERR; e.text = u8"工具循环超限";
                _transcript.push_back(e);
                _busy = false;
                return;
            }
        }

        // 主线程执行每个工具调用(这是整个设计的重点:工具必须在主线程跑,因为它们会碰 OSG 场景图)
        for (size_t i = 0; i < callsToRun.size(); ++i)
        {
            const FunctionCall& fc = callsToRun[i];
            picojson::value result;
            _registry->dispatch(fc.name, fc.args, result);

            std::string note = u8"⚙ " + fc.name + "(" + fc.args.serialize() + ")";
            std::lock_guard<std::mutex> g(_mutex);
            ChatEntry e; e.kind = ChatEntry::TOOL_NOTE; e.text = note;
            _transcript.push_back(e);

            HistoryItem hi; hi.role = HistoryItem::TOOL_RESPONSE;
            hi.callName = fc.name; hi.toolResponse = result;
            _history.push_back(hi);
        }

        // 起下一轮 worker(带上最新的工具结果),busy 保持 true 直到这一轮 worker 产出文本/错误
        startWorkerRound();
    }

    std::vector<ChatEntry> AIChatCore::transcript() const
    {
        std::lock_guard<std::mutex> g(_mutex);
        return _transcript;
    }

    std::string AIChatCore::historyContentsForTest() const
    {
        // 仅测试用:加锁快照 _history 并序列化,用于校验 call/response 配对
        std::lock_guard<std::mutex> g(_mutex);
        return buildContentsJson(_history);
    }

    // ---------------- GeminiProvider ----------------

    // 从响应体里尽量挖出人类可读的错误摘要(promptFeedback.blockReason 或顶层 error.message),
    // 截到 200 字符——body 本身可能很长(base64/大段文本),错误提示没必要塞爆日志/UI。
    static std::string truncate200(const std::string& s)
    { return s.size() > 200 ? s.substr(0, 200) : s; }

    LLMTurn parseGeminiResponse(const std::string& body)
    {
        LLMTurn turn;
        picojson::value v;
        std::string perr = picojson::parse(v, body);
        if (!perr.empty() || !v.is<picojson::object>())
        { turn.error = "bad json: " + truncate200(perr.empty() ? body : perr); return turn; }

        // candidates 缺失/为空:多半是 safety block 或顶层 error,尽量把原因带出来
        if (!v.contains("candidates") || !v.get("candidates").is<picojson::array>()
            || v.get("candidates").get<picojson::array>().empty())
        {
            if (v.contains("promptFeedback") && v.get("promptFeedback").is<picojson::object>()
                && v.get("promptFeedback").contains("blockReason"))
            {
                turn.error = "blocked: " + truncate200(v.get("promptFeedback").get("blockReason").to_str());
            }
            else if (v.contains("error") && v.get("error").is<picojson::object>()
                     && v.get("error").contains("message"))
            {
                turn.error = "api error: " + truncate200(v.get("error").get("message").to_str());
            }
            else turn.error = "no candidates: " + truncate200(body);
            return turn;
        }

        const picojson::value& cand0 = v.get("candidates").get<picojson::array>()[0];
        if (!cand0.is<picojson::object>() || !cand0.contains("content")
            || !cand0.get("content").is<picojson::object>()
            || !cand0.get("content").contains("parts")
            || !cand0.get("content").get("parts").is<picojson::array>())
        {
            // 也可能是 finishReason=SAFETY 等,没有 content——同样尽量带出原因
            if (cand0.is<picojson::object>() && cand0.contains("finishReason"))
                turn.error = "no content: finishReason=" + truncate200(cand0.get("finishReason").to_str());
            else turn.error = "no content parts: " + truncate200(body);
            return turn;
        }

        const picojson::array& parts = cand0.get("content").get("parts").get<picojson::array>();
        for (size_t i = 0; i < parts.size(); ++i)
        {
            const picojson::value& part = parts[i];
            if (!part.is<picojson::object>()) continue;
            if (part.contains("functionCall") && part.get("functionCall").is<picojson::object>())
            {
                const picojson::value& fcv = part.get("functionCall");
                FunctionCall fc;
                if (fcv.contains("name")) fc.name = fcv.get("name").to_str();
                if (fc.name.empty()) continue; // 没有名字的 functionCall 没法执行,跳过
                fc.args = fcv.contains("args") ? fcv.get("args") : picojson::value(picojson::object());
                turn.calls.push_back(fc);
            }
            else if (part.contains("text") && part.get("text").is<std::string>())
            {
                turn.text += part.get("text").to_str();
            }
        }

        if (turn.text.empty() && turn.calls.empty()) turn.error = "empty response";
        return turn;
    }

    GeminiProvider::GeminiProvider(const std::string& apiKey, const std::string& model)
        : _apiKey(apiKey), _model(model)
    {}

    LLMTurn GeminiProvider::chat(const std::string& contentsJson, const std::string& declsJson)
    {
        LLMTurn turn;
        std::string body = "{\"system_instruction\":{\"parts\":[{\"text\":" +
            picojson::value(_systemPrompt).serialize() + "}]},"
            "\"contents\":" + contentsJson;
        // Gemini 拒绝空 functionDeclarations 数组——declsJson 为 "[]"/空时整个 tools 字段都不带
        if (!declsJson.empty() && declsJson != "[]")
            body += ",\"tools\":[{\"functionDeclarations\":" + declsJson + "}]";
        body += "}";

        requests::Request req(new HttpRequest);
        req->method = HTTP_POST;
        req->timeout = 30;
        // 注意:key 拼在 URL 里,下面任何日志/错误信息都不得把 req->url 整串打印出来
        req->url = "https://generativelanguage.googleapis.com/v1beta/models/" + _model +
                   ":generateContent?key=" + _apiKey;
        req->headers["Content-Type"] = "application/json";
        req->body = body;

        requests::Response resp = requests::request(req);
        if (!resp || resp->status_code != 200)
        {
            turn.error = "HTTP " + std::to_string(resp ? (int)resp->status_code : -1) +
                         (resp ? (": " + truncate200(resp->body)) : "");
            return turn;
        }
        return parseGeminiResponse(resp->body);
    }
}
