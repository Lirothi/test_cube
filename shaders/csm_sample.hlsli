// Legacy CSM cascade sampling, shared by lighting_cs.hlsl and glass.hlsl.
//
// S3: this used to exist as TWO copies that had already drifted apart — glass.hlsl had neither the
// coarser-cascade fallback nor the blend band, so glass shaded against a visibly different shadow
// than the geometry next to it. The VSM path was factored out the same way (vsm_sample.hlsli), so
// this is the symmetric arrangement, not a new pattern.
//
// EVERYTHING arrives as an argument: the two callers have DIFFERENT cbuffer layouts and different
// field names for the same quantities, so this header must not touch a single global. That is also
// what lets it be included from a compute shader and a pixel shader at once.
#ifndef CSM_SAMPLE_HLSLI
#define CSM_SAMPLE_HLSLI

struct CsmParams
{
    float4x4 lightViewProj[4];
    float4   scaleBias[4];    // xy = atlas scale, zw = atlas bias
    float4   splitsVS;        // x = near (unused), yzw = far of cascades 0..2
    float4   depthBiasNDC;
    // World size of one texel, per cascade. Only the legacy 3x3 arm's per-cascade radius ratio
    // still reads it -- the receiver normal offset it used to carry is gone (UE has no such term).
    float4   cascadeTexelWS;
    float2   atlasSize;       // (W, H) in texels
    float3   camPosWS;
    float3   camDirWS;        // normalized
    float    pcfRadius;       // filter radius in texels (1.0 today)
    // S8: 1 / (width of the depth transition zone, in NDC) per cascade. The ramp replaces the
    // binary depth compare, and its width is proportional to the cascade's WORLD TEXEL -- which is
    // the whole point: UE's TransitionSize is CSMDepthBias/zRange * (radius/resolution), so a far
    // cascade softens by exactly the factor it loses resolution by. That proportionality is what
    // makes their cascade boundaries read as "blur with distance" instead of a resolution cliff.
    float4   transitionScale;
    // S8: multiplier applied to transitionScale at NoL == 0 (UE: r.Shadow.CSMReceiverBias = 0.9).
    // Widens the ramp at grazing incidence, where self-shadowing is worst.
    float    receiverBiasMin;
    float    sharpen;         // S8: already mapped to UE's shader units (artist*7+1); 1 = off
    float    overBlurCorrect; // S8: 1 = apply UE's Square(shadow), 0 = off
    // Receiver normal offset in TEXELS; the world amount is this times cascadeTexelWS[c].
    float    normalBiasTexels;
    // S10. `farSplit` is splitsVS[4] -- the last cascade's far plane, which splitsVS.xyzw cannot
    // carry (it holds splits 0..3). Both fractions are of a cascade's OWN slice length.
    float    farSplit;
    float    blendFraction;
    float    distanceFadeFraction;
    // S8 filter mode. 0 = legacy 3x3 SampleCmp box (A/B + emergency fallback),
    // 1 = 4x4 tent / 4 gathers  (UE Manual3x3PCF, their SHADOW_QUALITY 3),
    // 2 = 6x6 tent / 9 gathers  (UE Manual5x5PCF, their SHADOW_QUALITY 4-5 -- and r.ShadowQuality
    //     DEFAULTS TO 5, so this is what a stock Unreal actually runs).
    uint     useGatherPcf;
};

// (S10 moved the cross-fade fraction into CsmParams -- it is UE's CascadeTransitionFraction, an
// authored property, not a shader constant.)

// 4 = the sample fell past cascade 3, i.e. there is no shadow data for this pixel at all. Kept as a
// named constant because the debug tint (S0.3) and the fallback chain must agree on it.
static const int kCsmNoCascade = 4;

// --- S8: soft occlusion + Gather4 tent PCF ---------------------------------------------------
//
// Transcribed from UE (verified line by line against the drop, 2026-08-31):
//   CalculateOcclusion  -> ShadowFilteringCommon.ush:151
//   PCF3x3gather        -> ShadowFilteringCommon.ush:97   (16 terms, matched exactly)
//   Manual3x3PCF        -> ShadowFilteringCommon.ush:246  (gather placement)
//   Square / sharpen    -> ShadowProjectionPixelShader.usf:89, :375
//
// The ramp: instead of `stored < receiver ? 0 : 1`, occlusion falls off linearly over a depth band
// of width 1/transitionScale. UE's exact rearrangement is kept -- the per-pixel constant is hoisted
// out of the per-sample math.
float4 CsmOcclusion4(float4 storedDepth, float receiverDepth, float transitionScale)
{
    const float constantFactor = receiverDepth * transitionScale - 1.0f;
    return saturate(storedDepth * transitionScale - constantFactor);
}

// 4x4 tent from four Gather() quads. Gather returns the bilinear 2x2 in the order
// (-,+),(+,+),(+,-),(-,-), i.e. .xyzw = left-bottom, right-bottom, right-top, left-top.
// Weights: the outer columns/rows get (1-frac) and frac, the inner two get 1; total 3x3 = 9.
float CsmTent4x4Gather(Texture2D atlas, SamplerState smp, float2 uv, float receiverDepth,
                       float transitionScale, float2 atlasSize, float2 texel, float4 uvClamp)
{
    const float2 texelPos = uv * atlasSize - 0.5f;
    const float2 frac2 = frac(texelPos);
    // Gather samples half a texel around the point, so aim at the 2x2 quad's corner.
    const float2 quadUV = (floor(texelPos) + 1.0f) * texel;

    // Offsets are added to the UV rather than passed as Gather's int2 offsets (UE uses int2). Adding
    // them lets each quad be clamped to the cascade's content rect, which S5's gutter needs; with
    // int2 offsets the clamp is bypassed in hardware.
    const float2 o = texel;
    const float4 v00 = CsmOcclusion4(atlas.Gather(smp, clamp(quadUV + float2(-o.x, -o.y), uvClamp.xy, uvClamp.zw)), receiverDepth, transitionScale);
    const float4 v10 = CsmOcclusion4(atlas.Gather(smp, clamp(quadUV + float2( o.x, -o.y), uvClamp.xy, uvClamp.zw)), receiverDepth, transitionScale);
    const float4 v01 = CsmOcclusion4(atlas.Gather(smp, clamp(quadUV + float2(-o.x,  o.y), uvClamp.xy, uvClamp.zw)), receiverDepth, transitionScale);
    const float4 v11 = CsmOcclusion4(atlas.Gather(smp, clamp(quadUV + float2( o.x,  o.y), uvClamp.xy, uvClamp.zw)), receiverDepth, transitionScale);

    float4 rows;
    rows.x = v00.w * (1.0f - frac2.x) + v00.z + v10.w + v10.z * frac2.x;
    rows.y = v00.x * (1.0f - frac2.x) + v00.y + v10.x + v10.y * frac2.x;
    rows.z = v01.w * (1.0f - frac2.x) + v01.z + v11.w + v11.z * frac2.x;
    rows.w = v01.x * (1.0f - frac2.x) + v01.y + v11.x + v11.y * frac2.x;

    return saturate(dot(rows, float4(1.0f - frac2.y, 1.0f, 1.0f, frac2.y)) * (1.0f / 9.0f));
}

// UE HorizontalPCF5x2: folds three horizontally adjacent gather quads into the two rows they cover.
// Outer columns weigh (1-frac.x) and frac.x, the four inner ones weigh 1 -> 5 per row.
float2 CsmHorizontal5x2(float2 frac2, float4 v0, float4 v2, float4 v4)
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

// 6x6 tent from NINE gather quads -- UE's Manual5x5PCF, which is what `ManualPCF` selects at
// SHADOW_QUALITY 4 and 5, and `r.ShadowQuality` defaults to 5. Normalisation is 1/25: five weighted
// columns by five weighted rows. This is 2.25x the linear reach of the 4x4 variant, and that width
// is the reason a stock Unreal reads softer than a 4x4 tent at the same resolution.
float CsmTent6x6Gather(Texture2D atlas, SamplerState smp, float2 uv, float receiverDepth,
                       float transitionScale, float2 atlasSize, float2 texel, float4 uvClamp)
{
    const float2 texelPos = uv * atlasSize - 0.5f;
    const float2 frac2 = frac(texelPos);
    const float2 quadUV = (floor(texelPos) + 1.0f) * texel;

    // UE uses Gather's int2 offsets; we add to the UV so every quad stays clampable to the tile.
    #define CSM_G6(ox, oy) CsmOcclusion4(atlas.Gather(smp,         clamp(quadUV + float2((ox) * texel.x, (oy) * texel.y), uvClamp.xy, uvClamp.zw)),         receiverDepth, transitionScale)

    const float2 row0 = CsmHorizontal5x2(frac2, CSM_G6(-2, -2), CSM_G6(0, -2), CSM_G6(2, -2));
    float results = row0.x * (1.0f - frac2.y) + row0.y;

    const float2 row1 = CsmHorizontal5x2(frac2, CSM_G6(-2, 0), CSM_G6(0, 0), CSM_G6(2, 0));
    results += row1.x + row1.y;

    const float2 row2 = CsmHorizontal5x2(frac2, CSM_G6(-2, 2), CSM_G6(0, 2), CSM_G6(2, 2));
    results += row2.x + row2.y * frac2.y;

    #undef CSM_G6
    return saturate(0.04f * results); // 1/25
}

// PCF over-blurs the penumbra; UE squares the result to get the artistically expected profile.
float CsmCorrectOverBlur(float shadow) { return shadow * shadow; }

// Edge sharpness. 1 = unchanged, > 1 narrows the transition.
float CsmSharpen(float shadow, float sharpen)
{
    return saturate((shadow - 0.5f) * sharpen + 0.5f);
}

int CsmChooseCascade(CsmParams p, float3 Pws)
{
    const float z = dot(Pws - p.camPosWS, p.camDirWS);
    const float3 gt = saturate(sign(z.xxx - p.splitsVS.yzw));
    return (int)(gt.x + gt.y + gt.z);
}

float CsmPcf3x3(Texture2D atlas, SamplerComparisonState cmp,
                float2 uv, float zRef, float2 texel, float radiusPx)
{
    float s = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float2 off = float2(x, y) * texel * radiusPx;
            s += atlas.SampleCmpLevelZero(cmp, uv + off, zRef).r;
        }
    }
    return s / 9.0f;
}

// Sample starting at cascade `start`, falling back to a coarser one if the (normal-bias-offset)
// point lands outside `start`'s atlas tile. The test runs on the CASCADE-LOCAL uv (before atlas
// scale+bias), so a neighbour tile is never sampled, and it insets by the PCF reach so the 3x3 taps
// cannot bleed across the gutterless tile border. (S5 replaces that inset with a real gutter + clamp.)
// Returns 1.0 (lit) only past cascade 3.
//
// outCascade reports the cascade the chain RESOLVED to — not the one the split selection picked.
// The difference is exactly the silent tile-border fallback, which is what the S0.3 tint makes visible.
float CsmSampleChain(CsmParams p, Texture2D atlas, SamplerComparisonState cmp, SamplerState smp,
                     int start, float3 Pws, float3 Nws, float NdotL, out int outCascade)
{
    const float2 texel = 1.0f / p.atlasSize;
    outCascade = kCsmNoCascade;

    [unroll]
    for (int c = 0; c < 4; ++c)
    {
        if (c < start)
        {
            continue;
        }

        const float2 scale  = p.scaleBias[c].xy;
        const float2 biasUV = p.scaleBias[c].zw;

        // Receiver normal offset, per cascade (each has its own world texel). Not a UE term: theirs
        // rely on the depth-pass bias alone, and it shows. Offsetting the SAMPLE POINT instead of
        // pushing depth is what makes it cheap in peter-panning.
        const float3 Poff = Pws + Nws * (p.normalBiasTexels * p.cascadeTexelWS[c]);
        const float4 lc = mul(float4(Poff, 1.0f), p.lightViewProj[c]);
        const float2 uvLocal = (lc.xy / max(1e-6f, lc.w)) * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
        const float  z = lc.z / max(1e-6f, lc.w);

        // The Gather tent reaches TWO texels, the 3x3 SampleCmp grid one. Insetting by the actual
        // reach is what keeps a tap from crossing into the neighbouring tile (S5 replaces this with
        // a real gutter).
        // Kernel reach in texels: 3 for the 6x6 tent, 2 for the 4x4, the PCF radius for the box.
        const float reach = (p.useGatherPcf == 2u) ? 3.0f
                          : ((p.useGatherPcf == 1u) ? 2.0f : p.pcfRadius);
        const float2 margin = (reach * texel) / max(1e-6f, scale);
        if (any(uvLocal < margin) || any(uvLocal > 1.0f - margin))
        {
            continue;
        }

        const float2 uv = uvLocal * scale + biasUV;
        // This cascade's tile in atlas UV, inset by one texel: belt and braces for the Gather quads,
        // whose 2x2 footprint is picked by hardware rounding and can straddle a texel boundary.
        const float4 uvClampAll = float4(biasUV + texel, biasUV + scale - texel);

        outCascade = c;

        if (p.useGatherPcf != 0u)
        {
            // S8. Ramp width is per cascade and proportional to that cascade's world texel, so
            // softness and resolution drop by the SAME factor across a boundary -- measured before
            // this step: detail fell x4.00 from c0 to c1 while softness grew only x2.83, which is
            // exactly what made the boundary read as a cliff.
            //
            // The old per-cascade PCF radius shrink (pow(texel0/texelC, 0.25)) is GONE on purpose:
            // it was a stand-in for the missing ramp and it is what broke the proportion. The kernel
            // is now a fixed 4x4 texels in every cascade, like UE's.
            //
            // Attenuating the ramp by NoL is UE's receiver bias: the more grazing the light, the
            // wider the transition and the harder it is to self-shadow.
            const float ts = p.transitionScale[c] * lerp(p.receiverBiasMin, 1.0f, saturate(NdotL));
            // RAW receiver depth: the depth pass owns the bias now, exactly as in UE
            // (`Settings.SceneDepth = LightSpacePixelDepthForOpaque`, ShadowProjectionPixelShader.usf:248).
            float sh = (p.useGatherPcf == 2u)
                ? CsmTent6x6Gather(atlas, smp, uv, z, ts, p.atlasSize, texel, uvClampAll)
                : CsmTent4x4Gather(atlas, smp, uv, z, ts, p.atlasSize, texel, uvClampAll);
            // ORDER MATTERS and it is UE's: sharpen FIRST, over-blur correction SECOND
            // (ShadowProjectionPixelShader.usf:375 then :385). Squaring a sharpened curve is not the
            // same as sharpening a squared one; with sharpen at its 1.0 no-op the two agree, which is
            // exactly why getting this backwards would have gone unnoticed until the knob was exposed.
            sh = CsmSharpen(sh, p.sharpen);
            return (p.overBlurCorrect != 0.0f) ? CsmCorrectOverBlur(sh) : sh;
        }

        // Legacy path (useGatherPcf == 0): kept for the A/B and as the emergency fallback. The
        // radius shrink stays HERE only so this arm reproduces the pre-S8 image exactly.
        const float pcfR = p.pcfRadius * pow((p.cascadeTexelWS[0] / max(1e-6f, p.cascadeTexelWS[c])), 0.25f);
        return CsmPcf3x3(atlas, cmp, uv, z, texel, pcfR);
    }

    return 1.0f;
}

float CsmSampleShadow(CsmParams p, Texture2D atlas, SamplerComparisonState cmp, SamplerState smp,
                      float3 Pws, float3 Nws, float NdotL, out int outCascade)
{
    const int idx = CsmChooseCascade(p, Pws);
    float shadow = CsmSampleChain(p, atlas, cmp, smp, idx, Pws, Nws, NdotL, outCascade);

    const float zView = dot(Pws - p.camPosWS, p.camDirWS);
    // This cascade's own slice. near = the previous split (the camera near plane for c0), far = its
    // own. UE size the transition off THIS length (CascadeTransitionFraction * (SplitFar-SplitNear),
    // DirectionalLightComponent.cpp:925); measuring it off the absolute distance instead made far
    // cascades fade over half again too much ground.
    const float sNear = (idx == 0) ? p.splitsVS.x : ((idx == 1) ? p.splitsVS.y
                       : ((idx == 2) ? p.splitsVS.z : p.splitsVS.w));
    const float sFar  = (idx == 0) ? p.splitsVS.y : ((idx == 1) ? p.splitsVS.z
                       : ((idx == 2) ? p.splitsVS.w : p.farSplit));

    if (idx < 3)
    {
        // Cross-fade into the next cascade so the switch (and its jump in bias, texel density and
        // kernel footprint) reads as a gradient rather than a seam. Costs a second sample only
        // inside the band.
        const float band = max(1e-4f, (sFar - sNear) * p.blendFraction);
        const float t = saturate((zView - (sFar - band)) / band);
        if (t > 0.0f)
        {
            // The partner's resolved index is deliberately dropped: outCascade stays the primary
            // cascade so the debug tint shows zones, not a striped band.
            int blendCascade;
            const float shadowNext = CsmSampleChain(p, atlas, cmp, smp, idx + 1, Pws, Nws, NdotL, blendCascade);
            shadow = lerp(shadow, shadowNext, t);
        }
    }
    else if (p.distanceFadeFraction > 0.0f)
    {
        // The last cascade has no coarser neighbour to hand over to, so UE pull its fade plane
        // INWARD by the same extension and fade the shadow out to lit. Skipping this is what leaves
        // a hard terminator line at maxDistance -- the shadow simply stops.
        const float fade = max(1e-4f, (sFar - sNear) * p.distanceFadeFraction);
        const float t = saturate((zView - (sFar - fade)) / fade);
        shadow = lerp(shadow, 1.0f, t);
    }

    return shadow;
}

#endif // CSM_SAMPLE_HLSLI
