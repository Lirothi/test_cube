// RootSignature: CBV(b0) TABLE(SRV(t0) SRV(t1) SRV(t2)) TABLE(SAMPLER(s0))
#pragma pack_matrix(row_major)

#include "gbuffer_common.hlsl"

Texture2D gAlbedo : register(t0);
Texture2D gMR : register(t1);
Texture2D gNormalMap : register(t2); // на будущее
SamplerState gSmp : register(s0);

VSOut VSMain(VSIn i)
{
    return BaseVS(i.P, world, view, proj, i.N, i.UV);
}

PSOut PSMain(VSOut i)
{
    PSOut o;
    
    // выбор материала
    float3 albedoTex = gAlbedo.Sample(gSmp, i.UV).rgb;
    float2 mrTex = gMR.Sample(gSmp, i.UV).rg;

    float3 albedo = lerp(baseColor.rgb, albedoTex, texFlags.x);
    float2 mr = lerp(metalRough.xy, mrTex.rg, texFlags.y);

    albedo *= NrmTo01(normalize(i.NWS));
    o = BasePS(albedo, mr, float4(0, 0, 0, 0), i.NWS);

    return o;
}