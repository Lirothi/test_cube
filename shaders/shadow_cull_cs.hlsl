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
//
// Occlusion plan S5 (the camera's two-pass HZB occlusion): a camera candidate is also tested
// against LAST frame's depth pyramid (t12, the previous frame slot's HZB) with last frame's
// matrices -- Nanite's CULLING_PASS_OCCLUSION_MAIN, exactly as the cascades do it (S5b): frustum
// side test skipped, near crossing = visible. Hidden -> the camera DEFERRED list (u6) instead of
// pass A; cam_cull_post_cs.hlsl retests it after pass A against this frame's pyramid.
//
// Local-light shadow plan S7 (the shadow cache): with gLocalSplit a local view's STATIC caster -- CasterLod without
// kLocalDynamicBit -- goes to that view's static rows (u7/u8, row = view - kCascadeViews) instead of the main ones, which
// then hold only the movable casters. The cache renders the static rows into its pool on a miss and copies them in
// every frame; the movable rows draw on top.
//
// Local-light shadow plan S9e (UE's caster culling for a spot / point face): such a view's 16 slots are its 6 frustum
// planes, the light's reach sphere, a spot's cone and UE's light + camera-frustum hull (render::kLocalView*Slot). A
// caster must touch the light (UE AffectsBounds: the box against the sphere, its bounding sphere against the cone);
// one that is not cached -- every caster on a frame without the split -- must also lie in the hull
// (BuildLightViewFrustumConvexHull) and be no smaller than r.Shadow.RadiusThreshold of its distance to the camera (GI
// exempt). UE skips both for the static-primitives cache (SDCM_StaticPrimitivesOnly) and VSM, which cache pictures the
// camera must not shape; the CPU leaves VSM's hull empty and its threshold 0.
#define SHADOW_CULL_RS \
    "CBV(b0), CBV(b1), CBV(b2), " \
    "DescriptorTable(SRV(t0, numDescriptors=13, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=9, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer CullParams : register(b0)
{
    uint gNumCasters;
    uint gNumViews;
    uint gNumGroups;
    uint gCamView;      // S4: the camera's frustum slot, or 0xFFFFFFFF when the camera row is off
    uint gLocalSplit;   // S7: 1 = the local views' static casters into StaticArgs / StaticVisibleList
    uint gClipFirst;    // S9e: the clipmap slots [gClipFirst, gClipEnd) -- every other view past the cascades is local
    uint gClipEnd;
    uint gGiFirst;      // S9e: casters from here on are GI (exempt from the radius threshold)
    float4 gLocalCam;   // S9e: xyz the camera, w (RadiusThreshold x LODDistanceFactor)^2, 0 = off
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

// S5: mirrors ShadowGpuData::CameraHzbParams. The prev pair is last frame's camera (no jitter),
// the current pair this frame's (jittered, like the pyramid it is tested against).
cbuffer CameraHzbCB : register(b2)
{
    row_major float4x4 gCamPrevViewProj;
    row_major float4x4 gCamPrevViewToClip;
    row_major float4x4 gCamViewProj;
    row_major float4x4 gCamViewToClip;
    int4  gCamViewRect;      // (0, 0, renderWidth, renderHeight)
    uint2 gCamHzbSize;       // mip 0 of the camera pyramid
    uint  gCamHzbPrevValid;  // last frame's pyramid is this camera's, same size, built last frame
    uint  gCamHzbOn;
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
Texture2D<float>               CamHzbPrev  : register(t12); // S5: last frame's camera pyramid (placeholder when off)

static const uint kMaxShadowLods = TC_MAX_SHADOW_LODS; // render::kMaxShadowLods (plan S10a: Material.cpp EngineShaderDefines)
static const uint kCasterLodMask = 0x0Fu;
static const uint kNoCamView = 0xFFFFFFFFu;

RWByteAddressBuffer      Args          : register(u0); // InterlockedAdd on InstanceCount
RWStructuredBuffer<uint> VisibleList   : register(u1); // appended caster ids
RWStructuredBuffer<uint> DeferredList  : register(u2); // S5b: per cascade, caster ids the prev pyramid hid
RWStructuredBuffer<uint> DeferredCount : register(u3); // S5b: [v] deferred this frame (cleared by the cull-clear)
RWByteAddressBuffer      CamArgs       : register(u4); // S4: the camera's args (one row of virtual groups)
RWStructuredBuffer<uint> CamVisibleList : register(u5); // S4: the camera's visible caster ids
RWStructuredBuffer<uint> CamDeferred   : register(u6); // S5: candidates last frame's pyramid hid
RWByteAddressBuffer      StaticArgs    : register(u7); // S7: the local views' static rows [(view - 4) * groups + vg]
RWStructuredBuffer<uint> StaticVisibleList : register(u8); // S7: their caster ids
static const uint kCamDeferredCount = 8u;   // DeferredCount[] slots: [8] list length, [9] weighted (fade x2)
static const uint kCamDeferredWeighted = 9u;
static const uint kCascadeViews = TC_SHADOW_CASCADE_VIEWS; // render::kShadowViewCascades: the views below are cascades
static const uint kLocalDynamicBit = 0x100u;   // render::kCasterLocalDynamicBit (the uploaded CasterLod only)

static const uint kArgStride = 20u;

// S4: one camera candidate into virtual group `vg` (bucket base from the camera's own table).
void EmitCamera(uint vg, uint caster)
{
    const uint base = PerGroupCam[vg].x;
    uint slot;
    CamArgs.InterlockedAdd(vg * kArgStride + 4u, 1u, slot);
    CamVisibleList[base + slot] = caster;
}

// The bounds are world-space boxes already: the library's local-to-world is the identity.
static const float4x4 kIdentity = float4x4(1, 0, 0, 0,
                                           0, 1, 0, 0,
                                           0, 0, 1, 0,
                                           0, 0, 0, 1);

// S5: was this candidate hidden in last frame's camera view? (NaniteCullingCommon.ush:463-482:
// frustum side test skipped -- the clamped rect is a better guess than assuming either way, the
// post pass cleans up; a box crossing the near plane is visible.)
bool CamHiddenLastFrame(float3 c, float3 e)
{
    if (gCamHzbOn == 0u || gCamHzbPrevValid == 0u) { return false; }
    const HzbFrustumCull prev = HzbBoxCullFrustumPerspective(c, e, kIdentity, gCamPrevViewProj, gCamPrevViewToClip, true);
    if (!prev.isVisible || prev.crossesNearPlane) { return false; }
    const HzbScreenRect rect = HzbGetScreenRect(gCamViewRect, prev.rectMin, prev.rectMax, 4);
    return !HzbIsVisible(CamHzbPrev, gCamHzbSize, rect);
}

// S5b: was this caster hidden from the light in the previous frame's tile of cascade v?
bool HiddenLastFrame(uint v, float3 c, float3 e)
{
    if (v >= kHzbCascades || gHzbOn == 0u || gHzbPrevValid[v] == 0u) { return false; }
    const HzbFrustumCull prev = HzbBoxCullFrustumOrtho(c, e, kIdentity, gHzbPrevViewProj[v], false, true);
    if (!prev.isVisible || prev.crossesNearPlane) { return false; }
    const HzbScreenRect rect = HzbGetScreenRect(gHzbViewRect, prev.rectMin, prev.rectMax, 4);
    return !HzbIsVisible(CsmHzb[v], gHzbSize, rect);
}

// Positive-vertex AABB-vs-frustum test over planes [first, first + count) (mirrors Frustum::Intersects). Callers pass
// LITERAL ranges, so the loop stays unrolled. Inactive view slots carry a reject-all sentinel plane (0,0,0,-1) in plane 0
// so they cull everything -> zero visible casters.
bool PlanesPass(uint view, uint first, uint count, float3 c, float3 e)
{
    [unroll]
    for (uint i = 0u; i < count; ++i)
    {
        float4 p = Frustums[view].planes[first + i];
        float signedDist = dot(p.xyz, c) + p.w;
        float projRadius = dot(abs(p.xyz), e);
        if (signedDist + projRadius < 0.0f) { return false; }
    }
    return true;
}

bool Intersects(uint view, float3 c, float3 e) { return PlanesPass(view, 0u, kViewPlanes, c, e); }

// S9e: render::kLocalView*Slot.
static const uint kLocalFrustumPlanes = 6u;
static const uint kLocalSphereSlot = 6u;
static const uint kLocalConeSlot = 7u;
static const uint kLocalHullFirst = 8u;
static const uint kLocalHullPlanes = 8u;

// S9e: does the caster touch the light? The box against its reach sphere (the exact box distance: tighter than UE's
// bounding-sphere test, never culling what that keeps), then for a spot its bounding sphere (radius r) against the cone
// -- UE USpotLightComponent::AffectsBounds term for term: the apex pulled back by r / sin so the cone grows by r, and
// near the apex the sphere's own distance. Mirrored by CpuLightBoundsPass (ShadowGpuData.cpp).
bool LightBoundsPass(float4 sphere, float4 cone, float3 c, float3 e, float r)
{
    if (sphere.w >= 0.0f)
    {
        const float3 d = max(abs(c - sphere.xyz) - e, 0.0f);
        if (dot(d, d) > sphere.w * sphere.w) { return false; }
    }
    if (cone.w <= 1.0f)
    {
        const float cosA = cone.w;
        const float sinA = sqrt(saturate(1.0f - cosA * cosA));
        float3 d = c - (sphere.xyz - (r / max(sinA, 1e-4f)) * cone.xyz);
        float dsqr = dot(d, d);
        float e2 = dot(cone.xyz, d);
        if (!(e2 > 0.0f && e2 * e2 >= dsqr * (cosA * cosA))) { return false; }
        d = c - sphere.xyz;
        dsqr = dot(d, d);
        e2 = -dot(cone.xyz, d);
        if (e2 > 0.0f && e2 * e2 >= dsqr * (sinA * sinA)) { return dsqr <= r * r; }
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
    const uint lodEnc = CasterLod[caster];
    uint lod = lodEnc & kCasterLodMask;
    if (lod >= kMaxShadowLods) { lod = kMaxShadowLods - 1u; }
    uint g = CasterGroup[caster] * kMaxShadowLods + lod;
    uint base = PerGroup[g].x;
    const bool splitStatic = gLocalSplit != 0u && (lodEnc & kLocalDynamicBit) == 0u; // S7
    // S9e: r.Shadow.RadiusThreshold -- too small for its distance to the camera (GI instances exempt).
    const float3 toCam = c - gLocalCam.xyz;
    const bool tiny = gLocalCam.w > 0.0f && caster < gGiFirst && b.center.w * b.center.w <= gLocalCam.w * dot(toCam, toCam);
    // UE Cast Shadow off (ShadowGpuData::FillBounds: halfExtents.w = 1): in the table for the camera row below only.
    const bool castsShadow = b.halfExtents.w == 0.0f;

    for (uint v = 0; v < gNumViews && castsShadow; ++v)
    {
        bool hit;
        bool toStatic = false;
        if (v >= kCascadeViews && (v < gClipFirst || v >= gClipEnd))
        {
            // S9e: a spot / point face. Its frustum and the light's bounds for every caster; a caster the cache does not
            // hold (movable, or any on a frame without the split) also the hull and the threshold.
            hit = PlanesPass(v, 0u, kLocalFrustumPlanes, c, e) &&
                  LightBoundsPass(Frustums[v].planes[kLocalSphereSlot], Frustums[v].planes[kLocalConeSlot], c, e, b.center.w);
            toStatic = splitStatic;
            if (hit && !toStatic) { hit = !tiny && PlanesPass(v, kLocalHullFirst, kLocalHullPlanes, c, e); }
        }
        else
        {
            hit = Intersects(v, c, e);
        }
        if (hit)
        {
            if (HiddenLastFrame(v, c, e))
            {
                uint d;
                InterlockedAdd(DeferredCount[v], 1u, d);
                DeferredList[v * gNumCasters + d] = caster;
                continue;
            }
            if (toStatic)
            {
                const uint row = v - kCascadeViews;
                uint sslot;
                StaticArgs.InterlockedAdd((row * gNumGroups + g) * kArgStride + 4u, 1u, sslot);
                StaticVisibleList[row * gNumCasters + base + sslot] = caster;
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
            const bool fading = CasterFade[caster] > 0.0f && (lod + 1u) < kMaxShadowLods;
            if (CamHiddenLastFrame(c, e))
            {
                // S5: deferred once (the post pass re-derives the buckets), counted with the
                // weight pass A would have used so A + deferred == the CPU reference.
                uint d;
                InterlockedAdd(DeferredCount[kCamDeferredCount], 1u, d);
                CamDeferred[d] = caster;
                uint w;
                InterlockedAdd(DeferredCount[kCamDeferredWeighted], fading ? 2u : 1u, w);
            }
            else
            {
                EmitCamera(g, caster);
                if (fading) { EmitCamera(g + 1u, caster); }
            }
        }
    }
}
