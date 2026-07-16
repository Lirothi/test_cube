// GBuffer auto-instancing variant (Step 4). Per-object data is a constant-buffer array
// (b0) indexed by SV_InstanceID, filled on the CPU from a run of identical (mesh,material)
// objects. Shading mirrors gbuffer.hlsl exactly (shared BaseVS / FetchShadingValuesP /
// FinalizeGBuffer) so instanced objects render pixel-identical to the per-object path.
#pragma pack_matrix(row_major)
#define GBUFFER_SKIP_PEROBJECT
#include "gbuffer_common.hlsl"

Texture2D gAlbedo : register(t0);
Texture2D gMR : register(t1); // R=metal, G=rough
Texture2D gNormalMap : register(t2); // tangent-space, +Z
SamplerState gSmp : register(s0);

// B2b multi-slot instancing: material params come from a per-SUBMESH-draw CB (b2, one upload
// per slot per batch) instead of the per-instance entry — instances share slot materials, so
// carrying them per instance would waste the array and forbid per-slot textures anyway.
#ifndef INSTCB_SLOT_PARAMS
#define INSTCB_SLOT_PARAMS 0
#endif

#if INSTCB_SLOT_PARAMS
// Mirrors render::InstanceSlotParams (InstanceTypes.h) — 64 bytes, cbuffer packing.
cbuffer SlotParams : register(b2)
{
    float4 slotBaseColor;
    float2 slotMetalRough;
    float slotAlphaCutoff; // C1 alpha test (-1 disables)
    float _slotPad0;
    float4 slotTexOffsScale;
    float4 slotTexFlags;
    float3 slotEmissive; // D: premultiplied color*strength
    float _slotPad1;
};

#define GBUFFER_INSTCB_RS \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)," \
    "CBV(b0)," \
    "CBV(b1)," \
    "CBV(b2)," \
    "DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
#else
#define GBUFFER_INSTCB_RS \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)," \
    "CBV(b0)," \
    "CBV(b1)," \
    "DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
#endif

struct VSOutInst
{
    float4 H : SV_POSITION;
    float4 clipH : TEXCOORD4;
    float4 prevH : TEXCOORD3;
    float3 NWS : TEXCOORD1;
    float4 TWS : TEXCOORD2; // .xyz = tangent in world, .w = sign
    float2 UV : TEXCOORD0;
    nointerpolation uint IID : TEXCOORD5;
    nointerpolation uint objectId : TEXCOORD6;
};

[RootSignature(GBUFFER_INSTCB_RS)]
VSOutInst VSMain(VSInInst i)
{
    InstancePerObject d = inst[i.IID];
    VSOut b = BaseVS(i.P, d.world, d.prevWorld, viewProj, i.N, i.T, i.UV, d.objectId);

    VSOutInst o;
    o.H = b.H;
    o.clipH = b.clipH;
    o.prevH = b.prevH;
    o.NWS = b.NWS;
    o.TWS = b.TWS;
    o.UV = b.UV;
    o.IID = i.IID;
    o.objectId = b.objectId;
    return o;
}

[RootSignature(GBUFFER_INSTCB_RS)]
PSOut PSMain(VSOutInst i, bool isFrontFace : SV_IsFrontFace)
{
    InstancePerObject d = inst[i.IID];

#if INSTCB_SLOT_PARAMS
    const float4 mBaseColor    = slotBaseColor;
    const float2 mMetalRough   = slotMetalRough;
    const float4 mTexOffsScale = slotTexOffsScale;
    const float4 mTexFlags     = slotTexFlags;
    const float  mAlphaCutoff  = slotAlphaCutoff;
    const float3 mEmissive     = slotEmissive;
#else
    const float4 mBaseColor    = d.baseColor;
    const float2 mMetalRough   = d.metalRough;
    const float4 mTexOffsScale = d.texOffsScale;
    const float4 mTexFlags     = d.texFlags;
    const float  mAlphaCutoff  = d.alphaCutoff;
    const float3 mEmissive     = d.emissive;
#endif

    AlphaTestClip(gAlbedo, gSmp, i.UV, mTexOffsScale, mBaseColor.a, mAlphaCutoff);

    // Two-sided foliage: flip a backface normal to face out of the visible side (see gbuffer.hlsl).
    float3 NNorm = normalize(i.NWS);
    if (!isFrontFace) { NNorm = -NNorm; }

    float3 albedo;
    float2 mr;
    float3 N = NNorm;
    FetchShadingValuesP(gAlbedo, gMR, gNormalMap, gSmp, i.UV, i.TWS, mTexOffsScale, mTexFlags, albedo, mr, N);

#if MR_LAYOUT_GLTF
    albedo = mTexFlags.x > 0.5 ? albedo * mBaseColor.rgb : mBaseColor.rgb;
    mr     = mTexFlags.y > 0.5 ? mr * mMetalRough.xy     : mMetalRough.xy;
#else
    albedo = lerp(mBaseColor.rgb, albedo, mTexFlags.x);
    mr = lerp(mMetalRough.xy, mr, mTexFlags.y);
#endif
    if (mTexFlags.z < 0.5)
    {
        N = NNorm;
    }
    float2 currUv = ClipToUV(i.clipH);
    float2 prevUv = ClipToUV(i.prevH);
    float2 motion = currUv - prevUv;

    return FinalizeGBuffer(albedo, mr, N, float4(mEmissive, 0), motion, i.objectId);
}
