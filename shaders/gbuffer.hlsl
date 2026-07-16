#pragma pack_matrix(row_major)
#include "gbuffer_common.hlsl"

Texture2D gAlbedo : register(t0);
Texture2D gMR : register(t1); // R=metal, G=rough
Texture2D gNormalMap : register(t2); // tangent-space, +Z
SamplerState gSmp : register(s0);

#define GBUFFER_RS \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)," \
    "CBV(b0)," \
    "CBV(b1)," \
    "DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

[RootSignature(GBUFFER_RS)]
VSOut VSMain(VSIn i)
{
    return BaseVS(i.P, world, prevWorld, viewProj, i.N, i.T, i.UV, objectId);
}

[RootSignature(GBUFFER_RS)]
PSOut PSMain(VSOut i, bool isFrontFace : SV_IsFrontFace)
{
    AlphaTestClip(gAlbedo, gSmp, i.UV, texOffsScale, baseColor.a, alphaCutoff);

    // Two-sided foliage (CULL_NONE fronds): a backface reuses the front vertex normal, which points
    // away from the camera → wrong diffuse (dark) + spurious specular. Flip it to face out of the
    // visible side. No-op for single-sided meshes (backfaces are culled, isFrontFace always true).
    float3 NNorm = normalize(i.NWS);
    if (!isFrontFace) { NNorm = -NNorm; }

    float3 albedo;
    float2 mr;
    float3 N = NNorm;
    FetchShadingValues(gAlbedo, gMR, gNormalMap, gSmp, i.UV, i.TWS, albedo, mr, N);

#if MR_LAYOUT_GLTF
    // Raw (unimported) glTF preview: baseColorFactor / metallic / roughness factors MULTIPLY the
    // texture channels (falling back to the factor alone where the channel's texture is absent).
    // IMPORTED assets never hit this branch: the importer bakes the factors into the DDS and the
    // repacked MR resolves to the engine layout below.
    albedo = texFlags.x > 0.5 ? albedo * baseColor.rgb : baseColor.rgb;
    mr     = texFlags.y > 0.5 ? mr * metalRough.xy     : metalRough.xy;
#else
    albedo = lerp(baseColor.rgb, albedo, texFlags.x);
    mr = lerp(metalRough.xy, mr, texFlags.y);
#endif
    if (texFlags.z < 0.5)
    {
        N = NNorm;
    }
    float2 currUv = ClipToUV(i.clipH);
    float2 prevUv = ClipToUV(i.prevH);
    float2 motion = currUv - prevUv;

    return FinalizeGBuffer(albedo, mr, N, float4(emissive, 0), motion, i.objectId);
}
