#ifndef EARTH_AI_CARDS_H
#define EARTH_AI_CARDS_H
#include "ai_tools.h"
#include <picojson.h>
#include <vector>
#include <string>

// AI 生成的信息卡:图表(Task 7)/照片(Task 8)/进行中任务(Task 8,生图/未来生视频共用)。
// 从 ai_ui.* 抽出(Task 8 PART A 审查意见):卡片数据结构 + 堆叠/绘制逻辑与图表渲染
// 是独立于聊天条的一整块职责,单独放一个文件便于往里加 PHOTO/JOB 卡而不继续膨胀
// ai_ui.cpp。
struct AICard
{
    enum Type { CHART, PHOTO, JOB } type = CHART;
    picojson::value spec;     // CHART: {type,title,labels[],values[]}
    std::string path;         // PHOTO:生成图 PNG 路径;JOB:未用
    std::string title;        // PHOTO/JOB 标题(CHART 标题取自 spec.title,见 drawCards)
    // JOB:每帧从这里读实时进度(而非在卡片里缓存一份易过期的快照);
    // pushJob() 时传入,drawJobCard() 里 jobs->get(jobId,...) 轮询。
    earthai::JobManager* jobs = nullptr;
    int jobId = 0;
    bool open = true;
    // 上一帧该卡片 ImGui::Begin 窗口的实际高度(AlwaysAutoResize 窗口的高度要到 End() 之后
    // 才确定,当帧再拿去摆下一张卡的位置会有一帧延迟——缓存上一帧的值,下一帧堆叠用它,
    // 首帧(=0)时给个保守估计,避免第一帧卡片重叠。
    float lastHeight = 0.0f;
    // 卡片的稳定身份,用于拼 ImGui 窗口 ID(见 AICardPanel::draw 注释)。
    // 不能用 vector 下标:关掉第 i 张卡后,后面的卡下标会左移,ImGui 按 ID 认窗口,
    // 下标变了就被当成一个新窗口打开——出现一帧闪烁/位置错乱。serial 现由 pushChart/
    // pushPhoto/pushJob 三处统一从 _nextSerial 递增分配(每种卡片各自的 push 方法都会
    // 分配一个),卡片存活期间不变,关闭旧卡不影响其余卡的 serial。
    int serial = 0;
};

// 右上角卡片堆叠容器:图表/照片/生成任务卡的存储 + 每帧绘制。
// 从 AIChatUI 拆出(Task 8 PART A):AIChatUI 只管聊天条,卡片容器独立持有 _cards/_nextSerial,
// 方便 MediaManager(Task 8)等其它模块也能直接持有指针推卡片,不必都经过 AIChatUI 转发。
class AICardPanel
{
public:
    AICardPanel() : _nextSerial(0) {}

    // 主线程调用(工具 execute 在 drain 中,与 draw() 同线程,无需加锁——见 .cpp 头注释)。
    void pushChart(const picojson::value& spec);
    // Task 8:生图任务完成后调用,pngPath 为生成结果文件的绝对路径。
    void pushPhoto(const std::string& pngPath, const std::string& title);
    // Task 8:任务刚起步时调用,进度条实时从 jobs->get(jobId,...) 读取;
    // MediaManager::update() 检测到该 job DONE/FAILED 时应调用 removeJob(jobId) 收尾
    // (DONE 一般紧接着调 pushPhoto 展示结果,FAILED 直接移除、错误已走 OSG_WARN)。
    void pushJob(earthai::JobManager* jobs, int jobId, const std::string& title);
    void removeJob(int jobId);

    // 每帧绘制右上角卡片堆叠(叠在地震/航班详情卡之下,见 .cpp 注释)。
    void draw();

private:
    void drawChartCard(const picojson::value& spec, float width);
    void drawPhotoCard(const AICard& c);
    void drawJobCard(const AICard& c);

    std::vector<AICard> _cards;
    int _nextSerial;   // 下一张新卡分配的 serial(见 AICard::serial 注释)
};
#endif
