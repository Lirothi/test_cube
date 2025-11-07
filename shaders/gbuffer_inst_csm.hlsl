// RootSignature: CBV(b0) TABLE(SRV(t0))
// Use the shared b0 from gbuffer_common
#pragma pack_matrix(row_major)
#include "gbuffer_common.hlsl"

struct InstanceData
{
    row_major float4x4 world;
    row_major float4x4 prevWorld;
    float rotationY;
    float3 _pad_;
};

StructuredBuffer<InstanceData> gInstances : register(t0);

struct VSOutD { float4 H : SV_POSITION; };

VSOutD VSMain(VSInInst i)
{
    VSOutD o;
    float4x4 w = mul(gInstances[i.IID].world, world);
    o.H = TransformPositionH(i.P, w, viewProj);
    return o;
}

// Depth-only — pixel shader can remain empty
void PSMain(VSOutD i) { }