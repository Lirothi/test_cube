// Local-light plan S8: ALPHA_TEST (the material slot's own define -- the shadow desc is built from the slot's G-buffer
// desc) = the MASKED twin, UE's masked shadow depth: the albedo's alpha clips the caster exactly as the G-buffer clips it
// (AlphaTestClip). Its root signature adds the G-buffer's t0..t2 table and s0 (only t0 is read), filled by
// MaterialData::StageGBufferBindings. The indirect path has had this since C2 (shadow_indirect_csm SHADOW_MASKED).
#if defined(ALPHA_TEST) && ALPHA_TEST
#define GBUFFER_CSM_MASKED 1
#define GBUFFER_CSM_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0), CBV(b1), DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
#else
#define GBUFFER_CSM_MASKED 0
#define GBUFFER_CSM_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0), CBV(b1)"
#endif
// Use the shared b0 (per-object) + b1 (per-view: viewProj) from gbuffer_common
#pragma pack_matrix(row_major)
#include "gbuffer_common.hlsli"
#include "shadow_depth_common.hlsli"

// Local-light plan S4: SHADOW_LINEAR_DEPTH = the spot views' twin -- the pixel shader writes the linear depth
// (shadow_depth_common.hlsli ShadowLinearDepth) as SV_Depth. Every other view keeps the empty pixel shader.
#ifndef SHADOW_LINEAR_DEPTH
#define SHADOW_LINEAR_DEPTH 0
#endif

#if GBUFFER_CSM_MASKED
Texture2D gAlbedo : register(t0);
SamplerState gSmp : register(s0);
#endif

struct VSOutD
{
    float4 H : SV_POSITION;
#if SHADOW_LINEAR_DEPTH
    float linearDepth : TEXCOORD0;
#endif
#if GBUFFER_CSM_MASKED
    float2 UV : TEXCOORD1;
#endif
};

[RootSignature(GBUFFER_CSM_RS)]
VSOutD VSMain(VSIn i)
{
    VSOutD o;
    // W5: same sway as the gbuffer BaseVS (shared ApplyWindWS/WindOffset), so this fallback path's
    // shadow tracks the swaying tree exactly like the indirect path does.
    float4 wp = mul(float4(i.P, 1.0f), world);
    wp.xyz += ApplyWindWS(i.P, wp.xyz, world, windStrength, i.WIND, windFoliage,
                          windTrunkStiff, windLeafScale, windGustMul, windTime);
    // S6: the normal is already in this layout (VSIn is the full PNTUV gbuffer input), so the
    // fallback path costs no extra fetch. World scale only skews the bias -- acceptable.
    const float3 nWS = mul(i.N, (float3x3)world);
    const float4 bias = float4(shadowConstBias, shadowSlopeBias, shadowMaxSlope, shadowClampNear);
    o.H = ApplyShadowDepthBias(mul(wp, viewProj), nWS, viewProj, bias);
#if SHADOW_LINEAR_DEPTH
    o.linearDepth = ShadowLinearDepth(o.H, nWS, viewProj, bias);
#endif
#if GBUFFER_CSM_MASKED
    o.UV = i.UV;
#endif
    return o;
}

// Plan S8: the slot's cutoff (-1 = an opaque slot sharing the masked PSO: never clips). No terrain is masked. A glass
// slot's cutoff comes raised by kGlassShadowCutoff: its glass texels cast a dithered share (shadow_depth_common.hlsli).
void ShadowMaskClip(VSOutD i)
{
#if GBUFFER_CSM_MASKED
    if (alphaCutoff >= 0.0f)
    {
        const float a = gAlbedo.Sample(gSmp, tfUVp(i.UV, texOffsScale)).a * baseColor.a;
        ShadowAlphaClip(a, alphaCutoff, i.H.xy);
    }
#endif
}

#if SHADOW_LINEAR_DEPTH
[RootSignature(GBUFFER_CSM_RS)]
float PSMain(VSOutD i) : SV_Depth
{
    ShadowMaskClip(i);
    return saturate(i.linearDepth);
}
#elif GBUFFER_CSM_MASKED
[RootSignature(GBUFFER_CSM_RS)]
void PSMain(VSOutD i) { ShadowMaskClip(i); }
#else
// Depth-only — pixel shader can remain empty
[RootSignature(GBUFFER_CSM_RS)]
void PSMain(VSOutD i) { }
#endif
