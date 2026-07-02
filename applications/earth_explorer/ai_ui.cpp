#include "ai_ui.h"
#include <ui/ImGuiComponents.h>
#include <cstring>
#include <algorithm>

AIChatUI::AIChatUI() : _historyCollapsed(false), _lastEntryCount(0)
{
    _inputBuf[0] = '\0';
}

// 由 show_chart 工具的 execute 调用。execute 只在 AIChatCore::drainMainThread 里跑，
// 而 drain 又是从 FRAME handler（主线程）触发——与 draw() 同一线程，_cards 不会被
// 其它线程并发访问，因此这里不需要加锁。
void AIChatUI::pushChart(const picojson::value& spec)
{
    AICard c; c.type = AICard::CHART; c.spec = spec; c.open = true;
    _cards.push_back(c);
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

    drawCards();
}

// ---- 右上角图表卡堆叠 ----
// 锚定/风格与 EarthControlUI.h 里既有的「地震详情」「航班详情」卡保持一致：
// 固定 x = DisplaySize.x - 20，ImGuiCond_Always，锚点 (1,0)；两张既有卡分别在
// y=20 与 y=160（140px 一档，经验值，够放各卡内容不重叠）。AI 卡是新的一列，
// 从 y=300（地震+航班两档之后再留一档）开始向下堆叠，每卡实际高度用于摆下一张卡。
void AIChatUI::drawCards()
{
    if (_cards.empty()) return;
    ImGuiIO& io = ImGui::GetIO();
    const float kRightMargin = 20.0f;
    const float kCardWidth = 340.0f;
    const float kCardGap = 12.0f;
    float y = 300.0f;   // 地震卡(20)+航班卡(160,约140高)之下留出的起始位

    for (size_t i = 0; i < _cards.size(); )
    {
        AICard& c = _cards[i];
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - kRightMargin, y),
                                ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(kCardWidth, 0.0f), ImGuiCond_Always);

        std::string title = u8"图表 Chart";
        if (c.type == AICard::CHART && c.spec.is<picojson::object>() && c.spec.contains("title")
            && c.spec.get("title").is<std::string>())
            title = c.spec.get("title").get<std::string>();

        bool open = c.open;
        // ImGui 窗口的身份(ID)只看 "###" 之后的部分，不看它前面的可见标题；
        // 若所有卡片都用同一个 "###ai_card" 后缀，无论标题文字是什么，三张卡会被
        // 当成同一个窗口(后画的盖掉先画的、位置/尺寸互相污染)——这里踩过一次坑。
        // 用卡片在 vector 中的下标拼进 ID 后缀，确保每张卡是独立窗口。
        std::string winId = title + "###ai_card_" + std::to_string(i);
        float cardH = c.lastHeight;   // 上一帧没测到时(首次绘制)先用缓存的兜底值
        if (ImGui::Begin(winId.c_str(), &open,
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse))
        {
            if (c.type == AICard::CHART) drawChartCard(c.spec, kCardWidth);
            else ImGui::TextDisabled(u8"（暂未实现的卡片类型）");
            // 必须在 End() 之前读:GetWindowSize() 取的是"当前窗口"的尺寸，End() 之后
            // 当前窗口上下文已经弹出，再读到的就不是这张卡片的高度了(踩过的坑)。
            cardH = ImGui::GetWindowSize().y;
        }
        ImGui::End();

        c.open = open;
        c.lastHeight = cardH;
        y += cardH + kCardGap;
        ++i;
    }

    // 关闭的卡片本帧结束后一并清掉,避免在遍历中 erase 打乱下标
    _cards.erase(std::remove_if(_cards.begin(), _cards.end(),
                                 [](const AICard& c) { return !c.open; }),
                 _cards.end());
}

// ---- 数值取值小工具:picojson 的 JSON number 一律存成 double,int/double 字面量都能用 is<double>() 判出 ----
static double numOr(const picojson::value& v, double fallback)
{
    return v.is<double>() ? v.get<double>() : fallback;
}

// 从 spec 里取 labels[]/values[] 的公共部分(按 min 长度对齐),分别转成 string/double 数组。
static void extractLabelsValues(const picojson::value& spec,
                                std::vector<std::string>& labels, std::vector<double>& values)
{
    if (spec.contains("labels") && spec.get("labels").is<picojson::array>())
    {
        const picojson::array& arr = spec.get("labels").get<picojson::array>();
        for (size_t i = 0; i < arr.size(); ++i)
            labels.push_back(arr[i].is<std::string>() ? arr[i].get<std::string>() : arr[i].to_str());
    }
    if (spec.contains("values") && spec.get("values").is<picojson::array>())
    {
        const picojson::array& arr = spec.get("values").get<picojson::array>();
        for (size_t i = 0; i < arr.size(); ++i)
            values.push_back(numOr(arr[i], 0.0));
    }
    size_t n = std::min(labels.size(), values.size());
    labels.resize(n); values.resize(n);
}

// 六色调色板(hue 均分),donut 分片按序取色；用于区分不同切片而不需要每次算 HSV。
static ImU32 sliceColor(size_t idx)
{
    static const ImU32 palette[6] = {
        IM_COL32(90, 170, 255, 255),   // 蓝(accent)
        IM_COL32(255, 170, 60, 255),   // 橙
        IM_COL32(90, 220, 140, 255),   // 绿
        IM_COL32(230, 90, 140, 255),   // 粉
        IM_COL32(190, 130, 255, 255),  // 紫
        IM_COL32(240, 220, 80, 255),   // 黄
    };
    return palette[idx % 6];
}

static void drawBarChart(ImDrawList* dl, ImVec2 origin, float width,
                         const std::vector<std::string>& labels, const std::vector<double>& values)
{
    const ImU32 accent = IM_COL32(90, 170, 255, 255);
    const ImU32 track = IM_COL32(255, 255, 255, 25);
    double maxV = 0.0;
    for (size_t i = 0; i < values.size(); ++i) maxV = std::max(maxV, values[i]);

    const float rowH = 22.0f, rowGap = 6.0f;
    const float labelW = 56.0f, valueW = 40.0f;
    float barAreaW = width - labelW - valueW - 8.0f;
    if (barAreaW < 20.0f) barAreaW = 20.0f;

    float y = origin.y;
    for (size_t i = 0; i < values.size(); ++i)
    {
        // 标签(左)
        dl->AddText(ImVec2(origin.x, y + 3.0f), IM_COL32(220, 220, 220, 255), labels[i].c_str());

        // 条形背景 track + 前景 bar(0/负值钳到 0 长度,但数值文字仍照常显示)
        float barX = origin.x + labelW;
        dl->AddRectFilled(ImVec2(barX, y), ImVec2(barX + barAreaW, y + rowH), track, 4.0f);
        double v = values[i]; if (v < 0.0) v = 0.0;
        float frac = (maxV > 0.0) ? (float)(v / maxV) : 0.0f;
        float barW = barAreaW * frac;
        if (barW > 1.0f)
            dl->AddRectFilled(ImVec2(barX, y), ImVec2(barX + barW, y + rowH), accent, 4.0f);

        // 数值(右)
        char buf[32];
        double raw = values[i];
        if (raw == (double)(long long)raw) snprintf(buf, sizeof(buf), "%lld", (long long)raw);
        else snprintf(buf, sizeof(buf), "%.1f", raw);
        dl->AddText(ImVec2(barX + barAreaW + 8.0f, y + 3.0f), IM_COL32(230, 230, 230, 255), buf);

        y += rowH + rowGap;
    }
    ImGui::Dummy(ImVec2(width, y - origin.y));
}

// imgui.h(公开头)不带 IM_PI(定义在 imgui_internal.h，本文件不想拉内部头)，本地补一个。
static const float kTwoPi = 6.28318530718f;

static void drawDonutChart(ImDrawList* dl, ImVec2 origin, float width,
                           const std::vector<std::string>& labels, const std::vector<double>& values)
{
    double total = 0.0;
    for (size_t i = 0; i < values.size(); ++i) total += std::max(0.0, values[i]);

    const float radius = 58.0f, thickness = 18.0f;
    ImVec2 center(origin.x + width * 0.5f, origin.y + radius);

    if (total <= 0.0)
    {
        dl->AddCircle(center, radius, IM_COL32(255, 255, 255, 40), 64, thickness);
    }
    else
    {
        float startAngle = -kTwoPi * 0.25f;   // 12 点钟方向起(-90°)
        for (size_t i = 0; i < values.size(); ++i)
        {
            double v = std::max(0.0, values[i]);
            if (v <= 0.0) continue;
            float sweep = (float)(v / total) * kTwoPi;
            dl->PathArcTo(center, radius, startAngle, startAngle + sweep, 48);
            dl->PathStroke(sliceColor(i), 0, thickness);
            startAngle += sweep;
        }
    }

    // 中心大数字(总计)
    char totBuf[32];
    if (total == (double)(long long)total) snprintf(totBuf, sizeof(totBuf), "%lld", (long long)total);
    else snprintf(totBuf, sizeof(totBuf), "%.1f", total);
    ImVec2 tSize = ImGui::CalcTextSize(totBuf);
    dl->AddText(ImVec2(center.x - tSize.x * 0.5f, center.y - tSize.y * 0.5f),
               IM_COL32(255, 255, 255, 255), totBuf);

    ImGui::Dummy(ImVec2(width, radius * 2.0f + 4.0f));

    // 图例:色点 + 标签 + 数值,逐行排列
    for (size_t i = 0; i < values.size(); ++i)
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddCircleFilled(ImVec2(p.x + 6.0f, p.y + 8.0f), 5.0f, sliceColor(i));
        ImGui::Dummy(ImVec2(16.0f, 0.0f));
        ImGui::SameLine();
        char buf[64];
        double v = values[i];
        if (v == (double)(long long)v)
            snprintf(buf, sizeof(buf), "%s: %lld", labels[i].c_str(), (long long)v);
        else
            snprintf(buf, sizeof(buf), "%s: %.1f", labels[i].c_str(), v);
        ImGui::TextUnformatted(buf);
    }
}

static void drawLineChart(ImDrawList* dl, ImVec2 origin, float width,
                          const std::vector<std::string>& labels, const std::vector<double>& values)
{
    const float chartH = 90.0f;
    double vmin = values[0], vmax = values[0];
    for (size_t i = 1; i < values.size(); ++i) { vmin = std::min(vmin, values[i]); vmax = std::max(vmax, values[i]); }
    double range = vmax - vmin;
    if (range <= 0.0) range = 1.0;   // 全等值时避免除零,画一条水平线

    std::vector<ImVec2> pts(values.size());
    float stepX = (values.size() > 1) ? width / (float)(values.size() - 1) : 0.0f;
    for (size_t i = 0; i < values.size(); ++i)
    {
        float t = (float)((values[i] - vmin) / range);
        float x = origin.x + stepX * (float)i;
        float y = origin.y + chartH - t * chartH;
        pts[i] = ImVec2(x, y);
    }

    // 折线下方填充(简单梯形近似:折线 + 底边闭合成一个凸/近凸多边形；数据点少时视觉足够)
    if (pts.size() >= 2)
    {
        std::vector<ImVec2> fillPts = pts;
        fillPts.push_back(ImVec2(pts.back().x, origin.y + chartH));
        fillPts.push_back(ImVec2(pts.front().x, origin.y + chartH));
        dl->AddConvexPolyFilled(fillPts.data(), (int)fillPts.size(), IM_COL32(90, 170, 255, 40));
    }
    if (pts.size() >= 2)
        dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(90, 170, 255, 255), 0, 2.0f);
    for (size_t i = 0; i < pts.size(); ++i)
        dl->AddCircleFilled(pts[i], 3.0f, IM_COL32(255, 255, 255, 220));

    // 轴范围标注(左上角=max,左下角=min)
    char maxBuf[32], minBuf[32];
    snprintf(maxBuf, sizeof(maxBuf), "%.1f", vmax);
    snprintf(minBuf, sizeof(minBuf), "%.1f", vmin);
    dl->AddText(ImVec2(origin.x, origin.y - 2.0f), IM_COL32(180, 180, 180, 255), maxBuf);
    dl->AddText(ImVec2(origin.x, origin.y + chartH - 12.0f), IM_COL32(180, 180, 180, 255), minBuf);

    ImGui::Dummy(ImVec2(width, chartH + 6.0f));

    // 首尾标签(数据点多时中间标签容易挤在一起,先只标首尾,够用)
    if (!labels.empty())
    {
        ImGui::TextDisabled("%s", labels.front().c_str());
        if (labels.size() > 1) { ImGui::SameLine(width - ImGui::CalcTextSize(labels.back().c_str()).x); ImGui::TextDisabled("%s", labels.back().c_str()); }
    }
}

static void drawStatChart(const picojson::value& spec, const std::vector<std::string>& labels,
                          const std::vector<double>& values)
{
    double v = values.empty() ? 0.0 : values[0];
    char buf[32];
    if (v == (double)(long long)v) snprintf(buf, sizeof(buf), "%lld", (long long)v);
    else snprintf(buf, sizeof(buf), "%.2f", v);

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::SetWindowFontScale(2.2f);
    ImGui::TextUnformatted(buf);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();

    if (!labels.empty()) ImGui::TextDisabled("%s", labels[0].c_str());
    (void)spec;
}

void AIChatUI::drawChartCard(const picojson::value& spec, float width)
{
    if (!spec.is<picojson::object>()) { ImGui::TextDisabled(u8"无数据"); return; }

    std::string type = spec.contains("type") && spec.get("type").is<std::string>()
                        ? spec.get("type").get<std::string>() : "bar";

    std::vector<std::string> labels;
    std::vector<double> values;
    extractLabelsValues(spec, labels, values);

    if (values.empty())
    {
        // 居中的"无数据"提示
        const char* msg = u8"无数据";
        float avail = ImGui::GetContentRegionAvail().x;
        float tw = ImGui::CalcTextSize(msg).x;
        if (avail > tw) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - tw) * 0.5f);
        ImGui::TextDisabled("%s", msg);
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();

    if (type == "donut") drawDonutChart(dl, origin, width, labels, values);
    else if (type == "line") drawLineChart(dl, origin, width, labels, values);
    else if (type == "stat") drawStatChart(spec, labels, values);
    else drawBarChart(dl, origin, width, labels, values);   // 默认/未知类型按 bar 处理
}
