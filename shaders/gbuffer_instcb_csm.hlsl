// Depth-only shadow variant of the GBuffer auto-instancing path (Step 4). Reads only
// inst[SV_InstanceID].world from the per-object constant-buffer array (b0); viewProj
// (light) comes from the shared per-view CB (b1). Mirrors gbuffer_csm.hlsl.
#pragma pack_matrix(row_major)
#define GBUFFER_SKIP_PEROBJECT
#include "gbuffer_common.hlsli"

#define GBUFFER_INSTCB_CSM_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0), CBV(b1)"

struct VSOutD { float4 H : SV_POSITION; };

[RootSignature(GBUFFER_INSTCB_CSM_RS)]
VSOutD VSMain(VSInInst i)
{
    VSOutD o;
    // W5: mirror the gbuffer BaseVS sway. Safe to read windStrength here: the shadow instance array
    // is filled by the same IInstanceable::FillInstanceData as the gbuffer one (InstancedDrawBatch),
    // unlike the non-instanced b0 which UpdateShadowCB has to populate explicitly.
    const float4x4 w = inst[i.IID].world;
    float4 wp = mul(float4(i.P, 1.0f), w);
    wp.xyz += ApplyWindWS(i.P, w, inst[i.IID].windStrength, windTime);
    o.H = mul(wp, viewProj);
    return o;
}

// Depth-only — pixel shader can remain empty
[RootSignature(GBUFFER_INSTCB_CSM_RS)]
void PSMain(VSOutD i) { }
