// Rung 0 / Step 4: GPU frustum cull of shadow casters. One thread per caster; each loops the
// shadow views, tests the caster's world AABB against the view's 6 frustum planes (a direct
// port of Frustum::Intersects), and for a hit appends the caster id to that (view, mesh-group)
// slice of the visible list while atomically bumping the group's InstanceCount. Produces the
// indirect draw args consumed later by ExecuteIndirect (Step 6); nothing draws from it yet.
#define SHADOW_CULL_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer CullParams : register(b0)
{
    uint gNumCasters;
    uint gNumViews;
    uint gNumGroups;
    uint gPad;
};

struct CasterBounds
{
    float4 center;      // xyz world center, w bounding radius (unused here)
    float4 halfExtents; // xyz world half-extents, w unused
};
struct ViewFrustum
{
    float4 planes[6];   // inward unit-normal planes, inside == n.p + d >= 0
};

StructuredBuffer<CasterBounds> Bounds      : register(t0);
StructuredBuffer<ViewFrustum>  Frustums    : register(t1);
StructuredBuffer<uint>         CasterGroup : register(t2); // per-caster mesh-group id
StructuredBuffer<uint4>        PerGroup    : register(t3); // x = base offset within a view's region

RWByteAddressBuffer      Args        : register(u0); // InterlockedAdd on InstanceCount
RWStructuredBuffer<uint> VisibleList : register(u1); // appended caster ids

static const uint kArgStride = 20u;

// Positive-vertex AABB-vs-frustum test (mirrors Frustum::Intersects). Inactive view slots carry
// a reject-all sentinel plane (0,0,0,-1) so they cull everything -> zero visible casters.
bool Intersects(uint view, float3 c, float3 e)
{
    [unroll]
    for (int i = 0; i < 6; ++i)
    {
        float4 p = Frustums[view].planes[i];
        float signedDist = dot(p.xyz, c) + p.w;
        float projRadius = dot(abs(p.xyz), e);
        if (signedDist + projRadius < 0.0f) { return false; }
    }
    return true;
}

[numthreads(8, 8, 1)]
[RootSignature(SHADOW_CULL_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.y != 0u) { return; }
    uint caster = dtid.x;
    if (caster >= gNumCasters) { return; }

    CasterBounds b = Bounds[caster];
    float3 c = b.center.xyz;
    float3 e = b.halfExtents.xyz;
    uint g = CasterGroup[caster];
    uint base = PerGroup[g].x;

    for (uint v = 0; v < gNumViews; ++v)
    {
        if (Intersects(v, c, e))
        {
            uint slot;
            Args.InterlockedAdd((v * gNumGroups + g) * kArgStride + 4u, 1u, slot);
            VisibleList[v * gNumCasters + base + slot] = caster;
        }
    }
}
