uniform sampler2D SceneSampler, MaskSampler, ExtraLayerSampler, Overlay2Sampler;
uniform sampler2D TransmittanceSampler;
uniform sampler2D SkyIrradianceSampler;
uniform sampler3D InscatterSampler;
uniform sampler2D GlareSampler;
uniform vec4 UvOffset1, UvOffset2, UvOffset3, UvOffset4;
uniform vec3 WorldCameraPos, WorldSunDir, EarthOrigin;
uniform float HdrExposure, GlobalOpaque, LabelOpacity, Overlay2Opacity;
uniform float ClarityAltLo, ClarityAltHi;  // 地表清晰度高度 band(消补丁):低空→清晰影像,高空→太空大气

VERSE_FS_IN vec3 vertexInWorld, normalInWorld;
VERSE_FS_IN vec4 texCoord;
VERSE_FS_IN float isSkirt;

#ifdef VERSE_GLES3
layout(location = 0) VERSE_FS_OUT vec4 fragColor;
layout(location = 1) VERSE_FS_OUT vec4 fragOrigin;
#endif

#define SUN_INTENSITY 100.0
#define PLANET_RADIUS 6360000.0
#include "scattering.module.glsl"

vec3 hdr(vec3 L)
{
    L = L * HdrExposure;
    L.r = L.r < 1.413 ? pow(L.r * 0.38317, 1.0 / 2.2) : 1.0 - exp(-L.r);
    L.g = L.g < 1.413 ? pow(L.g * 0.38317, 1.0 / 2.2) : 1.0 - exp(-L.g);
    L.b = L.b < 1.413 ? pow(L.b * 0.38317, 1.0 / 2.2) : 1.0 - exp(-L.b);
    // 轻度提升饱和与对比，改善大气下偏灰的观感
    float luma = dot(L, vec3(0.299, 0.587, 0.114));
    L = mix(vec3(luma), L, 1.08);                 // +8% 饱和（原 1.18 把晨昏线橙带搞得过饱和刺眼，调淡）
    L = clamp((L - 0.5) * 1.06 + 0.5, 0.0, 1.0);  // +6% 对比
    return L;
}

//uniform vec4 clipPlane0, clipPlane1, clipPlane2;

void main()
{
    vec4 worldPos = vec4(vertexInWorld, 1.0);
    /*float clipD0 = dot(worldPos, clipPlane0), clipD1 = dot(worldPos, clipPlane1), clipD2 = dot(worldPos, clipPlane2);
    if (clipD0 > 0.0 && clipD1 > 0.0 && clipD2 > 0.0)
    {
#ifdef VERSE_GLES3
        fragColor = vec4(0.0);
        fragOrigin = vec4(0.0, 0.0, 1.0, 1.0);
#else
        gl_FragData[0] = vec4(0.0);
        gl_FragData[1] = vec4(0.0, 0.0, 1.0, 1.0);
#endif
        return;
    }*/

    vec4 groundColor = VERSE_TEX2D(SceneSampler, texCoord.st * UvOffset1.zw + UvOffset1.xy);
    vec4 layerColor = VERSE_TEX2D(ExtraLayerSampler, texCoord.st * UvOffset3.zw + UvOffset3.xy);
    groundColor.rgb = mix(groundColor.rgb, layerColor.rgb, layerColor.a * clamp(LabelOpacity, 0.0, 1.0));
    vec4 overlay2Color = VERSE_TEX2D(Overlay2Sampler, texCoord.st * UvOffset4.zw + UvOffset4.xy);
    groundColor.rgb = mix(groundColor.rgb, overlay2Color.rgb, overlay2Color.a * clamp(Overlay2Opacity, 0.0, 1.0));
    if (isSkirt < -0.1 && GlobalOpaque < 0.9) discard;  // hide skirt if transparent

    // Mask color: r = aspect, g = slope, b = mask (0 - 0.5: ocean, 0.5 - 1: land)
    vec2 uv = texCoord.xy * UvOffset2.zw + UvOffset2.xy;
    vec4 maskColor = VERSE_TEX2D(MaskSampler, uv.st);
    float off = 0.002, maskValue = maskColor.z;
    maskColor += VERSE_TEX2D(MaskSampler, uv.st + vec2(-off, 0.0));
    maskColor += VERSE_TEX2D(MaskSampler, uv.st + vec2(off, 0.0));
    maskColor += VERSE_TEX2D(MaskSampler, uv.st + vec2(0.0, -off));
    maskColor += VERSE_TEX2D(MaskSampler, uv.st + vec2(0.0, off));
    maskColor += VERSE_TEX2D(MaskSampler, uv.st + vec2(-off, -off));
    maskColor += VERSE_TEX2D(MaskSampler, uv.st + vec2(off, -off));
    maskColor += VERSE_TEX2D(MaskSampler, uv.st + vec2(off, off));
    maskColor += VERSE_TEX2D(MaskSampler, uv.st + vec2(-off, off));
    maskColor *= 1.0 / 9.0;

    vec3 WSD = WorldSunDir, WCP = WorldCameraPos;
    vec3 P = vertexInWorld, N = normalize(P);
    P = N * (length(P) * 0.99);  // FIXME

    // 平滑球面法线昼夜:不再用 z3 粗 OCEAN_MASK 派生法线扰动(那是逐瓦片块状阴影的根源;
    // mask 仅保留用于上方 maskValue 输出)。真实地形起伏由高程网格几何体现,不靠此着色。
    vec3 originalGroundColor = groundColor.rgb;
    float cTheta = max(dot(N, WSD), 0.0); vec3 sunL, skyE;
    sunRadianceAndSkyIrradiance(P, N, WSD, sunL, skyE);
    groundColor.rgb *= max((sunL * cTheta + skyE) / 3.14159265, vec3(0.1));
    groundColor.a *= clamp(GlobalOpaque, 0.0, 1.0);

    vec3 extinction = vec3(1.0);
    // 晨昏线处掠射太阳的 inscatter 是橙红落日色;调淡只柔化那条带、不影响白天侧原始影像。
    vec3 inscatter = inScattering(WCP, P, WSD, extinction, 0.0) * 0.5;
    vec3 compositeColor = groundColor.rgb * extinction + inscatter;

    // 高空/太空:现有观感(与原 finalColor 等价;法线改平滑后高空逐瓦片差异本就极小)。
    vec3 spaceColor = mix(hdr(compositeColor), originalGroundColor, cTheta);
    // 低空:清晰原始影像 × 平滑昼夜(白天≈1、夜≈0.06、晨昏平滑),无雾、无 HDR 重调色 → 消补丁。
    float dayNight = mix(0.06, 1.0, smoothstep(-0.05, 0.25, dot(N, WSD)));
    vec3 clearColor = originalGroundColor * dayNight;
    // 按相机高度混合:alt < AltLo → clearColor(清晰),alt > AltHi → spaceColor(太空)。
    float camAlt = length(WCP) - PLANET_RADIUS;
    float groundClarity = smoothstep(ClarityAltHi, ClarityAltLo, camAlt);
    vec4 finalColor = vec4(mix(spaceColor, clearColor, groundClarity), groundColor.a);

#ifdef VERSE_GLES3
    fragColor/*Atmospheric Color*/ = finalColor;
    fragOrigin/*Mask Color*/ = vec4(1.0 - maskValue);  // output: land => 0, ocean => 1
#else
    gl_FragData[0]/*Atmospheric Color*/ = finalColor;
    gl_FragData[1]/*Mask Color*/ = vec4(1.0 - maskValue);  // output: land => 0, ocean => 1
#endif
}
