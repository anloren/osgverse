// applications/earth_explorer/ai_chat.cpp
// AIChatCore 代理循环实现 + FakeProvider(离线可测)。
// 设计要点:worker 线程只读 provider->chat() 的输入(contents/decls 字符串快照),
// 产出结果通过 _mutex 保护的 _pending* 交给主线程;worker 绝不直接碰 registry/transcript。
#include "ai_chat.h"
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

            // 只保留最近 10 轮 user+assistant 交换(粗略按条目数裁剪,保留 functionCall/Response 配对的完整性由
            // 调用方 submit 时机保证——裁剪只在新一轮 submit 开始时做,不会切断进行中的一轮)
            const size_t kMaxHistoryItems = 40; // 10 轮 user+assistant 粗略上限(含工具调用/结果条目)
            if (_history.size() > kMaxHistoryItems)
                _history.erase(_history.begin(), _history.begin() + (_history.size() - kMaxHistoryItems));

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
            LLMTurn turn = provider->chat(contentsJson, declsJson);

            std::lock_guard<std::mutex> g(_mutex);
            _pendingEntries.clear();
            _pendingCalls.clear();

            if (!turn.error.empty())
            {
                ChatEntry e; e.kind = ChatEntry::ERR; e.text = turn.error;
                _pendingEntries.push_back(e);
                _busy = false; // 出错直接结束这一轮代理循环
            }
            else if (!turn.calls.empty())
            {
                _pendingCalls = turn.calls; // busy 保持 true,等 drainMainThread 执行工具
            }
            else
            {
                ChatEntry e; e.kind = ChatEntry::ASSISTANT; e.text = turn.text;
                _pendingEntries.push_back(e);
                _busy = false;
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
        }

        if (!haveCalls) return; // 纯文本或错误结果,这一轮代理循环已在 worker 里结束(busy 已清)

        // 代理循环轮次上限检查(在主线程执行工具前检查,避免无限循环)
        {
            std::lock_guard<std::mutex> g(_mutex);
            ++_round;
            if (_round > kMaxRounds)
            {
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
}
