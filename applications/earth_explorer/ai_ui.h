#ifndef EARTH_AI_UI_H
#define EARTH_AI_UI_H
#include "ai_chat.h"
#include <picojson.h>
#include <vector>
#include <string>

// AI 生成的信息卡:图表(本任务)/照片/生成任务(Task 8/9 接线,本任务只占位类型)。
struct AICard
{
    enum Type { CHART, PHOTO, JOB } type = CHART;
    picojson::value spec;     // CHART: {type,title,labels[],values[]}
    std::string path;         // PHOTO/JOB 结果文件(本任务未用,Task 8/9 填)
    int jobId = 0;
    bool open = true;
    // 上一帧该卡片 ImGui::Begin 窗口的实际高度(AlwaysAutoResize 窗口的高度要到 End() 之后
    // 才确定,当帧再拿去摆下一张卡的位置会有一帧延迟——缓存上一帧的值,下一帧堆叠用它,
    // 首帧(=0)时给个保守估计,避免第一帧卡片重叠。
    float lastHeight = 0.0f;
};

// 底部悬浮聊天条：输入框 + 历史面板（历史在输入行上方，同一窗口内）+ 右上角图表卡堆叠。
// core 为 null 时画禁用态输入框（提示设置 EARTH_AI_KEY），不画历史/按钮——对无 AI 场景零干扰。
class AIChatUI
{
public:
    AIChatUI();
    void draw(earthai::AIChatCore* core);

    // 主线程调用(工具 execute 在 drain 中,与 draw() 同线程,无需加锁——见 .cpp 头注释)。
    void pushChart(const picojson::value& spec);

private:
    void drawCards();   // 右上角卡片堆叠,叠在地震/航班详情卡之下(见 .cpp 注释)
    void drawChartCard(const picojson::value& spec, float width);

    char _inputBuf[1024];      // InputText 缓冲区，提交时转 std::string 再清空
    bool _historyCollapsed;    // 历史面板折叠状态（默认展开）
    size_t _lastEntryCount;    // 上次绘制时的历史条数，用于检测新增条目并自动滚动到底部
    std::vector<AICard> _cards;
};
#endif
