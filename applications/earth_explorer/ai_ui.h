#ifndef EARTH_AI_UI_H
#define EARTH_AI_UI_H
#include "ai_chat.h"
#include "ai_cards.h"
#include <picojson.h>
#include <string>

namespace earthai { class MediaManager; }
namespace osgVerse { class EarthManipulator; }

// 底部悬浮聊天条：输入框 + 历史面板（历史在输入行上方，同一窗口内）+ 右上角卡片堆叠
// (卡片存储/绘制委托给 AICardPanel，见 ai_cards.h)。
// core 为 null 时画禁用态输入框（提示设置 EARTH_AI_KEY），不画历史/按钮——对无 AI 场景零干扰。
class AIChatUI
{
public:
    AIChatUI();
    // media 为空(未配置 key/EARTH_AI_FAKE_IMG)时📷按钮保持禁用态,其余不受影响。
    // mani(Task 9):🎬 按钮记录 A/B 两点位姿需要读当前相机经纬度/高度
    // (EarthManipulator::computeEyeLatLonHeight());为空时🎬按钮保持禁用态。
    void draw(earthai::AIChatCore* core, earthai::MediaManager* media = nullptr,
              osgVerse::EarthManipulator* mani = nullptr);

    // 主线程调用(工具 execute 在 drain 中,与 draw() 同线程,无需加锁——见 ai_cards.cpp 头注释)。
    // 转发给 _cards,保留原有调用方(ai_setup.cpp 的 show_chart 工具)不必改动。
    void pushChart(const picojson::value& spec);

    // 供其它模块(如 Task 8 的 MediaManager)直接推卡片,不必都经 AIChatUI 转发方法。
    AICardPanel* cards() { return &_cards; }

private:
    char _inputBuf[1024];      // InputText 缓冲区，提交时转 std::string 再清空
    bool _historyCollapsed;    // 历史面板折叠状态（默认展开）
    size_t _lastEntryCount;    // 上次绘制时的历史条数，用于检测新增条目并自动滚动到底部
    AICardPanel _cards;
};
#endif
