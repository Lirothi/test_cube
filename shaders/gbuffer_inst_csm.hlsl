#define GBUFFER_INST_CSM_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0), CBV(b1), DescriptorTable(SRV(t0, flags=DATA_VOLATILE))"
// Use the shared b0 (per-object) + b1 (per-view: viewProj) from gbuffer_common
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

[RootSignature(GBUFFER_INST_CSM_RS)]
VSOutD VSMain(VSInInst i)
{
    VSOutD o;
    float4x4 w = mul(gInstances[i.IID].world, world);
    o.H = TransformPositionH(i.P, w, viewProj);
    return o;
}

// Depth-only — pixel shader can remain empty
[RootSignature(GBUFFER_INST_CSM_RS)]
void PSMain(VSOutD i) { }
