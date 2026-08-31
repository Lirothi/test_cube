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
    float4   normalBiasWS;
    float2   atlasSize;       // (W, H) in texels
    float3   camPosWS;
    float3   camDirWS;        // normalized
    float    pcfRadius;       // filter radius in texels (1.0 today)
};

// Fraction of the split distance over which cascade c cross-fades into cascade c+1.
static const float kCsmBlendFraction = 0.1f;

// 4 = the sample fell past cascade 3, i.e. there is no shadow data for this pixel at all. Kept as a
// named constant because the debug tint (S0.3) and the fallback chain must agree on it.
static const int kCsmNoCascade = 4;

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
float CsmSampleChain(CsmParams p, Texture2D atlas, SamplerComparisonState cmp,
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

        // Re-evaluated per cascade: each has its own world texel size.
        const float3 Poff = Pws + Nws * p.normalBiasWS[c];

        const float4 lc = mul(float4(Poff, 1.0f), p.lightViewProj[c]);
        const float2 uvLocal = (lc.xy / max(1e-6f, lc.w)) * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
        const float  z = lc.z / max(1e-6f, lc.w);

        const float2 margin = (p.pcfRadius * texel) / max(1e-6f, scale);
        if (any(uvLocal < margin) || any(uvLocal > 1.0f - margin))
        {
            continue;
        }

        const float2 uv = uvLocal * scale + biasUV;
        const float bBase = p.depthBiasNDC[c];
        const float b = bBase + (1.0f - saturate(NdotL)) * bBase;

        // Every cascade filters 3x3, but the texel radius is scaled per cascade so the WORLD-space
        // penumbra stays anchored to cascade 0 instead of growing with the cascade. A fixed 1-texel
        // radius blurs far cascades ~10-16x more in world space (their texels are that much larger),
        // which turned the last cascade to mush. normalBiasWS[c] is proportional to cascade c's world
        // texel, so its ratio to cascade 0 IS the scale (the normalBiasInTexels factor cancels);
        // c == 0 gives exactly 1.0.
        const float pcfR = p.pcfRadius * pow((p.normalBiasWS[0] / max(1e-6f, p.normalBiasWS[c])), 0.25f);

        outCascade = c;
        return CsmPcf3x3(atlas, cmp, uv, z - b, texel, pcfR);
    }

    return 1.0f;
}

float CsmSampleShadow(CsmParams p, Texture2D atlas, SamplerComparisonState cmp,
                      float3 Pws, float3 Nws, float NdotL, out int outCascade)
{
    const int idx = CsmChooseCascade(p, Pws);
    float shadow = CsmSampleChain(p, atlas, cmp, idx, Pws, Nws, NdotL, outCascade);

    // Blend band: in a band just before cascade idx's far split, cross-fade into cascade idx+1 so
    // the hard switch (and its jump in bias / texel density / PCF radius) becomes a gradient instead
    // of a seam. Cascade 3 has no coarser neighbour. Costs a second sample only inside the band.
    if (idx < 3)
    {
        const float zView = dot(Pws - p.camPosWS, p.camDirWS);
        const float splitNext = idx == 0 ? p.splitsVS.y : (idx == 1 ? p.splitsVS.z : p.splitsVS.w);
        const float band = splitNext * kCsmBlendFraction;
        const float t = saturate((zView - (splitNext - band)) / max(1e-4f, band));
        if (t > 0.0f)
        {
            // The partner's resolved index is deliberately dropped: outCascade stays the primary
            // cascade so the debug tint shows zones, not a striped band.
            int blendCascade;
            const float shadowNext = CsmSampleChain(p, atlas, cmp, idx + 1, Pws, Nws, NdotL, blendCascade);
            shadow = lerp(shadow, shadowNext, t);
        }
    }

    return shadow;
}

#endif // CSM_SAMPLE_HLSLI
