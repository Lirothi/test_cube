// Occlusion plan S4: the GPU-driven G-buffer. One ExecuteIndirect per virtual group (mesh submesh
// x camera LOD); every instance of the draw is a caster id the input assembler delivers from the
// camera visible list (slot 1, per-instance -- the shadow_indirect_csm.hlsl shape), and the
// per-object data comes out of the registry's per-caster record (t3, the SAME buffer the shadow
// VS reads). The pixel shader is gbuffer.hlsl's line for line; the per-object values it used to
// read from `cbuffer PerObject` (b0) travel from the VS as flat attributes instead, so the PS never
// touches the instance buffer (which therefore stays in its NON_PIXEL resting state).
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

Texture2D gAlbedo : register(t0);
Texture2D gMR : register(t1); // R=metal, G=rough
Texture2D gNormalMap : register(t2); // tangent-space, +Z
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
};

#define GBUFFER_INDIRECT_RS \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)," \
    "CBV(b0)," \
    "CBV(b1)," \
    "CBV(b2)," \
    "DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(SRV(t3, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

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

// VSOut plus the per-object material values the PS needs, flat.
struct VSOutIndirect
{
    VSOut b;
    nointerpolation float4 baseColor : TEXCOORD6;
    nointerpolation float4 mrCut : TEXCOORD7;      // metalRough.xy, alphaCutoff, mrMultiply
    nointerpolation float4 texOffsScale : TEXCOORD8;
    nointerpolation float4 texFlags : TEXCOORD9;
    nointerpolation float4 emissiveFade : TEXCOORD10; // emissive.xyz, lodFade
};

[RootSignature(GBUFFER_INDIRECT_RS)]
VSOutIndirect VSMain(VSInIndirect i)
{
    const InstancePerObject o = gInstances[i.casterId];
    VSOutIndirect v;
    v.b = BaseVS(i.P, o.world, o.prevWorld, viewProj, i.N, i.T, i.UV, o.objectId,
                 i.WIND, o.windStrength, o.windFoliage, o.windTrunkStiff, o.windLeafScale);
    v.baseColor = o.baseColor;
    v.mrCut = float4(o.metalRough, o.alphaCutoff, o.mrMultiply);
    v.texOffsScale = o.texOffsScale;
    v.texFlags = o.texFlags;
    // The crossfade: a fading caster sits in two buckets, its own tier (fading OUT, -f) and the
    // next (fading IN, +f) -- RenderableObject::Render's pass 0 / pass 1.
    const float f = gCasterFade[i.casterId];
    const uint lod = gCasterLod[i.casterId] & kCasterLodMask;
    const float fade = (f > 0.0f) ? ((gGroupLod == lod) ? -f : f) : 0.0f;
    v.emissiveFade = float4(o.emissive, fade);
    return v;
}

[RootSignature(GBUFFER_INDIRECT_RS)]
PSOut PSMain(VSOutIndirect v, bool isFrontFace : SV_IsFrontFace)
{
    const VSOut i = v.b;
    const float4 baseColor = v.baseColor;
    const float2 metalRough = v.mrCut.xy;
    const float alphaCutoff = v.mrCut.z;
    const float mrMultiply = v.mrCut.w;
    const float4 texOffsScale = v.texOffsScale;
    const float4 texFlags = v.texFlags;
    const float3 emissive = v.emissiveFade.xyz;

    LodFadeClip(i.H.xy, v.emissiveFade.w);
    AlphaTestClip(gAlbedo, gSmp, i.UV, texOffsScale, terrainTiling, terrainEdgeParams,
                  baseColor.a, alphaCutoff);

    float3 NNorm = normalize(i.NWS);
    if (!isFrontFace) { NNorm = -NNorm; }

    float3 albedo;
    float2 mr;
    float3 N = NNorm;
    FetchShadingValuesP(gAlbedo, gMR, gNormalMap, gSmp, i.UV, i.TWS, texOffsScale, texFlags,
                        terrainTiling, terrainEdgeParams, albedo, mr, N);

#if MR_LAYOUT_GLTF
    albedo = texFlags.x > 0.5 ? albedo * baseColor.rgb : baseColor.rgb;
#else
    albedo = lerp(baseColor.rgb, albedo, texFlags.x);
#endif
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
