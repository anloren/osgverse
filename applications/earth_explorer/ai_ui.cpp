#include "ai_ui.h"
#include "ai_media.h"
#include <ui/ImGuiComponents.h>
#include <readerwriter/EarthManipulator.h>
#include <osg/Math>
#include <osg/Notify>
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

void AIChatUI::draw(earthai::AIChatCore* core, earthai::MediaManager* media, osgVerse::EarthManipulator* mani)
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
    // 函数作用域(而非 Begin/End 块内)声明:确认 Modal 画在 AI 对话条窗口 End() 之后,
    // 需要跨过该窗口的 { } 作用域读到这个标志。
    bool openVideoModal = false;
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

        bool photoSubmit = false;
        // openVideoModal 声明在外层函数作用域(见上方),本帧是否需要 OpenPopup
        // (A/B 都就绪、Modal 尚未打开时置真)在下面 if(core) 块里赋值。
        if (core)
        {
            // 照片生成入口（Task 8 接线，见 ai_media.h/MediaManager）；🎬 视频（Task 9）三态按钮。
            // 内置中文字体（ChineseFull 范围）不含 emoji glyph，📷/🎬 会渲染成方块（tofu），
            // 因此用文字标签"照片"/"视频"代替。
            ImGui::SameLine();
            bool photoEnabled = (core && media && !busy);
            if (!photoEnabled) ImGui::BeginDisabled();
            if (ImGui::Button(u8"照片", ImVec2(40.0f, 0.0f))) photoSubmit = true;
            if (!photoEnabled) ImGui::EndDisabled();
            // ImGui 1.92 起 IsItemHovered() 默认对禁用项返回 false，需显式加
            // ImGuiHoveredFlags_AllowWhenDisabled 才能在禁用按钮上弹出 tooltip。
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip(media ? u8"生成当前视角实景照片，约 $0.04/张"
                                        : u8"生成照片（需设置 EARTH_AI_KEY）");
            }

            // 🎬 三态：空闲"视频" -> 已录 A"完成B点"(+取消) -> 两点都录完:自动弹确认 Modal。
            earthai::VideoPhaseKindPublic vphase = media ? media->videoPhase() : earthai::VIDEO_IDLE;
            bool videoEnabled = (core && media && mani && !busy
                                 && (vphase == earthai::VIDEO_IDLE || vphase == earthai::VIDEO_WAIT_B));
            ImGui::SameLine();
            if (vphase == earthai::VIDEO_WAIT_B)
            {
                if (!videoEnabled) ImGui::BeginDisabled();
                if (ImGui::Button(u8"完成B点", ImVec2(64.0f, 0.0f)) && mani)
                {
                    osg::Vec3d llaB = mani->computeEyeLatLonHeight();
                    media->captureVideoEnd(llaB);
                }
                if (!videoEnabled) ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::SmallButton(u8"取消") && media) media->cancelVideo();
            }
            else
            {
                bool idleEnabled = (core && media && mani && !busy && vphase == earthai::VIDEO_IDLE);
                if (!idleEnabled) ImGui::BeginDisabled();
                if (ImGui::Button(u8"视频", ImVec2(40.0f, 0.0f)) && mani)
                {
                    osg::Vec3d llaA = mani->computeEyeLatLonHeight();
                    media->beginVideoCapture(llaA);
                }
                if (!idleEnabled) ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::SetTooltip(media && mani
                        ? u8"记录起点 A，移动相机后点「完成B点」生成 8 秒巡航视频（约 $2-6）"
                        : u8"生成视频（需设置 EARTH_AI_KEY / EARTH_AI_FAKE_MP4）");
                }
            }

            // A/B 两点都就绪 -> 打开确认 Modal（本帧只判断是否需要 OpenPopup，实际弹窗内容
            // 在下面统一画,避免 OpenPopup 调用点分散）。
            if (vphase == earthai::VIDEO_AWAIT_CONFIRM) openVideoModal = true;
        }

        // 📷 走对话代理循环（而不是直接调用 MediaManager）：保持"一切能力皆工具"的架构,
        // 且让用户在对话历史里看到这次操作的记录；FAKE 模式下 fixture 脚本需要自己调用
        // generate_photo(见 test/ai_fake_photo.json)。🎬 走直接调用(见上面按钮),不经对话
        // 循环——两点采集 + 确认 Modal 是强 UI 流程,硬塞进自然语言指令反而别扭
        // (generate_video 工具走的是另一条路径,同样落到 MediaManager 同一套状态机)。
        if (photoSubmit && core) core->submit(u8"生成一张当前视角的实景照片");
        if (submitted && core) core->submit(submitText);
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);

    // ---- 视频确认 Modal(Task 9):居中弹窗,展示 A/B 坐标 + 运动提示词预览 + 费用提示。----
    // 放在 AI 对话条窗口 Begin/End 之外(Modal 是独立的顶层窗口,不依赖对话条是否展开)。
    if (media)
    {
        if (openVideoModal && !ImGui::IsPopupOpen(u8"确认生成巡航视频"))
            ImGui::OpenPopup(u8"确认生成巡航视频");

        ImGuiIO& ioModal = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(ioModal.DisplaySize.x * 0.5f, ioModal.DisplaySize.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Always);
        if (ImGui::BeginPopupModal(u8"确认生成巡航视频", NULL,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize))
        {
            earthai::MediaManager::PendingVideoInfo info = media->pendingVideoInfo();
            if (info.ready)
            {
                ImGui::Text(u8"起点 A：纬度 %.4f° 经度 %.4f° 高度 %.1fm",
                           osg::RadiansToDegrees(info.llaA[0]), osg::RadiansToDegrees(info.llaA[1]), info.llaA[2]);
                ImGui::Text(u8"终点 B：纬度 %.4f° 经度 %.4f° 高度 %.1fm",
                           osg::RadiansToDegrees(info.llaB[0]), osg::RadiansToDegrees(info.llaB[1]), info.llaB[2]);
                ImGui::Separator();
                ImGui::TextWrapped("%s", info.motionPrompt.c_str());
                ImGui::Separator();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
                ImGui::TextWrapped(u8"将调用 Veo 生成 8 秒视频，约 $2-6，确认？");
                ImGui::PopStyleColor();

                if (ImGui::Button(u8"确认生成", ImVec2(120.0f, 0.0f)))
                {
                    picojson::value r = media->confirmVideo();
                    OSG_NOTICE << "[AIChat] video confirm -> " << r.serialize() << std::endl;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(u8"取消", ImVec2(80.0f, 0.0f)) ||
                    ImGui::IsKeyPressed(ImGuiKey_Escape))
                {
                    media->cancelVideo();
                    ImGui::CloseCurrentPopup();
                }
            }
            else
            {
                // 理论上不会发生(openVideoModal 只在 AWAIT_CONFIRM 时置真),防御一下:
                // 状态已经被别的路径清掉(如按钮的「取消」在同一帧发生)就直接关掉弹窗。
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    _cards.draw();
}
