// RootSignature: CBV(b0) TABLE(SRV(t0) SRV(t1) SRV(t2) SRV(t3)) TABLE(SAMPLER(s0))
#pragma pack_matrix(row_major)

#include "gbuffer_common.hlsl"

// Под инстансинг: world на инстанс
struct InstanceData
{
    row_major float4x4 world; // должен совпадать с CPU-структурой
    float rotationY; // не используем здесь, но оставим для совместимости
    float3 _pad_;
};

StructuredBuffer<InstanceData> gInstances : register(t0);
Texture2D gAlbedo : register(t1);
Texture2D gMR : register(t2);
Texture2D gNormalMap : register(t3);
SamplerState gSmp : register(s0);

VSOut VSMain(VSInInst i)
{
    return BaseVS(i.P, mul(world, gInstances[i.IID].world), view, proj, i.N, i.UV);
}

PSOut PSMain(VSOut i)
{
    PSOut o;

    float3 albedoTex = gAlbedo.Sample(gSmp, i.UV).rgb;
    float2 mrTex = gMR.Sample(gSmp, i.UV).rg;

    float3 albedo = lerp(baseColor.rgb, albedoTex, texFlags.x);
    float2 mr = lerp(metalRough.xy, mrTex.rg, texFlags.y);

    albedo *= NrmTo01(normalize(i.NWS));
    o = BasePS(albedo, mr, float4(0, 0, 0, 0), i.NWS);
    return o;
}