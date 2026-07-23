// Rung 0 GI→VSM (Step 4): scatter one GPU-instanced object's per-instance transforms into the
// consolidated ShadowGpuData caster set so the existing cull + indirect draw handle them uniformly.
// One thread per instance i: read the instance's local world (gSrc[i].world), fold in the object's
// model matrix (gObjectWorld) to get the full model->world transform — matching gbuffer_inst_csm.hlsl
// (w = mul(gInstances[i].world, world)) so the indirect shadow VS produces identical positions — then
// write it to the unified instance buffer at global caster id giBase+i and compute a conservative
// world-space AABB (from the mesh-local AABB) for the cull. Only .world / bounds are written; the
// depth-only shadow VS + the cull read nothing else.
#define SHADOW_GI_SCATTER_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"
#pragma pack_matrix(row_major)

cbuffer ScatterParams : register(b0)
{
    uint   gGiBase;        // first global caster id of this object's instances
    uint   gCount;         // instance count
    float  gWindStrength;  // W5: the object's foliage sway strength (0 = rigid), same value gbuffer_inst.hlsl passes
    float  gSwayPad;       // W5: metres to grow the world AABB on X/Z for the sway (0 when rigid)
    float  gWindFoliage;   // GI objects are one mesh -> one foliage weight for the whole cloud
    float  gWindTrunkStiff;
    float  _pad1;
    float4 gAabbCenter;  // mesh-local AABB center (w unused)
    float4 gAabbExtent;  // mesh-local AABB half-extents (w unused)
    row_major float4x4 gObjectWorld; // object model matrix, folded onto each instance's local world
};

// Source: the object's own per-instance transform buffer (InstanceBuffer / instance_anim.hlsl).
struct InstanceData
{
    row_major float4x4 world;
    row_major float4x4 prevWorld;
    float  rotationY;
    float3 _pad;
};

// Matches render::InstancePerObject (224 bytes — W3 grew it 208 -> 224 for windStrength; this is the
// 4th mirror of that layout, alongside InstanceTypes.h, gbuffer_common.hlsli and
// shadow_indirect_csm.hlsl, and it MUST move with them). Shadows are depth-only, so the scatter
// writes only .world and (since W5) .windStrength — the shadow VS reads both, and the GI region of
// the unified DEFAULT-heap buffer is never otherwise initialised, so anything the VS reads here that
// the scatter does not write is garbage.
struct InstancePerObject
{
    float4x4 world;
    float4x4 prevWorld;
    float4   baseColor;
    float2   metalRough;
    float    alphaCutoff;
    float    mrMultiply;
    float4   texOffsScale;
    float4   texFlags;
    uint     objectId;
    uint3    _instPad1;
    float    windStrength;  // 208
    float    _windReserved;  // 212 (free since W7.3)
    float    windFoliage;    // 216
    float    windTrunkStiff; // 220
};
// Matches render::CasterBounds (32 bytes): world center + radius, world half-extents.
struct CasterBounds
{
    float4 center;      // xyz world center, w bounding radius
    float4 halfExtents; // xyz world half-extents, w unused
};

StructuredBuffer<InstanceData>        gSrc    : register(t0);
RWStructuredBuffer<InstancePerObject> gInst   : register(u0);
RWStructuredBuffer<CasterBounds>      gBounds : register(u1);

// 8x8 (=64) thread groups to fit the RecordComputeDispatch ceil(width/8) convention; the y lane is
// unused (one thread per instance along x), mirroring shadow_cull_cs.hlsl.
[numthreads(8, 8, 1)]
[RootSignature(SHADOW_GI_SCATTER_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.y != 0u) { return; }
    uint i = dtid.x;
    if (i >= gCount) { return; }

    const float4x4 world = mul(gSrc[i].world, gObjectWorld); // full model->world (== gbuffer_inst_csm)
    const uint dst = gGiBase + i;
    gInst[dst].world = world;
    // W5: the indirect shadow VS sways by this. It is NOT written anywhere else for GI ids, so
    // leaving it out feeds the sway uninitialised buffer memory (= scrambled GI shadows).
    gInst[dst].windStrength = gWindStrength;
    gInst[dst].windFoliage = gWindFoliage;
    gInst[dst].windTrunkStiff = gWindTrunkStiff;

    // Conservative world AABB from the mesh-local AABB under `world` (row-vector convention:
    // p' = mul(float4(p,1), world); extents map through abs of the upper-left 3x3).
    const float3 c = gAabbCenter.xyz;
    const float3 e = gAabbExtent.xyz;
    const float3 cW = mul(float4(c, 1.0f), world).xyz;
    float3 eW = mul(e, abs((float3x3)world));
    // W5: grow by the max sway on the horizontal axes (mirrors ShadowGpuData::FillBounds for the
    // static casters) so the per-page / per-view cull can't clip a swaying tip. 0 when rigid.
    eW.x += gSwayPad;
    eW.y += gSwayPad; // Tier 0: the arc bend dips the crown and the leaf flutter bobs vertically
    eW.z += gSwayPad;
    gBounds[dst].center = float4(cW, length(eW));
    gBounds[dst].halfExtents = float4(eW, 0.0f);
}
