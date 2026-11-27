#include "rendering/shadows/ShadowViewPlanner.h"

// Architecture plan R6.1: the bodies below were Scene's (app/scene/Scene.cpp), moved verbatim; the Scene members they
// read are the inputs' fields now (ShadowPlanInputs, built by Scene::ShadowInputs).
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/diagnostics/ArtifactWriter.h"
#include "core/logging/Log.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/lighting/DirectionalLight.h"
#include "rendering/renderables/InstancedDrawBatch.h"
#include "rendering/renderables/RenderableObjectBase.h"
#include "rendering/shadows/SdsmShadows.h"
#include "rendering/view/Camera.h"
#include "rendering/wind/WindState.h"
#include "rendering/renderables/RenderableObject.h"

// R6.1: the cascade fitting's helpers, moved from Scene.cpp with UpdateCascades (their only reader).
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
            // corner rays from the apex. The anchor is the edge's own first corner -- EXCEPT for
            // the side edges, which are anchored at the APEX (the origin here, exactly on the ray by
            // definition). Anchoring them at the near corner put the far corner's distance to its
            // own edge plane at the mercy of the near corner's float error TIMES the lever arm to
            // the far corner: measured 2026-09-12 on cascade 3 (corner at |p| ~ 530 m, ulp 6e-5),
            // the far top-left corner sat 1.2 mm outside its side-edge plane and the self-check's
            // "closed toward the sun" probe fired. Through the apex, d(far corner) = n . c[far] is
            // the cross product's own rounding (~1e-4 m) and the near corner's error is not amplified.
            const float3 a = (e >= 8) ? float3(0.0f, 0.0f, 0.0f) : c[kEdges[e][2]];
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
} // namespace

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

void ShadowViewPlanner::UpdateCascades(const ShadowPlanInputs& in, const Camera& camera, Renderer* renderer)
{
    CPU_SCOPE(ProfilerScopes::kUpdateCascades);
    if (!renderer)
    {
        return;
    }

    const float zNear = camera.GetZNear();
    const float zFar = camera.GetZFar();
    SceneFrameData::CascadeData& cascades = in.frame.cascades;
    const auto splits = in.cascadeConfig.BuildSplitScheme(zNear, zFar);
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
    const float3 sunDirWS = in.sun.GetDirection();

    std::string c0Survivors; // S14 readout diagnostic, filled only on the dump frame
    // UpdateCascades runs before this frame's queues are rebuilt. Its diagnostic reads below are
    // therefore allowed to borrow LAST frame's raw object pointers only while the object set and
    // layer mask still match that queue. Editor placement removes the preview and bumps the set
    // version before this function; without this gate the previous queue retains a dangling
    // pointer until PrepareViewQueue clears it later in this same frame.
    const bool previousCascadeQueuesCurrent =
        in.previousCascadeQueuesCurrent; // R6.1: the scene's queue sources, Scene::ShadowInputs
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
        const float radius = sphereRadius + in.cascadeConfig.overlapInTexels * texelEstimate;
        const float unitsPerTexel = (2.0f * radius) / static_cast<float>(contentRes);

        float3 up(0, 1, 0);
        // S7: the light "eye" sits behind the cascade sphere by casterReachWS, not at a fixed
        // maxDistance for every cascade. It does NOT set the depth range (near/far below do) -- it
        // only guarantees that any caster within casterReachWS of the light still has z_ls > 0, i.e.
        // lies inside the culling ortho box. The old max(1, maxDistance) put the eye INSIDE the
        // volume for the far cascade (lightDistance < radius), where near hit the 0.001 clamp.
        const float lightDistance = radius + in.cascadeConfig.casterReachWS + 1.0f;

        // Texel snap — the actual stabilization. Snap the cascade center along the FIXED
        // light right/up axes to whole-texel steps in WORLD space, BEFORE building the
        // view. (The previous `lightView * center` snap was a no-op: center is the LookAt
        // target, so its light-space XY is always (0,0).) Snapping the center in a fixed
        // light frame makes the covered world region shift in whole-texel increments as
        // the camera moves, pinning shadow texels to fixed world cells -> no edge crawl.
        const float3 fwd = sunDirWS.Normalized();
        float3 right = up.Cross(fwd);
        if (right.Dot(right) < 1e-12f)
        {
            up = float3(0, 0, 1); // a sun straight overhead: LookAtLH below takes it too, or its view has no x axis (a level's default sun, and the quaternion a frustum makes of it)
            right = up.Cross(fwd);
        }
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
        const float zPad = in.cascadeConfig.zPadding;
        const float nearProjLS = std::max(0.001f, minZ - in.cascadeConfig.pancakeSlackWS);
        const float nearCullLS = std::max(0.001f, minZ - in.cascadeConfig.casterReachWS);
        const float farLS = maxZ + zPad;

        const mat4 lightProj = mat4::OrthoOffCenterLH(minX, maxX, minY, maxY, nearProjLS, farLS);

        const float depthBiasInTexels = in.cascadeConfig.depthBiasInTexels;
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
                in.cascadeConfig.scissorPadTexels);
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
                ? (cascades.splitsVS[idx] - cascades.splitsVS[idx - 1]) * in.cascadeConfig.blendFraction
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
            const Frustum& otherPrev = in.cascadeConfig.accurateCasterCull ? cascadeBoxPrev_[idx] : cascadeAccPrev_[idx];
            const auto settledCaster = [](const RenderableObjectBase* o)
            {
                const RenderableObject* ro = o->As<RenderableObject>();
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
                            const InstancedDrawBatch* batch = obj->As<InstancedDrawBatch>();
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
                const Frustum& otherNow = in.cascadeConfig.accurateCasterCull ? boxFrustum : accFrustum;
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
                        const InstancedDrawBatch* batch = obj->As<InstancedDrawBatch>();
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
                                      in.cascadeConfig.accurateCasterCull ? 1 : 0, vol.count);
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
            if (in.cascadeConfig.accurateCasterCull && vol.count > 0)
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
                float worstTol = 0.0f;
                // THE TOLERANCE SCALES WITH THE COORDINATES. A plane test is three products of
                // unit-normal components with world coordinates plus an offset of the same size,
                // so its float32 rounding is a few ulps OF |p| (6e-5 m per ulp at 512 m), and a
                // probe stepped `lever` metres toward the sun adds the normal's residual n.sun
                // (~1e-7) times that lever. 2e-6 per metre is ~30 ulps: 2.3 mm on cascade 3's
                // 600-metre corners, where a fixed 1 mm was exceeded by float alone (measured
                // -1.19 mm, 2026-09-12) while a real defect of the volume is decimetres.
                const auto tolerance = [](const float3& p, float lever)
                {
                    return 1e-3f + 2e-6f * (p.Length() + lever);
                };
                const auto inside = [&](const float3& p, float lever = 0.0f)
                {
                    worstPlane = -1; worstDist = 1e30f;
                    for (int i = 0; i < vol.count; ++i)
                    {
                        const float4& pl = vol.planes[i];
                        const float d = pl.x * p.x + pl.y * p.y + pl.z * p.z + pl.w;
                        if (d < worstDist) { worstDist = d; worstPlane = i; }
                    }
                    worstTol = tolerance(p, lever);
                    return worstDist >= -worstTol;
                };
                const auto report = [&](const char* what, int corner, const float3& p)
                {
                    // Fatal: flushed synchronously, so the record is on disk whatever the dialog's
                    // button does to the process.
                    LOG_FATAL(logging::LogCategory::RenderShadow,
                              "S14 cascade {}: {}  corner {}  p=({:.3f},{:.3f},{:.3f})  worst plane {}/{} dist {:.5f} (tol {:.5f})  "
                              "sun=({:.4f},{:.4f},{:.4f}) reach={:.1f} nearCull={:.3f} minZ~{:.3f} radius={:.3f}",
                              idx, what, corner, p.x, p.y, p.z, worstPlane, vol.count, worstDist, worstTol,
                              sunDirWS.x, sunDirWS.y, sunDirWS.z, in.cascadeConfig.casterReachWS,
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
                    if (!inside(q, step)) { report("closed toward the sun", ci, q); }
                    assert(inside(q, step) && "S14 cull volume closed toward the sun");
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
                        const InstancedDrawBatch* batch = obj->As<InstancedDrawBatch>();
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
            // S5b: `hzbDef` = casters the GPU cull deferred for that cascade (hidden from the sun
            // in last frame's tile), `hzbB` = how many of them pass B drew after the retest; the
            // cut is the difference. GPU counters of frame N - 3, 0 with the knob off.
            f.Printf("cascade  slice(m)         tile  texel(mm)  radius(m)  nearLS   farLS   zRange(m)  D16step(mm)  scissor%%  rect(atlas)  cullPl  casters  leak  hzbDef  hzbB  applied=%d accurateCull=%d hzbCull=%d\n",
                     in.cascadeConfig.scissorOptim ? 1 : 0, in.cascadeConfig.accurateCasterCull ? 1 : 0,
                     in.cascadeConfig.hzbCull ? 1 : 0);
            for (int i = 0; i < kCascades; ++i)
            {
                const float zRange = cascades.farLsDbg[i] - cascades.nearLsDbg[i];
                const auto& sr = cascades.scissor[i];
                f.Printf("%d  %8.2f..%-8.2f %5u  %8.3f  %9.2f  %7.2f %7.2f  %9.2f  %11.4f  %7.1f  %d,%d-%d,%d  %6u  %7u  %4u  %6u  %4u\n",
                         i, cascades.splitsVS[i], cascades.splitsVS[i + 1],
                         cascades.tileSizeDbg[i], cascades.unitsPerTexelDbg[i] * 1000.0f,
                         cascades.radiusDbg[i], cascades.nearLsDbg[i], cascades.farLsDbg[i],
                         zRange, (zRange / 65535.0f) * 1000.0f,
                         cascades.scissorAreaDbg[i] * 100.0f, sr.x0, sr.y0, sr.x1, sr.y1,
                         cascades.cullPlanesDbg[i], cascades.cullCastersDbg[i], cascades.cullLeakDbg[i],
                         in.shadowGpu.HzbDeferred(static_cast<unsigned>(i)), in.shadowGpu.HzbDrawnB(static_cast<unsigned>(i)));
            }
            if (!c0Survivors.empty()) { f.Printf("cascade 0 survivors (last frame), box/acc verdicts:\n%s", c0Survivors.c_str()); }
        }
    }
}

void ShadowViewPlanner::UpdateClipmap(const ShadowPlanInputs& in, const Camera& camera)
{
    // UE's FVirtualShadowMapClipmap: N camera-centred nested ortho views along the sun. Level L must cover a radius of 2^(L+1) cm
    // and is twice that either way, so whatever the snap leaves, the camera's own circle stays in it. Its centre snaps to whole radii
    // in the fixed light frame (a radius is 32 pages: the page grid keeps its world place and the pages move with it, PlanClipmapPan),
    // along the sun it stays put while the camera is inside the depth range. The matrix is written out instead of LookAtLH: a
    // dot with the centre would round differently at every snap and read as a change in depth.
    const float3 sunDirWS = in.sun.GetDirection();
    const float3 fwd = sunDirWS.Normalized();
    if (fwd.Dot(fwd) < 1e-8f) // no valid sun direction -> reject-all views
    {
        clipmapSquares_ = {}; // no sun -> no squares; the fallback chain skips every level
        clipmapZValid_ = false;
        for (auto& v : clipmapViews_)
        {
            v.frustum = Frustum{};
            v.type = SceneView::Type::Shadow;
            v.renderLayerMask = camera.GetRenderLayerMask();
            v.requiresDepthCheck = false;
        }
        return;
    }
    float3 up(0, 1, 0);
    float3 right = up.Cross(fwd);
    if (right.Dot(right) < 1e-12f)
    {
        up = float3(0, 0, 1); // a sun straight overhead: the levels' LookAtLH takes it too
        right = up.Cross(fwd);
    }
    right = right.Normalized();
    const float3 trueUp = fwd.Cross(right);

    const float3 camPos = camera.GetPosition();
    const float camX = camPos.Dot(right);
    const float camY = camPos.Dot(trueUp);
    const float camZ = camPos.Dot(fwd);
    const bool sunMoved = !clipmapZValid_ || std::memcmp(&clipmapZSun_, &fwd, sizeof(fwd)) != 0;
    clipmapZSun_ = fwd;
    clipmapZValid_ = true;

    for (size_t i = 0; i < clipmapViews_.size(); ++i)
    {
        const float radius = std::ldexp(1.0f, vsm::g_clipmapFirstLevel + static_cast<int>(i) + 1) * 0.01f; // 2^(L+1) cm
        const float halfSide = 2.0f * radius;
        const float zRadius = radius * vsm::g_clipmapZRangeScale;
        const float snapX = std::round(camX / radius);
        const float snapY = std::round(camY / radius);
        float& centreZ = clipmapZ_[i];
        if (sunMoved || std::abs(camZ - centreZ) + radius > 0.9f * zRadius) { centreZ = camZ; }
        const float centreX = snapX * radius;
        const float centreY = snapY * radius;
        const float3 center = right * centreX + trueUp * centreY + fwd * centreZ;

        mat4 lightView = mat4::Identity();
        lightView.m._11 = right.x;  lightView.m._12 = trueUp.x;  lightView.m._13 = fwd.x;
        lightView.m._21 = right.y;  lightView.m._22 = trueUp.y;  lightView.m._23 = fwd.y;
        lightView.m._31 = right.z;  lightView.m._32 = trueUp.z;  lightView.m._33 = fwd.z;
        lightView.m._41 = -centreX; lightView.m._42 = -centreY;  lightView.m._43 = -centreZ;
        const float minX = -halfSide, maxX = halfSide;
        const float minY = -halfSide, maxY = halfSide;
        const float nearLS = -zRadius;
        const float farLS = zRadius;

        const mat4 lightProj = mat4::OrthoOffCenterLH(minX, maxX, minY, maxY, nearLS, farLS);
        // For the LOD fallback chain: this level's square in the FIXED light frame.
        clipmapSquares_.level[i].centreX = centreX;
        clipmapSquares_.level[i].centreY = centreY;
        clipmapSquares_.level[i].extent = 2.0f * halfSide;
        clipmapSquares_.snapX[i] = static_cast<std::int32_t>(snapX);
        clipmapSquares_.snapY[i] = static_cast<std::int32_t>(snapY);
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

// S15 -- the per-frame inputs of the SDSM analysis, and the gate that decides whether this frame
// has an SDSM path at all. Published into in.frame so every pass builder asks ONE question
// ("is frame_->sdsm non-null?") instead of re-deriving the mode, the residency and the PSO state.
void ShadowViewPlanner::UpdateSdsmFrameParams(const ShadowPlanInputs& in, Renderer* renderer)
{
    in.frame.sdsm = nullptr;
    if (!renderer || !render::SdsmActive() || !in.sdsm.Ready()) { return; }

    const auto& D = renderer->GetDeferredForFrame();
    const UINT tileRes = D.shadowRes > 0 ? D.shadowRes / 2u : 0u;
    if (tileRes == 0u) { return; }
    const UINT borderRes = (tileRes > 2u * render::kCascadeAtlasBorder) ? render::kCascadeAtlasBorder : 0u;
    const UINT contentRes = tileRes - 2u * borderRes;

    render::sdsm::AnalyzeConstants cb{};
    // NON-JITTERED, exactly as S1's cascade fit is: the DLSS sub-pixel offset has no business
    // moving a shadow partition, and the reduction would carry it straight into the box.
    cb.invProj = mat4::Inverse(in.camera.GetProjMatrixNoJitter()).m;
    cb.invView = in.camera.GetInvViewMatrix().m;

    // The light frame: orthonormal, anchored at the camera so light-space metres stay small (a
    // world-anchored frame puts a kilometre of magnitude into every reduced coordinate, and the
    // box we are measuring is metres wide). Built by hand rather than through LookAtLH so the
    // sun-parallel-to-up degeneracy has an explicit fallback instead of a NaN matrix.
    //
    // The texel snap is NOT here (S21): the box is a GPU result, so Finalize snaps it -- the size
    // is held by hysteresis first, which is what gives the snap a fixed grid to snap to, and the
    // grid is anchored to the world through this matrix's translation row.
    {
        const float3 fwd = in.sun.GetDirection().Normalized();
        const float3 up(0.0f, 1.0f, 0.0f);
        float3 right = up.Cross(fwd);
        if (right.Dot(right) < 1e-12f) { right = float3(0, 0, 1).Cross(fwd); }
        right = right.Normalized();
        const float3 trueUp = fwd.Cross(right);
        const float3 eye = in.camera.GetPosition();
        mat4 lv{};
        lv.m._11 = right.x;  lv.m._12 = trueUp.x;  lv.m._13 = fwd.x;  lv.m._14 = 0.0f;
        lv.m._21 = right.y;  lv.m._22 = trueUp.y;  lv.m._23 = fwd.y;  lv.m._24 = 0.0f;
        lv.m._31 = right.z;  lv.m._32 = trueUp.z;  lv.m._33 = fwd.z;  lv.m._34 = 0.0f;
        lv.m._41 = -eye.Dot(right);
        lv.m._42 = -eye.Dot(trueUp);
        lv.m._43 = -eye.Dot(fwd);
        lv.m._44 = 1.0f;
        cb.lightView = lv.m;
    }

    // tan(halfFov) from the projection, the same read S1's sphere does.
    const mat4& proj = in.camera.GetProjMatrixNoJitter();
    const float tanHalfX = 1.0f / std::max(1e-6f, proj.m._11);
    const float tanHalfY = 1.0f / std::max(1e-6f, proj.m._22);
    const float3 camPos = in.camera.GetPosition();
    const float3 camDir = in.camera.GetDirection();
    cb.camPosWS = DirectX::XMFLOAT4(camPos.x, camPos.y, camPos.z, tanHalfX);
    cb.camDirWS = DirectX::XMFLOAT4(camDir.x, camDir.y, camDir.z, tanHalfY);

    cb.nearZ = in.camera.GetZNear();
    // The reduction's accept window, NOT the camera's far plane: a partition set that has to reach
    // 10 km would put the whole island in one logarithmic step. `maxDistance` is the same number
    // the Legacy splits end at, so the two modes shade the same range and the A/B is honest.
    cb.farZ = std::max(cb.nearZ + 1.0f, in.cascadeConfig.maxDistance);
    cb.partitions = std::min<std::uint32_t>(render::sdsm::g_partitions, render::sdsm::kMaxPartitions);
    cb.depthWidth = renderer->GetRenderWidth();
    cb.depthHeight = renderer->GetRenderHeight();
    cb.reduceTileDim = render::sdsm::kReduceTileDim;
    cb.borderTexels = render::sdsm::g_borderTexels;
    cb.dilation = render::sdsm::g_dilation;
    cb.minScaleOverSphere = render::sdsm::g_minScaleOverSphere;
    cb.zMargin = render::sdsm::g_zMargin;
    // The ONE place the human unit becomes the shader's: half-life in frames -> per-frame
    // retention. Below a frame there is nothing to average, so that end is simply off.
    {
        const float halfLife = std::clamp(render::sdsm::g_stabilityFrames, 0.0f, 240.0f);
        cb.stability = (halfLife < 1.0f) ? 0.0f
                                         : std::pow(0.5f, 1.0f / halfLife);
    }
    // S21: stable partitions (see Finalize).
    cb.texelSnap = render::sdsm::g_texelSnap ? 1u : 0u;
    cb.sizeHysteresis = std::clamp(render::sdsm::g_sizeHysteresis, 0.0f, 0.5f);
    cb.pancakeSlack = in.cascadeConfig.pancakeSlackWS;
    cb.casterReach = in.cascadeConfig.casterReachWS;
    cb.depthBiasTexels = in.cascadeConfig.depthBiasInTexels;
    cb.slopeScale = in.cascadeConfig.slopeScale;
    cb.maxSlope = in.cascadeConfig.maxSlope;
    cb.clampNear = in.cascadeConfig.pancakeCasters ? 1.0f : 0.0f;
    cb.atlasRes = static_cast<float>(D.shadowRes);
    cb.contentRes = static_cast<float>(contentRes);
    cb.tileRes = tileRes;
    cb.borderRes = borderRes;
    // Clamped to what the SDSM frustum buffer holds (MaxShadowViews() + 1, the camera slot
    // included). ShadowGpuData owns this count; a slot added there must not run off this buffer.
    cb.viewFrustumCount = std::min<std::uint32_t>(in.shadowGpu.ViewFrustumCount(), in.sdsm.FrustumViewCapacity());
    // S15.4: the accurate cull volume, on the SAME knob Legacy uses -- it is the same construction
    // (UE's ComputeShadowCullingVolume), just evaluated in the analyze instead of on the CPU,
    // because the partition intervals it needs are a GPU result.
    cb.blendFraction = in.cascadeConfig.blendFraction;
    cb.accurateCull = in.cascadeConfig.accurateCasterCull ? 1u : 0u;

    // S20 -- the partition cache: which partitions RENDER this frame (render::sdsm::g_cacheFrames).
    // Partition 0 always; partition p >= 1 every min(N, 2^p) frames, staggered (phases 0 / 1 / 3)
    // so a frame re-renders at most one of them besides 0, and out of turn as soon as the camera
    // has turned or moved half as far as the partition's widened box covers. EVERYTHING renders
    // whenever a kept tile could be wrong: the sun moved, the camera cut, the atlas was re-created
    // or resized, the partition count or the caster set changed, or the analyze did not run last
    // frame (the mode was off). The analyze does the GPU half -- see Finalize.
    {
        const std::uint32_t cacheFrames = std::clamp<std::uint32_t>(render::sdsm::g_cacheFrames, 1u, 16u);
        const float dilation = std::clamp(render::sdsm::g_cacheDilation, 0.0f, 0.5f);
        const std::uint64_t frame = renderer->GetTotalFrameNumber();
        const std::uint32_t sunVersion = in.sun.GetTransformVersion();
        const std::uint64_t camRevision = in.camera.GetHistoryRevision();
        const void* atlas = D.shadow;
        // A frame whose light does not reach the ground (a closed deck, a body under the horizon)
        // renders no partition at all (the shadow passes are off, FrameDecisions::sunShadowsOff) --
        // the "analyze did not run" case: the frame after it keeps nothing, however still the camera
        // stayed while the tiles went unrendered.
        const bool sunOff = !in.sun.ReachesGround();
        const bool keep = !sunOff && cacheFrames > 1u && sdsmCacheFrame_ + 1u == frame &&
            sunVersion == sdsmCacheSunVersion_ && camRevision == sdsmCacheCamRevision_ &&
            D.shadowRes == sdsmCacheAtlasRes_ && atlas == sdsmCacheAtlas_ &&
            cb.partitions == sdsmCachePartitions_ && in.staticSetVersion == sdsmCacheStaticSet_;
        sdsmCacheFrame_ = sunOff ? 0u : frame;
        sdsmCacheSunVersion_ = sunVersion;
        sdsmCacheCamRevision_ = camRevision;
        sdsmCacheAtlasRes_ = D.shadowRes;
        sdsmCacheAtlas_ = atlas;
        sdsmCachePartitions_ = cb.partitions;
        sdsmCacheStaticSet_ = in.staticSetVersion;

        std::uint32_t mask = ~0u;
        if (keep)
        {
            mask = 1u;
            // The widened box reaches `dilation` of its own extent past the slice it was fitted to;
            // at the far end of the partition that is about 2 * dilation * z * tan(halfFovX) per
            // side. The guard fires at HALF of it, before a receiver can walk off.
            const float turnCos = std::cos(0.5f * std::atan(2.0f * dilation * tanHalfX));
            for (std::uint32_t p = 1u; p < cb.partitions; ++p)
            {
                const SdsmCacheSlot& slot = sdsmCache_[p];
                const std::uint32_t period = std::min<std::uint32_t>(cacheFrames, 1u << p);
                const std::uint32_t phase = ((p == 1u) ? 0u : (p == 2u ? 1u : 3u)) % period;
                bool render = !slot.valid || (frame % period) == phase;
                render = render || camDir.Dot(slot.camDir) < turnCos;
                // How far the partition reaches, from the readout (a few frames old, which is
                // plenty for a threshold); without one, the shadow distance -- the safe side.
                const float reach = in.sdsm.ReadoutValid() ? std::max(in.sdsm.Readout()[p].intervalEnd, 1.0f) : cb.farZ;
                const float moveLimit = dilation * reach * tanHalfX;
                const float3 moved = camPos - slot.camPos;
                render = render || moved.Dot(moved) > moveLimit * moveLimit;
                if (render) { mask |= 1u << p; }
            }
        }
        for (std::uint32_t p = 0u; p < render::sdsm::kMaxPartitions; ++p)
        {
            if ((mask >> p) & 1u) { sdsmCache_[p] = SdsmCacheSlot{ true, camPos, camDir }; }
        }
        cb.updateMask = mask;
        cb.cacheDilation = (cacheFrames > 1u) ? dilation : 0.0f;
    }

    // S16: the moments atlas, allocated only while EVSM is on -- four times the bytes per texel
    // of the depth it is derived from. The EXPONENTS are zero when it is off, and that single
    // zero is what makes csm_sample take the PCF arm: one switch, no second opinion.
    // RESIDENCY IS NOT DECIDED HERE. `ReconcileShadowMode` (which runs earlier in the same frame,
    // with the GPU idle) owns allocating and freeing the moments atlas; this only asks whether the
    // thing exists and matches THIS frame's atlas. The size compare is the guard for the one frame
    // where the deferred atlas edge and the reconciled one can disagree: EVSM simply sits out that
    // frame on the PCF arm instead of the moments pass writing a texture of the wrong size.
    const bool evsmReady = in.sdsm.MomentsReady() && in.sdsm.MomentsRes() == D.shadowRes;
    cb.evsmOn = evsmReady ? 1u : 0u;
    cb.evsmPos = evsmReady ? std::min(render::sdsm::g_evsmPos, render::sdsm::kEvsmMaxExponent) : 0.0f;
    cb.evsmNeg = evsmReady ? std::min(render::sdsm::g_evsmNeg, render::sdsm::kEvsmMaxExponent) : 0.0f;

    // The gbuffer's wind, so the analyze can write a COMPLETE PerView block per partition. Read
    // from the SAME WindState the gbuffer CB is built from -- a shadow swaying to different
    // numbers than its own tree detaches from it (W5).
    cb.windTime = in.wind.time;
    cb.windPrevTime = in.wind.prevTime;
    cb.windDirX = in.wind.windDirXZ.x;
    cb.windDirZ = in.wind.windDirXZ.y;
    cb.windSwayAmp = in.wind.swayAmplitude;
    cb.windSwayFreq = in.wind.swayFrequency;
    cb.windGustMul = in.wind.gustMul;
    cb.windPrevGustMul = in.wind.prevGustMul;

    // Occlusion S5b for SDSM: the same pyramids and the same knob as Legacy. The MATRICES are the
    // analyze's own (this frame's partitions and last frame's copy); these four rows are the part
    // only the CPU knows, and they travel in the analyze CB so the block it writes is complete.
    {
        const bool wantHzb = in.cascadeConfig.hzbCull && render::g_indirectShadowsEnabled;
        const ShadowGpuData::SdsmHzbState hzb =
            in.shadowGpu.SetSdsmHzbViews(renderer, renderer->GetTotalFrameNumber(), wantHzb, contentRes);
        for (int c = 0; c < 4; ++c)
        {
            cb.hzbPrevValid[c] = hzb.prevValid[c];
            cb.hzbViewRect[c] = hzb.viewRect[c];
        }
        cb.hzbSize[0] = hzb.hzbSize[0];
        cb.hzbSize[1] = hzb.hzbSize[1];
        cb.hzbOn = hzb.on ? 1u : 0u;
    }

    in.sdsm.SetFrameParams(cb);
    in.sdsm.SetSourceFrustumSrv(in.shadowGpu.ViewFrustumSrv(renderer->GetCurrentFrameIndex()));
    in.sdsm.PollReadout(renderer);
    // The Anno grabble, made visible: a GPU-instanced caster the GI fold did not take is drawn in
    // Legacy by Pass_CSM's CPU tail with the cascade's matrix. SDSM has no such matrix on the CPU,
    // so those casters do not cast at all -- and this is the number that says whether that matters.
    in.sdsm.SetCpuCasterCount(in.shadowGpu.CountCpuTailCasters(in.objects));
    in.frame.sdsm = &in.sdsm;

    // --sdsm-readout: the same table the dev window shows, into the SESSION LOG, once, past the
    // streaming window (frame 600 -- the same reason --csm-readout waits: at frame 8 the level is
    // still loading and the reduction sees half a scene). A headless --shot run has no dev window,
    // and the per-axis texel is the whole acceptance criterion of the step.
    if (render::sdsm::g_dumpReadout && in.sdsm.ReadoutValid() && renderer->GetTotalFrameNumber() > 600)
    {
        // EIGHT CONSECUTIVE FRAMES, not one. A single table says what the partitions ARE; the
        // question that keeps coming up is what they DO between frames (S16.7: the boundaries
        // visibly walk on some viewpoints), and that is only readable as a run.
        static int s_readoutFrames = 0;
        if (s_readoutFrames == 0) { s_readoutFrames = 8; }
        if (--s_readoutFrames == 0) { render::sdsm::g_dumpReadout = false; }
        const auto& parts = in.sdsm.Readout();
        LOG_INFO(logging::LogCategory::RenderShadow,
                 "SDSM readout frame {} partitions={} border={:.1f} dilation={:.3f} minScale={:.3f} stability={:.2f} cpuTailCasters={} hzb={} cache={} renders=0x{:x} snap={} hysteresis={:.3f}",
                 renderer->GetTotalFrameNumber(), cb.partitions, cb.borderTexels, cb.dilation,
                 cb.minScaleOverSphere, cb.stability, in.sdsm.CpuCasterCount(), cb.hzbOn,
                 render::sdsm::g_cacheFrames, cb.updateMask & 0xFu, cb.texelSnap, cb.sizeHysteresis);
        LOG_INFO(logging::LogCategory::RenderShadow,
                 "  evsm={} exponents {:.1f}/{:.1f} atlas {}x{} moments {:.1f} MB",
                 cb.evsmOn, cb.evsmPos, cb.evsmNeg, D.shadowRes, D.shadowRes,
                 cb.evsmOn ? (static_cast<double>(D.shadowRes) * D.shadowRes * 16.0) / (1024.0 * 1024.0) : 0.0);
        if (cb.hzbOn != 0u)
        {
            // What the occlusion cull actually did, per partition: how many casters last frame's
            // tile hid (deferred) and how many of those turned out to be visible after all (pass
            // B). deferred - drawnB is the CUT; drawnB is the price of a bad guess. Counters of
            // frame N - kFrameCount, which is the latency of the readback that carries them.
            for (unsigned c = 0; c < render::CascadeHzb::kCascades; ++c)
            {
                LOG_INFO(logging::LogCategory::RenderShadow,
                         "  p{} hzb deferred {} drawnB {} (cut {})", c,
                         in.shadowGpu.HzbDeferred(c), in.shadowGpu.HzbDrawnB(c),
                         in.shadowGpu.HzbDeferred(c) >= in.shadowGpu.HzbDrawnB(c)
                             ? in.shadowGpu.HzbDeferred(c) - in.shadowGpu.HzbDrawnB(c) : 0u);
            }
        }
        for (std::uint32_t p = 0; p < render::sdsm::kMaxPartitions; ++p)
        {
            const render::sdsm::Partition& sp = parts[p];
            const float ex = sp.boundsMax.x - sp.boundsMin.x;
            const float ey = sp.boundsMax.y - sp.boundsMin.y;
            const float ez = sp.boundsMax.z - sp.boundsMin.z;
            LOG_INFO(logging::LogCategory::RenderShadow,
                     "  p{} interval {:.2f}-{:.2f}  box {:.1f}x{:.1f}x{:.1f} m at {:.4f},{:.4f}  texel {:.4f}/{:.4f} mm  "
                     "bias {:.2f} mm  samples {}  flags {}{}  legacyTexel {:.2f} mm",
                     p, sp.intervalBegin, sp.intervalEnd, ex, ey, ez,
                     // The origin is light-space (camera-anchored): with a still camera and the
                     // S21 snap on it must not move at all between frames.
                     sp.boundsMin.x, sp.boundsMin.y,
                     sp.texelWS[0] * 1000.0f, sp.texelWS[1] * 1000.0f,
                     sp.depthBiasNDC * ez * 1000.0f, sp.sampleCount,
                     (sp.flags & render::sdsm::kPartitionFlagEmpty) ? "EMPTY " : "",
                     (sp.flags & render::sdsm::kPartitionFlagVolumeLeak) ? "VOLUME-LEAK "
                         : ((sp.flags & render::sdsm::kPartitionFlagScaleClamped) ? "CLAMP" : "-"),
                     // The Legacy cascade of the SAME index, this frame -- the A/B baseline, on
                     // the same line, so the comparison needs no second artefact.
                     in.frame.cascades.unitsPerTexelDbg[p] * 1000.0f);
        }
    }
}

unsigned ShadowViewPlanner::GrowCapacity(unsigned have, unsigned want, unsigned ceiling)
{
    unsigned cap = std::max(have, 1u);
    while (cap < want && cap < ceiling) { cap *= 2u; }
    return std::min(std::max(cap, have), ceiling);
}

void ShadowViewPlanner::ResizeLocalViews(unsigned spots, unsigned points)
{
    spotShadowViews_ = std::vector<SceneView>(spots);
    pointShadowViews_ = std::vector<SceneView>(6u * static_cast<size_t>(points));
    localViewCull_.resize(render::MaxShadowViews() + 1u);
    localViewCullPtr_.resize(render::MaxShadowViews() + 1u, nullptr);
}
