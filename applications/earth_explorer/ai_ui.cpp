#include "ai_ui.h"
#include <ui/ImGuiComponents.h>
#include <cstring>

AIChatUI::AIChatUI() : _historyCollapsed(false), _lastEntryCount(0)
{
    _inputBuf[0] = '\0';
}

// 转发给 AICardPanel(Task 8 PART A 从本文件抽出,见 ai_cards.h/.cpp)。
void AIChatUI::pushChart(const picojson::value& spec)
{
    _cards.pushChart(spec);
}

void AIChatUI::draw(earthai::AIChatCore* core)
{
    ImGuiIO& io = ImGui::GetIO();
    float winWidth = (720.0f < io.DisplaySize.x * 0.6f) ? 720.0f : io.DisplaySize.x * 0.6f;

    // 底部居中悬浮，锚点在窗口底边中点——不与左上角操作面板 / 右上角信息卡重叠。
    // 左侧操作面板（Earth Control）自适应宽度约 610px（含"透明度"滑块最长行），且默认展开时
    // 高度可达全屏，与正下方居中的对话条在窄屏下会重叠；把居中点右移半个面板宽，
    // 让对话条左边界不早于面板右边界 + 留白，宽屏（面板远窄于半屏）时右移量趋近 0 视觉上仍居中。
    // 同时钳制右边界不超出屏幕（窄屏下右移可能把窗口推出可视区），必要时收窄宽度，
    // 保证对话条左右都留在面板与屏幕边界之间、始终完整可见。
    static const float kLeftPanelClearance = 630.0f;   // 面板右边界 + ~20px 留白
    static const float kRightMargin = 16.0f;
    float availRight = io.DisplaySize.x - kRightMargin;
    if (winWidth > availRight - kLeftPanelClearance)
        winWidth = (availRight - kLeftPanelClearance > 240.0f) ? availRight - kLeftPanelClearance : 240.0f;
    float centerX = io.DisplaySize.x * 0.5f;
    float minCenterX = kLeftPanelClearance + winWidth * 0.5f;
    float maxCenterX = availRight - winWidth * 0.5f;
    if (centerX < minCenterX) centerX = minCenterX;
    if (centerX > maxCenterX) centerX = maxCenterX;
    ImGui::SetNextWindowPos(ImVec2(centerX, io.DisplaySize.y - 16.0f),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(winWidth, 0.0f), ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.09f, 0.11f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.12f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin(u8"AI 对话条", NULL, flags))
    {
        bool busy = core && core->busy();

        // ---- 历史面板（在输入行上方；无内容或无 core 时不画）----
        if (core)
        {
            std::vector<earthai::ChatEntry> transcript = core->transcript();   // 每帧一次快照
            if (!transcript.empty())
            {
                if (ImGui::SmallButton(_historyCollapsed ? u8"展开 v" : u8"收起 ^"))
                    _historyCollapsed = !_historyCollapsed;
                ImGui::SameLine();
                ImGui::TextDisabled(u8"对话历史（%d 条）", (int)transcript.size());

                if (!_historyCollapsed)
                {
                    float maxH = io.DisplaySize.y * 0.3f;
                    ImGui::BeginChild("##ai_history", ImVec2(winWidth - 24.0f, maxH), true,
                                      ImGuiWindowFlags_HorizontalScrollbar);
                    for (size_t i = 0; i < transcript.size(); ++i)
                    {
                        const earthai::ChatEntry& e = transcript[i];
                        bool isLast = (i + 1 == transcript.size());
                        switch (e.kind)
                        {
                        case earthai::ChatEntry::USER:
                        {
                            // 右对齐：按可用宽度估算文本起始 x
                            float avail = ImGui::GetContentRegionAvail().x;
                            float textW = ImGui::CalcTextSize(e.text.c_str()).x;
                            float pad = avail - textW; if (pad < 0.0f) pad = 0.0f;
                            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.75f, 1.0f, 1.0f));
                            ImGui::TextWrapped(u8"%s", e.text.c_str());
                            ImGui::PopStyleColor();
                            break;
                        }
                        case earthai::ChatEntry::ASSISTANT:
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                            ImGui::TextWrapped(u8"%s", e.text.c_str());
                            ImGui::PopStyleColor();
                            break;
                        case earthai::ChatEntry::TOOL_NOTE:
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                            ImGui::TextWrapped(u8"%s", e.text.c_str());
                            ImGui::PopStyleColor();
                            break;
                        case earthai::ChatEntry::ERR:
                        default:
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                            ImGui::TextWrapped(u8"%s", e.text.c_str());
                            ImGui::PopStyleColor();
                            break;
                        }
                        // 忙碌态：最后一行追加动画小圆点/转轮，提示仍在处理
                        if (busy && isLast)
                        {
                            static const char spin[4] = { '|', '/', '-', '\\' };
                            int idx = (int)(ImGui::GetTime() * 8.0) % 4;
                            ImGui::SameLine();
                            ImGui::TextDisabled(" %c", spin[idx]);
                        }
                    }
                    // 新增条目时自动滚动到底部
                    if (transcript.size() != _lastEntryCount)
                    {
                        ImGui::SetScrollHereY(1.0f);
                        _lastEntryCount = transcript.size();
                    }
                    ImGui::EndChild();
                }
            }
            else if (busy)
            {
                static const char spin[4] = { '|', '/', '-', '\\' };
                int idx = (int)(ImGui::GetTime() * 8.0) % 4;
                ImGui::TextDisabled(u8"思考中 %c", spin[idx]);
            }
        }

        // ---- 输入行 ----
        bool submitted = false;
        std::string submitText;
        if (!core) ImGui::BeginDisabled();
        else if (busy) ImGui::BeginDisabled();

        // 24=窗口左右 padding 12x2；44=后面两个按钮各占「按钮宽 40 + SameLine 间距 4」。
        ImGui::SetNextItemWidth(winWidth - (core ? 24.0f + 2.0f * 44.0f : 24.0f));
        const char* hint = core ? u8"问我：飞到纽约 / 打开航班层 / 统计全球地震…"
                                 : u8"设置 EARTH_AI_KEY 启用 AI 对话";
        if (ImGui::InputTextWithHint("##ai_input", hint, _inputBuf, sizeof(_inputBuf),
                                     ImGuiInputTextFlags_EnterReturnsTrue))
        {
            if (_inputBuf[0] != '\0') { submitText = _inputBuf; submitted = true; }
            _inputBuf[0] = '\0';
            ImGui::SetKeyboardFocusHere(-1);
        }

        if (!core || busy) ImGui::EndDisabled();

        if (core)
        {
            // 照片/视频生成入口（Task 8/9 接线）；本任务只画禁用按钮 + tooltip。
            // 内置中文字体（ChineseFull 范围）不含 emoji glyph，📷/🎬 会渲染成方块（tofu），
            // 因此用文字标签"照片"/"视频"代替。
            ImGui::SameLine();
            ImGui::BeginDisabled();
            ImGui::Button(u8"照片", ImVec2(40.0f, 0.0f));
            ImGui::EndDisabled();
            // ImGui 1.92 起 IsItemHovered() 默认对禁用项返回 false，需显式加
            // ImGuiHoveredFlags_AllowWhenDisabled 才能在禁用按钮上弹出 tooltip。
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(u8"生成照片（即将上线）");

            ImGui::SameLine();
            ImGui::BeginDisabled();
            ImGui::Button(u8"视频", ImVec2(40.0f, 0.0f));
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(u8"生成视频（即将上线）");
        }

        if (submitted && core) core->submit(submitText);
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);

    _cards.draw();
}
