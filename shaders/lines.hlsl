#define LINES_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0)"
#pragma pack_matrix(row_major)

cbuffer MVP : register(b0)
{
    float4x4 modelViewProj;
};

struct VSIn {
    float3 position : POSITION;
    float4 color    : COLOR;
};

struct VSOut {
    float4 position : SV_POSITION;
    float4 color    : COLOR;
};

[RootSignature(LINES_RS)]
VSOut VSMain(VSIn i)
{
    VSOut o;
    o.position = mul(float4(i.position, 1.0f), modelViewProj);
    o.color = i.color;
    return o;
}

[RootSignature(LINES_RS)]
float4 PSMain(VSOut i) : SV_Target
{
    return i.color; // alpha comes from the vertex
}
