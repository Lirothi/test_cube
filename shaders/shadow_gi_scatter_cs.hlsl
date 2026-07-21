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
    uint   gGiBase;      // first global caster id of this object's instances
    uint   gCount;       // instance count
    uint2  _pad0;
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

// Matches render::InstancePerObject (208 bytes). Shadows are depth-only, so only .world is written.
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

    // Conservative world AABB from the mesh-local AABB under `world` (row-vector convention:
    // p' = mul(float4(p,1), world); extents map through abs of the upper-left 3x3).
    const float3 c = gAabbCenter.xyz;
    const float3 e = gAabbExtent.xyz;
    const float3 cW = mul(float4(c, 1.0f), world).xyz;
    const float3 eW = mul(e, abs((float3x3)world));
    gBounds[dst].center = float4(cW, length(eW));
    gBounds[dst].halfExtents = float4(eW, 0.0f);
}
