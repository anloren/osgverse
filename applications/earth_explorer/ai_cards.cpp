// applications/earth_explorer/ai_cards.cpp
// 从 ai_ui.cpp 抽出(Task 8 PART A):卡片容器 AICardPanel 的存储/堆叠/绘制 + 4 种
// 图表手绘渲染器 + 数值/取值小工具。图表部分纯移动,逻辑与原 ai_ui.cpp 版本逐行一致;
// 照片卡(drawPhotoCard/pushPhoto)是 Task 8 新增。
#include "ai_cards.h"
#include <ui/ImGuiComponents.h>
#include <osg/Notify>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>

// 由 show_chart 工具的 execute 调用。execute 只在 AIChatCore::drainMainThread 里跑，
// 而 drain 又是从 FRAME handler（主线程）触发——与 draw() 同一线程，_cards 不会被
// 其它线程并发访问，因此这里不需要加锁。
void AICardPanel::pushChart(const picojson::value& spec)
{
    AICard c; c.type = AICard::CHART; c.spec = spec; c.open = true;
    c.serial = _nextSerial++;
    _cards.push_back(c);
}

// 由 MediaManager::update() 在生图 Job 完成(DONE)时调用,主线程同源(见上方注释),无需加锁。
void AICardPanel::pushPhoto(const std::string& pngPath, const std::string& title)
{
    AICard c; c.type = AICard::PHOTO; c.path = pngPath; c.title = title; c.open = true;
    c.serial = _nextSerial++;
    _cards.push_back(c);
}

// 由 MediaManager::startPhotoJob() 在建 Job 后立即调用(同一主线程,无需加锁)。
void AICardPanel::pushJob(earthai::JobManager* jobs, int jobId, const std::string& title)
{
    AICard c; c.type = AICard::JOB; c.jobs = jobs; c.jobId = jobId; c.title = title; c.open = true;
    c.serial = _nextSerial++;
    _cards.push_back(c);
}

// 由 MediaManager::update() 在 Job 结束(DONE/FAILED)时调用,把进度卡从堆叠里摘掉——
// DONE 那一刻调用方会紧接着 pushPhoto() 换上结果卡,FAILED 则直接消失(错误已走 OSG_WARN)。
void AICardPanel::removeJob(int jobId)
{
    for (size_t i = 0; i < _cards.size(); ++i)
        if (_cards[i].type == AICard::JOB && _cards[i].jobId == jobId) { _cards[i].open = false; break; }
}

// ---- 右上角图表卡堆叠 ----
// 锚定/风格与 EarthControlUI.h 里既有的「地震详情」「航班详情」卡保持一致：
// 固定 x = DisplaySize.x - 20，ImGuiCond_Always，锚点 (1,0)；两张既有卡分别在
// y=20 与 y=160（140px 一档，经验值，够放各卡内容不重叠）。AI 卡是新的一列，
// 最右列从 y=300（地震+航班两档之后再留一档）开始向下堆叠，每卡实际高度用于摆下一张卡。
// 溢出换列:多张卡总高可能超过屏幕(1280x800 逻辑分辨率下三张卡 ≈554px,起点 300 就
// 到底裁切并压住聊天条),放卡前先用上一帧缓存的高度(首帧未知用估计值)判断会不会
// 越过底部保留区,越过就向左开新列。地震/航班详情卡只占最右列,所以向左的新列可以
// 从 y=20 顶部开始,不会压到它们。极端很多卡时列数不设上限、一直向左铺
// (实际场景卡片就个位数,不做更多处理)。
// 已知权衡:1280 逻辑宽下,第二列(向左新列)的右边界会与左侧控制面板(约 610px 宽,
// 详见 ai_ui.cpp draw() 里 kLeftPanelClearance=630 的注释)相交约 40px。这里不做进一步避让——
// 卡片可以 [x] 关闭、面板可以拖动/折叠,用户的偏好是"信息面板右上角、操作面板左上角"
// 这种逻辑分区(见 ui-info-panels-top-right 偏好),而不是要求两者像素级不相交。
void AICardPanel::draw()
{
    if (_cards.empty()) return;
    ImGuiIO& io = ImGui::GetIO();
    const float kRightMargin = 20.0f;
    const float kCardWidth = 340.0f;
    const float kCardGap = 12.0f;
    const float kFirstColTopY = 300.0f;    // 最右列起始 y:地震卡(20)+航班卡(160,约140高)之下
    const float kNextColTopY = 20.0f;      // 向左新列起始 y:该列没有详情卡,可从顶部开始
    const float kBottomReserve = 160.0f;   // 底部保留区:给底部居中的 AI 聊天条留空(收起态高度+边距)
    const float kEstimateHeight = 200.0f;  // 首帧 lastHeight 未知时用于换列判断的估计卡高
    float curX = io.DisplaySize.x - kRightMargin;   // 当前列右边界(配合 (1,0) 锚点)
    float curY = kFirstColTopY;

    for (size_t i = 0; i < _cards.size(); )
    {
        AICard& c = _cards[i];

        // 已被标记关闭(如 removeJob() 在 JOB→PHOTO 替换时先摘掉旧 JOB 卡)的卡片本帧
        // 直接跳过、不占布局位置——否则会在"标记关闭"与"本函数末尾 erase 清理"之间
        // 出现一帧"关闭中"的鬼影(仍占一格位置,但内容是"任务信息不可用"之类的占位文案)。
        // 放在循环最前面判断,不进入下面的换列/绘制逻辑,直接推进到下一张卡。
        if (!c.open) { ++i; continue; }

        // 溢出换列:用已知(上一帧)或估计的卡高预判,这一列放不下就向左开新列。
        // curY > kNextColTopY 防御:若单卡本身比可用高度还高,仍要在列顶画出来(裁切总好过死循环)。
        float predictH = (c.lastHeight > 0.0f) ? c.lastHeight : kEstimateHeight;
        if (curY + predictH > io.DisplaySize.y - kBottomReserve && curY > kNextColTopY)
        {
            curX -= kCardWidth + kCardGap;
            curY = kNextColTopY;
        }

        ImGui::SetNextWindowPos(ImVec2(curX, curY), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(kCardWidth, 0.0f), ImGuiCond_Always);

        std::string title = u8"图表 Chart";
        if (c.type == AICard::CHART)
        {
            if (c.spec.is<picojson::object>() && c.spec.contains("title")
                && c.spec.get("title").is<std::string>())
                title = c.spec.get("title").get<std::string>();
        }
        else if (c.type == AICard::PHOTO)
            title = c.title.empty() ? u8"实景照片" : c.title;
        else if (c.type == AICard::JOB)
            title = c.title.empty() ? u8"生成中" : c.title;

        bool open = c.open;
        // ImGui 窗口的身份(ID)只看 "###" 之后的部分，不看它前面的可见标题；
        // 若所有卡片都用同一个 "###ai_card" 后缀，无论标题文字是什么，三张卡会被
        // 当成同一个窗口(后画的盖掉先画的、位置/尺寸互相污染)——这里踩过一次坑。
        // 用卡片的 serial(而非 vector 下标)拼进 ID 后缀:下标会因为前面的卡关闭而
        // 左移，同一张卡在关卡前后被 ImGui 认成两个不同窗口，产生一帧闪烁/错位；
        // serial 在 pushChart 时分配、卡片存活期间不变，不受其它卡关闭影响。
        std::string winId = title + "###ai_card_" + std::to_string(c.serial);
        float cardH = c.lastHeight;   // 上一帧没测到时(首次绘制)先用缓存的兜底值
        if (ImGui::Begin(winId.c_str(), &open,
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse))
        {
            // 用内容区宽度(已扣除 WindowPadding)而不是窗口全宽常量传给 drawChartCard，
            // 否则条形图/折线图右缘的数值文字会被 WindowPadding 裁掉一部分。
            if (c.type == AICard::CHART) drawChartCard(c.spec, ImGui::GetContentRegionAvail().x);
            else if (c.type == AICard::PHOTO) drawPhotoCard(c);
            else if (c.type == AICard::JOB) drawJobCard(c);
            else ImGui::TextDisabled(u8"（暂未实现的卡片类型）");
            // 必须在 End() 之前读:GetWindowSize() 取的是"当前窗口"的尺寸，End() 之后
            // 当前窗口上下文已经弹出，再读到的就不是这张卡片的高度了(踩过的坑)。
            cardH = ImGui::GetWindowSize().y;
        }
        ImGui::End();

        c.open = open;
        c.lastHeight = cardH;
        curY += cardH + kCardGap;
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

// ---- 数值格式化小工具:整数样式的值用 %lld 显示,否则 %.1f ----
// 原先各处直接写 `v == (double)(long long)v` 判断"是不是整数",对 NaN/inf/
// 超出 long long 范围的巨大值(如 1e20)做 (long long) 转换是未定义行为(UB)。
// 这里先做有限性+范围检查,异常值统一走 %.3g 兜底,再进整数/小数分支。
static void formatNum(char* buf, size_t n, double v)
{
    if (!(fabs(v) < 9e15))   // 覆盖 NaN(比较恒假)、+-inf、超出安全整数范围的巨大值
    {
        snprintf(buf, n, "%.3g", v);
        return;
    }
    long long iv = (long long)v;
    if (v == (double)iv) snprintf(buf, n, "%lld", iv);
    else snprintf(buf, n, "%.1f", v);
}

// 从 spec 里取 labels[]/values[] 数组,分别转成 string/double 数组。
// 以 values 为准对齐:labels 不足时用 "" 补齐到 values.size(),labels 多则截断。
// 工具 schema 的 required 只有 type+values(见 ai_setup.cpp show_chart.parametersJson),
// labels 本就是可选参数(stat 图的 labels[0] 副标题尤其常被省略),不能按 min 长度双向
// 截断——否则合法调用如 {"type":"stat","values":[7]} 会被截成空数据,整卡显示"无数据"。
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
    labels.resize(values.size());   // 不足补 ""(resize 默认值),多则截断
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
        formatNum(buf, sizeof(buf), values[i]);
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
    formatNum(totBuf, sizeof(totBuf), total);
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
        char numBuf[32], buf[96];
        formatNum(numBuf, sizeof(numBuf), values[i]);
        snprintf(buf, sizeof(buf), "%s: %s", labels[i].c_str(), numBuf);
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

    // 折线下方填充:逐段画梯形(而不是把整条折线所有顶点丢给 AddConvexPolyFilled)。
    // 折线顶边 + 底边两角拼成的多边形,只要数据不是单调的(如 420→455→430→500→480
    // 这种有起伏的锯齿数据)就是凹多边形——ImGui 的 convex fill 对凹多边形填色会出错,
    // 颜色可能涂到折线上方、或半透明区深浅不均。改成对每相邻两点 pts[i]/pts[i+1] 单独
    // 画一个梯形(两个顶点在折线上、两个顶点在底边上),每个梯形都必然是凸的,不会出错。
    if (pts.size() >= 2)
    {
        const ImU32 fillCol = IM_COL32(90, 170, 255, 40);
        float baseY = origin.y + chartH;
        for (size_t i = 0; i + 1 < pts.size(); ++i)
        {
            dl->AddQuadFilled(pts[i], pts[i + 1], ImVec2(pts[i + 1].x, baseY), ImVec2(pts[i].x, baseY),
                              fillCol);
        }
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

static void drawStatChart(const std::vector<std::string>& labels, const std::vector<double>& values)
{
    double v = values.empty() ? 0.0 : values[0];
    char buf[32];
    formatNum(buf, sizeof(buf), v);

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::SetWindowFontScale(2.2f);
    ImGui::TextUnformatted(buf);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();

    if (!labels.empty()) ImGui::TextDisabled("%s", labels[0].c_str());
}

void AICardPanel::drawChartCard(const picojson::value& spec, float width)
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
    else if (type == "stat") drawStatChart(labels, values);
    else drawBarChart(dl, origin, width, labels, values);   // 默认/未知类型按 bar 处理
}

// ---- 照片卡(Task 8):文件名 + 「打开」按钮,用系统默认查看器打开(macOS `open`)。----
// 缩略图故意跳过:ImGui 需要把 PNG 解码上传成 GL 纹理再画 image()——这条纹理管线在本文件
// (纯 ImDrawList 手绘,零额外依赖)里没有现成基础设施,留给后续任务按需补。
static std::string basename(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

void AICardPanel::drawPhotoCard(const AICard& c)
{
    ImGui::TextWrapped("%s", basename(c.path).c_str());
    if (c.path.empty())
    {
        ImGui::TextDisabled(u8"（无文件）");
        return;
    }
    if (ImGui::Button(u8"打开"))
    {
        // macOS `open`;路径用单引号包住防止空格/特殊字符断开命令(与 spec 约定一致)。
        std::string cmd = "open '" + c.path + "'";
        int rc = system(cmd.c_str());
        if (rc != 0) OSG_WARN << "[AIChat] open photo failed, rc=" << rc << " path=" << c.path << std::endl;
    }
}

// ---- 进行中任务卡(Task 8):转轮 + 进度条,实时从 JobManager 读(不缓存快照,避免过期)。
// DONE/FAILED 由 MediaManager::update() 主动 removeJob() 摘掉,这里画的始终是"进行中"态,
// 因此拿不到 job(理论上不会发生,防御一下)就显示占位文案而不是崩溃。
void AICardPanel::drawJobCard(const AICard& c)
{
    earthai::AIJob job;
    if (!c.jobs || !c.jobs->get(c.jobId, job))
    {
        ImGui::TextDisabled(u8"（任务信息不可用）");
        return;
    }
    static const char spin[4] = { '|', '/', '-', '\\' };
    int idx = (int)(ImGui::GetTime() * 8.0) % 4;
    ImGui::Text(u8"生成中 %c", spin[idx]);
    ImGui::ProgressBar(job.progress, ImVec2(-1.0f, 0.0f));
}
