// RootSignature: TABLE(SRV(t0))
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

VSOutput VSMain(VSInput input, uint instanceID : SV_InstanceID)
{
    VSOutput output;
    InstanceData inst = gInstances[instanceID];
    output.position = mul(float4(input.position, 1.0f), inst.mvp);
    output.color = input.vertexColor * inst.color;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    return input.color;
}
