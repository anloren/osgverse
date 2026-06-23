# 消除"补丁状瓦片"——随高度退场的地表着色(设计)

> 日期 2026-06-23。修复用户报告的"全球操作时地球花得像补丁、各瓦片阴影程度不同"。**根因经决定性诊断确认 = 渲染着色,非 Google 源影像。** 只动 `assets/shaders/scattering_globe.frag.glsl`(+少量 C++ 设 uniform);属 4 类回归雷区,每步必验。

## 根因(已诊断,三方同条件对比 `--goto 35 105 2000` 默认太阳)

| 实验 | 结果 |
|---|---|
| 纯原始影像(去掉所有着色/HDR/散射) | 干净、自然、均匀——补丁消失 |
| 完整着色(现状) | 发灰发雾 + 块状亮暗补丁 |
| `terrainDetails=0`(关 z3 mask 法线扰动) | 仍发雾,但块状明显减轻 |

两个叠加因素:
1. **大气散射 + HDR 调色**把影像整体洗灰发雾(中低空尤甚),且随各瓦片 `cTheta`(太阳入射)不同而 haze 程度不同 → 块状。
2. **地形法线来自 z3 极粗的 OCEAN_MASK**(深瓦片继承父级一小块)→ 逐瓦片太阳着色块状跳变。
现有 `mix(hdr(着色), 原始影像, cTheta)` 用块状的 `cTheta` 决定混合比 → 混合也块状。

**洞察**:高空(太空看地球)着色/大气是对的、好看;问题在**中低空**——本该看清晰影像却被"太空大气"着色洗成补丁。现有自适应曝光(随高度调 `HdrExposure`)已是这思路,但着色/雾本身没随高度退场。

## 设计:最终像素 = 两种着色按相机高度混合

```glsl
// 平滑球面法线(不再用 z3 粗 mask 法线扰动 → cTheta 平滑、无块状)
N = normalize(P);
float cThetaSmooth = max(dot(N, WSD), 0.0);

clearColor = originalGroundColor * dayNight(cThetaSmooth);  // 低空:清晰影像+柔和昼夜,无雾、无块、无 HDR 重调色
spaceColor = hdr(compositeColor);                            // 高空:现有大气观感(原样保留)
finalColor.rgb = mix(spaceColor, clearColor, GroundClarity); // 按高度混合
```

- **GroundClarity**:由相机高度算的 smoothstep。相机高度 `= length(WorldCameraPos) - PLANET_RADIUS`(`WorldCameraPos` 已是 per-frame uniform)。`GroundClarity = smoothstep(AltHi, AltLo, alt)`(alt<AltLo→1 清晰、alt>AltHi→0 太空)。`AltLo/AltHi` 为 uniform,C++ 从 env `EARTH_CLARITY_ALTLO/ALTHI`(km)读、默认约 AltLo=300km、AltHi=4000km(实现时按截图微调)。
- **dayNight(cThetaSmooth)**:平滑昼夜系数(白天≈1、夜侧压暗到一个低值如 0.06、晨昏线平滑过渡),保住晨昏线但不发雾、不块状。
- **方向①**(随高度变清晰):GroundClarity 直接实现。**方向②**(消块状):`N=normalize(P)`、clearColor 用平滑 cThetaSmooth。**方向③**(调淡雾/HDR):clearColor 完全不含散射雾与 +饱和/+对比;spaceColor(高空)不动。

## 保住 4 类回归(硬约束)

- **晨昏线**:clearColor 的 dayNight + spaceColor 都随太阳 → 各高度都有平滑昼夜分界。低空不再块状但仍有昼夜。
- **海洋不橙**:clearColor 走原始影像(海洋瓦片本就蓝),低空不经那条橙色 inscatter;高空 spaceColor 保持现状(现有已无橙)。
- **倾斜穿模 / 海洋开关**:完全不碰几何 / OceanOpaque / 太阳方向 / 相机地形地板。
- spaceColor 分支与现有 `hdr(compositeColor)` 逐字保持(高空观感零变化),只在"低空混合"这一层叠加。

## 涉及改动

- `assets/shaders/scattering_globe.frag.glsl`:加 `GroundClarity`(及 `dayNight`)逻辑、`N=normalize(P)`、clearColor/spaceColor 混合。**保留** skirt discard、mask 输出(`fragOrigin`)、overlay/label 合成、`HdrExposure`/`GlobalOpaque` 等现有行为。
- C++(`EarthAtmosphereOcean` 或 `EnvironmentHandler`):新增 `GroundClarity` 或 `ClarityAltLo/Hi` uniform,从 env 读阈值并 per-frame/启动设值(仿现有 `EARTH_EXP_*` / `HdrExposure` 套路)。**不碰太阳/海洋/几何。**

## 验证计划(每步 headless 截图 + 真机)

- **补丁消失**:`--goto 35 105 2000`(原补丁处)低空 → 影像清晰、无块状补丁(对比诊断的 rawonly)。
- **多高度过渡**:低空(清晰)、中空、高空全球(太空观感与现状一致)各一张,过渡自然不跳变。
- **晨昏线**:默认太阳全球 + 低空晨昏侧 → 昼夜平滑分界、海洋不橙、无刺眼橙带。
- **倾斜不穿模**:低空 `EARTH_TILT` 视角。
- env `EARTH_CLARITY_ALTLO/ALTHI` 可调 band 复验。
- 4 类回归全部不复现方可提交/推送(用户最终真机判断观感)。

## 明确不做(YAGNI)

源影像色彩归一化(诊断已排除源影像是主因);从真实高程重算法线(去掉粗法线即可消块,真实起伏由几何体现);运行时切色板。
