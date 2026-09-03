// Rung 0 / Step 4: GPU frustum cull of shadow casters. One thread per caster; each loops the
// shadow views, tests the caster's world AABB against the view's cull planes (a direct
// port of Frustum::Intersects), and for a hit appends the caster id to that (view, mesh-group)
// slice of the visible list while atomically bumping the group's InstanceCount. Produces the
// indirect draw args consumed later by ExecuteIndirect (Step 6); nothing draws from it yet.
#define SHADOW_CULL_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=5, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
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
// S14: 16 planes, not 6 -- a cascade's caster cull is UE's ShadowBoundsAccurate (the camera slice
// extruded toward the sun: up to 3 faces + 6 silhouette edges + 2 depth caps + the box's 4 XY
// faces). Views with fewer planes arrive padded with the accept-all plane (0,0,0,+1), so the loop
// below is a fixed LITERAL 16 and never reads a count out of a buffer. Must match
// render::kShadowViewPlanes / Frustum::kMaxPlanes.
static const uint kViewPlanes = 16u;
struct ViewFrustum
{
    float4 planes[16];  // inward unit-normal planes, inside == n.p + d >= 0
};

StructuredBuffer<CasterBounds> Bounds      : register(t0);
StructuredBuffer<ViewFrustum>  Frustums    : register(t1);
StructuredBuffer<uint>         CasterGroup : register(t2); // per-caster mesh-group id
StructuredBuffer<uint4>        PerGroup    : register(t3); // per VIRTUAL group; x = base offset
// S3.6: the LOD the caster's RECEIVER draws this frame (bit 7 = chunk EXACT, irrelevant here -- the
// bucket is that LOD either way). This is what keeps a caster from being FINER than its receiver,
// which is UE's rule: the shadow depth pass reuses the LOD the CAMERA picked for that primitive.
StructuredBuffer<uint>         CasterLod   : register(t4);

static const uint kMaxShadowLods = 4u;   // must match render::kMaxShadowLods
static const uint kCasterLodMask = 0x0Fu;

RWByteAddressBuffer      Args        : register(u0); // InterlockedAdd on InstanceCount
RWStructuredBuffer<uint> VisibleList : register(u1); // appended caster ids

static const uint kArgStride = 20u;

// Positive-vertex AABB-vs-frustum test (mirrors Frustum::Intersects). Inactive view slots carry
// a reject-all sentinel plane (0,0,0,-1) so they cull everything -> zero visible casters.
bool Intersects(uint view, float3 c, float3 e)
{
    [unroll]
    for (uint i = 0u; i < kViewPlanes; ++i)
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
    // Real mesh group -> VIRTUAL group (group * kMaxShadowLods + receiver LOD). The bucket is the
    // same for every shadow view, so it is chosen once here instead of per view in a scatter.
    uint lod = CasterLod[caster] & kCasterLodMask;
    if (lod >= kMaxShadowLods) { lod = kMaxShadowLods - 1u; }
    uint g = CasterGroup[caster] * kMaxShadowLods + lod;
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
