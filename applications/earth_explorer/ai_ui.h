#ifndef EARTH_AI_UI_H
#define EARTH_AI_UI_H
#include "ai_chat.h"

// 底部悬浮聊天条：输入框 + 历史面板（历史在输入行上方，同一窗口内）。
// core 为 null 时画禁用态输入框（提示设置 EARTH_AI_KEY），不画历史/按钮——对无 AI 场景零干扰。
class AIChatUI
{
public:
    AIChatUI();
    void draw(earthai::AIChatCore* core);

private:
    char _inputBuf[1024];      // InputText 缓冲区，提交时转 std::string 再清空
    bool _historyCollapsed;    // 历史面板折叠状态（默认展开）
    size_t _lastEntryCount;    // 上次绘制时的历史条数，用于检测新增条目并自动滚动到底部
};
#endif
