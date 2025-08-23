// RootSignature: CBV(b0) TABLE(SRV(t0) SRV(t1)) TABLE(SAMPLER(s0))
#pragma pack_matrix(row_major)

#include "utils.hlsl"

struct InstanceData
{
    row_major float4x4 world;
    float rotationY;
    float3 pad_;
};

struct VSInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD0;
    uint instanceID : SV_InstanceID;
};

struct VSOutput {
    float4 position : SV_POSITION;
    float4 color    : COLOR;
    float2 uv       : TEXCOORD0;
    float3 nrm : TEXCOORD1;
};

Texture2D gTex : register(t1);
SamplerState gSmp : register(s0);

StructuredBuffer<InstanceData> gInstances : register(t0);
cbuffer ViewProjCB : register(b0) {
    float4x4 viewProj;
}

VSOutput VSMain(VSInput input) {
    VSOutput o;

    float4x4 world = gInstances[input.instanceID].world;

    // row-vector стиль: posW = pos * World; posH = posW * ViewProj;
    float4 posW = mul(float4(input.position, 1.0), world);
    o.position = mul(posW, viewProj);
    //o.nrm = input.normal;
    o.nrm = normalize(mul(input.normal, (float3x3) world)); // нормаль в мировых координатах
    o.color = float4(1, 1, 1, 1);
    o.uv = input.uv;
    return o;
}

struct PSOut
{
    float4 RT0 : SV_Target0; // Albedo.rgb + Metal
    float4 RT1 : SV_Target1; // NormalOcta.rg + Rough + pad
    float4 RT2 : SV_Target2; // Emissive.rgb
};

PSOut PSMain(VSOutput input)
{
    PSOut o;
    float rough = 0.4f;
    float metal = 0.95f;
    float3 n = normalize(input.nrm);
    float3 c = float4(0.5 + 0.5 * abs(n), 1.0f); // яркая раскраска по нормали для проверки
    c = gTex.Sample(gSmp, input.uv).rgb * c * input.color.rgb;
    o.RT0 = float4(c, PackRM(rough, metal));
    float3 n01 = n * 0.5 + 0.5;
    o.RT1 = float4(n01, 1);
    o.RT2 = float4(0, 0, 0, 0); // emissive подключишь позже
    return o;
}