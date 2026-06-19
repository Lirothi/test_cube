#define DEBUG_DRAW_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), DescriptorTable(SRV(t0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"
#pragma pack_matrix(row_major)

struct VSInput
{
    float3 position    : POSITION;
    float4 vertexColor : COLOR0;
};

struct InstanceData
{
    row_major float4x4 mvp;
    float4 color;
};

StructuredBuffer<InstanceData> gInstances : register(t0);

struct VSOutput
{
    float4 position : SV_POSITION;
    float4 color    : COLOR;
};

[RootSignature(DEBUG_DRAW_RS)]
VSOutput VSMain(VSInput input, uint instanceID : SV_InstanceID)
{
    VSOutput output;
    InstanceData inst = gInstances[instanceID];
    output.position = mul(float4(input.position, 1.0f), inst.mvp);
    output.color = input.vertexColor * inst.color;
    return output;
}

[RootSignature(DEBUG_DRAW_RS)]
float4 PSMain(VSOutput input) : SV_TARGET
{
    return input.color;
}
