#ifndef LOCAL_SHADOW_FILTER_HLSLI
#define LOCAL_SHADOW_FILTER_HLSLI

// Local-light shadow plan S2 (docs/local_light_shadows_plan.md): UE's filters for the spot / point atlases, ONE copy
// for every pass that samples them (spotlight_cs, pointlight_cs, glass). Include local_shadow_bias.hlsli first.

// UE ShadowProjectionPixelShader.usf:394-409 / :513-518: sharpen FIRST (ShadowSharpen: (s - 0.5) * (7 * sharpen + 1)
// + 0.5), then the PCF over-blur correction (ApplyPCFOverBlurCorrection = Square). `sharpen` arrives as 7 * s + 1.
float LocalShadowFinish(float shadow, float sharpen)
{
    shadow = saturate((shadow - 0.5f) * sharpen + 0.5f);
    return shadow * shadow;
}

// --- Spot: UE Manual5x5PCF (ShadowFilteringCommon.ush:264-365), a 6x6 tent from nine Gathers --------------------
// with the soft occlusion ramp of CalculateOcclusion (:151-170): occlusion falls off over 1/transitionScale behind the
// stored depth instead of a hard compare. UE ramps on a LINEAR depth; our atlas holds a perspective NDC depth, so every
// gathered texel is linearised (w = n f / (f - z (f - n))) and the ramp is in metres.
float4 SpotOcclusion4(float4 storedNdc, float receiverW, float transitionScale, float n, float f)
{
    const float4 w = (n * f) / max(f - storedNdc * (f - n), 1e-6f);
    return saturate((w - receiverW) * transitionScale + 1.0f);
}

// UE HorizontalPCF5x2: three horizontally adjacent quads folded into the two rows they cover (outer columns 1-frac /
// frac, the four inner ones 1 -> 5 per row). Gather order: .xyzw = (-,+), (+,+), (+,-), (-,-).
float2 LocalHorizontal5x2(float2 frac2, float4 v0, float4 v2, float4 v4)
{
    float r0 = v0.w * (1.0f - frac2.x);
    float r1 = v0.x * (1.0f - frac2.x);
    r0 += v0.z;            r1 += v0.y;
    r0 += v2.w;            r1 += v2.x;
    r0 += v2.z;            r1 += v2.y;
    r0 += v4.w;            r1 += v4.x;
    r0 += v4.z * frac2.x;  r1 += v4.y * frac2.x;
    return float2(r0, r1);
}

float SpotShadowTent6x6(Texture2DArray atlas, SamplerState pointClamp, float2 uv, float slice, float receiverW,
                        float transitionScale, float n, float f, float2 atlasSize, float2 texel)
{
    const float2 texelPos = uv * atlasSize - 0.5f;
    const float2 frac2 = frac(texelPos);
    const float2 quadUV = (floor(texelPos) + 1.0f) * texel;
    #define SPOT_G6(ox, oy) SpotOcclusion4(atlas.Gather(pointClamp, float3(quadUV + float2((ox) * texel.x, (oy) * texel.y), slice)), receiverW, transitionScale, n, f)
    const float2 row0 = LocalHorizontal5x2(frac2, SPOT_G6(-2, -2), SPOT_G6(0, -2), SPOT_G6(2, -2));
    float results = row0.x * (1.0f - frac2.y) + row0.y;
    const float2 row1 = LocalHorizontal5x2(frac2, SPOT_G6(-2, 0), SPOT_G6(0, 0), SPOT_G6(2, 0));
    results += row1.x + row1.y;
    const float2 row2 = LocalHorizontal5x2(frac2, SPOT_G6(-2, 2), SPOT_G6(0, 2), SPOT_G6(2, 2));
    results += row2.x + row2.y * frac2.y;
    #undef SPOT_G6
    return saturate(0.04f * results); // 1/25
}

// --- Point: UE CubemapHardwarePCF at SHADOW_QUALITY 5 (ShadowProjectionCommon.ush:151-254) ----------------------
// 29 hardware-compare taps on a disc of radius 2.5 / Res around the light vector (about 1.25 texels), each compared
// with the bias times its distance from the centre -- UE's `CompareDistance - ShadowDepthBias * length(DiscSamples29[i])`.
static const float2 kDiscSamples29[29] = { // ShadowProjectionCommon.ush:89-120, "29 random points in disc with radius 2.5"
    float2(0.000000, 2.500000),   float2(1.016842, 2.283864),   float2(1.857862, 1.672826),   float2(2.377641, 0.772542),
    float2(2.486305, -0.261321),  float2(2.165063, -1.250000),  float2(1.469463, -2.022543),  float2(0.519779, -2.445369),
    float2(-0.519779, -2.445369), float2(-1.469463, -2.022542), float2(-2.165064, -1.250000), float2(-2.486305, -0.261321),
    float2(-2.377641, 0.772543),  float2(-1.857862, 1.672827),  float2(-1.016841, 2.283864),  float2(0.091021, -0.642186),
    float2(0.698035, 0.100940),   float2(0.959731, -1.169393),  float2(-1.053880, 1.180380),  float2(-1.479156, -0.606937),
    float2(-0.839488, -1.320002), float2(1.438566, 0.705359),   float2(0.067064, -1.605197),  float2(0.728706, 1.344722),
    float2(1.521424, -0.380184),  float2(-0.199515, 1.590091),  float2(-1.524323, 0.364010),  float2(-0.692694, -0.086749),
    float2(-0.082476, 0.654088)
};

// `d` = receiver - light (unbiased: UE has no normal offset), `m` its major-axis distance. shadowParams / slopeScale as in
// local_shadow_bias.hlsli (UE mode: shadowParams.y < 0).
float PointShadowDisc29(TextureCubeArray cube, SamplerComparisonState cmp, float3 d, float m, float4 shadowParams,
                        float slopeScale, float NoL, float invRes)
{
    const float nearP = shadowParams.z;
    const float farP = max(shadowParams.w, nearP + 1e-3f);
    const float rel = -shadowParams.y * (1.0f + slopeScale * (1.0f - saturate(NoL)));
    const float3 ad = abs(d);
    const float3 seed = (ad.x < ad.y && ad.x < ad.z) ? float3(1, 0, 0) : (ad.y < ad.z) ? float3(0, 1, 0) : float3(0, 0, 1);
    const float3 dn = normalize(d);
    const float3 side = normalize(cross(seed, dn)) * invRes;
    const float3 up = cross(dn, side);
    float shadow = 0.0f;
    [unroll]
    for (int i = 0; i < 29; ++i)
    {
        const float mBiased = max(m / (1.0f + rel * length(kDiscSamples29[i])), nearP);
        const float zc = (farP / (farP - nearP)) * (1.0f - nearP / mBiased);
        shadow += cube.SampleCmpLevelZero(cmp, float4(dn + side * kDiscSamples29[i].x + up * kDiscSamples29[i].y, shadowParams.x), zc);
    }
    return shadow * (1.0f / 29.0f);
}

#endif // LOCAL_SHADOW_FILTER_HLSLI
