#ifndef LOCAL_SHADOW_FILTER_HLSLI
#define LOCAL_SHADOW_FILTER_HLSLI

// Local-light shadow plan S2 (docs/local_light_shadows_plan.md): UE's filters for the spot / point atlases, ONE copy
// for every pass that samples them (spotlight_cs, pointlight_cs, glass). Include local_shadow_bias.hlsli first.

// --- Plan S4: a light's square in its atlas slice ---------------------------------------------------------------
// Every light draws into the top-left res x res of its slice, res = UE's resolution for it (LightManager); `rect` =
// res / slice. Taps are clamped `margin` slice texels inside that square, so no filter reads what another frame's
// larger square left beyond it.
float2 LocalShadowRectUV(float2 uv, float rect, float sliceTexel, float margin)
{
    return clamp(uv * rect, margin * sliceTexel, rect - margin * sliceTexel);
}

// A cube direction -> (uv, slice) of the point atlas, which is a 2D array of 6 faces per light since S4 (a hardware
// cube lookup always reads the whole face). D3D's cube convention -- the one the faces were rendered for: +X, -X, +Y,
// -Y, +Z, -Z, face uv from (-z,-y) (+z,-y) (x,z) (x,-z) (x,-y) (-x,-y) over the major axis. Half a texel inside the
// square, so each tap's 2x2 compare footprint stays on its own face (UE's hardware cube filters across the seam).
// The face `v` lands on as a basis: st = (v.U, v.V) / v.W.
void PointShadowFace(float3 v, out float face, out float3 U, out float3 V, out float3 W)
{
    const float3 a = abs(v);
    if (a.x >= a.y && a.x >= a.z)
    {
        const float s = v.x >= 0.0f ? 1.0f : -1.0f;
        face = v.x >= 0.0f ? 0.0f : 1.0f; W = float3(s, 0, 0); U = float3(0, 0, -s); V = float3(0, -1, 0);
    }
    else if (a.y >= a.z)
    {
        const float s = v.y >= 0.0f ? 1.0f : -1.0f;
        face = v.y >= 0.0f ? 2.0f : 3.0f; W = float3(0, s, 0); U = float3(1, 0, 0); V = float3(0, 0, s);
    }
    else
    {
        const float s = v.z >= 0.0f ? 1.0f : -1.0f;
        face = v.z >= 0.0f ? 4.0f : 5.0f; W = float3(0, 0, s); U = float3(s, 0, 0); V = float3(0, -1, 0);
    }
}

float3 PointShadowCubeUV(float3 v, float slot, float rect, float sliceTexel)
{
    float face;
    float3 U, V, W;
    PointShadowFace(v, face, U, V, W);
    const float2 uv = float2(dot(v, U), dot(v, V)) / max(dot(v, W), 1e-9f) * 0.5f + 0.5f;
    return float3(LocalShadowRectUV(uv, rect, sliceTexel, 0.5f), slot * 6.0f + face);
}

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
// local_shadow_bias.hlsli (UE mode: shadowParams.y < 0). S4: `invRes` = 1 / the light's own face resolution, `rect` /
// `sliceTexel` its square (PointShadowCubeUV).
float PointShadowDisc29(Texture2DArray atlas, SamplerComparisonState cmp, float3 d, float m, float4 shadowParams,
                        float slopeScale, float NoL, float invRes, float rect, float sliceTexel)
{
    const float nearP = shadowParams.z;
    const float farP = max(shadowParams.w, nearP + 1e-3f);
    const float rel = -shadowParams.y * (1.0f + slopeScale * (1.0f - saturate(NoL)));
    const float3 ad = abs(d);
    const float3 seed = (ad.x < ad.y && ad.x < ad.z) ? float3(1, 0, 0) : (ad.y < ad.z) ? float3(0, 1, 0) : float3(0, 0, 1);
    const float3 dn = normalize(d);
    const float3 side = normalize(cross(seed, dn)) * invRes;
    const float3 up = cross(dn, side);
    // S4: the hardware cube picked each tap's face for free; here the disc's centre face is found ONCE and every tap is
    // linear in its disc offset on it -- (st, w) = c + sx * k.x + sy * k.y. A tap past that face's seam (the disc is
    // ~1.25 texels) reads the face's edge texel instead of the neighbour face -- which keeps it in the metric zc is in
    // (the centre face's depth axis; the hardware cube compared it against the neighbour's other axis). Measured on
    // wind_test's lamps, Pass_PointLights: a per-tap face lookup 0.114 ms, a fallback branch for the seam taps 0.119,
    // this 0.074 -- the hardware cube's was 0.078 (plan S2); 0.035-0.039 without the map.
    float face;
    float3 U, V, W;
    PointShadowFace(dn, face, U, V, W);
    const float3 c = float3(dot(dn, U), dot(dn, V), dot(dn, W));
    const float3 sx = float3(dot(side, U), dot(side, V), dot(side, W));
    const float3 sy = float3(dot(up, U), dot(up, V), dot(up, W));
    const float faceSlice = shadowParams.x * 6.0f + face;
    float shadow = 0.0f;
    [unroll]
    for (int i = 0; i < 29; ++i)
    {
        const float mBiased = max(m / (1.0f + rel * length(kDiscSamples29[i])), nearP);
        const float zc = (farP / (farP - nearP)) * (1.0f - nearP / mBiased);
        const float3 t = c + sx * kDiscSamples29[i].x + sy * kDiscSamples29[i].y;
        const float2 st = t.xy / t.z;
        const float3 tap = float3(LocalShadowRectUV(st * 0.5f + 0.5f, rect, sliceTexel, 0.5f), faceSlice);
        shadow += atlas.SampleCmpLevelZero(cmp, tap, zc);
    }
    return shadow * (1.0f / 29.0f);
}

#endif // LOCAL_SHADOW_FILTER_HLSLI
