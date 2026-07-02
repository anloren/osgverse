#ifndef EARTH_AI_SETUP_H
#define EARTH_AI_SETUP_H
#include <osgViewer/Viewer>
#include "ai_tools.h"
#include "ai_chat.h"
#include "LayerManager.h"
class QuakeLayer; class FlightLayer;
class AIChatUI;
namespace osgVerse { class EarthManipulator; }
namespace earthai { class MediaManager; }

// Task 9 PART A 审查意见:configureAIChat 原先用一堆位置参数 + 一个出参指针(outMedia),
// 调用点稍不注意就会传错顺序/漏传出参。改成"入参结构体 + 返回结构体"——
// 入参一目了然(字段名即语义),返回值把 core/media 两个"进程期存活的裸指针"打包
// 一起给调用方,不再需要额外的出参指针。此签名为最终形态。
struct AIChatDeps
{
    osgViewer::Viewer* viewer = nullptr;
    osgVerse::EarthManipulator* mani = nullptr;
    LayerManager* layers = nullptr;
    QuakeLayer* quakes = nullptr;
    FlightLayer* flights = nullptr;
    AIChatUI* ui = nullptr;   // show_chart 等要回调它塞右上角卡片;可为 null(对应工具返回 error)
};

struct AIChatRuntime
{
    earthai::AIChatCore* core = nullptr;    // 无 EARTH_AI_KEY/EARTH_AI_FAKE 时为 null=零影响
    earthai::MediaManager* media = nullptr; // 无 EARTH_AI_KEY/EARTH_AI_FAKE 时为 null
};

// 注册全部 AI 工具并按 EARTH_AI_KEY/EARTH_AI_FAKE 创建对话核心;同时挂 FRAME drain handler
// 与 EARTH_AI_AUTOSUBMIT。返回结构体里的指针进程期存活。
AIChatRuntime configureAIChat(const AIChatDeps& deps);
#endif
