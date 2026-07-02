#ifndef EARTH_AI_MOTION_H
#define EARTH_AI_MOTION_H
// Task 9(首尾帧巡航视频):A/B 两点位姿 → Veo 运动提示词。纯函数、header-only,
// 只依赖 <osg/Vec3d> + <string> + <cmath>,不拉 osgViewer 等重依赖——这样
// tests/ai_chat_tests.cpp 可以直接 include 本头文件做单测,不必把整个 ai_media.cpp
// (它 include 了 osgViewer/libhv 等)拖进测试翻译单元。ai_media.cpp 也 include 本头文件,
// 两边共用同一份实现,不重复。
#include <osg/Vec3d>
#include <cmath>
#include <cstdio>
#include <string>

namespace earthai
{
    // 由 A/B 两点位姿生成 Veo 运动提示词(纯函数,单测覆盖)。lla=(纬度弧度,经度弧度,高度米),
    // 与 EarthManipulator::computeEyeLatLonHeight() 的返回值约定一致。
    inline std::string buildMotionPrompt(const osg::Vec3d& llaA, const osg::Vec3d& llaB)
    {
        // review:kDeg2Rad 未被使用——输入 llaA/llaB 的纬经度本来就是弧度(与
        // EarthManipulator::computeEyeLatLonHeight() 的返回值约定一致),函数体内所有三角
        // 运算直接吃弧度,只有输出的 bearingDeg 需要"弧度转角度"这一个方向,故只留 kRad2Deg。
        const double kRad2Deg = 57.29577951308232;
        const double kEarthRadiusM = 6371000.0;

        double latA = llaA[0], lonA = llaA[1], altA = llaA[2];
        double latB = llaB[0], lonB = llaB[1], altB = llaB[2];

        // 大圆方位角(bearing,正北=0°,顺时针):atan2(y,x)
        double dLon = lonB - lonA;
        double y = std::sin(dLon) * std::cos(latB);
        double x = std::cos(latA) * std::sin(latB) - std::sin(latA) * std::cos(latB) * std::cos(dLon);
        double bearingDeg = std::atan2(y, x) * kRad2Deg;
        if (bearingDeg < 0.0) bearingDeg += 360.0;

        // haversine 大圆距离(米→公里)
        double dPhi = latB - latA;
        double dLambda = lonB - lonA;
        double a = std::sin(dPhi * 0.5) * std::sin(dPhi * 0.5) +
                   std::cos(latA) * std::cos(latB) * std::sin(dLambda * 0.5) * std::sin(dLambda * 0.5);
        double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
        double distKm = (kEarthRadiusM * c) / 1000.0;

        // 8 方位罗盘(每 45° 一档,以 0°=north 为中心,北方跨 337.5°~22.5° 需要 wrap)。
        static const char* kCompass[8] = {
            "north", "northeast", "east", "southeast",
            "south", "southwest", "west", "northwest"
        };
        int octant = (int)std::floor((bearingDeg + 22.5) / 45.0) % 8;
        if (octant < 0) octant += 8;
        std::string direction = kCompass[octant];

        // 高度变化描述:两点几乎等高(<1m 差)时视作持平,避免浮点噪声制造出假的升降描述。
        double altDiff = altB - altA;
        char altBuf[128];
        if (std::fabs(altDiff) < 1.0)
            snprintf(altBuf, sizeof(altBuf), "holding altitude at approximately %.0fm", altA);
        else if (altDiff < 0.0)
            snprintf(altBuf, sizeof(altBuf), "descending from %.0fm to %.0fm", altA, altB);
        else
            snprintf(altBuf, sizeof(altBuf), "ascending from %.0fm to %.0fm", altA, altB);

        char distBuf[64];
        snprintf(distBuf, sizeof(distBuf), "%.1f", distKm);

        // 英文提示词(Veo 对英文理解最好,见任务约定):方位 + 距离 + 高度变化 +
        // 平滑运镜措辞 + 写实航拍关键词。
        std::string prompt = "Aerial drone camera gliding smoothly toward the ";
        prompt += direction;
        prompt += ", covering approximately ";
        prompt += distBuf;
        prompt += "km, ";
        prompt += altBuf;
        prompt += ". Smooth cinematic camera motion, steady continuous glide, "
                  "no jitter, natural motion blur. Photorealistic aerial footage, "
                  "stable smooth camera path.";
        return prompt;
    }
}
#endif
