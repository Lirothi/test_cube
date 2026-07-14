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
PSOut PSMain(VSOut i)
{
    AlphaTestClip(gAlbedo, gSmp, i.UV, texOffsScale, baseColor.a, alphaCutoff);

    float3 NNorm = normalize(i.NWS);

    float3 albedo;
    float2 mr;
    float3 N = NNorm;
    FetchShadingValues(gAlbedo, gMR, gNormalMap, gSmp, i.UV, i.TWS, albedo, mr, N);

#if MR_LAYOUT_GLTF
    // glTF: baseColorFactor / metallic / roughness factors MULTIPLY the texture channels
    // (falling back to the factor alone where the channel's texture is absent).
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

    //albedo = N * 0.5 + 0.5;
    return FinalizeGBuffer(albedo, mr, N, float4(0, 0, 0, 0), motion, i.objectId);
}
