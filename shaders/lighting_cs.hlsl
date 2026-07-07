// t0..t3 : GBuffer textures (GB0, GB1, GB2, GBVelocity)
// t4     : Depth (R32F)
// t5     : Shadow atlas
// u0     : Light accumulation target (RWTexture2D)
// s0     : PointClamp
// s1     : ComparisonLinearClamp

#pragma pack_matrix(row_major)

#include "utils.hlsl"
#include "vsm_sample.hlsli"

Texture2D GB0 : register(t0);
Texture2D GB1 : register(t1);
Texture2D GB2 : register(t2);
Texture2D GBVelocity : register(t3);
Texture2D DepthT : register(t4);
Texture2D ShadowAtlas : register(t5);
StructuredBuffer<uint> VsmPageTable : register(t6); // Rung 2 / Step 24f: directional clipmap page table
Texture2D VsmPool : register(t7);                    // VSM physical page pool depth
RWTexture2D<float4> LightTarget : register(u0);

SamplerState gSmpPoint : register(s0);
SamplerComparisonState gSmpLinear : register(s1);

cbuffer PerFrame : register(b0)
{
    float3 sunDirWS;
    float ambientIntensity;
    float3 lightRgb;
    float exposure;
    float3 camPosWS;
    float3 camDirWS;

    float4x4 invView;
    float4x4 invProj;

    float4x4 lightViewProj[4];
    float4 cascadeScaleBias[4];
    float4 cascadeSplitsVS;
    float2 shadowAtlasSize;
    float4 shadowBiasNDC;
    float4 normalBiasWS;
    float2 screenSize;
    float2 invScreenSize;
    // Configurable artist boost for the analytic sun specular on metals. The lobe is
    // scaled by (1 + metal*sunMetalSpec): metal=0 is a no-op (dielectrics stay physical),
    // metal=1 amplifies the sun highlight so it reads against the environment reflection
    // that would otherwise swamp it. 0 = pure physical.
    float sunMetalSpec;
    // Sun angular size, added to the GGX alpha for the analytic sun only (see EvalBRDF).
    // Floors the specular lobe width so a smooth surface shows a finite, bright,
    // sample-able sun glint instead of a sub-pixel spike. 0 = punctual (no change).
    float sunAngularSize;
    float2 _padSun;
    // Rung 2 / Step 24f: sample directional shadows from the VSM clipmap (VSM mode) instead of the
    // CSM cascades. clipmapViewProj[i] = clipmap level i's camera-centered ortho viewProj.
    uint useVsm;
    float vsmDepthBias;
    float clipmapBaseExtent;  // finest clipmap level's world extent (for per-level texel-scaled bias)
    float clipmapNormalBias;  // normal offset in texels
    float4x4 clipmapViewProj[8];
}

static const float pcfRadius = 1.0f;

int ChooseCascadeIndex(float3 Pws)
{
    float z = dot(Pws - camPosWS, camDirWS);
    float3 gt = saturate(sign(z.xxx - cascadeSplitsVS.yzw));
    return (int)(gt.x + gt.y + gt.z);
}

float ShadowPCF3x3(float2 uv, float zRef, float2 texel, float radiusPx)
{
    float s = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 off = float2(x, y) * texel * radiusPx;
            s += ShadowAtlas.SampleCmpLevelZero(gSmpLinear, uv + off, zRef).r;
        }
    }
    return s / 9.0;
}

// Blend-band width as a fraction of each split distance (Step 3).
static const float kBlendFraction = 0.1;

// Sample the shadow factor starting at cascade `start`, falling back to a coarser cascade
// if the (normal-bias-offset) point lands outside `start`'s atlas tile. Tests the
// cascade-local UV (BEFORE atlas scale+bias) so a neighbour tile is never sampled, and
// insets by the PCF reach so 3x3 taps never bleed across the gutterless tile border.
// Returns 1.0 (lit) only when the point is beyond cascade 3 — past the shadow range.
float SampleCascadeChain(int start, float3 Pws, float NdotL, float3 Nws)
{
    const float2 texel = 1.0 / shadowAtlasSize;

    [unroll]
    for (int c = 0; c < 4; ++c)
    {
        if (c < start)
        {
            continue;
        }

        const float4x4 LVP = lightViewProj[c];
        const float4 sb = cascadeScaleBias[c];
        const float2 scale = sb.xy;
        const float2 biasUV = sb.zw;

        // Re-evaluate the offset per cascade: each has its own texel size.
        const float3 Poff = Pws + Nws * normalBiasWS[c];

        const float4 lc = mul(float4(Poff, 1), LVP);
        const float2 uvLocal = (lc.xy / max(1e-6, lc.w)) * float2(0.5, -0.5) + float2(0.5, 0.5);
        const float z = lc.z / max(1e-6, lc.w);

        const float2 margin = (pcfRadius * texel) / max(1e-6, scale);
        if (any(uvLocal < margin) || any(uvLocal > 1.0 - margin))
        {
            continue;
        }

        const float2 uv = uvLocal * scale + biasUV;
        const float bBase = shadowBiasNDC[c];
        const float b = bBase + (1.0 - saturate(NdotL)) * bBase;

        // Step 4: every cascade uses 3x3 PCF, but the texel radius is scaled per cascade
        // so the WORLD-space penumbra is anchored to cascade 0 instead of growing with the
        // cascade. A fixed 1-texel radius blurs far cascades ~10-16x more in world space
        // (their texels are that much larger) — that turned the last cascade into mush.
        // normalBiasWS[c] is proportional to cascade c's world texel size, so its ratio to
        // cascade 0 is the scale (the normalBiasInTexels factor cancels); c==0 -> 1.0.
        const float pcfR = pcfRadius * pow((normalBiasWS[0] / max(1e-6, normalBiasWS[c])), 0.25);
        return ShadowPCF3x3(uv, z - b, texel, pcfR);
    }

    return 1.0;
}

float SampleShadowCSM(float3 Pws, float NdotL, float3 Nws)
{
    const int idx = ChooseCascadeIndex(Pws);
    float shadow = SampleCascadeChain(idx, Pws, NdotL, Nws);

    // Step 3: blend band. In a band just before cascade idx's far split, cross-fade into
    // cascade idx+1 so the hard cascade switch (and its bias / texel-density / PCF
    // discontinuity) becomes a gradient instead of a visible seam. Cascade 3 has no
    // coarser neighbour, so it never blends. Costs a second sample only inside the band.
    if (idx < 3)
    {
        const float zView = dot(Pws - camPosWS, camDirWS);
        const float splitNext = idx == 0 ? cascadeSplitsVS.y : (idx == 1 ? cascadeSplitsVS.z : cascadeSplitsVS.w);
        const float band = splitNext * kBlendFraction;
        const float t = saturate((zView - (splitNext - band)) / max(1e-4, band));
        if (t > 0.0)
        {
            const float shadowNext = SampleCascadeChain(idx + 1, Pws, NdotL, Nws);
            shadow = lerp(shadow, shadowNext, t);
        }
    }

    return shadow;
}

#define LIGHTING_RS \
    "CBV(b0)," \
    "DescriptorTable(SRV(t0, numDescriptors=8, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(Sampler(s0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE))"

[RootSignature(LIGHTING_RS)]
[numthreads(8,8,1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width = (uint)screenSize.x;
    uint height = (uint)screenSize.y;
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
    {
        return;
    }

    float2 uv = (float2(dispatchThreadId.xy) + 0.5f) * invScreenSize;

    float4 gb0 = GB0.SampleLevel(gSmpPoint, uv, 0);
    float4 gb1 = GB1.SampleLevel(gSmpPoint, uv, 0);

    float3 albedo = gb0.rgb;
    float2 rm = UnpackRM(gb0.a);
    float rough = rm.x;
    float metal = rm.y;

    float3 N = normalize(gb1.rgb * 2.0 - 1.0);
    float z = DepthT.SampleLevel(gSmpPoint, uv, 0).r;
    if (z <= kEpsilon)
    {
        LightTarget[dispatchThreadId.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    float3 P = ReconstructPosWS(uv, z, invProj, invView);

    const float3 V = normalize(camPosWS - P);
    const float3 L = normalize(-sunDirWS);

    // Q2: diffuse ambient applies to the dielectric (non-metal) fraction only. Metals have
    // no Lambertian response; their ambient arrives specularly via the env reflection in the
    // compose pass. Without the (1-metal) gate, metals get a flat albedo floor that washes
    // them out and kills highlight contrast.
    const float3 ambient = albedo * (1.0 - metal) * ambientIntensity;

    BRDFInput bi;
    bi.albedo = albedo;
    bi.rough = rough;
    bi.metal = metal;
    bi.N = N;
    bi.V = V;
    bi.L = L;

    BRDFResult br = EvalBRDF(bi, sunAngularSize);
    float3 color = ambient * lightRgb;
    if (br.NdotL > 0.0)
    {
        // Step 24f: VSM mode samples the directional clipmap; Legacy samples the CSM cascades.
        float shadow;
        if (useVsm != 0u)
        {
            shadow = VsmClipmapShadow(P, N, camPosWS, clipmapBaseExtent, clipmapNormalBias, vsmDepthBias,
                                      clipmapViewProj, VsmPageTable, VsmPool, gSmpLinear);
        }
        else
        {
            shadow = SampleShadowCSM(P, br.NdotL, N);
        }
        // Boost the analytic sun specular on metals (1 + metal*sunMetalSpec) so the
        // highlight reads against the environment reflection. metal=0 -> no change.
        const float3 specSun = br.specBRDF * (1.0 + metal * sunMetalSpec * 1);
        float3 direct = (br.diffBRDF + specSun) * br.NdotL * lightRgb * shadow;
        color += direct;
    }

    LightTarget[dispatchThreadId.xy] = float4(color * exposure, 1.0);
}
