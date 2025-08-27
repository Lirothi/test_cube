// RootSignature: CBV(b0) TABLE(SRV(t0) SRV(t1) SRV(t2) SRV(t3)) TABLE(SAMPLER(s0))
#pragma pack_matrix(row_major)
#include "gbuffer_common.hlsl"

// world на инстанс
struct InstanceData
{
    row_major float4x4 world;
    float rotationY;
    float3 _pad_;
};

StructuredBuffer<InstanceData> gInstances : register(t0);
Texture2D gAlbedo : register(t1);
Texture2D gMR : register(t2);
Texture2D gNormalMap : register(t3);
SamplerState gSmp : register(s0);

VSOut VSMain(VSInInst i)
{
    float4x4 w = mul(gInstances[i.IID].world, world);
    return BaseVS(i.P, w, view, proj, i.N, i.T, i.UV);
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

    return FinalizeGBuffer(albedo, mr, N, float4(0, 0, 0, 0));
}