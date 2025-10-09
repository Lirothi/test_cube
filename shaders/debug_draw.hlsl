// RootSignature: CBV(b0)
#pragma pack_matrix(row_major)

cbuffer DebugDrawCB : register(b0)
{
    float4x4 modelViewProj;
    float4 color;
};

struct VSInput
{
    float3 position : POSITION;
    float4 color    : COLOR;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float4 color    : COLOR;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = mul(float4(input.position, 1.0f), modelViewProj);
    output.color = input.color * color;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    return input.color;
}
