#ifndef EARTH_AI_SETUP_H
#define EARTH_AI_SETUP_H
#include <osgViewer/Viewer>
#include "ai_tools.h"
#include "ai_chat.h"
#include "LayerManager.h"
class QuakeLayer; class FlightLayer;
class AIChatUI;
namespace osgVerse { class EarthManipulator; }

// 注册全部 AI 工具并按 EARTH_AI_KEY/EARTH_AI_FAKE 创建对话核心(可为 null=零影响);
// 同时挂 FRAME drain handler 与 EARTH_AI_AUTOSUBMIT。返回的指针进程期存活。
// ui:show_chart 等需要往右上角卡片队列塞数据的工具用它回调(ui 可为 null,对应工具会返回 error)。
earthai::AIChatCore* configureAIChat(osgViewer::Viewer& viewer,
                                     osgVerse::EarthManipulator* mani,
                                     LayerManager* layers,
                                     QuakeLayer* quakes, FlightLayer* flights,
                                     AIChatUI* ui);
#endif
