// RootSignature: CBV(b0) TABLE(SRV(t0) SRV(t1) SRV(t2)) TABLE(SAMPLER(s0))
#pragma pack_matrix(row_major)

#include "utils.hlsl"

cbuffer PerObject : register(b0)
{
    float4x4 world;
    float4x4 view;
    float4x4 proj;
    float4 baseColor; // fallback для альбедо
    float2 mr; // x = metallic, y = roughness (fallback)
    float2 texFlags; // x=useAlbedo(0/1), y=useMR(0/1)
};

Texture2D gAlbedo : register(t0);
Texture2D gMR : register(t1);
Texture2D gNormalMap : register(t2);
SamplerState gSmp : register(s0);

struct VSIn
{
    float3 P : POSITION;
    float3 N : NORMAL;
    float4 T : TANGENT;
    float2 UV : TEXCOORD0;
};
struct VSOut
{
    float4 H : SV_POSITION;
    float3 N : NORMAL_VS;
    float2 UV : TEXCOORD0;
};

VSOut VSMain(VSIn i)
{
    VSOut o;
    float4 wp = mul(float4(i.P, 1), world);
    float4 vp = mul(wp, view);
    o.H = mul(vp, proj);
    float3 nWS = mul(i.N, (float3x3) world);
    o.N = nWS;
    o.UV = i.UV;
    return o;
}

struct PSOut
{
    float4 RT0 : SV_Target0; // Albedo.rgb + Metal
    float4 RT1 : SV_Target1; // NormalOcta.rg + Rough + pad
    float4 RT2 : SV_Target2; // Emissive.rgb
};

PSOut PSMain(VSOut i)
{
    PSOut o;

    // флаги наличия текстур
    const float useAlbedo = texFlags.x;
    const float useMR = texFlags.y;

    // семплы (если SRV нулевой, в D3D12 вернутся нули — это ок для наших lerp'ов)
    const float3 albedoTex = gAlbedo.Sample(gSmp, i.UV).rgb;
    const float2 mrTex = gMR.Sample(gSmp, i.UV).rg;

    float3 albedo = lerp(baseColor.rgb, albedoTex, useAlbedo);
    albedo *= normalize(i.N) * 0.5 + 0.5;
    //const float3 albedo = float3(normalize(i.N) * 0.5 + 0.5);
    const float metal = lerp(mr.x, mrTex.r, useMR);
    const float rough = lerp(mr.y, mrTex.g, useMR);

    o.RT0 = float4(albedo, PackRM(rough, metal));
    //o.RT1 = float4(no, saturate(rough), 0.0);

    float3 n01 = normalize(i.N) * 0.5 + 0.5; // [-1..1] -> [0..1]
    o.RT1 = float4(n01, 1);

    o.RT2 = float4(0, 0, 0, 0); // emissive подключишь позже
    //o.RT2 = float4(i.N, 0);
    return o;
}