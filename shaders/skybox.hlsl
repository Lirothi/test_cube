// RootSignature: CBV(b0) TABLE(SRV(t0)) TABLE(SAMPLER(s0))
#pragma pack_matrix(row_major)
#include "utils.hlsl"

cbuffer PerFrame : register(b0)
{
    float4x4 view;      // обычная view
    float4x4 proj;
    float4x4 invView;
}

struct VSIn {
    float3 pos : POSITION;
};

struct VSOut {
    float4 pos : SV_Position;
    float3 dir : TEXCOORD0; // направление выборки
};

VSOut VSMain(VSIn i)
{
    // 1) позиция: view без трансляции, затем proj, и толкаем на дальнюю плоскость
    float4x4 v = view;
    v._41 = 0.0;
    v._42 = 0.0;
    v._43 = 0.0; // убрать translate (для HLSL именно _41.._43)
    VSOut o;
    float4 viewPos = mul(float4(i.pos, 1.0), v);
    o.pos = mul(viewPos, proj);
    o.pos.z = o.pos.w;

    //float3 dirWS = mul(viewPos.xyz, (float3x3) invView).xyz;
    float3 dirWS = i.pos;
    o.dir = normalize(dirWS);
    
    return o;
}

TextureCube sky : register(t0);
SamplerState samLinear : register(s0);

float4 PSMain(VSOut i) : SV_Target
{
    float3 c = sky.Sample(samLinear, i.dir).rgb;
    //c = SRGBToLinear(c);
    return float4(c, 1.0);
}