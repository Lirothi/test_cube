// RootSignature: CBV(b0) TABLE(SRV(t0)) TABLE(SAMPLER(s0))
#pragma pack_matrix(row_major)

cbuffer CB : register(b0)
{
    matrix modelViewProj;
};

//struct VSInput
//{
//    float3 position : POSITION;
//    float4 color : COLOR;
//};

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float3 nrm : TEXCOORD1;
};

Texture2D gTex : register(t0);
SamplerState gSmp : register(s0);

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = mul(float4(input.position, 1.0f), modelViewProj);
    //output.position = mul(modelViewProj, float4(input.position, 1.0f));
    output.uv = input.uv;
    output.nrm = input.normal;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 n = pow(input.nrm, 1.0f);
    float3 c = 0.5 + 0.5 * normalize(input.nrm); // яркая раскраска по нормали для проверки
    return gTex.Sample(gSmp, input.uv) * float4(c, 1.0);
}