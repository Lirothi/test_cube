#define SELECTION_STENCIL_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0)"
#pragma pack_matrix(row_major)

cbuffer SelectionStencilParams : register(b0)
{
    float4x4 world;
    float4x4 viewProj;
};

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
};

[RootSignature(SELECTION_STENCIL_RS)]
VSOut VSMain(VSIn input)
{
    VSOut output;
    const float4 worldPos = mul(float4(input.P, 1.0f), world);
    output.H = mul(worldPos, viewProj);
    return output;
}

[RootSignature(SELECTION_STENCIL_RS)]
void PSMain(VSOut input)
{
}
