// Rung 0 / Step 4: GPU frustum cull of shadow casters. One thread per caster; each loops the
// shadow views, tests the caster's world AABB against the view's cull planes (a direct
// port of Frustum::Intersects), and for a hit appends the caster id to that (view, mesh-group)
// slice of the visible list while atomically bumping the group's InstanceCount. Produces the
// indirect draw args consumed later by ExecuteIndirect (Step 6); nothing draws from it yet.
//
// Occlusion plan S5b (light-space occlusion, cascades only): after the frustum test, a caster of
// cascade view v < 4 is tested against LAST frame's pyramid of that cascade's tile with last
// frame's light matrices -- Nanite's CULLING_PASS_OCCLUSION_MAIN (NaniteCullingCommon.ush:
// 463-482): frustum side test skipped ("clamped rect HZB provides a better guess for occlusion
// than assuming true or false; post pass will clean up bad guesses"), no near clip (a pancaked
// caster crosses the near plane and stays visible). A caster the pyramid hides goes to the
// DEFERRED list instead of the visible list; shadow_cull_post_cs.hlsl retests it against THIS
// frame's pyramid after pass A and draws it in pass B if it was a bad guess.
#include "hzb_cull.hlsli"

//
// Occlusion plan S4 (the camera's indirect G-buffer): the same casters, tested once more against
// the CAMERA frustum (slot gCamView of the frustum buffer, past the shadow views), into the
// camera's OWN args + visible list (u4/u5, bases from t9) -- the shadow rows stay untouched.
// Per caster the camera flags (t10) say whether the object draws indirect at all (bit 0) and
// whether the CPU-side verdicts skip it this frame (bit 1: the S3a occlusion history for the
// object, the S1 chunk mask for a chunk); the fade (t11) puts a crossfading caster in its LOD
// bucket AND the next one, which is the CPU path's two draws.
#define SHADOW_CULL_RS \
    "CBV(b0), CBV(b1), " \
    "DescriptorTable(SRV(t0, numDescriptors=12, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=6, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer CullParams : register(b0)
{
    uint gNumCasters;
    uint gNumViews;
    uint gNumGroups;
    uint gCamView;      // S4: the camera's frustum slot, or 0xFFFFFFFF when the camera row is off
};

// Mirrors render::CascadeHzb::GpuParams. Both matrices are the cascade's light view-projection
// with z FLIPPED (reverse-Z for the library; the pyramid stores 1 - z to match).
static const uint kHzbCascades = 4u;
cbuffer CascadeHzbCB : register(b1)
{
    row_major float4x4 gHzbPrevViewProj[4];
    row_major float4x4 gHzbViewProj[4];
    uint4 gHzbPrevValid;   // per cascade: 1 = the pyramid holds last frame's tile
    int4  gHzbViewRect;    // (0, 0, content, content) -- the tile's content rect, full resolution
    uint2 gHzbSize;        // mip 0 of the pyramids
    uint  gHzbOn;          // 0 = no test this frame (placeholders bound in t5..t8)
    uint  gHzbPad;
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
// S5b: the four cascade pyramids (R32, 1 - z, all mips). Placeholders when gHzbOn == 0.
Texture2D<float>               CsmHzb[4]   : register(t5);
// S4: the camera's per-virtual-group bases, per-caster flags and crossfade (placeholders when off).
StructuredBuffer<uint4>        PerGroupCam : register(t9);
StructuredBuffer<uint>         CasterCam   : register(t10);
StructuredBuffer<float>        CasterFade  : register(t11);

static const uint kMaxShadowLods = 4u;   // must match render::kMaxShadowLods
static const uint kCasterLodMask = 0x0Fu;
static const uint kNoCamView = 0xFFFFFFFFu;

RWByteAddressBuffer      Args          : register(u0); // InterlockedAdd on InstanceCount
RWStructuredBuffer<uint> VisibleList   : register(u1); // appended caster ids
RWStructuredBuffer<uint> DeferredList  : register(u2); // S5b: per cascade, caster ids the prev pyramid hid
RWStructuredBuffer<uint> DeferredCount : register(u3); // S5b: [v] deferred this frame (cleared by the cull-clear)
RWByteAddressBuffer      CamArgs       : register(u4); // S4: the camera's args (one row of virtual groups)
RWStructuredBuffer<uint> CamVisibleList : register(u5); // S4: the camera's visible caster ids

static const uint kArgStride = 20u;

// S4: one camera candidate into virtual group `vg` (bucket base from the camera's own table).
void EmitCamera(uint vg, uint caster)
{
    const uint base = PerGroupCam[vg].x;
    uint slot;
    CamArgs.InterlockedAdd(vg * kArgStride + 4u, 1u, slot);
    CamVisibleList[base + slot] = caster;
}

static const float4x4 kIdentity = float4x4(1, 0, 0, 0,
                                           0, 1, 0, 0,
                                           0, 0, 1, 0,
                                           0, 0, 0, 1);

// S5b: was this caster hidden from the light in the previous frame's tile of cascade v?
bool HiddenLastFrame(uint v, float3 c, float3 e)
{
    if (v >= kHzbCascades || gHzbOn == 0u || gHzbPrevValid[v] == 0u) { return false; }
    const HzbFrustumCull prev = HzbBoxCullFrustumOrtho(c, e, kIdentity, gHzbPrevViewProj[v], false, true);
    if (!prev.isVisible || prev.crossesNearPlane) { return false; }
    const HzbScreenRect rect = HzbGetScreenRect(gHzbViewRect, prev.rectMin, prev.rectMax, 4);
    return !HzbIsVisible(CsmHzb[v], gHzbSize, rect);
}

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
            if (HiddenLastFrame(v, c, e))
            {
                uint d;
                InterlockedAdd(DeferredCount[v], 1u, d);
                DeferredList[v * gNumCasters + d] = caster;
                continue;
            }
            uint slot;
            Args.InterlockedAdd((v * gNumGroups + g) * kArgStride + 4u, 1u, slot);
            VisibleList[v * gNumCasters + base + slot] = caster;
        }
    }

    // S4: the camera row. Eligible (bit 0), not skipped by the CPU verdicts (bit 1), inside the
    // camera frustum; a crossfading caster goes to its tier and the next one.
    if (gCamView != kNoCamView)
    {
        const uint cam = CasterCam[caster];
        if ((cam & 3u) == 1u && Intersects(gCamView, c, e))
        {
            EmitCamera(g, caster);
            if (CasterFade[caster] > 0.0f && (lod + 1u) < kMaxShadowLods)
            {
                EmitCamera(g + 1u, caster);
            }
        }
    }
}
