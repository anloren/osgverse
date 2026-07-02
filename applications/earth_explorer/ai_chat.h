#ifndef EARTH_AI_CHAT_H
#define EARTH_AI_CHAT_H
// AI Chat 代理循环:LLMProvider 抽象 + FakeProvider(离线可测) + AIChatCore 状态机。
// 本头文件不依赖 OSG,纯 std + picojson,可被 tests/ 单测直接 include 实现文件。
#include "ai_tools.h"
#include <thread>

namespace earthai
{
    // 一轮模型输出:要么纯文本,要么一批函数调用
    struct FunctionCall { std::string name; picojson::value args; };
    struct LLMTurn { std::string text; std::vector<FunctionCall> calls; std::string error; };

    class LLMProvider
    {
    public:
        virtual ~LLMProvider() {}
        // contentsJson: Gemini contents 数组(序列化字符串);declsJson: functionDeclarations 数组。
        // 同步调用,由 AIChatCore 的工作线程执行,不得在主线程调。
        virtual LLMTurn chat(const std::string& contentsJson, const std::string& declsJson) = 0;
    };

    // EARTH_AI_FAKE=<json文件>:数组,按序出队,每项 {"text":...} 或 {"calls":[{name,args}]}
    class FakeProvider : public LLMProvider
    {
    public:
        bool loadFromFile(const std::string& path);
        bool loadFromString(const std::string& json);
        virtual LLMTurn chat(const std::string& contentsJson, const std::string& declsJson);
    private:
        std::vector<LLMTurn> _script; size_t _cursor = 0; std::mutex _mutex;
    };

    struct ChatEntry { enum Kind { USER, ASSISTANT, TOOL_NOTE, ERR } kind; std::string text; };

    // 会话历史条目(内部使用),用于序列化 Gemini contents 数组
    struct HistoryItem
    {
        enum Role { USER_TEXT, MODEL_TEXT, MODEL_CALL, TOOL_RESPONSE } role;
        std::string text;              // USER_TEXT / MODEL_TEXT 用
        std::string callName;          // MODEL_CALL / TOOL_RESPONSE 用
        picojson::value callArgs;      // MODEL_CALL 用(functionCall.args)
        picojson::value toolResponse;  // TOOL_RESPONSE 用(functionResponse.response)
    };

    // 把历史条目序列化为 Gemini contents JSON 数组字符串;Task 3 的 GeminiProvider 直接复用
    std::string buildContentsJson(const std::vector<HistoryItem>& history);

    class AIChatCore
    {
    public:
        AIChatCore(LLMProvider* p, ToolRegistry* reg);   // 不持有 provider/registry 所有权
        ~AIChatCore();
        void submit(const std::string& userText);        // 主线程调;若 busy 则忽略并提示
        bool busy() const;
        // 主线程每帧调:执行排队的工具调用(工具必须主线程),推进代理循环
        void drainMainThread();
        std::vector<ChatEntry> transcript() const;       // UI 读(加锁快照)
        // 仅测试用:导出当前 _history 序列化后的 Gemini contents JSON,
        // 供单测校验 functionCall/functionResponse 严格配对;业务代码不要调用
        std::string historyContentsForTest() const;
        std::string systemPrompt;                        // 中文系统提示词,earth_main 里设置

    private:
        void startWorkerRound();   // 内部:基于当前 _history 拼 contents,起一轮工作线程
        void joinWorkerIfAny();    // 内部:等待并回收上一轮 worker(若还在跑)

        LLMProvider* _provider;
        ToolRegistry* _registry;

        mutable std::mutex _mutex;
        std::vector<ChatEntry> _transcript;     // 供 UI 读的会话文本
        std::vector<HistoryItem> _history;      // 供 contents 序列化的完整历史(含函数调用/结果)
        bool _busy = false;
        int _round = 0;                         // 当前 submit 的代理循环轮次计数
        static const int kMaxRounds = 6;

        // 工作线程产出,主线程 drain 时消费
        std::vector<ChatEntry> _pendingEntries;
        std::vector<FunctionCall> _pendingCalls;
        bool _hasPendingResult = false;         // worker 已产出(文本/错误/调用)但还未被 drain 消费
        std::string _pendingAssistantText;      // worker 产出的纯文本回复,drain 时补记入 _history
        bool _hasPendingAssistantText = false;

        std::thread _worker;
        bool _workerJoinable = false;
    };
}
#endif
