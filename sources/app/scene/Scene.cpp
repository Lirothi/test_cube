#include "app/scene/Scene.h"

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

float Scene::GetCascadeNormalBias(size_t index) const
{
    assert(index < static_cast<size_t>(kCascades));
    return frameData_.cascades.normalBiasWS[index];
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
        const float texelEstimate = (2.0f * sphereRadius) / static_cast<float>(tileRes);
        const float radius = sphereRadius + cascadeConfig_.overlapInTexels * texelEstimate;
        const float unitsPerTexel = (2.0f * radius) / static_cast<float>(tileRes);

        const float3 up(0, 1, 0);
        const float lightDistance = std::max(1.0f, cascadeConfig_.maxDistance);

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

        // Step 2b: pull the near plane TOWARD the light by casterReachWS so casters
        // between the sun and this slice still render and cast; the far side stays fitted.
        const float zPad = cascadeConfig_.zPadding;
        const float nearLS = std::max(0.001f, minZ - cascadeConfig_.casterReachWS);
        const float farLS = maxZ + zPad;

        const mat4 lightProj = mat4::OrthoOffCenterLH(minX, maxX, minY, maxY, nearLS, farLS);

        const float normalBiasInTexels = cascadeConfig_.normalBiasInTexels;
        const float depthBiasInTexels = cascadeConfig_.depthBiasInTexels;
        cascades.normalBiasWS[idx] = normalBiasInTexels * unitsPerTexel;
        cascades.depthBiasNDC[idx] = (depthBiasInTexels * unitsPerTexel) / (farLS - nearLS);

        const float2 scale = float2(static_cast<float>(tileRes) / static_cast<float>(deferred.shadowRes));
        const float2 bias = float2((idx % 2) * scale.x, (idx / 2) * scale.y);
        cascades.atlasScale[idx] = scale;
        cascades.atlasBias[idx] = bias;
        cascades.lightView[idx] = lightView;
        cascades.lightProj[idx] = lightProj;

        SceneView& cascadeView = cascadeViews_[idx];
        cascadeView.view = lightView;
        cascadeView.proj = lightProj;
        cascadeView.invView = mat4::Inverse(lightView);
        cascadeView.invProj = mat4::Inverse(lightProj);
        cascadeView.frustum = Frustum::FromOrthoBounds(cascadeView.invView, minX, maxX, minY, maxY, nearLS, farLS);
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
        cascades.nearLsDbg[idx] = nearLS;
        cascades.farLsDbg[idx] = farLS;
        cascades.tileSizeDbg[idx] = tileRes;
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

        // PER-LEVEL depth range that scales with the level extent (Step 24f): tight for fine near
        // levels (good D16 precision -> a small NDC bias actually clears acne), proportionally larger
        // for coarse far levels. Because the range ∝ extent, a SINGLE NDC depth bias works at every
        // level. depthUp = caster reach up-sun (well above the level's ground); depthDown = receivers.
        const float depthUp = extent * 5.0f;
        const float depthDown = extent * 1.0f;
        const float originDist = depthUp + 1.0f;
        const mat4 lightView = mat4::LookAtLH(center - sunDirWS * originDist, center, up);
        const float2 centerLS = (lightView * float4(center, 1)).xy(); // ~(0,0): center is the target
        const float minX = centerLS.x - radius, maxX = centerLS.x + radius;
        const float minY = centerLS.y - radius, maxY = centerLS.y + radius;
        const float nearLS = 1.0f;
        const float farLS = originDist + depthDown;

        const mat4 lightProj = mat4::OrthoOffCenterLH(minX, maxX, minY, maxY, nearLS, farLS);
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

void Scene::ReconcileShadowLodBias(Renderer* renderer)
{
    // The shadow LOD bias picks a coarser (or finer) caster LOD to rasterize into the shadow maps.
    // The geometry lives in the consolidated mega buffer built at load, so a change needs a GPU-idle
    // rebuild. Cheap to poll (one int compare); only rebuilds on an actual change (slider drag).
    // (Chunked-terrain LOD needs NO rebuild anywhere: its per-chunk camera tiers travel through a
    // per-frame CB override — see ShadowGpuData::RefreshChunkGroupLods.)
    if (!renderer) { return; }
    if (shadowGpu_.BuiltShadowLod() == render::g_shadowLodBias) { return; }
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

    for (PointLight& light : lightManager_.PointLights())
    {
        light.Tick(deltaTime);
    }

#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
    size_t batchSize = 32;
    TaskSystem::ParallelFor(objects_.size(),
        [this, deltaTime](size_t index) {
            if (index >= objects_.size()) {
                return;
            }
            objects_[index]->Tick(deltaTime);
        }, batchSize);

    TaskSystem::ParallelFor(objects_.size(),
        [this, deltaTime](size_t index) {
            if (index >= objects_.size()) {
                return;
            }
            objects_[index]->PostTick(deltaTime);
        }, batchSize);
#else
    for (auto& obj : objects_)
    {
        obj->Tick(deltaTime);
    }

    for (auto& obj : objects_)
    {
        obj->PostTick(deltaTime);
    }
#endif

    // W1: advance the global wind from the SAME clock the ocean uses (its elapsedTime_), so waves
    // and foliage sway stay phase-coherent. No ocean -> standalone monotonic accumulator (coherence
    // is moot). Runs after the object-Tick barrier above, so the ocean's clock is current.
    {
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
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "[wind] frame=%d time=%.4f active=%d windDir=%.1f strength=%.2f swayAmp=%.3f | ocean=%s oceanDir=%.1f oceanForce=%.2f |dir|=%.4f\n",
                s_windLogFrames, windState_.time, windState_.active ? 1 : 0,
                windState_.directionDeg, windState_.strength, windState_.swayAmplitude,
                ocean ? "yes" : "none", oceanDir, oceanForce, len);
            OutputDebugStringA(buf);
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
        view.queue.SelectLods(camera_);
    }
    if (!usesSharedShadowSource && !usesSharedCameraSource)
    {
        view.queue.SortOpaque();
    }
    view.queue.BuildInstancedBatches(view.type == SceneView::Type::Camera);
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
    frameData_.spotShadowViews = &spotShadowViews_;
    frameData_.pointShadowViews = &pointShadowViews_;
    frameData_.lightManager = &lightManager_;
    frameData_.skybox = skyBox_.get();
    frameData_.objects = &objects_;
    frameData_.dirLight = &dirLight_;
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
    if (!renderer) {
        return;
    }
    CPU_SCOPE(ProfilerScopes::kSceneRender);

    ReconcileShadowMode(renderer); // Step 24b: apply a pending Legacy<->VSM switch (GPU-idle free/alloc)
    ReconcileShadowLodBias(renderer); // apply a pending shadow-LOD-bias change (GPU-idle caster rebuild)

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

    // Chunked-terrain LOD: publish THIS frame's per-chunk camera tiers (chosen by SelectLods inside
    // PrepareViews above) as the shadow caster overrides — after PrepareViews on purpose, so the
    // caster can never lag the receiver by a frame at a LOD transition.
    shadowGpu_.RefreshChunkGroupLods(objects_);

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
