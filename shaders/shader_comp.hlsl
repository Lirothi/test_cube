// shader.hlsl
struct InstanceData
{
    float4x4 world;
    float rotationY;
    float3 pad_;
};

struct VSInput {
    float3 position : POSITION;
    float4 color    : COLOR;
    uint instanceID : SV_InstanceID;
};

struct VSOutput {
    float4 position : SV_POSITION;
    float4 color    : COLOR;
};

StructuredBuffer<InstanceData> gInstances : register(t0);
cbuffer ViewProjCB : register(b0) {
    float4x4 viewProj;
}

VSOutput VSMain(VSInput input) {
    VSOutput o;
    float4x4 world = gInstances[input.instanceID].world;
    o.position = mul(mul(float4(input.position, 1.0), world), viewProj);
    o.color = input.color;
    return o;
}

float4 PSMain(VSOutput input) : SV_TARGET {
    return input.color;
}