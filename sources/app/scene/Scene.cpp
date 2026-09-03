#include "app/scene/Scene.h"
#include "core/logging/Log.h"

#include <memory>
#include <algorithm>
#include <array>
#include <cmath>
#include <cassert>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>

#include "app/camera/Camera.h"
#include "app/Systems.h"
#include "core/Helpers.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/meshes/LodSelect.h" // render::g_shadowLodBias (shadow caster LOD)
#include "rendering/debug/LodDebugView.h" // render::DrawLodDebug (LOD selection debug view)
#include "rendering/shadows/ShadowSettings.h"
#include "rendering/renderables/InstancedDrawBatch.h" // S14 readout: count a batch's members, not the batch
#include "rendering/core/VisibilityStats.h" // occlusion plan S0: per-view visibility counters
#include "rendering/core/RenderStats.h"     // occlusion plan S0: draw/primitive totals in the readout
#include "core/diagnostics/ArtifactWriter.h" // S7: headless cascade readout dump
#include "ocean/OceanSimulation.h"
#include "ocean/OceanRenderable.h"
#include "core/task/TaskSystem.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "core/math/AABB.h"
#include "core/math/Frustum.h"
#include "core/containers/inl_vector.h"

const mat4& Scene::GetCascadeView(size_t index) const
{
    assert(index < static_cast<size_t>(kCascades));
    return frameData_.cascades.lightView[index];
}

const mat4& Scene::GetCascadeProj(size_t index) const
{
    assert(index < static_cast<size_t>(kCascades));
    return frameData_.cascades.lightProj[index];
}

float2 Scene::GetCascadeScale(size_t index) const
{
    assert(index < static_cast<size_t>(kCascades));
    return frameData_.cascades.atlasScale[index];
}

float2 Scene::GetCascadeBias(size_t index) const
{
    assert(index < static_cast<size_t>(kCascades));
    return frameData_.cascades.atlasBias[index];
}

float Scene::GetCascadeTexelWS(size_t index) const
{
    assert(index < static_cast<size_t>(kCascades));
    return frameData_.cascades.cascadeTexelWS[index];
}

float Scene::GetCascadeDepthBias(size_t index) const
{
    assert(index < static_cast<size_t>(kCascades));
    return frameData_.cascades.depthBiasNDC[index];
}

static void BuildFrustumSliceCornersWS(const mat4& invView, const mat4& invProj,
    float zNearVS, float zFarVS, std::array<float3, 8>& outCornersWS)
{
    const float2 ndc[4] = { {-1,-1}, {+1,-1}, {+1,+1}, {-1,+1} };
    int idx = 0;
    for (int i = 0; i < 4; ++i)
    {
        // Take the ray from the camera toward the frustum corner (at z=1 in view space)
        float4 farVS = invProj.Transform(float4(ndc[i].x, ndc[i].y, 1.0f, 1.0f));
        float3 dirVS = farVS.xyz() / farVS.w; // direction on the far plane

        // Point at the desired depth z: scale the ray so its z matches the required depth
        float nz = std::max(1e-6f, dirVS.z);
        float3 nearVS = dirVS * (zNearVS / nz);
        float3 farV = dirVS * (zFarVS / nz);

        // Transform into world space
        float3 nearWS = (invView * float4(nearVS, 1)).xyz();
        float3 farWS = (invView * float4(farV, 1)).xyz();

        outCornersWS[idx++] = nearWS;
        outCornersWS[idx++] = farWS;
    }
}

struct CascadeSphere
{
    float3 center{};
    float  radius = 0.0f;
};

// S1: minimal enclosing sphere of a frustum slice, in world space.
//
// The centroid of the 8 corners is NOT the minimal sphere's centre. The minimal sphere of a
// frustum slice always has its centre ON the view axis, and the offset has a closed form: with
// a = |far diagonal|, b = |near diagonal| and L = splitFar - splitNear, equating the distance to
// the near and far corners gives
//     c = splitFar - [ (b*b - a*a) / (2L) + L/2 ]
// When the slice is wide relative to its length (large FOV / short slice) the solution runs past
// the far plane; the centre is then clamped ONTO the far plane, which is still the minimal sphere
// for that case (the far rectangle's circumcircle already encloses the near corners).
//
// Centre depends only on (splitNear, splitFar, FOV) and radius only on the corner geometry, so
// BOTH are invariant to camera and sun rotation — which is what the light-space texel snap in
// UpdateCascades relies on. The radius is measured from the real corners rather than derived, so
// it is self-validating: the "ortho radius under-covers slice corners" assert cannot regress.
static CascadeSphere ComputeCascadeSphere(const Camera& camera,
                                          const std::array<float3, 8>& cornersWS,
                                          float splitNear, float splitFar)
{
    // tan(halfFov) straight from the projection: for LH perspective (XMMatrixPerspectiveFovLH)
    //   m._11 = 1/(aspect*tan(vfov/2)) = 1/tan(hfov/2)
    //   m._22 = 1/tan(vfov/2)
    // Non-jittered on purpose — see the corner build in UpdateCascades.
    const mat4& proj = camera.GetProjMatrixNoJitter();
    const float tanHalfX = 1.0f / std::max(1e-6f, proj.m._11);
    const float tanHalfY = 1.0f / std::max(1e-6f, proj.m._22);

    const float farX = tanHalfX * splitFar;
    const float farY = tanHalfY * splitFar;
    const float nearX = tanHalfX * splitNear;
    const float nearY = tanHalfY * splitNear;

    const float diagFarSq = farX * farX + farY * farY;
    const float diagNearSq = nearX * nearX + nearY * nearY;
    const float sliceLen = std::max(1e-4f, splitFar - splitNear);

    const float offset = (diagNearSq - diagFarSq) / (2.0f * sliceLen) + sliceLen * 0.5f;
    const float centreZ = Clamp(splitFar - offset, splitNear, splitFar);

    CascadeSphere out{};
    out.center = camera.GetPosition() + camera.GetDirection() * centreZ;

    float rSq = 0.0f;
    for (const float3& c : cornersWS)
    {
        const float3 d = c - out.center;
        rSq = std::max(rSq, d.Dot(d));
    }
    // Never 0: a degenerate ortho extent produces INF matrices downstream.
    out.radius = std::max(std::sqrt(rSq), 1.0f);
    return out;
}

void Scene::InitializeCommonResources(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    sceneRenderer_.InitializeCommonResources(renderer, uploadCmdList, uploadKeepAlive);
}

void Scene::FinalizeLevelLoad(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    sceneRenderer_.FinalizeLevelLoad(renderer, objects_, uploadCmdList, uploadKeepAlive, skyBox_.get());
    SyncObjectsForRender(SceneObjectSyncReason::LevelLoad);
    // Rung 0 / Steps 1-2: build the persistent per-caster shadow buffers (instance + bounds)
    // once the object transforms are finalized (SyncSceneState above resets motion history so
    // prevWorld == world). Level load is GPU-idle, safe for the alloc.
    // NOTE: anything FillBounds/Rebuild consumes must be identical here and in a mid-session
    // RebuildShadowCasters — a value published elsewhere on a different schedule (like the removed
    // W5 sway-extent global, docs/bug_shadow_lod_bias_perf.md) splits the two builds' cost/behavior.
    shadowGpu_.Rebuild(renderer, objects_);
    // Rung 2 mega-buffer: concatenate the caster meshes' VB/IB on this GPU-idle upload CL (meshes
    // are all in COMMON here, so the copy uses implicit promotion) for the VSM per-page draws.
    shadowGpu_.EnsureMegaBuffer(renderer, uploadCmdList);
    BumpStaticSetVersion(); // Step 11: a fresh level = a new static caster set
    // Rung 2 (Step 24b): allocate the persistent VSM page pool + page table only when VSM is the
    // active mode — Legacy mode keeps ZERO VSM resources resident. A runtime Ctrl+V switch reconciles
    // this at GPU idle in Scene::Render. (The mega-buffer above stays built regardless — it's tiny
    // and lets a runtime switch to VSM use the fast per-page draw path immediately.)
    if (render::VsmActive()) { vsm_.EnsureResources(renderer); }
    // Fresh level = every resident VSM page is stale (same view slots, different level content).
    // Drop all mappings so the first frames re-request/re-render cleanly.
    vsm_.InvalidateAllPages();
}

void Scene::SyncObjectsForRender(SceneObjectSyncReason reason)
{
    for (const auto& obj : objects_)
    {
        if (obj)
        {
            obj->SyncSceneState(reason);
        }
    }
}

namespace
{
struct CascadeScissor
{
    SceneFrameData::CascadeData::ScissorRect rect;
    float areaFrac = 1.0f;
};

// S11 -- UE FProjectedShadowInfo::ComputeScissorRectOptim (ShadowSetup.cpp:2945), transcribed.
// The cascade tile is a square around the slice's bounding SPHERE, but the camera sees only a
// pyramid inside it, and everything the lighting pass can ever sample from this cascade projects,
// along the light, into the image of that pyramid. UE take the slice's four far corners, the
// far-plane centre and the camera position, project them into the tile, and -- the step that makes
// it a CONE rather than a pyramid -- extend every camera->point ray to the tile border. The
// extension covers receivers beyond the slice's far plane that still sample this tile (our blend
// band into the next cascade, the UV-containment fallback chain) at the price of a somewhat larger
// rect. The bounding box of those points is the scissor. It only fires when the camera itself
// projects inside the tile; otherwise the whole tile is drawn, as in UE.
//
// Deltas from the original, all deliberate:
//  * texel space is our CONTENT rect (0..contentRes), not UE's border-inclusive FullRes: the
//    viewport already excludes the S5 gutter, so a rect reaching into it would draw nothing extra
//    and would void the "border stays clear" guarantee;
//  * the far corners come from BuildFrustumSliceCornersWS (the same tan(halfFov)*SplitFar
//    construction UE spell out inline); their AsymmetricFOVScale terms are the off-centre part of
//    the projection, which for us is only the DLSS jitter, and the cascade is fitted to the
//    non-jittered frustum (S1);
//  * `padTexels` is OURS, UE pad nothing -- see CascadeShadowConfig::scissorPadTexels;
//  * a degenerate ray (camera looking along the light: origin and corner coincide in the tile)
//    falls back to the whole tile instead of UE's silent (0,0) point, which drags their rect to
//    the tile corner;
//  * the atlas offset is added ONCE (UE add X,Y both here and again in SetStateForView).
CascadeScissor ComputeCascadeScissor(const std::array<float3, 8>& cornersWS, const float3& eye,
                                     const mat4& lightViewProj, UINT contentRes,
                                     UINT tileOriginX, UINT tileOriginY, float padTexels)
{
    const float res = static_cast<float>(contentRes);
    CascadeScissor full;
    full.rect = { static_cast<std::int32_t>(tileOriginX), static_cast<std::int32_t>(tileOriginY),
                  static_cast<std::int32_t>(tileOriginX + contentRes),
                  static_cast<std::int32_t>(tileOriginY + contentRes) };
    full.areaFrac = 1.0f;
    if (contentRes == 0) { return full; }

    // UE's FrustumCorners[0..3] = far corners, [4] = far-plane centre, [5] = view origin.
    // BuildFrustumSliceCornersWS interleaves near/far per corner, so the far ones are the odd slots.
    float3 pts[6] = { cornersWS[1], cornersWS[3], cornersWS[5], cornersWS[7], float3(0, 0, 0), eye };
    pts[4] = (pts[0] + pts[1] + pts[2] + pts[3]) * 0.25f;

    // World -> content texels. Ortho, so w == 1; the divide is kept for parity with the original.
    float2 tp[6];
    for (int i = 0; i < 6; ++i)
    {
        const float4 clip = lightViewProj * float4(pts[i], 1.0f);
        const float w = (std::abs(clip.w) > 1e-6f) ? clip.w : 1.0f;
        tp[i] = float2(((clip.x / w) * 0.5f + 0.5f) * res, ((clip.y / w) * -0.5f + 0.5f) * res);
    }

    // UE: the optimisation applies only when the view origin lands inside the tile.
    const float2 origin = tp[5];
    if (origin.x < 0.0f || origin.x > res || origin.y < 0.0f || origin.y > res) { return full; }

    float minX = origin.x, maxX = origin.x, minY = origin.y, maxY = origin.y;
    for (int i = 0; i < 5; ++i)
    {
        // Extend the origin->point ray to the first tile border it exits through (UE's
        // ComputeScissorIntersection: nearest positive-distance hit among the four border lines).
        const float dx = tp[i].x - origin.x;
        const float dy = tp[i].y - origin.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-3f) { return full; }   // degenerate: camera axis parallel to the light
        const float ux = dx / len, uy = dy / len;
        float t = 1e30f;
        if (ux > 0.0f) { t = std::min(t, (res - origin.x) / ux); }
        if (ux < 0.0f) { t = std::min(t, (0.0f - origin.x) / ux); }
        if (uy > 0.0f) { t = std::min(t, (res - origin.y) / uy); }
        if (uy < 0.0f) { t = std::min(t, (0.0f - origin.y) / uy); }
        const float px = std::clamp(origin.x + ux * t, 0.0f, res);
        const float py = std::clamp(origin.y + uy * t, 0.0f, res);
        minX = std::min(minX, px); maxX = std::max(maxX, px);
        minY = std::min(minY, py); maxY = std::max(maxY, py);
    }

    const float pad = std::max(0.0f, padTexels);
    const std::int32_t x0 = std::clamp(static_cast<std::int32_t>(std::floor(minX - pad)), 0, static_cast<std::int32_t>(contentRes));
    const std::int32_t y0 = std::clamp(static_cast<std::int32_t>(std::floor(minY - pad)), 0, static_cast<std::int32_t>(contentRes));
    const std::int32_t x1 = std::clamp(static_cast<std::int32_t>(std::ceil (maxX + pad)), 0, static_cast<std::int32_t>(contentRes));
    const std::int32_t y1 = std::clamp(static_cast<std::int32_t>(std::ceil (maxY + pad)), 0, static_cast<std::int32_t>(contentRes));
    if (x1 <= x0 || y1 <= y0) { return full; }

    CascadeScissor r;
    r.rect = { static_cast<std::int32_t>(tileOriginX) + x0, static_cast<std::int32_t>(tileOriginY) + y0,
               static_cast<std::int32_t>(tileOriginX) + x1, static_cast<std::int32_t>(tileOriginY) + y1 };
    r.areaFrac = static_cast<float>(x1 - x0) * static_cast<float>(y1 - y0) / (res * res);
    return r;
}

// S14 -- UE ComputeShadowCullingVolume (DirectionalLightComponent.cpp:101), transcribed. The
// volume a directional cascade culls its CASTERS against is not the cascade's ortho box but the
// camera slice EXTRUDED TOWARD THE SUN: a caster can only shadow the slice if some point of it
// lies on a light ray that enters the slice, i.e. inside slice + t*toSun, t >= 0. Its boundary:
//   1. the slice's faces that look AWAY from the sun (their outward normal against toSun) -- the
//      caps on the far-from-sun side; the sun-facing faces sweep off to infinity and vanish;
//   2. for every slice edge whose two faces disagree about the sun (one lit, one not) -- the
//      silhouette of the slice as seen from the sun -- the plane through that edge and toSun;
//   3. ours: the ortho box's six faces. UE's volume is open toward the sun; casterReachWS is
//      this engine's "how far toward the light to look", so its near plane keeps capping that
//      side; the XY faces are kept for the AABB test's sake (see the body).
// In light-space XY the result is the convex hull of the slice's projection, tighter than the S11
// scissor's bounding box, and it acts on both culls before a single vertex is shaded.
//
// Deltas from the original, all deliberate: planes are oriented by an interior point (the slice
// centroid) instead of by corner winding, which is what makes UE need bReverseCulling; UE's
// LightDirection here is -GetDirection(), i.e. TOWARD the sun, and so is `toSun`; the corner
// layout is BuildFrustumSliceCornersWS's (2*ndcCorner + {0 near, 1 far}, corners BL/BR/TR/TL),
// so UE's index tables are re-expressed for it. Stored INWARD (inside == n.p + d >= 0), the
// convention of Frustum and of the GPU cull. Empty (count 0) on a degenerate slice -> the caller
// keeps the ortho box, which is always correct.
struct CascadeCullVolume
{
    float4 planes[Frustum::kMaxPlanes] = {};
    int count = 0;
};

CascadeCullVolume BuildCascadeCullVolume(const std::array<float3, 8>& cornersWorld, const float3& origin,
                                         const float3& toSun, const float4* boxPlanes, int boxPlaneCount)
{
    enum { nBL = 0, fBL = 1, nBR = 2, fBR = 3, nTR = 4, fTR = 5, nTL = 6, fTL = 7 };

    // PRECISION, and it is not optional: cascade 0's near quad is 2 cm across (zNear 0.01), and
    // a corner at world |p| ~ 300 m carries a float ulp of 3e-5 m -- so an edge direction taken
    // from world-space corners is only good to 1.5e-3, the plane normal inherits that, and 75 m
    // toward the sun that is a decimetre of wrong cull. Measured before this: an edge plane with
    // n.toSun = -3e-4 (it must be 0) and a slice corner 1.4 mm outside its own face. UE do the
    // same arithmetic in translated world space (PreShadowTranslation) for exactly this reason.
    // Everything below works relative to `origin` (the camera, the slice apex); only the plane
    // offsets are moved back to world at the end: n.(p - o) + d' = n.p + (d' - n.o).
    float3 c[8];
    for (int i = 0; i < 8; ++i) { c[i] = cornersWorld[i] - origin; }
    // UE's six faces (Near, Left, Right, Top, Bottom, Far) by three of their corners, and their
    // twelve edges as (faceA, faceB, cornerA, cornerB): AdjacentPlanePairs + LineVertexIndices.
    static const int kFaces[6][3] = {
        { nBL, nBR, nTR }, { nBL, nTL, fTL }, { nBR, nTR, fTR },
        { nTL, nTR, fTR }, { nBL, nBR, fBR }, { fBL, fBR, fTR } };
    static const int kEdges[12][4] = {
        { 0, 1, nBL, nTL }, { 0, 2, nBR, nTR }, { 0, 3, nTL, nTR }, { 0, 4, nBL, nBR },
        { 5, 1, fBL, fTL }, { 5, 2, fBR, fTR }, { 5, 3, fTL, fTR }, { 5, 4, fBL, fBR },
        { 1, 3, nTL, fTL }, { 2, 3, nTR, fTR }, { 1, 4, nBL, fBL }, { 2, 4, nBR, fBR } };

    float3 centroid(0.0f, 0.0f, 0.0f);
    for (const float3& p : c) { centroid += p; }
    centroid = centroid * (1.0f / 8.0f);

    // DIRECTIONS COME FROM FAR-SCALE GEOMETRY ONLY. Subtracting the origin above fixes the plane
    // arithmetic but not the corners themselves: BuildFrustumSliceCornersWS makes each corner in
    // world space, so a corner at |p| ~ 30 m already carries ~3e-6 m of error, and cascade 0's
    // near quad is 2 cm across -- a direction taken across it is good to ~1.5e-4, a plane through
    // two near corners and one far corner misses the fourth far corner by 20 m x 1.5e-4 = 3 mm,
    // and an edge plane came out with n.toSun = -3e-4 where it must be 0 (measured; the Debug
    // asserts caught both). So: side faces and side edges pass through the apex (the camera,
    // the origin here) and take their directions from the corner RAYS c[far]; the near and far
    // faces share the far quad's normal; near-quad edges are parallel to far-quad edges and
    // borrow their direction. Only anchors come from near corners, and an anchor's absolute
    // error does not amplify. (UE's literal corner arithmetic survives because their cascade 0
    // starts at the 10 cm camera near plane and works in translated world space.)
    const float3 farU = c[fBR] - c[fBL];        // far quad, BL -> BR
    const float3 farV = c[fTL] - c[fBL];        // far quad, BL -> TL
    const float3 viewN = farU.Cross(farV);      // near/far face normal (sign fixed below)
    struct FaceDef { float3 n; float3 anchor; };
    const FaceDef faceDefs[6] = {
        { viewN,                    c[nBL] },              // Near
        { c[fTL].Cross(c[fBL]),     float3(0.0f, 0.0f, 0.0f) }, // Left   (through the apex)
        { c[fTR].Cross(c[fBR]),     float3(0.0f, 0.0f, 0.0f) }, // Right
        { c[fTR].Cross(c[fTL]),     float3(0.0f, 0.0f, 0.0f) }, // Top
        { c[fBR].Cross(c[fBL]),     float3(0.0f, 0.0f, 0.0f) }, // Bottom
        { viewN,                    c[fBL] },              // Far
    };
    (void)kFaces; // the quad table documents the face/edge topology; the planes come from the rays

    // Outward unit normals of the six faces.
    float3 n[6];
    float d[6];
    for (int f = 0; f < 6; ++f)
    {
        float3 nn = faceDefs[f].n;
        const float len = nn.Length();
        if (len < 1e-9f) { return CascadeCullVolume{}; }
        nn = nn * (1.0f / len);
        float dd = -nn.Dot(faceDefs[f].anchor);
        if (nn.Dot(centroid) + dd > 0.0f) { nn = nn * -1.0f; dd = -dd; }
        n[f] = nn;
        d[f] = dd;
    }

    CascadeCullVolume v;
    bool overflow = false;
    const auto push = [&](const float3& pn, float pd)
    {
        if (v.count < Frustum::kMaxPlanes) { v.planes[v.count++] = float4(pn, pd); }
        else { overflow = true; }
    };
    // A plane computed in origin-relative space, moved to world.
    const auto pushRel = [&](const float3& pn, float pdRel) { push(pn, pdRel - pn.Dot(origin)); };

    // 1) Faces looking away from the sun (UE: `Normal | LightDirection < 0`), stored inward.
    for (int f = 0; f < 6; ++f)
    {
        if (n[f].Dot(toSun) < 0.0f) { pushRel(n[f] * -1.0f, -d[f]); }
    }
    // 2) Silhouette edges, extruded along the light (UE: C = A + LightDirection * |A - B|).
    for (int e = 0; e < 12; ++e)
    {
        const float dotA = n[kEdges[e][0]].Dot(toSun);
        const float dotB = n[kEdges[e][1]].Dot(toSun);
        if (dotA * dotB < 0.0f)
        {
            // Edge direction from far-scale geometry (see above): near-quad edges 0..3 borrow the
            // parallel far-quad edge 4..7, far-quad edges use their own, side edges 8..11 are the
            // corner rays from the apex. The anchor is the edge's own first corner.
            const float3& a = c[kEdges[e][2]];
            float3 dir;
            if (e < 4)       { dir = c[kEdges[e + 4][3]] - c[kEdges[e + 4][2]]; }
            else if (e < 8)  { dir = c[kEdges[e][3]] - c[kEdges[e][2]]; }
            else             { dir = c[kEdges[e][3]]; }
            const float dirLen = dir.Length();
            if (dirLen < 1e-9f) { continue; }
            // UE: the plane through A, B and A + LightDirection * |AB|, i.e. spanned by the edge
            // and the light. Same plane, from the direction alone.
            float3 pn = dir.Cross(toSun);
            const float len = pn.Length();
            // Ill-conditioned when the edge runs nearly along the light: |cross| = |dir| * sin(angle),
            // and a plane whose normal is float noise can cut through the volume. Dropping the plane
            // is always safe -- the volume just stays open along that edge (more casters, never fewer).
            if (len < 1e-4f * dirLen) { continue; }
            pn = pn * (1.0f / len);
            float pd = -pn.Dot(a);
            if (pn.Dot(centroid) + pd < 0.0f) { pn = pn * -1.0f; pd = -pd; }
            pushRel(pn, pd);
        }
    }
    // 3) The ortho box, all six faces. The depth caps bound the sweep toward the sun (UE's
    //    volume is open there) and past the slice; the four XY faces are geometrically implied
    //    by the prism but NOT by the AABB test that consumes it -- the positive-vertex test
    //    over-includes at the prism's acute corners, and measured without them cascade 0 passed
    //    17 casters where the box passed 13. With them the volume is the intersection of both
    //    tests and can never pass a box the plain box rejects.
    for (int i = 0; i < boxPlaneCount; ++i)
    {
        push(float3(boxPlanes[i].x, boxPlanes[i].y, boxPlanes[i].z), boxPlanes[i].w);
    }
    return overflow ? CascadeCullVolume{} : v;
}

// Occlusion plan S0: fill one view's visibility counters from its culled queue (before batching).
// Triangles are the CPU-path estimate: index count / 3 at the object's camera LOD (chunked meshes:
// per chunk at the chunk's tier), times the GI instance count where there is one.
// S1: `chunksDrawn` / `trianglesSubmitted` / `instancesDrawn` count what the view's frustum KEEPS
// below the object level. The camera reads the mask its SelectLod just wrote (and the GI cloud's
// visible-instance count); a cascade re-tests the chunk boxes against its own frustum right here,
// through the same predicate RenderShadow applies at draw time -- it must not read the camera's
// mask, and not only because a chunk behind the camera still casts: this runs on the cascade's
// prepare task, concurrently with the camera task that is writing that mask.
void AccumulateVisibility(render::VisibilityViewCounters& c, const SceneRenderQueue& queue, size_t sourceCount,
                          const Frustum& frustum, bool cameraView)
{
    c.objectsIn = static_cast<std::uint32_t>(sourceCount);
    c.objectsFrustum = static_cast<std::uint32_t>(queue.VisibleObjectCount());
    for (const auto& bucket : queue.VisibleBuckets())
    {
        for (const RenderableObjectBase* obj : bucket)
        {
            if (!obj) { continue; }
            const UINT gi = cameraView ? obj->GetCameraInstanceCount() : obj->GetInstanceCasterCount();
            const std::uint32_t instances = gi > 0u ? static_cast<std::uint32_t>(gi) : 1u;
            c.instancesDrawn += instances;
            const RenderableObject* ro = obj->AsRenderableObject();
            const Mesh* mesh = ro ? ro->GetMesh() : nullptr;
            if (!mesh) { continue; }
            const UINT lodCount = std::max(1u, mesh->GetLodCount());
            if (mesh->IsChunkedSubmeshes())
            {
                const std::vector<std::uint8_t>& lods = ro->ChunkCameraLods();
                const std::vector<std::uint8_t>& camMask = ro->ChunkCameraVisible();
                const std::vector<AABB>& boxes = mesh->GetSubmeshBounds();
                const Math::mat4& model = ro->GetModelMatrix();
                const size_t n = std::min(lods.size(), mesh->SubmeshesForLod(0).size());
                c.chunksIn += static_cast<std::uint32_t>(n);
                for (size_t s = 0; s < n; ++s)
                {
                    const bool drawn = cameraView
                        ? (s >= camMask.size() || camMask[s] != 0u)
                        : (s >= boxes.size() || RenderableObject::ChunkInFrustum(boxes[s].Transform(model), frustum));
                    if (!drawn) { continue; }
                    ++c.chunksDrawn;
                    const UINT lod = std::min<UINT>(lods[s], lodCount - 1u);
                    const auto& subs = mesh->SubmeshesForLod(lod);
                    if (s < subs.size()) { c.trianglesSubmitted += subs[s].indexCount / 3u; }
                }
            }
            else
            {
                const UINT lod = std::min<UINT>(obj->GetCameraLod(), lodCount - 1u);
                c.trianglesSubmitted += static_cast<std::uint64_t>(mesh->GetLodIndexCount(lod) / 3u) * instances;
            }
        }
    }
}
} // namespace

void Scene::BumpStaticSetVersion()
{
    ++staticSetVersion_;

    // Queue entries are non-owning raw pointers. An editor mutation can destroy an object between
    // frames, before UpdateCascades and other previous-frame diagnostics run. Drop every borrowed
    // pointer at the mutation boundary; PrepareViews repopulates all active queues this frame.
    renderQueueSourcesValid_ = false;
    shadowCasterSource_.Clear();
    cameraObjectSource_.Clear();
    camera_.GetView().queue.Clear();
    for (SceneView& view : cascadeViews_) { view.queue.Clear(); }
    for (SceneView& view : clipmapViews_) { view.queue.Clear(); }
    for (SceneView& view : spotShadowViews_) { view.queue.Clear(); }
    for (SceneView& view : pointShadowViews_) { view.queue.Clear(); }
}

void Scene::UpdateCascades(const Camera& camera, Renderer* renderer)
{
    CPU_SCOPE(ProfilerScopes::kUpdateCascades);
    if (!renderer)
    {
        return;
    }

    const float zNear = camera.GetZNear();
    const float zFar = camera.GetZFar();
    SceneFrameData::CascadeData& cascades = frameData_.cascades;
    const auto splits = cascadeConfig_.BuildSplitScheme(zNear, zFar);
    for (size_t i = 0; i < splits.size(); ++i)
    {
        cascades.splitsVS[i] = splits[i];
    }

    const auto& deferred = renderer->GetDeferredForFrame();
    const UINT tileRes = deferred.shadowRes > 0 ? deferred.shadowRes / 2u : 0u;
    // S5: the depth pass draws into the inner rect only (Renderer::BindShadowTarget), so the
    // cascade's world square covers `contentRes` texels, not `tileRes`. EVERY texel-derived
    // quantity below -- the snap grid, unitsPerTexel, both biases, the atlas scale/bias -- has to
    // use this one, or the sampled rect and the rendered rect stop agreeing.
    const UINT borderRes = (tileRes > 2u * render::kCascadeAtlasBorder) ? render::kCascadeAtlasBorder : 0u;
    const UINT contentRes = tileRes - 2u * borderRes;
    if (tileRes == 0)
    {
        for (auto& view : cascadeViews_)
        {
            view.frustum = Frustum{};
            view.type = SceneView::Type::Shadow;
            view.renderLayerMask = camera.GetRenderLayerMask();
            view.zNear = 0.0f;
            view.zFar = 0.0f;
            view.hfov = 0.0f;
            view.requiresDepthCheck = false;
            view.queue.Clear();
        }
        return;
    }

    const mat4& invView = camera.GetInvViewMatrix();
    // S1: fit the cascades to the NON-JITTERED frustum. view_.invProj is the inverse of the
    // DLSS-jittered projection (Camera.cpp: _31/_32 carry the sub-pixel offset), so building the
    // slice corners from it made the fitted sphere — and therefore unitsPerTexel and the snap grid
    // — wobble every frame. The wobble is ~0.5-1 cascade texel (the jitter is +-0.5 px of render
    // width, which at the slice's far plane is a fraction of a shadow texel of the same order),
    // i.e. exactly the scale the texel snap exists to pin down. The shadow map has no business
    // tracking a sub-pixel camera jitter.
    const mat4& invProj = camera.GetInvProjMatrixNoJitter();
    const float3 sunDirWS = dirLight_.GetDirection();

    std::string c0Survivors; // S14 readout diagnostic, filled only on the dump frame
    // UpdateCascades runs before this frame's queues are rebuilt. Its diagnostic reads below are
    // therefore allowed to borrow LAST frame's raw object pointers only while the object set and
    // layer mask still match that queue. Editor placement removes the preview and bumps the set
    // version before this function; without this gate the previous queue retains a dangling
    // pointer until PrepareViewQueue clears it later in this same frame.
    const bool previousCascadeQueuesCurrent =
        renderQueueSourcesValid_ &&
        renderQueueSourceVersion_ == staticSetVersion_ &&
        renderQueueSourceMask_ == camera.GetRenderLayerMask();
    for (size_t idx = 0; idx < cascadeViews_.size(); ++idx)
    {
        const float sliceNear = cascades.splitsVS[idx];
        const float sliceFar = cascades.splitsVS[idx + 1];

        std::array<float3, 8> cornersWS{};
        BuildFrustumSliceCornersWS(invView, invProj, sliceNear, sliceFar, cornersWS);

        // Step 2a / S1: fit each cascade to the rotation-INVARIANT bounding sphere of its
        // slice corners. The sphere center and radius depend only on the slice shape
        // (near/far + FOV), never on camera/light orientation, so unitsPerTexel is
        // constant frame-to-frame and the light-space texel snap below is the ONLY
        // stabilization needed (no edge crawl / bias swim on rotation). A sphere also
        // projects to a circle of the same radius in any light orientation, so the ortho
        // extent never changes with the sun/camera angle. (The old far-plane center +
        // far-corner radius + coarse world-space snap did NOT enclose the near corners
        // once the snap shifted the center, so the radius had to be clamped to the
        // rotation-dependent corner extent — that clamp was the source of the shimmer.)
        //
        // S1 replaces the centroid of the 8 corners with the true MINIMAL enclosing sphere
        // (centre on the view axis, closed form — see ComputeCascadeSphere). The centroid was
        // never the minimal sphere: for cascade 0 it gives 12.51 m where 11.47 m suffices, and
        // every millimetre of radius is millimetres of shadow texel, for free.
        const CascadeSphere sphere = ComputeCascadeSphere(camera, cornersWS, sliceNear, sliceFar);
        const float3 sphereCenter = sphere.center;
        const float sphereRadius = sphere.radius;

        // S2: the padding is expressed in texels, but a texel's size depends on radius, which
        // depends on the padding. One pass is enough: seeding the estimate from the unpadded
        // radius makes the final texel larger than the estimate by only overlapInTexels*2/tileRes
        // (~0.2% for cascade 0), and the slack below is a whole texel, so the fixed point is
        // never needed. Assert safety is structural, not empirical: the snap shifts the centre by
        // at most one unitsPerTexel per axis, and the padding is two estimated texels.
        const float texelEstimate = (2.0f * sphereRadius) / static_cast<float>(contentRes);
        const float radius = sphereRadius + cascadeConfig_.overlapInTexels * texelEstimate;
        const float unitsPerTexel = (2.0f * radius) / static_cast<float>(contentRes);

        const float3 up(0, 1, 0);
        // S7: the light "eye" sits behind the cascade sphere by casterReachWS, not at a fixed
        // maxDistance for every cascade. It does NOT set the depth range (near/far below do) -- it
        // only guarantees that any caster within casterReachWS of the light still has z_ls > 0, i.e.
        // lies inside the culling ortho box. The old max(1, maxDistance) put the eye INSIDE the
        // volume for the far cascade (lightDistance < radius), where near hit the 0.001 clamp.
        const float lightDistance = radius + cascadeConfig_.casterReachWS + 1.0f;

        // Texel snap — the actual stabilization. Snap the cascade center along the FIXED
        // light right/up axes to whole-texel steps in WORLD space, BEFORE building the
        // view. (The previous `lightView * center` snap was a no-op: center is the LookAt
        // target, so its light-space XY is always (0,0).) Snapping the center in a fixed
        // light frame makes the covered world region shift in whole-texel increments as
        // the camera moves, pinning shadow texels to fixed world cells -> no edge crawl.
        const float3 fwd = sunDirWS.Normalized();
        float3 right = up.Cross(fwd);
        if (right.Dot(right) < 1e-12f) { right = float3(0, 0, 1).Cross(fwd); }
        right = right.Normalized();
        const float3 trueUp = fwd.Cross(right);

        float3 center = sphereCenter;
        if (unitsPerTexel > 0.0f)
        {
            const float cx = center.Dot(right);
            const float cy = center.Dot(trueUp);
            center = center
                + right  * (std::floor(cx / unitsPerTexel) * unitsPerTexel - cx)
                + trueUp * (std::floor(cy / unitsPerTexel) * unitsPerTexel - cy);
        }

        const mat4 lightView = mat4::LookAtLH(center - sunDirWS * lightDistance, center, up);

        const float2 centerLS = (lightView * float4(center, 1)).xy(); // ~(0,0): center is the target
        float minZ = +1e9f;
        float maxZ = -1e9f;
        float rLS = 0.0f;
        for (const auto& corner : cornersWS)
        {
            const float3 ls = (lightView * float4(corner, 1)).xyz();
            rLS = std::max(rLS, std::max(std::abs(ls.x - centerLS.x), std::abs(ls.y - centerLS.y)));
            minZ = std::min(minZ, ls.z);
            maxZ = std::max(maxZ, ls.z);
        }
        // The bounding sphere encloses every corner by construction (plus the <=1-texel
        // snap shift, absorbed by `overlapInTexels`), so the ortho square contains every corner.
        assert(radius + 1e-3f >= rLS && "cascade ortho radius under-covers slice corners");
        (void)rLS;

        const float minX = centerLS.x - radius;
        const float maxX = centerLS.x + radius;
        const float minY = centerLS.y - radius;
        const float maxY = centerLS.y + radius;

        // S7 -- PANCAKING. `nearLS` used to be ONE number pulled back by casterReachWS so casters
        // between the sun and this slice would not be clipped; that inflated cascade 0's ortho depth
        // range to ~200 m on a D16 atlas. It is now TWO numbers, and keeping them apart is the whole
        // step:
        //
        //   nearProjLS  tight, = minZ            -> OrthoOffCenterLH. Shrinks the D16 range.
        //   nearCullLS  wide,  = minZ - reach    -> Frustum::FromOrthoBounds. Keeps the casters.
        //
        // Casters in front of the projection near plane are NOT clipped: the pancake clamp in the
        // shadow-depth VS (ApplyShadowDepthBias, shadow_depth_common.hlsli) presses them onto it.
        // Shrinking the CULL near instead would delete exactly the casters pancaking exists to save
        // -- they would never reach the VS -- and `Frustum::FromOrthoBounds` feeds both the CPU cull
        // (SceneView::frustum) and the GPU cull (Frustum::Planes -> ShadowGpuData::UpdateViewFrustums,
        // slots 0..3). So `casterReachWS` stays alive; only its meaning narrows, from "how far back
        // to push the projection" to "how far toward the light to look for casters".
        const float zPad = cascadeConfig_.zPadding;
        const float nearProjLS = std::max(0.001f, minZ - cascadeConfig_.pancakeSlackWS);
        const float nearCullLS = std::max(0.001f, minZ - cascadeConfig_.casterReachWS);
        const float farLS = maxZ + zPad;

        const mat4 lightProj = mat4::OrthoOffCenterLH(minX, maxX, minY, maxY, nearProjLS, farLS);

        const float depthBiasInTexels = cascadeConfig_.depthBiasInTexels;
        // The cascade's world texel. Was `normalBiasInTexels * unitsPerTexel` until the receiver
        // normal offset was deleted for UE parity; the legacy 3x3 filter arm still needs the
        // per-cascade RATIO, which is the same number with the artist multiplier divided out.
        cascades.cascadeTexelWS[idx] = unitsPerTexel;
        cascades.depthBiasNDC[idx] = (depthBiasInTexels * unitsPerTexel) / (farLS - nearProjLS);

        // S5: the sampled rect is the CONTENT rect -- tile origin pushed in by the border, sized
        // contentRes. uvLocal in [0,1] therefore means "inside the cascade's world square" again,
        // which is what lets the shader drop the old margin test entirely.
        const float atlasRes = static_cast<float>(deferred.shadowRes);
        const float2 scale = float2(static_cast<float>(contentRes) / atlasRes);
        const float tileOriginX = static_cast<float>((idx % 2) * tileRes + borderRes);
        const float tileOriginY = static_cast<float>((idx / 2) * tileRes + borderRes);
        const float2 bias = float2(tileOriginX / atlasRes, tileOriginY / atlasRes);
        cascades.atlasScale[idx] = scale;
        cascades.atlasBias[idx] = bias;
        cascades.lightView[idx] = lightView;
        cascades.lightProj[idx] = lightProj;

        // S11: the view-cone scissor, from the FINAL projection (anything else and the rect drifts
        // off the tile's content). Computed even while the toggle is off, for the readout.
        {
            const CascadeScissor sc = ComputeCascadeScissor(
                cornersWS, camera.GetPosition(), lightView * lightProj, contentRes,
                static_cast<UINT>(tileOriginX), static_cast<UINT>(tileOriginY),
                cascadeConfig_.scissorPadTexels);
            cascades.scissor[idx] = sc.rect;
            cascades.scissorAreaDbg[idx] = sc.areaFrac;
        }

        SceneView& cascadeView = cascadeViews_[idx];
        cascadeView.view = lightView;
        cascadeView.proj = lightProj;
        cascadeView.invView = mat4::Inverse(lightView);
        cascadeView.invProj = mat4::Inverse(lightProj);
        // The ONLY consumer of the wide near: everything else (proj, invProj, depthBiasNDC, the
        // readout) takes the projection pair.
        cascadeView.frustum = Frustum::FromOrthoBounds(cascadeView.invView, minX, maxX, minY, maxY,
                                                      nearCullLS, farLS);

        // S14: replace the box with UE's accurate caster volume (BuildCascadeCullVolume). The
        // slice is extended toward the camera by the PREVIOUS cascade's blend band: receivers in
        // that band sample this cascade too (S10 cross-fade), so the casters over them have to
        // survive this cull. The box's depth caps carry over as planes; its XY faces are implied.
        cascades.cullPlanesDbg[idx] = 6u;
        cascades.cullLeakDbg[idx] = 0u;
        {
            const float bandPrev = (idx > 0)
                ? (cascades.splitsVS[idx] - cascades.splitsVS[idx - 1]) * cascadeConfig_.blendFraction
                : 0.0f;
            std::array<float3, 8> cullCorners{};
            BuildFrustumSliceCornersWS(invView, invProj, std::max(zNear, sliceNear - bandPrev),
                                       sliceFar, cullCorners);
            // The ortho box's six inward planes, taken from the box Frustum just built rather than
            // re-derived: the volume must be the box INTERSECTED with the prism, and the only way
            // to guarantee that is to reuse the very planes the box arm tests with.
            const Frustum boxFrustum = cascadeView.frustum;
            const CascadeCullVolume vol = BuildCascadeCullVolume(cullCorners, camera.GetPosition(),
                                                                 fwd * -1.0f, boxFrustum.Planes(),
                                                                 boxFrustum.PlaneCount());
            const Frustum accFrustum = (vol.count > 0) ? Frustum::FromPlanes(vol.planes, vol.count)
                                                       : boxFrustum;
            // Built even while the toggle is off (a few hundred flops), so the readout can report
            // both directions of the cross-check: see CascadeData::cullLeakDbg.
            //
            // The survivors in the queue were culled LAST frame, by last frame's volume, so the
            // cross-check tests them against last frame's OTHER arm (cascadeBoxPrev_/AccPrev_) --
            // this frame's box has moved with the camera, and on a fly-through the object it no
            // longer covers is not a leak but a turn of the head (Debug assert, 2026-09-03). Objects
            // whose bounds may have changed since that cull -- dynamic casters, an editor move this
            // frame -- prove nothing either way and are left out. Frame 0 has no previous pair; an
            // invalid Frustum passes everything.
            const Frustum& otherPrev = cascadeConfig_.accurateCasterCull ? cascadeBoxPrev_[idx] : cascadeAccPrev_[idx];
            const auto settledCaster = [](const RenderableObjectBase* o)
            {
                const RenderableObject* ro = o->AsRenderableObject();
                return !o->IsDynamicCaster() && !(ro && ro->MovedThisFrame());
            };
            const bool listSurvivors = previousCascadeQueuesCurrent && render::g_csmDumpReadout &&
                renderer->GetTotalFrameNumber() > 600 && idx == 0;
            if (previousCascadeQueuesCurrent)
            {
                for (const auto& bucket : cascadeView.queue.VisibleBuckets())
                {
                    for (const RenderableObjectBase* obj : bucket)
                    {
                        if (!obj) { continue; }
                        const AABB& b = obj->GetWorldBounds();
                        if (settledCaster(obj) && b.IsValid() && !otherPrev.Intersects(b)) { ++cascades.cullLeakDbg[idx]; }
                        if (listSurvivors)
                        {
                            // Readout diagnostic: every cascade-0 survivor with both verdicts (of the
                            // frame that culled it), so a box-vs-volume count mismatch can be traced
                            // to the object, not guessed at.
                            char line[200];
                            const float3 mn = b.IsValid() ? b.GetMin() : float3(0, 0, 0);
                            const float3 mx = b.IsValid() ? b.GetMax() : float3(0, 0, 0);
                            const InstancedDrawBatch* batch = obj->AsInstancedDrawBatch();
                            std::snprintf(line, sizeof(line), "  c0 %p members=%u valid=%d box=%d acc=%d min=(%.1f,%.1f,%.1f) max=(%.1f,%.1f,%.1f)\n",
                                          static_cast<const void*>(obj), batch ? static_cast<unsigned>(batch->InstanceCount()) : 1u,
                                          b.IsValid() ? 1 : 0,
                                          (!b.IsValid() || cascadeBoxPrev_[idx].Intersects(b)) ? 1 : 0,
                                          (!b.IsValid() || cascadeAccPrev_[idx].Intersects(b)) ? 1 : 0,
                                          mn.x, mn.y, mn.z, mx.x, mx.y, mx.z);
                            c0Survivors += line;
                        }
                    }
                }
            }
#ifndef NDEBUG
            // Session-log records BEFORE the assert below can abort -- the dialog names the check,
            // these name the object, with its verdict against BOTH frames' pairs:
            //   LEAK  (Fatal, flushed synchronously) -- a settled survivor fails last frame's other
            //          arm: a real defect of the volume; every leaker and the volume's planes are
            //          logged, then the assert fires;
            //   stale (Debug) -- it passes last frame's arm but fails THIS frame's: the camera moved
            //          between the cull and the check. This is what the cross-frame check asserted
            //          on until 2026-09-03; capped so a fly-through cannot flood the log.
            if (previousCascadeQueuesCurrent)
            {
                static std::uint32_t staleLogged = 0;
                const Frustum& otherNow = cascadeConfig_.accurateCasterCull ? boxFrustum : accFrustum;
                for (const auto& bucket : cascadeView.queue.VisibleBuckets())
                {
                    for (const RenderableObjectBase* obj : bucket)
                    {
                        if (!obj) { continue; }
                        const AABB& b = obj->GetWorldBounds();
                        if (!b.IsValid()) { continue; }
                        const bool failPrev = !otherPrev.Intersects(b);
                        const bool failNow = !otherNow.Intersects(b);
                        if (!failPrev && !failNow) { continue; }
                        const bool settled = settledCaster(obj);
                        const bool leak = failPrev && settled;
                        if (!leak && staleLogged >= 32u) { continue; }
                        if (!leak) { ++staleLogged; }
                        const float3 mn = b.GetMin(), mx = b.GetMax();
                        const float3 cam = camera.GetPosition();
                        const InstancedDrawBatch* batch = obj->AsInstancedDrawBatch();
                        char line[400];
                        std::snprintf(line, sizeof(line),
                                      "S14 cascade %zu: %s obj %p members=%u settled=%d dyn=%d boxPrev=%d accPrev=%d boxNow=%d accNow=%d "
                                      "min=(%.2f,%.2f,%.2f) max=(%.2f,%.2f,%.2f) cam=(%.2f,%.2f,%.2f) accurateCull=%d planes=%d",
                                      idx, leak ? "LEAK" : "stale",
                                      static_cast<const void*>(obj), batch ? static_cast<unsigned>(batch->InstanceCount()) : 1u,
                                      settled ? 1 : 0, obj->IsDynamicCaster() ? 1 : 0,
                                      cascadeBoxPrev_[idx].Intersects(b) ? 1 : 0, cascadeAccPrev_[idx].Intersects(b) ? 1 : 0,
                                      boxFrustum.Intersects(b) ? 1 : 0, accFrustum.Intersects(b) ? 1 : 0,
                                      mn.x, mn.y, mn.z, mx.x, mx.y, mx.z, cam.x, cam.y, cam.z,
                                      cascadeConfig_.accurateCasterCull ? 1 : 0, vol.count);
                        if (leak)
                        {
                            LOG_FATAL(logging::LogCategory::RenderShadow, "{}", line);
                            for (int i = 0; i < cascadeAccPrev_[idx].PlaneCount(); ++i)
                            {
                                const float4& pl = cascadeAccPrev_[idx].Planes()[i];
                                LOG_FATAL(logging::LogCategory::RenderShadow, "   prev plane {:2d}: ({:.5f},{:.5f},{:.5f}, {:.3f})",
                                          i, pl.x, pl.y, pl.z, pl.w);
                            }
                        }
                        else
                        {
                            LOG_DEBUG(logging::LogCategory::RenderShadow, "{}", line);
                        }
                    }
                }
            }
#endif
            // Remembered for next frame's cross-check: the pair the queue about to be rebuilt will
            // have been culled with (accFrustum already IS the box when the volume degenerated).
            cascadeBoxPrev_[idx] = boxFrustum;
            cascadeAccPrev_[idx] = accFrustum;
            if (cascadeConfig_.accurateCasterCull && vol.count > 0)
            {
                cascadeView.frustum = accFrustum;
                cascades.cullPlanesDbg[idx] = static_cast<std::uint32_t>(vol.count);
#ifndef NDEBUG
                // The volume carries every box plane, so it can pass no object the box rejects:
                // last frame's survivors (each passed last frame's volume) must all pass last
                // frame's box. Leakers, if any, are already in the session log (LEAK records above).
                assert(cascades.cullLeakDbg[idx] == 0u && "S14 cull volume passed an object the ortho box rejects");
                // Self-check, the same spirit as the ortho-radius assert above: the volume must
                // hold the whole (unextended) slice and everything toward the sun within reach,
                // and must reject points past the far cap and beside the silhouette.
                // `inside` also reports the worst plane, and a failing probe goes to the session
                // log (Fatal) BEFORE the assert aborts -- the dialog names the check, not
                // the plane, and the plane is what tells the geometry from the tolerance.
                int worstPlane = -1;
                float worstDist = 0.0f;
                const auto inside = [&](const float3& p)
                {
                    worstPlane = -1; worstDist = 1e30f;
                    for (int i = 0; i < vol.count; ++i)
                    {
                        const float4& pl = vol.planes[i];
                        const float d = pl.x * p.x + pl.y * p.y + pl.z * p.z + pl.w;
                        if (d < worstDist) { worstDist = d; worstPlane = i; }
                    }
                    return worstDist >= -1e-3f;
                };
                const auto report = [&](const char* what, int corner, const float3& p)
                {
                    // Fatal: flushed synchronously, so the record is on disk whatever the dialog's
                    // button does to the process.
                    LOG_FATAL(logging::LogCategory::RenderShadow,
                              "S14 cascade {}: {}  corner {}  p=({:.3f},{:.3f},{:.3f})  worst plane {}/{} dist {:.5f}  "
                              "sun=({:.4f},{:.4f},{:.4f}) reach={:.1f} nearCull={:.3f} minZ~{:.3f} radius={:.3f}",
                              idx, what, corner, p.x, p.y, p.z, worstPlane, vol.count, worstDist,
                              sunDirWS.x, sunDirWS.y, sunDirWS.z, cascadeConfig_.casterReachWS,
                              nearCullLS, minZ, radius);
                    for (int i = 0; i < vol.count; ++i)
                    {
                        LOG_FATAL(logging::LogCategory::RenderShadow, "   plane {:2d}: ({:.5f},{:.5f},{:.5f}, {:.3f})",
                                  i, vol.planes[i].x, vol.planes[i].y, vol.planes[i].z, vol.planes[i].w);
                    }
                };
                // Toward the sun the volume is open down to the CULL near plane -- which is
                // `nearCullLS`, not "minZ - casterReachWS": the two differ when the clamp to 0.001
                // bites. So the probe steps half-way from the corner to that plane, measured the
                // way the box measures it, (p - eye) . fwd.
                const float3 eyeWS = center - sunDirWS * lightDistance;
                for (int ci = 0; ci < 8; ++ci)
                {
                    const float3& p = cornersWS[ci];
                    if (!inside(p)) { report("drops a slice corner", ci, p); }
                    assert(inside(p) && "S14 cull volume drops a slice corner");
                    const float zp = (p - eyeWS).Dot(fwd);
                    const float step = std::max(0.0f, 0.5f * (zp - nearCullLS));
                    const float3 q = p - fwd * step;
                    if (!inside(q)) { report("closed toward the sun", ci, q); }
                    assert(inside(q) && "S14 cull volume closed toward the sun");
                }
                {
                    const float3 q = center + fwd * (2.0f * radius + zPad + 10.0f);
                    if (inside(q)) { report("open past the far cap", -1, q); }
                    assert(!inside(q) && "S14 cull volume open past the far cap");
                }
                {
                    const float3 q = center + right * (2.0f * radius + 1.0f);
                    if (inside(q)) { report("open beside the silhouette", -1, q); }
                    assert(!inside(q) && "S14 cull volume open beside the silhouette");
                }
#endif
            }
        }
        {
            // LAST frame's CPU cull result for this view (the queue is rebuilt after UpdateCascades),
            // so a headless readout can show what the volume cuts. Counts CASTERS, not queue
            // entries: an InstancedDrawBatch stands for InstanceCount() of them, and whether a run
            // batches at all depends on the whole bucket clearing kInstancingThreshold -- culling
            // one bush out of a bucket of 8 un-batched six palms and made 17 entries out of 13.
            std::uint32_t seen = 0;
            if (previousCascadeQueuesCurrent)
            {
                for (const auto& bucket : cascadeView.queue.VisibleBuckets())
                {
                    for (const RenderableObjectBase* obj : bucket)
                    {
                        if (!obj) { continue; }
                        const InstancedDrawBatch* batch = obj->AsInstancedDrawBatch();
                        seen += batch ? static_cast<std::uint32_t>(batch->InstanceCount()) : 1u;
                    }
                }
            }
            cascades.cullCastersDbg[idx] = seen;
        }
        cascadeView.renderLayerMask = camera.GetRenderLayerMask();
        cascadeView.position = center;
        cascadeView.type = SceneView::Type::Shadow;
        cascadeView.zNear = sliceNear;
        cascadeView.zFar = sliceFar;
        cascadeView.hfov = 0.0f;
        cascadeView.requiresDepthCheck = true;

        // S0.1: publish the values this cascade was actually built with. Every tuning decision
        // downstream (sphere fit, texel-space overlap, per-cascade resolution, pancaking) is
        // judged on these numbers, so they are copied here rather than re-derived in the UI.
        cascades.sphereRadiusDbg[idx] = sphereRadius;
        cascades.radiusDbg[idx] = radius;
        cascades.unitsPerTexelDbg[idx] = unitsPerTexel;
        cascades.nearLsDbg[idx] = nearProjLS;
        cascades.farLsDbg[idx] = farLS;
        cascades.tileSizeDbg[idx] = contentRes;
    }

    // S7: the readout the dev-window CSM tab shows, dumped once so a HEADLESS run can check it.
    // zRange and the D16 step are the whole acceptance criterion for pancaking, and until now they
    // existed only behind a GUI that a --shot capture cannot drive.
    // Not on frame 0: `--set=` is applied after the first UpdateCascades, so an immediate dump
    // reports the compile-time defaults no matter what the command line asked for -- a readout
    // that quietly contradicts the flags beside it.
    // 600, not 8 (S14): the `casters` column reads LAST frame's cull result, and at frame 8 the
    // level is still streaming in -- two configs that cull identically showed 13 vs 17 objects on
    // cascade 0 purely from load progress. Two seconds at 300 fps is past that; a --shot run lasts
    // eight seconds anyway.
    if (render::g_csmDumpReadout && previousCascadeQueuesCurrent &&
        renderer->GetTotalFrameNumber() > 600)
    {
        render::g_csmDumpReadout = false;
        // One complete table, replaced atomically: a reader never sees a half-written readout.
        diag::ArtifactFile f("csm_readout.log", diag::ArtifactMode::AtomicReplace);
        if (f)
        {
            // S11: the last two columns are the view-cone scissor -- what share of the tile it
            // rasterises and the rect itself (atlas texels). Printed whether or not the toggle is on
            // (the rect is always computed), so a headless A/B can prove the scissor actually FIRED
            // rather than infer it from a neutral image; `applied` says which.
            // S14: `cullPl` = planes of the caster-cull volume (6 = ortho box, more = the accurate
            // volume), `casters` = objects the CPU cull passed for that cascade LAST frame.
            f.Printf("cascade  slice(m)         tile  texel(mm)  radius(m)  nearLS   farLS   zRange(m)  D16step(mm)  scissor%%  rect(atlas)  cullPl  casters  leak  applied=%d accurateCull=%d\n",
                     cascadeConfig_.scissorOptim ? 1 : 0, cascadeConfig_.accurateCasterCull ? 1 : 0);
            for (int i = 0; i < kCascades; ++i)
            {
                const float zRange = cascades.farLsDbg[i] - cascades.nearLsDbg[i];
                const auto& sr = cascades.scissor[i];
                f.Printf("%d  %8.2f..%-8.2f %5u  %8.3f  %9.2f  %7.2f %7.2f  %9.2f  %11.4f  %7.1f  %d,%d-%d,%d  %6u  %7u  %4u\n",
                         i, cascades.splitsVS[i], cascades.splitsVS[i + 1],
                         cascades.tileSizeDbg[i], cascades.unitsPerTexelDbg[i] * 1000.0f,
                         cascades.radiusDbg[i], cascades.nearLsDbg[i], cascades.farLsDbg[i],
                         zRange, (zRange / 65535.0f) * 1000.0f,
                         cascades.scissorAreaDbg[i] * 100.0f, sr.x0, sr.y0, sr.x1, sr.y1,
                         cascades.cullPlanesDbg[i], cascades.cullCastersDbg[i], cascades.cullLeakDbg[i]);
            }
            if (!c0Survivors.empty()) { f.Printf("cascade 0 survivors (last frame), box/acc verdicts:\n%s", c0Survivors.c_str()); }
        }
    }
}

void Scene::UpdateClipmap(const Camera& camera)
{
    // Step 24d: N camera-centered nested ortho shadow views along the sun — the directional clipmap
    // VSM mode uses instead of CSM cascades. Level i covers extent E0*2^i (E0 = the finest,
    // runtime-tunable g_clipmapBaseExtent), texel-snapped in the fixed light frame for stability
    // (mirrors the cascade snap). Consumed only in VSM mode; add-dormant until 24e/24f render+sample.
    const float3 sunDirWS = dirLight_.GetDirection();
    const float3 fwd = sunDirWS.Normalized();
    if (fwd.Dot(fwd) < 1e-8f) // no valid sun direction -> reject-all views
    {
        clipmapSquares_ = {}; // no sun -> no squares; the fallback chain skips every level
        for (auto& v : clipmapViews_)
        {
            v.frustum = Frustum{};
            v.type = SceneView::Type::Shadow;
            v.renderLayerMask = camera.GetRenderLayerMask();
            v.requiresDepthCheck = false;
        }
        return;
    }
    const float3 up(0, 1, 0);
    float3 right = up.Cross(fwd);
    if (right.Dot(right) < 1e-12f) { right = float3(0, 0, 1).Cross(fwd); }
    right = right.Normalized();
    const float3 trueUp = fwd.Cross(right);

    const float tileRes = static_cast<float>(vsm::kVirtualRes); // texels per clipmap level edge
    const float baseExtent = std::max(1.0f, vsm::g_clipmapBaseExtent);
    const float3 camPos = camera.GetPosition();

    for (size_t i = 0; i < clipmapViews_.size(); ++i)
    {
        const float extent = baseExtent * static_cast<float>(1u << static_cast<unsigned>(i)); // E0*2^i
        const float radius = 0.5f * extent;
        const float unitsPerTexel = extent / tileRes;

        // Texel-snap the level center (= camera) in the FIXED light frame, so shadow texels pin to
        // world cells as the camera moves (no swim) — same stabilization as the cascades.
        float3 center = camPos;
        const float cx = center.Dot(right);
        const float cy = center.Dot(trueUp);
        center = center
            + right  * (std::floor(cx / unitsPerTexel) * unitsPerTexel - cx)
            + trueUp * (std::floor(cy / unitsPerTexel) * unitsPerTexel - cy);

        // PER-LEVEL depth range that scales with the level extent (Step 24f): proportionally larger
        // for coarse far levels, so a SINGLE NDC depth bias works at every level. depthUp = caster
        // reach up-sun (well above the level's ground); depthDown = receivers.
        // Reach up-sun is radius * g_clipmapZRangeScale, the same shape as UE's
        // r.Shadow.Virtual.Clipmap.ZRangeScale. It used to be `extent * 5` = radius * 10, which is
        // UE's stated MINIMUM and left the finest levels reaching 40 m -- short enough to clip the
        // top off a tall caster and delete the shadow it casts. (The original "tight range for D16
        // precision" reasoning died when the page pool became D32_FLOAT; see VirtualShadowMap.cpp.)
        const float depthUp = radius * vsm::g_clipmapZRangeScale;
        const float depthDown = extent * 1.0f;
        const float originDist = depthUp + 1.0f;
        const mat4 lightView = mat4::LookAtLH(center - sunDirWS * originDist, center, up);
        const float2 centerLS = (lightView * float4(center, 1)).xy(); // ~(0,0): center is the target
        const float minX = centerLS.x - radius, maxX = centerLS.x + radius;
        const float minY = centerLS.y - radius, maxY = centerLS.y + radius;
        const float nearLS = 1.0f;
        const float farLS = originDist + depthDown;

        const mat4 lightProj = mat4::OrthoOffCenterLH(minX, maxX, minY, maxY, nearLS, farLS);
        // For the LOD fallback chain: this level's square in the FIXED light frame. `center` is
        // already texel-snapped in that frame just above, so the value stored here is exactly the
        // one the page mapping must agree with.
        clipmapSquares_.level[i].centreX = center.Dot(right);
        clipmapSquares_.level[i].centreY = center.Dot(trueUp);
        clipmapSquares_.level[i].extent = extent;
        clipmapSquares_.count = static_cast<std::uint32_t>(i + 1);

        SceneView& v = clipmapViews_[i];
        v.view = lightView;
        v.proj = lightProj;
        v.invView = mat4::Inverse(lightView);
        v.invProj = mat4::Inverse(lightProj);
        v.frustum = Frustum::FromOrthoBounds(v.invView, minX, maxX, minY, maxY, nearLS, farLS);
        v.renderLayerMask = camera.GetRenderLayerMask();
        v.position = center;
        v.type = SceneView::Type::Shadow;
        v.zNear = nearLS;
        v.zFar = farLS;
        v.hfov = 0.0f;
        v.requiresDepthCheck = true;
    }
}

void Scene::SetDirectionalLight(DirectionalLight light)
{
    dirLight_ = light;
}

void Scene::SetSkybox(std::unique_ptr<Skybox> skybox)
{
    skyBox_ = std::move(skybox);
}

void Scene::AddObject(std::unique_ptr<RenderableObjectBase> obj) {
#if WITH_EDITOR
    if (obj)
    {
        obj->SetEditorObjectId(0);
    }
#endif
    objects_.push_back(std::move(obj));
#if WITH_EDITOR
    objectIds_.push_back(0); // runtime object: no editor identity
#endif
    BumpStaticSetVersion();
}

bool Scene::AddInitializedObject(Renderer& renderer, UploadBatch& uploads, std::unique_ptr<RenderableObjectBase> obj)
{
    if (!obj || !uploads.IsOpen())
    {
        return false;
    }

    obj->Init(&renderer, uploads.CommandList(), uploads.KeepAlive());
    obj->SyncSceneState(SceneObjectSyncReason::RuntimeSpawn);
#if WITH_EDITOR
    obj->SetEditorObjectId(0);
#endif
    objects_.push_back(std::move(obj));
#if WITH_EDITOR
    objectIds_.push_back(0);
#endif
    BumpStaticSetVersion();
    return true;
}

bool Scene::RemoveOceanObjects()
{
    bool removed = false;
    for (size_t i = 0; i < objects_.size();)
    {
        if (objects_[i] && objects_[i]->AsOceanRenderable())
        {
            objects_.erase(objects_.begin() + static_cast<ptrdiff_t>(i));
#if WITH_EDITOR
            objectIds_.erase(objectIds_.begin() + static_cast<ptrdiff_t>(i));
#endif
            removed = true;
            continue;
        }
        ++i;
    }
    if (removed) { BumpStaticSetVersion(); }
    return removed;
}

void Scene::SetOceanVisible(bool visible)
{
    bool changed = false;
    for (std::unique_ptr<RenderableObjectBase>& obj : objects_)
    {
        if (obj && obj->AsOceanRenderable())
        {
            changed |= obj->IsVisible() != visible;
            obj->SetVisible(visible);
        }
    }
    if (changed) { BumpStaticSetVersion(); }
}

#if WITH_EDITOR
Scene::SceneObjectId Scene::AddEditorObject(std::unique_ptr<RenderableObjectBase> obj)
{
    const SceneObjectId id = nextEditorId_++;
    if (obj)
    {
        obj->SetEditorObjectId(id);
    }
    objects_.push_back(std::move(obj));
    objectIds_.push_back(id);
    BumpStaticSetVersion();
    return id;
}

void Scene::AddObjectWithEditorId(std::unique_ptr<RenderableObjectBase> obj, SceneObjectId id)
{
    assert(id != 0 && "AddObjectWithEditorId requires a non-zero editor id");

    if (obj)
    {
        obj->SetEditorObjectId(id);
    }
    objects_.push_back(std::move(obj));
    objectIds_.push_back(id);
    BumpStaticSetVersion();

    // Keep the auto-allocator ahead of level-supplied ids so AddEditorObject
    // never hands out a colliding id later.
    if (id >= nextEditorId_)
    {
        nextEditorId_ = id + 1;
    }
}

bool Scene::AddInitializedEditorObject(Renderer& renderer, UploadBatch& uploads, SceneObjectId id, std::unique_ptr<RenderableObjectBase> obj)
{
    if (!obj || !uploads.IsOpen())
    {
        return false;
    }

    obj->Init(&renderer, uploads.CommandList(), uploads.KeepAlive());
    obj->SetEditorObjectId(id);
    obj->SyncSceneState(SceneObjectSyncReason::EditorSpawn);

    // Keep the auto-allocator ahead of editor-supplied ids so AddEditorObject
    // never hands out a colliding id later.
    if (id >= nextEditorId_)
    {
        nextEditorId_ = id + 1;
    }

    objects_.push_back(std::move(obj));
    objectIds_.push_back(id);
    BumpStaticSetVersion();
    return true;
}

bool Scene::RemoveEditorObject(SceneObjectId id)
{
    if (id == 0) // 0 is shared by all non-editor objects; never match on it
    {
        return false;
    }
    bool removed = false;
    for (size_t i = 0; i < objectIds_.size();)
    {
        if (objectIds_[i] == id)
        {
            objects_.erase(objects_.begin() + static_cast<ptrdiff_t>(i));
            objectIds_.erase(objectIds_.begin() + static_cast<ptrdiff_t>(i));
            removed = true;
            continue;
        }
        ++i;
    }
    if (removed) { BumpStaticSetVersion(); }
    return removed;
}

void Scene::RefreshShadowGpuForEditor(Renderer& renderer)
{
    // An editor spawn/delete changed the caster set, so ShadowGpuData's next UpdateForFrame will
    // Rebuild and drop megaReady_ — but nothing rebuilds the consolidated mega VB/IB mid-game (it is
    // only built at level load). Without it, VirtualShadowMap::RecordPageRender falls back to per-group
    // binding: 1024 pool pages × mesh-groups × (bind VB/IB + ExecuteIndirect) → ~10ms CPU, forever.
    RebuildShadowCasters(renderer);
}

RenderableObjectBase* Scene::FindEditorObject(SceneObjectId id)
{
    if (id == 0)
    {
        return nullptr;
    }
    for (size_t i = 0; i < objectIds_.size(); ++i)
    {
        if (objectIds_[i] == id)
        {
            return objects_[i].get();
        }
    }
    return nullptr;
}

const RenderableObjectBase* Scene::FindEditorObject(SceneObjectId id) const
{
    if (id == 0)
    {
        return nullptr;
    }
    for (size_t i = 0; i < objectIds_.size(); ++i)
    {
        if (objectIds_[i] == id)
        {
            return objects_[i].get();
        }
    }
    return nullptr;
}

void Scene::SetSelectedEditorObjectIds(const std::vector<SceneObjectId>& ids)
{
    selectedEditorObjectIds_.fill(0);
    selectedEditorObjectCount_ = 0;
    for (const SceneObjectId id : ids)
    {
        if (id == 0 || selectedEditorObjectCount_ >= selectedEditorObjectIds_.size())
        {
            continue;
        }

        bool alreadySelected = false;
        for (std::uint32_t i = 0; i < selectedEditorObjectCount_; ++i)
        {
            if (selectedEditorObjectIds_[i] == id)
            {
                alreadySelected = true;
                break;
            }
        }
        if (!alreadySelected)
        {
            selectedEditorObjectIds_[selectedEditorObjectCount_++] = id;
        }
    }
}

Scene::SceneObjectId Scene::RaycastEditorObject(const Math::float3& origin,
    const Math::float3& dir,
    float* outDistance,
    SceneObjectId ignoredObjectId,
    const std::vector<SceneObjectId>* ignoredObjectIds) const
{
    SceneObjectId best = 0;
    float bestT = FLT_MAX;
    if (outDistance)
    {
        *outDistance = bestT;
    }
    const float o[3] = { origin.x, origin.y, origin.z };
    const float d[3] = { dir.x, dir.y, dir.z };

    for (size_t i = 0; i < objects_.size(); ++i)
    {
        const bool ignoredBySet = ignoredObjectIds &&
            std::find(ignoredObjectIds->begin(), ignoredObjectIds->end(), objectIds_[i]) != ignoredObjectIds->end();
        if (objectIds_[i] == 0 || objectIds_[i] == ignoredObjectId || ignoredBySet ||
            !objects_[i] || !objects_[i]->IsVisible() || !objects_[i]->IsRaycastPickable())
        {
            continue; // editor-owned + visible + pickable only (skip emitters/helpers)
        }

        const AABB& bounds = objects_[i]->GetWorldBounds();
        if (!bounds.IsValid())
        {
            continue;
        }

        const Math::float3 mn = bounds.GetMin();
        const Math::float3 mx = bounds.GetMax();
        const float lo[3] = { mn.x, mn.y, mn.z };
        const float hi[3] = { mx.x, mx.y, mx.z };

        // Slab test.
        float tmin = 0.0f;
        float tmax = FLT_MAX;
        bool hit = true;
        for (int a = 0; a < 3; ++a)
        {
            if (std::fabs(d[a]) < 1e-8f)
            {
                if (o[a] < lo[a] || o[a] > hi[a]) { hit = false; break; }
            }
            else
            {
                float inv = 1.0f / d[a];
                float t1 = (lo[a] - o[a]) * inv;
                float t2 = (hi[a] - o[a]) * inv;
                if (t1 > t2) { std::swap(t1, t2); }
                tmin = std::max(tmin, t1);
                tmax = std::min(tmax, t2);
                if (tmin > tmax) { hit = false; break; }
            }
        }

        if (!hit || tmin >= bestT)
        {
            continue;
        }

        float candidateT = tmin;
        RtInstanceDesc instance{};
        if (objects_[i]->GetRtInstance(instance) && instance.mesh &&
            instance.mesh->HasRaycastTriangles())
        {
            const Math::mat4 inverseWorld = Math::mat4::Inverse(instance.world);
            const Math::float3 localOrigin = inverseWorld.TransformPoint(origin);
            const Math::float3 localDirection = inverseWorld.TransformDirection(dir);
            const bool validLocalRay =
                std::isfinite(localOrigin.x) && std::isfinite(localOrigin.y) && std::isfinite(localOrigin.z) &&
                std::isfinite(localDirection.x) && std::isfinite(localDirection.y) && std::isfinite(localDirection.z) &&
                localDirection.Dot(localDirection) >= 1.0e-16f;
            if (validLocalRay)
            {
                if (!instance.mesh->RaycastLocal(localOrigin, localDirection, &candidateT))
                {
                    continue; // The ray crossed the AABB but missed the actual mesh surface.
                }
            }
        }

        if (candidateT < bestT)
        {
            bestT = candidateT;
            best = objectIds_[i];
        }
    }

    if (outDistance)
    {
        *outDistance = bestT;
    }
    return best;
}
#endif // WITH_EDITOR

void Scene::RebuildShadowCasters(Renderer& renderer)
{
    // Rebuild the caster data + consolidated mega VB/IB on a fresh GPU-idle upload batch (the meshes'
    // buffers have decayed to COMMON, so EnsureMegaBuffer's implicit-promotion copies are valid).
    // Mirrors FinalizeLevelLoad. The Rebuild pre-empts the per-frame UpdateForFrame rebuild (counts
    // already match), so there is no double work. Used by the editor caster-set refresh and the
    // shadow-LOD-bias change — both need the mega buffer (only ever built here) regenerated.
    renderer.WaitForPreviousFrame(); // no in-flight frame references the old mega buffers before we free them
    UploadBatch uploads;
    if (!uploads.Begin(&renderer)) { return; }
    shadowGpu_.Rebuild(&renderer, objects_);
    shadowGpu_.EnsureMegaBuffer(&renderer, uploads.CommandList());
    uploads.SubmitAndWait(&renderer);
    // Content (materials/geometry) may have changed without a transform change. Keep the next VSM
    // frame from reusing cached pages rendered with the previous descriptors / previous LOD.
    shadowGpu_.ForceContentRefreshNextFrame();
}

void Scene::ReconcileShadowLodCurve(Renderer* renderer)
{
    // The shadow LOD bias and tier stride pick the caster LOD rasterized into each shadow view.
    // The geometry lives in the consolidated mega buffer built at load, so a curve change needs a
    // GPU-idle rebuild. Cheap to poll; only rebuilds on an actual slider/sweep change.
    // (Chunked-terrain LOD needs NO rebuild anywhere: its per-chunk camera tiers travel through a
    // per-frame CB override — see ShadowGpuData::RefreshChunkGroupLods.)
    if (!renderer) { return; }
    if (shadowGpu_.BuiltShadowLod() == render::g_shadowLodBias &&
        shadowGpu_.BuiltShadowLodTierStride() == render::ShadowLodTierStride() &&
        shadowGpu_.BuiltShadowLodBiasNearTier() == render::g_shadowLodBiasNearTier) { return; }
    RebuildShadowCasters(*renderer);
}

OceanRenderable* Scene::FindOceanRenderable()
{
    // W1: the ocean's clock lives on the OceanRenderable, which enters objects_ through several
    // paths (level registry, editor spawn), and Clear() doesn't bump staticSetVersion_ — so a
    // cached pointer would be fragile. A once-per-frame scan is robust and negligible next to the
    // per-object Tick that just ran.
    for (std::unique_ptr<RenderableObjectBase>& obj : objects_)
    {
        if (obj)
        {
            if (OceanRenderable* ocean = obj->AsOceanRenderable())
            {
                return ocean;
            }
        }
    }
    return nullptr;
}

void Scene::Tick(float deltaTime) {
    CPU_SCOPE(ProfilerScopes::kSceneTick);

    {
        CPU_SCOPE(ProfilerScopes::kSceneTickPointLights);
        for (PointLight& light : lightManager_.PointLights())
        {
            light.Tick(deltaTime);
        }
    }

#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
    const size_t batchSize = 64;
    {
        CPU_SCOPE(ProfilerScopes::kSceneTickObjects);
        TaskSystem::ParallelFor(objects_.size(),
            [this, deltaTime](size_t index) {
                if (index >= objects_.size()) {
                    return;
                }
                objects_[index]->Tick(deltaTime);
            }, batchSize);
    }

    {
        CPU_SCOPE(ProfilerScopes::kSceneTickPostObjects);
        TaskSystem::ParallelFor(objects_.size(),
            [this, deltaTime](size_t index) {
                if (index >= objects_.size()) {
                    return;
                }
                objects_[index]->PostTick(deltaTime);
            }, batchSize);
    }
#else
    {
        CPU_SCOPE(ProfilerScopes::kSceneTickObjects);
        for (auto& obj : objects_)
        {
            obj->Tick(deltaTime);
        }
    }

    {
        CPU_SCOPE(ProfilerScopes::kSceneTickPostObjects);
        for (auto& obj : objects_)
        {
            obj->PostTick(deltaTime);
        }
    }
#endif

    // W1: advance the global wind from the SAME clock the ocean uses (its elapsedTime_), so waves
    // and foliage sway stay phase-coherent. No ocean -> standalone monotonic accumulator (coherence
    // is moot). Runs after the object-Tick barrier above, so the ocean's clock is current.
    {
        CPU_SCOPE(ProfilerScopes::kSceneTickWind);
        OceanRenderable* ocean = FindOceanRenderable();
        const float clock = ocean ? ocean->GetElapsedTime() : (windState_.time + deltaTime);
        windState_.Tick(clock);
        // W8: the fade origin is the CAMERA, shared by the gbuffer and every shadow view, so both
        // sides of vfx::WindDistanceFade agree and the shadow cannot detach from the tree.
        vfx::g_windFadeOriginWS = camera_.GetPosition();
        // W8: and the drift push for GPU particle emitters (they have no Scene pointer).
        vfx::g_windDriftXZ = Math::float2(
            windState_.windDirXZ.x * windState_.strength * windState_.gustMul,
            windState_.windDirXZ.y * windState_.strength * windState_.gustMul);

        // W1/W2 verify (self-limiting): the wind clock tracks the ocean, windDirXZ is unit, and (W2)
        // the ocean's wind dir/force reflect the authored wind entity when it is active.
        static int s_windLogFrames = 0;
        if (s_windLogFrames < 8)
        {
            ++s_windLogFrames;
            const float len = std::sqrt(windState_.windDirXZ.x * windState_.windDirXZ.x +
                                        windState_.windDirXZ.y * windState_.windDirXZ.y);
            float oceanDir = -1.0f, oceanForce = -1.0f;
            if (ocean && ocean->GetSimulation())
            {
                oceanDir = ocean->GetSimulation()->GetLocalWindDirectionDegrees();
                oceanForce = ocean->GetSimulation()->GetWindForce01();
            }
            LOG_DEBUG(logging::LogCategory::Vfx,
                "wind verify frame={} time={:.4f} active={} windDir={:.1f} strength={:.2f} swayAmp={:.3f} | ocean={} oceanDir={:.1f} oceanForce={:.2f} |dir|={:.4f}",
                s_windLogFrames, windState_.time, windState_.active ? 1 : 0,
                windState_.directionDeg, windState_.strength, windState_.swayAmplitude,
                ocean ? "yes" : "none", oceanDir, oceanForce, len);
        }
    }
}

void Scene::PrepareViewQueue(SceneView& view, uint32_t cameraLayerMask)
{
    CPU_SCOPE(ProfilerScopes::kPrepareQueue);
    if (view.type == SceneView::Type::Shadow && !view.frustum.IsValid())
    {
        view.queue.Clear();
        return;
    }

    // Shadow and camera views using the camera's layer mask reuse their already bucketized,
    // BatchKey-sorted sources. Cull preserves source order, so neither view needs to sort again.
    const bool usesSharedShadowSource =
        view.type == SceneView::Type::Shadow && view.renderLayerMask == cameraLayerMask;
    const bool usesSharedCameraSource =
        view.type == SceneView::Type::Camera && view.renderLayerMask == cameraLayerMask;
    if (usesSharedShadowSource)
    {
        view.queue.Cull(view.frustum, shadowCasterSource_);
    }
    else if (usesSharedCameraSource)
    {
        view.queue.Cull(view.frustum, cameraObjectSource_);
    }
    else if (g_useFusedBucketizeCull)
    {
        view.queue.BucketizeCull(objects_, view.renderLayerMask,
            view.type == SceneView::Type::Shadow, view.frustum);
    }
    else
    {
        view.queue.Bucketize(objects_, view.renderLayerMask, view.type == SceneView::Type::Shadow);
        view.queue.Cull(view.frustum);
    }
    if (view.type == SceneView::Type::Camera)
    {
        view.queue.SortTransparent(view.view);
        // Camera LOD must be selected before camera batches build their per-tier member lists.
        // S1: the view's own frustum rides along -- the chunk/instance masks use the planes the
        // object cull above used, so "object in, chunk out" is the same test at a finer grain.
        view.queue.SelectLods(camera_, view.frustum);
    }
    if (!usesSharedShadowSource && !usesSharedCameraSource)
    {
        view.queue.SortOpaque();
    }
    // Occlusion plan S0: visibility counters, taken BEFORE batching so every object counts once.
    // Only the camera and the four cascades have a slot; the fused path's queue holds survivors
    // only, so there `objectsIn` == `objectsFrustum` by construction.
    if (const int slot = VisibilitySlotFor(view); slot >= 0)
    {
        const size_t sourceCount = usesSharedShadowSource ? shadowCasterSource_.SourceObjectCount()
                                 : usesSharedCameraSource ? cameraObjectSource_.SourceObjectCount()
                                 : view.queue.SourceObjectCount();
        AccumulateVisibility(render::g_visibilityStats.current[static_cast<size_t>(slot)], view.queue, sourceCount,
                             view.frustum, view.type == SceneView::Type::Camera);
    }
    view.queue.BuildInstancedBatches(view.type == SceneView::Type::Camera);
}

int Scene::VisibilitySlotFor(const SceneView& view) const
{
    if (&view == &camera_.GetView()) { return static_cast<int>(render::kVisibilityViewCamera); }
    const SceneView* first = cascadeViews_.data();
    if (&view >= first && &view < first + cascadeViews_.size())
    {
        return static_cast<int>(render::kVisibilityViewCascade0 + (&view - first));
    }
    return -1;
}

AABB Scene::ComputeStaticBounds() const
{
    // Union of every renderable's world box except the ocean (its sheet spans the world). Used by
    // the replicate stress knob for its grid step; nothing hot calls this.
    bool any = false;
    float3 mn(0, 0, 0), mx(0, 0, 0);
    for (const auto& obj : objects_)
    {
        if (!obj || obj->AsOceanRenderable()) { continue; }
        const AABB& b = obj->GetWorldBounds();
        if (!b.IsValid()) { continue; }
        const float3 bmn = b.GetMin(), bmx = b.GetMax();
        if (!any) { mn = bmn; mx = bmx; any = true; continue; }
        mn = float3(std::min(mn.x, bmn.x), std::min(mn.y, bmn.y), std::min(mn.z, bmn.z));
        mx = float3(std::max(mx.x, bmx.x), std::max(mx.y, bmx.y), std::max(mx.z, bmx.z));
    }
    return any ? AABB(mn, mx) : AABB::Empty();
}

void Scene::PrepareViews(Renderer* renderer)
{
    CPU_SCOPE(ProfilerScopes::kPrepareViews);
    if (!renderer)
    {
        return;
    }

    OceanSimulation* oceanSimulation = Systems::GetOceanSimulation();
    SceneView& mainView = camera_.GetView();
    SceneView* shoreViewPtr = nullptr;
    const uint32_t camMask = camera_.GetRenderLayerMask();
    std::optional<Profiler::ScopedCpu> prepareViewsSetupScope(
        std::in_place, ProfilerScopes::kPrepareViewsSetup);
    camera_.CalcMatrices(renderer);
    renderer->UpdateDlssCameraData(camera_);

    // Publish this frame's pass inputs. SceneRenderer's pass bodies read frameData_,
    // not Scene members.
    frameData_.camera = &camera_;
    frameData_.mainView = &camera_.GetView();
    frameData_.cascadeViews = &cascadeViews_;
    frameData_.clipmapViews = &clipmapViews_;
    frameData_.clipmapSquares = &clipmapSquares_;
    frameData_.smrtFrameIndex = vsm::g_smrtTemporalDither
        ? static_cast<std::uint32_t>((renderer->GetTotalFrameNumber() & 63ull) + 1ull)
        : 0u;
    frameData_.spotShadowViews = &spotShadowViews_;
    frameData_.pointShadowViews = &pointShadowViews_;
    frameData_.lightManager = &lightManager_;
    frameData_.skybox = skyBox_.get();
    frameData_.objects = &objects_;
    frameData_.dirLight = &dirLight_;
    frameData_.cascadeConfig = &cascadeConfig_;
    frameData_.shadowGpu = &shadowGpu_;
    frameData_.vsm = &vsm_;
    frameData_.wind = &windState_; // W3: gbuffer per-view CB reads this
    frameData_.ocean = FindOceanRenderable(); // caustics source for the deferred lighting pass
    frameData_.settings = renderSettings_;
    frameData_.cameraExposure = cameraExposure_;
    frameData_.colorPipeline = colorPipeline_;
#if WITH_EDITOR
    frameData_.selectedEditorObjectIds = selectedEditorObjectIds_;
    frameData_.selectedEditorObjectCount = selectedEditorObjectCount_;
    frameData_.selectionOutlineRadius = std::clamp<std::uint32_t>(selectionOutlineRadius_, 1u, 8u);
#else
    frameData_.selectedEditorObjectIds.fill(0);
    frameData_.selectedEditorObjectCount = 0;
    frameData_.selectionOutlineRadius = 1;
#endif

    mainView.renderLayerMask = camera_.GetRenderLayerMask();
    mainView.frustum = Frustum::FromInvViewProj(mainView.invView, mainView.proj, camera_.GetZNear(), camera_.GetZFar());
    mainView.type = SceneView::Type::Camera;
    mainView.requiresDepthCheck = false;

    // The camera queue is independent of cascade/local-light view construction once its matrices
    // and shared source are ready. Publish it first so a worker can overlap its expensive
    // Cull/LOD/instancing chain with the remainder of PrepareViews setup on the main thread.
    if (!renderQueueSourcesValid_ || renderQueueSourceVersion_ != staticSetVersion_ ||
        renderQueueSourceMask_ != camMask)
    {
        shadowCasterSource_.Bucketize(objects_, camMask, /*filterShadowCaster=*/true);
        shadowCasterSource_.SortOpaqueSource();
        cameraObjectSource_.Bucketize(objects_, camMask, /*filterShadowCaster=*/false);
        cameraObjectSource_.SortOpaqueSource();
        renderQueueSourceVersion_ = staticSetVersion_;
        renderQueueSourceMask_ = camMask;
        renderQueueSourcesValid_ = true;
    }

    TaskSystem& tasks = TaskSystem::Get();
    TaskSystem::TaskHandle mainViewTask = nullptr;
    {
        CPU_SCOPE(ProfilerScopes::kPrepareViewsDispatch);
        mainViewTask = tasks.Submit([this, &mainView, camMask]()
        {
            CPU_SCOPE(ProfilerScopes::kPrepareMainView);
            PrepareViewQueue(mainView, camMask);
        });
    }

    if (oceanSimulation)
    {
        // The shore field is a static map of the level, so it has to be told where the level IS.
        // Centred on the terrain's footprint, computed once per load — walking 600 objects every
        // frame for a value that only changes when the level does would be silly.
        if (!shoreAreaValid_)
        {
            float minX = FLT_MAX, minZ = FLT_MAX, maxX = -FLT_MAX, maxZ = -FLT_MAX;
            for (const auto& obj : objects_)
            {
                if (!obj || !IsLayerEnabled(obj->GetRenderLayerMask(), RenderLayer::Terrain))
                {
                    continue;
                }
                const AABB& bounds = obj->GetWorldBounds();
                minX = std::min(minX, bounds.GetMin().x);
                minZ = std::min(minZ, bounds.GetMin().z);
                maxX = std::max(maxX, bounds.GetMax().x);
                maxZ = std::max(maxZ, bounds.GetMax().z);
            }
            if (minX <= maxX)
            {
                oceanSimulation->SetShoreArea(float2((minX + maxX) * 0.5f, (minZ + maxZ) * 0.5f));
                shoreAreaValid_ = true;
            }
        }
        oceanSimulation->UpdateShoreView(camera_);
        shoreViewPtr = &oceanSimulation->GetShoreDepthView();
    }
    UpdateCascades(camera_, renderer);
    UpdateClipmap(camera_); // Step 24d: directional clipmap views (consumed only in VSM mode)

    // Choose which lit spots cast shadows this frame (highest projected size among
    // those whose influence intersects the view frustum) and their atlas slots,
    // then build one shadow view per slot from its owning light. Must precede the
    // view build and the Pass_SpotLights buffer fill.
    //
    // Use a NON-JITTERED frustum: mainView.frustum is built from the DLSS-jittered
    // projection, whose sub-pixel offset changes every frame. Feeding that into a
    // discrete per-frame selection makes a spot sitting near the frustum edge flip
    // in/out of the shadowed set as the jitter oscillates the planes — its shadow
    // flickers (Release-only, since jitter is active there). Shadow-caster
    // selection must be temporally stable, so cull against the un-jittered frustum.
    const Frustum shadowSelectFrustum = Frustum::FromInvViewProj(
        mainView.invView, camera_.GetProjMatrixNoJitter(), camera_.GetZNear(), camera_.GetZFar());
    lightManager_.SelectShadowedSpots(camera_.GetPosition(), shadowSelectFrustum);
    // B2a: choose which point lights cast (cube) shadows this frame, same non-jittered
    // frustum. Drives Pass_PointLights' shadowParams.x; the cube views + render pass are
    // B2b, sampling is B3, so this is inert (no visual change) until those land.
    lightManager_.SelectShadowedPoints(camera_.GetPosition(), shadowSelectFrustum);
    const size_t shadowedSpotCount = lightManager_.GetShadowedSpotCount();
    const auto& spotLights = lightManager_.SpotLights();
    for (size_t s = 0; s < shadowedSpotCount; ++s)
    {
        const size_t lightIndex = lightManager_.GetShadowedSpotLightIndex(s);
        const auto& light = spotLights[lightIndex];
        SceneView& view = spotShadowViews_[s];
        view.type = SceneView::Type::Shadow;
        view.view = light.GetViewMatrix();
        view.proj = light.GetProjMatrix();
        view.invView = mat4::Inverse(view.view);
        view.invProj = mat4::Inverse(view.proj);
        const auto& desc = light.GetDesc();
        const float nearPlane = std::max(desc.nearPlane, 0.01f);
        const float farPlane = std::max(desc.range, nearPlane + 0.1f);
        view.frustum = Frustum::FromInvViewProj(view.invView, view.proj, nearPlane, farPlane);
        view.renderLayerMask = camera_.GetRenderLayerMask();
        view.position = desc.position;
        view.zNear = nearPlane;
        view.zFar = farPlane;
        view.hfov = 0.0f;
        view.requiresDepthCheck = false;
    }

    // B2b: point-light cube shadow views — 6 faces per shadowed point light. Face order
    // is the D3D cube-map convention (+X,-X,+Y,-Y,+Z,-Z) so a runtime TextureCubeArray
    // sample by direction (P - lightPos) selects the matching slice (rendered into
    // pointShadowViews_[slot*6 + face], matching BindPointShadowTarget's faceIndex). All
    // faces share one 90° FOV perspective; only orientation differs.
    static const Math::float3 kCubeDir[6] = {
        { 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f,  1.0f, 0.0f },
        { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f } };
    static const Math::float3 kCubeUp[6] = {
        { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, -1.0f },
        { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    constexpr float kHalfPi = 1.57079632679f; // 90deg FOV per cube face
    const size_t shadowedPointCount = lightManager_.GetShadowedPointCount();
    const auto& pointLights = lightManager_.PointLights();
    for (size_t s = 0; s < shadowedPointCount; ++s)
    {
        const size_t lightIndex = lightManager_.GetShadowedPointLightIndex(s);
        const auto& desc = pointLights[lightIndex].GetDesc();
        const float3 P = desc.position;
        // near = 2% of radius (min 0.2) so far/near stays ~20 (not ~200) for usable D16
        // precision. MUST equal Pass_PointLights' shadowParams.z — the sampler reconstructs
        // the compare depth with this exact near/far (see PointShadowFactor).
        const float nearPlane = std::max(0.2f, desc.radius * 0.02f);
        const float farPlane = std::max(desc.radius, nearPlane + 0.1f);
        const mat4 proj = mat4::PerspectiveFovLH(kHalfPi, 1.0f, nearPlane, farPlane);
        for (int face = 0; face < 6; ++face)
        {
            SceneView& view = pointShadowViews_[s * 6 + static_cast<size_t>(face)];
            view.type = SceneView::Type::Shadow;
            view.view = mat4::LookAtLH(P, P + kCubeDir[face], kCubeUp[face]);
            view.proj = proj;
            view.invView = mat4::Inverse(view.view);
            view.invProj = mat4::Inverse(view.proj);
            view.frustum = Frustum::FromInvViewProj(view.invView, view.proj, nearPlane, farPlane);
            view.renderLayerMask = camera_.GetRenderLayerMask();
            view.position = P;
            view.zNear = nearPlane;
            view.zFar = farPlane;
            view.hfov = 0.0f;
            view.requiresDepthCheck = false;
        }
    }

    prepareViewsSetupScope.reset();

    // main + shore + cascades + up to kMaxShadowedSpotLights spot views + up to
    // 6*kMaxShadowedPointLights point cube-face views.
    tc::inl_vector<SceneView*, 48> viewsToCull;
    auto enqueueView = [this, camMask, &viewsToCull](SceneView& view)
    {
        if (view.type == SceneView::Type::Shadow && !view.frustum.IsValid())
        {
            PrepareViewQueue(view, camMask);
            return;
        }

        if (viewsToCull.size() < viewsToCull.capacity())
        {
            viewsToCull.push_back(&view);
        }
        else
        {
            PrepareViewQueue(view, camMask);
        }
    };

    {
        CPU_SCOPE(ProfilerScopes::kPrepareViewsBuildList);
        if (shoreViewPtr)
        {
            enqueueView(*shoreViewPtr);
        }
        if (oceanSimulation && oceanSimulation->ShouldBuildShoreSdf())
        {
            // Only on the frame the SDF is actually rebuilt — the rest of the time this view has no
            // work and culling it would be pure overhead.
            enqueueView(oceanSimulation->GetShoreSdfView());
        }
        for (auto& cascadeView : cascadeViews_)
        {
            enqueueView(cascadeView);
        }
        for (size_t s = 0; s < lightManager_.GetShadowedSpotCount(); ++s)
        {
            enqueueView(spotShadowViews_[s]);
        }
        for (size_t v = 0; v < lightManager_.GetShadowedPointCount() * 6; ++v)
        {
            enqueueView(pointShadowViews_[v]);
        }
    }

    // Camera preparation was published before cascade/light-view setup. Publish the remaining
    // independent views now; while waiting for the camera task, main may help drain these tasks.
    tc::inl_vector<TaskSystem::TaskHandle, 48> viewTasks;
    {
        CPU_SCOPE(ProfilerScopes::kPrepareViewsDispatch);
        for (SceneView* view : viewsToCull)
        {
            if (!view) { continue; }
            if (TaskSystem::TaskHandle task = tasks.Submit([this, view, camMask]()
                {
                    PrepareViewQueue(*view, camMask);
                }))
            {
                viewTasks.push_back(task);
            }
        }
    }

    {
        CPU_SCOPE(ProfilerScopes::kPrepareViewsJoin);
        tasks.Wait(mainViewTask);
        tasks.Release(mainViewTask);
        for (TaskSystem::TaskHandle& task : viewTasks)
        {
            tasks.Wait(task);
            tasks.Release(task);
        }
    }
}

void Scene::ReconcileShadowMode(Renderer* renderer)
{
    // Step 24b: make the VSM resource state match the active shadow mode. The common path is a cheap
    // bool compare; only when they disagree (a Ctrl+V toggle since last frame) do we stall to GPU idle
    // and free/allocate — the same idle-then-realloc pattern the level-load path uses. So only ONE
    // mode's shadow resources are ever resident (memory-optimal). Legacy atlas freeing is Step 24c.
    if (!renderer) { return; }
    const bool wantVsm = render::VsmActive();
    const bool wantAtlasFull = !wantVsm; // legacy spot/point atlases full-res only in Legacy mode
    const bool vsmOk = (wantVsm == vsm_.IsAllocated());
    const bool atlasOk = (wantAtlasFull == renderer->IsLocalShadowFull());
    if (vsmOk && atlasOk) { return; }    // both in sync — the common per-frame path
    // Reconciled independently so a resize (which rebuilds the atlases full-res) also self-corrects.
    renderer->WaitForPreviousFrame(); // GPU idle before freeing/allocating shadow resources
    if (!vsmOk) { if (wantVsm) { vsm_.EnsureResources(renderer); } else { vsm_.ReleaseResources(); } }
    if (!atlasOk) { renderer->SetLocalShadowResidency(wantAtlasFull); }
}

void Scene::Render(Renderer* renderer) {
    // Occlusion plan S0: headless visibility readout, once, past the streaming window (frame 600,
    // same reason as csm_readout). Reads LAST frame's snapshot -- complete by construction.
    if (render::g_visDumpReadout && renderer && renderer->GetTotalFrameNumber() > 600)
    {
        render::g_visDumpReadout = false;
        diag::ArtifactFile f("visibility_readout.log", diag::ArtifactMode::AtomicReplace);
        if (f)
        {
            static const char* const kNames[render::kVisibilityViews] = { "camera", "c0", "c1", "c2", "c3" };
            f.Printf("view     objectsIn  frustum  occluded  chunksIn  chunksDrawn  instances   triangles    frame=%llu drawCalls=%u primitives=%llu\n",
                     static_cast<unsigned long long>(renderer->GetTotalFrameNumber()),
                     render::g_renderStats.lastDrawCalls,
                     static_cast<unsigned long long>(render::g_renderStats.lastPrimitives));
            for (unsigned v = 0; v < render::kVisibilityViews; ++v)
            {
                const render::VisibilityViewCounters& c = render::g_visibilityStats.last[v];
                f.Printf("%-8s %9u  %7u  %8u  %8u  %11u  %9u  %10llu\n", kNames[v], c.objectsIn, c.objectsFrustum,
                         c.objectsOccluded, c.chunksIn, c.chunksDrawn, c.instancesDrawn,
                         static_cast<unsigned long long>(c.trianglesSubmitted));
            }
        }
    }
    if (!renderer) {
        return;
    }
    CPU_SCOPE(ProfilerScopes::kSceneRender);

    ReconcileShadowMode(renderer); // Step 24b: apply a pending Legacy<->VSM switch (GPU-idle free/alloc)
    ReconcileShadowLodCurve(renderer); // apply pending shadow-LOD curve changes (GPU-idle caster rebuild)

    if (renderer->ConsumeMaterialHotReloadFlag())
    {
        sceneRenderer_.RefreshMaterialHandles(renderer, objects_, skyBox_.get());
    }

    lightManager_.UpdateSpotLightCache();

    // Rung 0 / Steps 1-2: refresh the persistent per-caster shadow buffers (instance + bounds),
    // re-uploading only the movers' entries into this frame's ring region. Pure CPU write into
    // mapped upload memory; no consumer yet.
    shadowGpu_.UpdateForFrame(renderer, objects_);
    shadowGpu_.PollValidation(renderer); // Step 4: one-shot GPU-vs-CPU cull-count check when ready
    vsm_.PollPageRequestDebug(renderer);  // Step 19: one-shot page-request count log when ready

    PrepareViews(renderer);

    // Per-caster shadow LOD: publish THIS frame's receiver LODs (per chunk / per instance, inside
    // PrepareViews above) as the shadow caster overrides — after PrepareViews on purpose, so the
    // caster can never lag the receiver by a frame at a LOD transition.
    shadowGpu_.RefreshCasterLods(renderer, objects_, camera_.GetPosition());

    // LOD selection debug view (dev window "LOD" tab, or --set=lod.debug). Emitted HERE because
    // this is the one point where both halves of what it shows are valid: this frame's tiers are
    // final (SelectLods ran inside PrepareViews just above) and the HUD text buffer is still open
    // for writing (AppController::WaitForHudBuild returned before Scene::Render was called, and
    // TextManager::Build runs later, inside SceneRenderer::Render). Off by default; the body
    // early-outs on the mode before touching anything.
#if WITH_EDITOR
    // The editor selection, resolved from ids to pointers here because objectIds_ is what holds the
    // lockstep mapping. Kept out of SceneFrameData: this is a same-frame main-thread read, and the
    // frame data is for what the render threads consume.
    {
        tc::inl_vector<const RenderableObjectBase*, SceneFrameData::kMaxEditorSelection> sel;
        for (std::uint32_t i = 0; i < selectedEditorObjectCount_; ++i)
        {
            const SceneObjectId id = selectedEditorObjectIds_[i];
            for (size_t o = 0; o < objectIds_.size() && o < objects_.size(); ++o)
            {
                if (objectIds_[o] == id) { sel.push_back(objects_[o].get()); break; }
            }
        }
        render::DrawLodDebug(renderer, camera_, objects_, sel.data(), sel.size());
    }
#else
    render::DrawLodDebug(renderer, camera_, objects_);
#endif

    // Rung 0 / Step 2: upload the active shadow views' frustum planes (the per-view cull input)
    // into this frame's ring region. Fixed slot layout [cascades | spots | point-faces] so a
    // view's slot index is stable for the future cull; inactive slots pass null → zeroed.
    {
        constexpr size_t kCascadeSlots = static_cast<size_t>(kCascades);
        constexpr size_t kSpotSlots = LightManager::kMaxShadowedSpotLights;
        constexpr size_t kPointFaceSlots = LightManager::kMaxShadowedPointLights * 6;
        constexpr size_t kClipmapSlots = vsm::kNumClipmapLevels; // Step 24e: directional clipmap cull views
        // Single source of truth: the indirect buffers (ShadowGpuData) size per view against
        // render::kMaxShadowViews; keep it equal to the real cap sum so they can't drift.
        static_assert(kCascadeSlots + kSpotSlots + kPointFaceSlots + kClipmapSlots == render::kMaxShadowViews,
                      "render::kMaxShadowViews must equal the shadow-view slot layout");
        // Direct constant base offsets (not a running index) so the array writes are provably
        // in-bounds — [cascades | spots | point faces | clipmap]. The VSM setup's rung0View =
        // view + kNumCascades relies on this exact ordering.
        std::array<const Frustum*, kCascadeSlots + kSpotSlots + kPointFaceSlots + kClipmapSlots> frustums{};
        for (size_t i = 0; i < kCascadeSlots; ++i)
        {
            frustums[i] = &cascadeViews_[i].frustum;
        }
        const size_t spotCount = lightManager_.GetShadowedSpotCount();
        for (size_t i = 0; i < kSpotSlots; ++i)
        {
            frustums[kCascadeSlots + i] = (i < spotCount) ? &spotShadowViews_[i].frustum : nullptr;
        }
        const size_t pointFaceCount = lightManager_.GetShadowedPointCount() * 6;
        for (size_t i = 0; i < kPointFaceSlots; ++i)
        {
            frustums[kCascadeSlots + kSpotSlots + i] = (i < pointFaceCount) ? &pointShadowViews_[i].frustum : nullptr;
        }
        // Step 24e: directional clipmap levels — culled ONLY in VSM mode (null = reject-all in Legacy,
        // so the cull emits zero for them and the Legacy atlas path is untouched).
        for (size_t i = 0; i < kClipmapSlots; ++i)
        {
            frustums[kCascadeSlots + kSpotSlots + kPointFaceSlots + i] =
                render::VsmActive() ? &clipmapViews_[i].frustum : nullptr;
        }
        shadowGpu_.UpdateViewFrustums(renderer, frustums.data(), frustums.size());
    }

    sceneRenderer_.Render(renderer, frameData_);
}

void Scene::Clear()
{
    sceneRenderer_.Reset();
    frameData_ = SceneFrameData{}; // drop pointers into objects we are about to destroy
    lightManager_.Reset();
    shadowGpu_.Reset(); // drop CPU caster state; GPU buffers retained
    shadowCasterSource_.Clear();
    cameraObjectSource_.Clear();
    renderQueueSourcesValid_ = false;
    objects_.clear();
    shoreAreaValid_ = false; // the next level's terrain sits somewhere else
#if WITH_EDITOR
    objectIds_.clear();
    selectedEditorObjectIds_.fill(0);
    selectedEditorObjectCount_ = 0;
    selectionOutlineRadius_ = 1;
#endif
    camera_.GetView().queue.Clear();
    for (auto& view : cascadeViews_)
    {
        view.queue.Clear();
    }
    skyBox_.reset();
}
