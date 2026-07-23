#define GBUFFER_INST_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0), CBV(b1), CBV(b2), CBV(b3), DescriptorTable(SRV(t0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
#pragma pack_matrix(row_major)
#include "gbuffer_common.hlsli"

// Per-instance world matrix
struct InstanceData
{
    row_major float4x4 world;
    row_major float4x4 prevWorld;
    float rotationY;
    float3 _pad_;
};

StructuredBuffer<InstanceData> gInstances : register(t0);
Texture2D gAlbedo : register(t1);
Texture2D gMR : register(t2);
Texture2D gNormalMap : register(t3);
SamplerState gSmp : register(s0);

// Step 6 per-instance LOD: the cloud is drawn one range per LOD tier. gInstanceBase is the
// tier's start offset into gRemap; gRemap maps draw-instance -> real instance index (the
// instance buffer stays in grid order so prevWorld is stable). gRemap[64] = up to 256 insts.
cbuffer InstDraw : register(b2)
{
    uint gInstanceBase;
    uint3 _instDrawPad;
    float3 subsurfaceColor;
    float transmissionStrength;
    float ambientOcclusion;
    float indirectSpecularScale;
    float transmissionAlbedoPower;
    float transmissionNormalWeight;
};
cbuffer InstRemap : register(b3) { uint4 gRemap[64]; };

[RootSignature(GBUFFER_INST_RS)]
VSOut VSMain(VSInInst i)
{
    const uint slot = gInstanceBase + i.IID;
    const uint ri = gRemap[slot >> 2u][slot & 3u];
    float4x4 w = mul(gInstances[ri].world, world);
    float4x4 pw = mul(gInstances[ri].prevWorld, prevWorld);
    // windStrength comes from the batch-shared b0 (0 for the instanced cloud => no sway).
    return BaseVS(i.P, w, pw, viewProj, i.N, i.T, i.UV, objectId,
                  i.WIND, windStrength, windFoliage, windTrunkStiff, windLeafScale);
}

[RootSignature(GBUFFER_INST_RS)]
PSOut PSMain(VSOut i, bool isFrontFace : SV_IsFrontFace)
{
    AlphaTestClip(gAlbedo, gSmp, i.UV, texOffsScale, baseColor.a, alphaCutoff);

    // Two-sided foliage: flip a backface normal to face out of the visible side (see gbuffer.hlsl).
    float3 NNorm = normalize(i.NWS);
    if (!isFrontFace) { NNorm = -NNorm; }

    float3 albedo;
    float2 mr;
    float3 N = NNorm;
    FetchShadingValues(gAlbedo, gMR, gNormalMap, gSmp, i.UV, i.TWS, albedo, mr, N);

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
