#define GBUFFER_INST_CSM_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0), CBV(b1), DescriptorTable(SRV(t0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"
// Use the shared b0 (per-object) + b1 (per-view: viewProj) from gbuffer_common
#pragma pack_matrix(row_major)
#include "gbuffer_common.hlsli"

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
    // W5: mirror gbuffer_inst.hlsl's BaseVS call — windStrength comes from the batch-shared b0
    // (0 for the instanced cloud today, so this is a no-op unless an instanced object is flagged).
    float4 wp = mul(float4(i.P, 1.0f), w);
    wp.xyz += ApplyWindWS(i.P, wp.xyz, w, windStrength, windInvHeight, windFoliage,
                          windTrunkStiff, windGustMul, windTime);
    o.H = mul(wp, viewProj);
    return o;
}

// Depth-only — pixel shader can remain empty
[RootSignature(GBUFFER_INST_CSM_RS)]
void PSMain(VSOutD i) { }
