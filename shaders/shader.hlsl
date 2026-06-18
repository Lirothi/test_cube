#define SHADER_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0)"
#pragma pack_matrix(row_major)

cbuffer CB : register(b0)
{
    matrix modelViewProj;
};

struct VSInput
{
    float3 position : POSITION;
    float4 color : COLOR;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

[RootSignature(SHADER_RS)]
PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = mul(float4(input.position, 1.0f), modelViewProj);
    output.color = input.color;
    return output;
}

[RootSignature(SHADER_RS)]
float4 PSMain(PSInput input) : SV_TARGET
{
    return input.color;
}
