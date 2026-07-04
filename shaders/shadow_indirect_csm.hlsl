// Rung 0 / Step 5: indirect depth-only shadow vertex shader. Reads the caster's world matrix
// from the persistent per-caster instance buffer (t0), indexed by a caster id the input
// assembler delivers per-instance — the visible list is bound as a per-instance vertex stream
// (slot 1), and the draw's StartInstanceLocation offsets it, so instance i reads
// visibleList[StartInstanceLocation + i]. Light viewProj comes from the shared per-view CB (b1).
// A plain DRAW_INDEXED command signature (no per-draw root args) drives this; nothing draws
// from it until the Step 6 behavioral flip.
#pragma pack_matrix(row_major)
#include "utils.hlsl"

#define SHADOW_INDIRECT_CSM_RS \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), " \
    "CBV(b1), " \
    "DescriptorTable(SRV(t0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

// Matches render::InstancePerObject (208 bytes) / the InstanceArray element in gbuffer_common.
struct InstancePerObject
{
    float4x4 world;
    float4x4 prevWorld;
    float4   baseColor;
    float2   metalRough;
    float2   _instPad0;
    float4   texOffsScale;
    float4   texFlags;
    uint     objectId;
    uint3    _instPad1;
};

StructuredBuffer<InstancePerObject> Instances : register(t0);

// Shared per-view CB (light viewProj for the shadow passes); same layout as gbuffer_common's.
cbuffer PerView : register(b1)
{
    float4x4 viewProj;
    float4x4 viewProjNoJitter;
    float4x4 prevViewProjNoJitter;
};

struct VSInIndirect
{
    float3 P        : POSITION;  // per-vertex, from the mesh vertex buffer (slot 0)
    uint   casterId : CASTERID;  // per-instance, from the visible-list stream (slot 1)
};
struct VSOutD { float4 H : SV_POSITION; };

[RootSignature(SHADOW_INDIRECT_CSM_RS)]
VSOutD VSMain(VSInIndirect i)
{
    VSOutD o;
    const float4x4 world = Instances[i.casterId].world;
    o.H = TransformPositionH(i.P, world, viewProj);
    return o;
}

// Depth-only — empty pixel shader.
[RootSignature(SHADOW_INDIRECT_CSM_RS)]
void PSMain(VSOutD i) { }
