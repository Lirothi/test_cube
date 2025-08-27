// RootSignature: CBV(b0) TABLE(SRV(t0) SRV(t1) SRV(t2)) TABLE(SAMPLER(s0))
#pragma pack_matrix(row_major)
#include "gbuffer_common.hlsl"

Texture2D gAlbedo : register(t0);
Texture2D gMR : register(t1); // R=metal, G=rough
Texture2D gNormalMap : register(t2); // tangent-space, +Z
SamplerState gSmp : register(s0);

VSOut VSMain(VSIn i)
{
    return BaseVS(i.P, world, view, proj, i.N, i.T, i.UV);
}

PSOut PSMain(VSOut i)
{
    float3 NNorm = normalize(i.NWS);
    
    float3 albedo;
    float2 mr;
    float3 N = NNorm;
    FetchShadingValues(gAlbedo, gMR, gNormalMap, gSmp, i.UV, i.TWS, albedo, mr, N);

    albedo = lerp(baseColor.rgb, albedo, texFlags.x);
    mr = lerp(metalRough.xy, mr, texFlags.y);
    if (texFlags.z < 0.5)
    {
        N = NNorm;
    }
    //albedo = N * 0.5 + 0.5;
    return FinalizeGBuffer(albedo, mr, N, float4(0, 0, 0, 0));
}