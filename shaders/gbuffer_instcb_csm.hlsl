// Depth-only shadow variant of the GBuffer auto-instancing path (Step 4). Reads only
// inst[SV_InstanceID].world from the per-object constant-buffer array (b0); viewProj
// (light) comes from the shared per-view CB (b1). Mirrors gbuffer_csm.hlsl.
#pragma pack_matrix(row_major)
#define GBUFFER_SKIP_PEROBJECT
#include "gbuffer_common.hlsli"
#include "shadow_depth_common.hlsli"

// Local-light plan S8: INSTCB_SLOT_PARAMS = a multi-slot batch's per-SUBMESH slot CB (b2), as gbuffer_instcb.hlsl's --
// the slot's foliage weight for the sway and, masked, its cutoff / UV transform / base alpha. ALPHA_TEST = the masked
// twin (gbuffer_csm.hlsl): the albedo's alpha clips the caster, the G-buffer's t0..t2 table + s0 in the root signature.
#ifndef INSTCB_SLOT_PARAMS
#define INSTCB_SLOT_PARAMS 0
#endif
#if defined(ALPHA_TEST) && ALPHA_TEST
#define GBUFFER_INSTCB_CSM_MASKED 1
#else
#define GBUFFER_INSTCB_CSM_MASKED 0
#endif

#if INSTCB_SLOT_PARAMS
#define GBUFFER_INSTCB_CSM_RS_B2 ", CBV(b2)"
// The leading fields of gbuffer_instcb.hlsl's SlotParams (render::InstanceSlotParams, InstanceTypes.h); a cbuffer may
// read a prefix of what is bound.
cbuffer SlotParams : register(b2)
{
    float4 slotBaseColor;
    float2 slotMetalRough;
    float slotAlphaCutoff; // C1 alpha test (-1 disables)
    float slotMrMultiply;
    float4 slotTexOffsScale;
    float4 slotTexFlags;
    float3 slotEmissive;
    float slotWindFoliage; // per-slot foliage weight (0 = trunk, 1 = leaves)
};
#else
#define GBUFFER_INSTCB_CSM_RS_B2 ""
#endif
#if GBUFFER_INSTCB_CSM_MASKED
#define GBUFFER_INSTCB_CSM_RS_TEX ", DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
Texture2D gAlbedo : register(t0);
SamplerState gSmp : register(s0);
#else
#define GBUFFER_INSTCB_CSM_RS_TEX ""
#endif

#define GBUFFER_INSTCB_CSM_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0), CBV(b1)" GBUFFER_INSTCB_CSM_RS_B2 GBUFFER_INSTCB_CSM_RS_TEX

// Local-light plan S4: SHADOW_LINEAR_DEPTH = the spot views' twin (gbuffer_csm.hlsl).
#ifndef SHADOW_LINEAR_DEPTH
#define SHADOW_LINEAR_DEPTH 0
#endif

struct VSOutD
{
    float4 H : SV_POSITION;
#if SHADOW_LINEAR_DEPTH
    float linearDepth : TEXCOORD0;
#endif
#if GBUFFER_INSTCB_CSM_MASKED
    float2 UV : TEXCOORD1;
    nointerpolation uint IID : TEXCOORD2;
#endif
};

[RootSignature(GBUFFER_INSTCB_CSM_RS)]
VSOutD VSMain(VSInInst i)
{
    VSOutD o;
    // W5: mirror the gbuffer BaseVS sway. Safe to read windStrength here: the shadow instance array
    // is filled by the same IInstanceable::FillInstanceData as the gbuffer one (InstancedDrawBatch),
    // unlike the non-instanced b0 which UpdateShadowCB has to populate explicitly.
    const float4x4 w = inst[i.IID].world;
    float4 wp = mul(float4(i.P, 1.0f), w);
    // The foliage weight is PER SLOT: a multi-slot batch's slot CB carries it (plan S8); a single-slot object's
    // per-instance value IS that slot's. The MAIN bend is per-object either way.
#if INSTCB_SLOT_PARAMS
    const float foliageW = slotWindFoliage;
#else
    const float foliageW = inst[i.IID].windFoliage;
#endif
    wp.xyz += ApplyWindWS(i.P, wp.xyz, w, inst[i.IID].windStrength, i.WIND,
                          foliageW, inst[i.IID].windTrunkStiff,
                          inst[i.IID].windLeafScale, windGustMul, windTime);
    const float3 nWS = mul(i.N, (float3x3)w); // S6
    const float4 bias = float4(shadowConstBias, shadowSlopeBias, shadowMaxSlope, shadowClampNear);
    o.H = ApplyShadowDepthBias(mul(wp, viewProj), nWS, viewProj, bias);
#if SHADOW_LINEAR_DEPTH
    o.linearDepth = ShadowLinearDepth(o.H, nWS, viewProj, bias);
#endif
#if GBUFFER_INSTCB_CSM_MASKED
    o.UV = i.UV;
    o.IID = i.IID;
#endif
    return o;
}

// Plan S8: the slot's cutoff (-1 = an opaque slot: never clips) -- the slot CB's in a multi-slot batch, else the
// instance's (FillInstanceData writes slot 0's).
void ShadowMaskClip(VSOutD i)
{
#if GBUFFER_INSTCB_CSM_MASKED
#if INSTCB_SLOT_PARAMS
    const float4 texOffsScaleV = slotTexOffsScale;
    const float  baseAlpha = slotBaseColor.a;
    const float  cutoff = slotAlphaCutoff;
#else
    const float4 texOffsScaleV = inst[i.IID].texOffsScale;
    const float  baseAlpha = inst[i.IID].baseColor.a;
    const float  cutoff = inst[i.IID].alphaCutoff;
#endif
    AlphaTestClip(gAlbedo, gSmp, i.UV, texOffsScaleV, float4(0.0f, 0.0f, 0.0f, 0.0f), float4(0.0f, 0.0f, 0.0f, 0.0f),
                  baseAlpha, cutoff);
#endif
}

#if SHADOW_LINEAR_DEPTH
[RootSignature(GBUFFER_INSTCB_CSM_RS)]
float PSMain(VSOutD i) : SV_Depth
{
    ShadowMaskClip(i);
    return saturate(i.linearDepth);
}
#elif GBUFFER_INSTCB_CSM_MASKED
[RootSignature(GBUFFER_INSTCB_CSM_RS)]
void PSMain(VSOutD i) { ShadowMaskClip(i); }
#else
// Depth-only — pixel shader can remain empty
[RootSignature(GBUFFER_INSTCB_CSM_RS)]
void PSMain(VSOutD i) { }
#endif
