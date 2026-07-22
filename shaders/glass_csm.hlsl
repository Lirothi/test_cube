#define GLASS_CSM_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0)"
#pragma pack_matrix(row_major)
#include "utils.hlsli"

cbuffer PerObject : register(b0)
{
    float4x4 world;
    float4x4 view;
    float4x4 proj;
};

struct VSIn
{
    float3 P : POSITION;
};

struct VSOut
{
    float4 posH : SV_POSITION;
};

[RootSignature(GLASS_CSM_RS)]
VSOut VSMain(VSIn input)
{
    VSOut o;
    float4 worldPos = mul(float4(input.P, 1.0f), world);
    o.posH = mul(mul(worldPos, view), proj);
    return o;
}

[RootSignature(GLASS_CSM_RS)]
void PSMain() {}
