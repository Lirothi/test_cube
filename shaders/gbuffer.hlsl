#pragma pack_matrix(row_major)
#include "gbuffer_common.hlsli"

// Texture streaming plan A6: GBUFFER_BINDLESS=1 reaches the three material textures through SM6.6
// ResourceDescriptorHeap[] by the indices SurfaceParams carries (texIndices), so the t0..t2 table
// leaves the root signature; the sampler stays a table (static per material, DLSS-biased).
#ifndef GBUFFER_BINDLESS
#define GBUFFER_BINDLESS 0
#endif

#if !GBUFFER_BINDLESS
Texture2D gAlbedo : register(t0);
Texture2D gMR : register(t1); // R=metal, G=rough
Texture2D gNormalMap : register(t2); // tangent-space, +Z
#endif
SamplerState gSmp : register(s0);

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
#define GBUFFER_RS \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "CBV(b0)," \
    "CBV(b1)," \
    "CBV(b2)," \
    "DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
#else
#define GBUFFER_RS \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)," \
    "CBV(b0)," \
    "CBV(b1)," \
    "CBV(b2)," \
    "DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
#endif

[RootSignature(GBUFFER_RS)]
VSOut VSMain(VSIn i)
{
    return BaseVS(i.P, world, prevWorld, viewProj, i.N, i.T, i.UV, objectId,
                  i.WIND, windStrength, windFoliage, windTrunkStiff, windLeafScale);
}

[RootSignature(GBUFFER_RS)]
PSOut PSMain(VSOut i, bool isFrontFace : SV_IsFrontFace)
{
#if GBUFFER_BINDLESS
    Texture2D gAlbedo = ResourceDescriptorHeap[texIndices.x];
    Texture2D gMR = ResourceDescriptorHeap[texIndices.y];
    Texture2D gNormalMap = ResourceDescriptorHeap[texIndices.z];
#endif
    LodFadeClip(i.H.xy, lodFade);
    AlphaTestClip(gAlbedo, gSmp, i.UV, texOffsScale, terrainTiling, terrainEdgeParams,
                  baseColor.a, alphaCutoff);

    // Two-sided foliage (CULL_NONE fronds): a backface reuses the front vertex normal, which points
    // away from the camera → wrong diffuse (dark) + spurious specular. Flip it to face out of the
    // visible side. No-op for single-sided meshes (backfaces are culled, isFrontFace always true).
    float3 NNorm = normalize(i.NWS);
    if (!isFrontFace) { NNorm = -NNorm; }

    float3 albedo;
    float2 mr;
    float3 N = NNorm;
    FetchShadingValues(gAlbedo, gMR, gNormalMap, gSmp, i.UV, i.TWS, terrainTiling,
                       terrainEdgeParams, albedo, mr, N);

    // baseColor multiplies the texture in BOTH layouts. An imported material's factors are baked
    // into its DDS, so its tint is 1 unless someone authored one (material "tint", object
    // "baseColor") -- and then they mean it: a lerp that dropped it made the tint a control that
    // did nothing on every textured imported mesh, while RT reflections (rt_reflect_common.hlsli)
    // already multiplied it.
    // MR multiplication is selected by mrMultiply below; raw glTF defaults it on, while imported
    // DDS bakes the factors.
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
