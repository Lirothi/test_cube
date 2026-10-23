// Occlusion plan S4: the GPU-driven G-buffer. One ExecuteIndirect per virtual group (mesh submesh
// x camera LOD); every instance of the draw is a caster id the input assembler delivers from the
// camera visible list (slot 1, per-instance -- the shadow_indirect_csm.hlsl shape), and the
// per-object data comes out of the registry's per-caster record (t3, the SAME buffer the shadow
// VS reads). The pixel shader is gbuffer.hlsl's line for line; the per-object values it used to
// read from `cbuffer PerObject` (b0) it reads from that record itself, by the caster id the VS
// passes (one flat uint) -- 2026-10-23: the VS used to carry them as five flat float4 outputs (~42
// floats per vertex with the shared ones), and at foliage triangle sizes the pass was bound by its
// outputs: GBuffer_Indirect 0.38-0.40 -> 0.30-0.31 ms on wind_test. Dropping the unjittered clip
// position as well (rebuilt from SV_Position) measured nothing more and was not kept. The instance
// buffer therefore rests PIXEL | NON_PIXEL (ShadowGpuData kInstancesReadState).
//
// Parity with the CPU path (RenderableObject::Render / GBufferRenderable::Render) is the whole
// point: same BaseVS, same FetchShadingValuesP/FinalizeGBuffer, same wind, same dithered LOD
// crossfade -- the fade of THIS draw is (groupLod == the caster's tier ? -f : +f), which is what
// the CPU path writes into lodFade for its two draws of a fading object.
//
// b0: IndirectDraw (the virtual group's LOD)   b1: PerView (gbuffer_common)   b2: SurfaceParams
// t0..t2: the group's material textures       t3: InstancePerObject[]   t4: caster fade[]
// t5: caster camera LOD[]                     s0: the material sampler
#pragma pack_matrix(row_major)
#define GBUFFER_SKIP_PEROBJECT
#include "gbuffer_common.hlsli"

// A6: GBUFFER_BINDLESS=1 -- the material textures come through ResourceDescriptorHeap[] by the
// indices SurfaceParams carries, and b0 (the group LOD) is ROOT CONSTANTS instead of a CBV: that is
// what lets one ExecuteIndirect span every group -- the command signature writes the constants and
// the b2 address per command ({VBV, IBV, CBV(b2), CONSTANT(b0), DRAW_INDEXED}).
#ifndef GBUFFER_BINDLESS
#define GBUFFER_BINDLESS 0
#endif

#if !GBUFFER_BINDLESS
Texture2D gAlbedo : register(t0);
Texture2D gMR : register(t1); // R=metal, G=rough
Texture2D gNormalMap : register(t2); // tangent-space, +Z
#endif
StructuredBuffer<InstancePerObject> gInstances : register(t3);
StructuredBuffer<float> gCasterFade : register(t4); // > 0: the caster fades between lod and lod + 1
StructuredBuffer<uint>  gCasterLod  : register(t5); // the caster's camera tier (bit 7 = chunk EXACT)
SamplerState gSmp : register(s0);

cbuffer IndirectDraw : register(b0)
{
    uint gGroupLod;   // the LOD of the virtual group this ExecuteIndirect draws
    uint3 gIndirectPad;
};

cbuffer SurfaceParams : register(b2)
{
    float3 subsurfaceColor;
    float transmissionStrength;
    float ambientOcclusion;
    float indirectSpecularScale;
    float transmissionAlbedoPower;
    float transmissionNormalWeight;
    float4 terrainTiling;
    float4 terrainEdgeParams;
    uint4 texIndices; // A6: albedo, MR, normal slots in the bindless heap (read under GBUFFER_BINDLESS)
};

#if GBUFFER_BINDLESS
#define GBUFFER_INDIRECT_RS \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(num32BitConstants=4, b0)," \
    "CBV(b1)," \
    "CBV(b2)," \
    "DescriptorTable(SRV(t3, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
#else
#define GBUFFER_INDIRECT_RS \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)," \
    "CBV(b0)," \
    "CBV(b1)," \
    "CBV(b2)," \
    "DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(SRV(t3, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
#endif

static const uint kCasterLodMask = 0x0Fu;

struct VSInIndirect
{
    float3 P : POSITION;
    float3 N : NORMAL;
    float4 T : TANGENT;
    float2 UV : TEXCOORD0;
    float4 WIND : COLOR0;
    uint casterId : CASTERID; // slot 1, per instance: the camera visible list
};

// VSOut plus what the PS needs to find the object's material values: its caster id, and this draw's crossfade.
struct VSOutIndirect
{
    VSOut b;
    nointerpolation uint casterId : TEXCOORD6;
    nointerpolation float lodFade : TEXCOORD7;
};

[RootSignature(GBUFFER_INDIRECT_RS)]
VSOutIndirect VSMain(VSInIndirect i)
{
    const InstancePerObject o = gInstances[i.casterId];
    VSOutIndirect v;
    v.b = BaseVS(i.P, o.world, o.prevWorld, viewProj, i.N, i.T, i.UV, o.objectId,
                 i.WIND, o.windStrength, o.windFoliage, o.windTrunkStiff, o.windLeafScale);
    v.casterId = i.casterId;
    // The crossfade: a fading caster sits in two buckets, its own tier (fading OUT, -f) and the
    // next (fading IN, +f) -- RenderableObject::Render's pass 0 / pass 1.
    const float f = gCasterFade[i.casterId];
    const uint lod = gCasterLod[i.casterId] & kCasterLodMask;
    v.lodFade = (f > 0.0f) ? ((gGroupLod == lod) ? -f : f) : 0.0f;
    return v;
}

[RootSignature(GBUFFER_INDIRECT_RS)]
PSOut PSMain(VSOutIndirect v, bool isFrontFace : SV_IsFrontFace)
{
#if GBUFFER_BINDLESS
    Texture2D gAlbedo = ResourceDescriptorHeap[texIndices.x];
    Texture2D gMR = ResourceDescriptorHeap[texIndices.y];
    Texture2D gNormalMap = ResourceDescriptorHeap[texIndices.z];
#endif
    const VSOut i = v.b;
    const InstancePerObject o = gInstances[v.casterId];
    const float4 baseColor = o.baseColor;
    const float2 metalRough = o.metalRough;
    const float alphaCutoff = o.alphaCutoff;
    const float mrMultiply = o.mrMultiply;
    const float4 texOffsScale = o.texOffsScale;
    const float4 texFlags = o.texFlags;
    const float3 emissive = o.emissive;

    LodFadeClip(i.H.xy, v.lodFade);
    AlphaTestClip(gAlbedo, gSmp, i.UV, texOffsScale, terrainTiling, terrainEdgeParams,
                  baseColor.a, alphaCutoff);

    float3 NNorm = normalize(i.NWS);
    if (!isFrontFace) { NNorm = -NNorm; }

    float3 albedo;
    float2 mr;
    float3 N = NNorm;
    FetchShadingValuesP(gAlbedo, gMR, gNormalMap, gSmp, i.UV, i.TWS, texOffsScale, texFlags,
                        terrainTiling, terrainEdgeParams, albedo, mr, N);

    // baseColor multiplies the texture in BOTH layouts. An imported material's factors are baked
    // into its DDS, so its tint is 1 unless someone authored one (material "tint", object
    // "baseColor") -- and then they mean it: a lerp that dropped it made the tint a control that
    // did nothing on every textured imported mesh, while RT reflections (rt_reflect_common.hlsli)
    // already multiplied it.
    albedo = texFlags.x > 0.5 ? albedo * baseColor.rgb : baseColor.rgb;
    float2 texturedMR = lerp(mr, mr * metalRough.xy, mrMultiply);
    mr = lerp(metalRough.xy, texturedMR, texFlags.y);
    if (texFlags.z < 0.5)
    {
        N = NNorm;
    }
    float2 currUv = ClipToUV(i.clipH);
    float2 prevUv = ClipToUV(i.prevH);
    float2 motion = currUv - prevUv;

    return FinalizeGBuffer(albedo, mr, N, emissive, subsurfaceColor, transmissionStrength,
                           ambientOcclusion, indirectSpecularScale, transmissionAlbedoPower,
                           transmissionNormalWeight, motion, i.objectId);
}
