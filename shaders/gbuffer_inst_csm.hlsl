// RootSignature: CBV(b0) TABLE(SRV(t0))
// Используем твой общий b0 из gbuffer_common: world/view/proj
#pragma pack_matrix(row_major)
#include "gbuffer_common.hlsl"

struct InstanceData
{
    row_major float4x4 world;
    float rotationY;
    float3 _pad_;
};

StructuredBuffer<InstanceData> gInstances : register(t0);

struct VSOutD { float4 H : SV_POSITION; };

VSOutD VSMain(VSInInst i)
{
    VSOutD o;
    float4x4 w = mul(gInstances[i.IID].world, world);
    o.H = TransformPositionH(i.P, w, view, proj);
    return o;
}

// depth-only — PS можно оставить пустым
void PSMain(VSOutD i) { }