#include "app/scene/Scene.h"

#include <memory>
#include <algorithm>
#include <array>
#include <cmath>
#include <cassert>
#include <string>
#include <utility>

#include "app/camera/Camera.h"
#include "app/Systems.h"
#include "core/Helpers.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "ocean/OceanSimulation.h"
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
    shadowGpu_.Rebuild(renderer, objects_);
    BumpStaticSetVersion(); // Step 11: a fresh level = a new static caster set
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
    const mat4& invProj = camera.GetInvProjMatrix();
    const float3 sunDirWS = dirLight_.GetDirection();

    for (size_t idx = 0; idx < cascadeViews_.size(); ++idx)
    {
        const float sliceNear = cascades.splitsVS[idx];
        const float sliceFar = cascades.splitsVS[idx + 1];

        std::array<float3, 8> cornersWS{};
        BuildFrustumSliceCornersWS(invView, invProj, sliceNear, sliceFar, cornersWS);

        // Step 2a: fit each cascade to the rotation-INVARIANT bounding sphere of its
        // slice corners. The sphere center and radius depend only on the slice shape
        // (near/far + FOV), never on camera/light orientation, so unitsPerTexel is
        // constant frame-to-frame and the light-space texel snap below is the ONLY
        // stabilization needed (no edge crawl / bias swim on rotation). A sphere also
        // projects to a circle of the same radius in any light orientation, so the ortho
        // extent never changes with the sun/camera angle. (The old far-plane center +
        // far-corner radius + coarse world-space snap did NOT enclose the near corners
        // once the snap shifted the center, so the radius had to be clamped to the
        // rotation-dependent corner extent — that clamp was the source of the shimmer.)
        float3 sphereCenter = float3(0.0f, 0.0f, 0.0f);
        for (const auto& corner : cornersWS) { sphereCenter = sphereCenter + corner; }
        sphereCenter = sphereCenter / static_cast<float>(cornersWS.size());

        float sphereRadius = 0.0f;
        for (const auto& corner : cornersWS)
        {
            const float3 d = corner - sphereCenter;
            sphereRadius = std::max(sphereRadius, std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z));
        }

        const float radius = sphereRadius + cascadeConfig_.overlap;
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
        // snap shift, absorbed by `overlap`), so the ortho square contains every corner.
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
    for (std::unique_ptr<RenderableObjectBase>& obj : objects_)
    {
        if (obj && obj->AsOceanRenderable())
        {
            obj->SetVisible(visible);
        }
    }
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

Scene::SceneObjectId Scene::RaycastEditorObject(const Math::float3& origin, const Math::float3& dir) const
{
    SceneObjectId best = 0;
    float bestT = FLT_MAX;
    const float o[3] = { origin.x, origin.y, origin.z };
    const float d[3] = { dir.x, dir.y, dir.z };

    for (size_t i = 0; i < objects_.size(); ++i)
    {
        if (objectIds_[i] == 0 || !objects_[i] || !objects_[i]->IsVisible())
        {
            continue; // editor-owned + visible only
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

        if (hit && tmin < bestT)
        {
            bestT = tmin;
            best = objectIds_[i];
        }
    }

    return best;
}
#endif // WITH_EDITOR

void Scene::Tick(float deltaTime) {
    CPU_SCOPE(ProfilerScopes::kSceneTick);

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
}

void Scene::PrepareViews(Renderer* renderer)
{
    CPU_SCOPE(ProfilerScopes::kPrepareViews);
    if (!renderer)
    {
        return;
    }

    OceanSimulation* oceanSimulation = Systems::GetOceanSimulation();
    camera_.CalcMatrices(renderer);
    renderer->UpdateDlssCameraData(camera_);

    // Publish this frame's pass inputs. SceneRenderer's pass bodies read frameData_,
    // not Scene members.
    frameData_.camera = &camera_;
    frameData_.mainView = &camera_.GetView();
    frameData_.cascadeViews = &cascadeViews_;
    frameData_.spotShadowViews = &spotShadowViews_;
    frameData_.pointShadowViews = &pointShadowViews_;
    frameData_.lightManager = &lightManager_;
    frameData_.skybox = skyBox_.get();
    frameData_.objects = &objects_;
    frameData_.dirLight = &dirLight_;
    frameData_.shadowGpu = &shadowGpu_;
    frameData_.settings = renderSettings_;
#if WITH_EDITOR
    frameData_.selectedEditorObjectId = selectedEditorObjectId_;
    frameData_.selectionOutlineRadius = std::clamp<std::uint32_t>(selectionOutlineRadius_, 1u, 8u);
#else
    frameData_.selectedEditorObjectId = 0;
    frameData_.selectionOutlineRadius = 1;
#endif

    SceneView& mainView = camera_.GetView();
    mainView.renderLayerMask = camera_.GetRenderLayerMask();
    mainView.frustum = Frustum::FromInvViewProj(mainView.invView, mainView.proj, camera_.GetZNear(), camera_.GetZFar());
    mainView.type = SceneView::Type::Camera;
    mainView.requiresDepthCheck = false;

    SceneView* shoreViewPtr = nullptr;
    if (oceanSimulation)
    {
        oceanSimulation->UpdateShoreView(camera_);
        shoreViewPtr = &oceanSimulation->GetShoreDepthView();
    }
    UpdateCascades(camera_, renderer);

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

    // Step 6e: bucketize the shared shadow-caster set ONCE — every directional cascade and
    // spot view uses identical inputs (same objects, camera layer mask, shadow-caster
    // filter), so re-bucketizing per view was ~5 redundant passes/frame. Each shadow view
    // now only runs its own per-frustum Cull against this. CPU-only (does not move GPU FPS).
    const uint32_t camMask = camera_.GetRenderLayerMask();
    shadowCasterSource_.Bucketize(objects_, camMask, /*filterShadowCaster=*/true);

    auto prepareQueue = [this, camMask](SceneView& view)
    {
        CPU_SCOPE(ProfilerScopes::kPrepareQueue);
        if (view.type == SceneView::Type::Shadow && !view.frustum.IsValid())
        {
            view.queue.Clear();
            return;
        }

        // Shadow views with the camera's layer mask reuse the shared shadow-caster set (6e);
        // anything else (e.g. the camera view, filterShadowCaster=false) bucketizes its own.
        // Step 6a pure frustum cull: the old camera-distance "depth clamp" was wrong-axis and
        // disabled; the light-space ortho frustum (Step 2b pancaked) is the correct cull.
        if (view.type == SceneView::Type::Shadow && view.renderLayerMask == camMask)
        {
            view.queue.Cull(view.frustum, shadowCasterSource_);
        }
        else if (g_useFusedBucketizeCull)
        {
            // Fused single pass — the camera view's dominant cost was bucketizing all visible
            // objects and then culling them in a second pass; this stores only survivors.
            view.queue.BucketizeCull(objects_, view.renderLayerMask, view.type == SceneView::Type::Shadow, view.frustum);
        }
        else
        {
            view.queue.Bucketize(objects_, view.renderLayerMask, view.type == SceneView::Type::Shadow);
            view.queue.Cull(view.frustum);
        }
        if (view.type == SceneView::Type::Camera)
        {
            view.queue.SortTransparent(view.view);
            // Step 6: choose each visible object's camera LOD here (per-view, before parallel
            // recording) so Render stays side-effect-free; persistent per-object state gives
            // hysteresis. Shadow views keep their per-cascade LOD floor (chosen at record time).
            view.queue.SelectLods(camera_);
        }
        // Step 3: group opaque draws by pipeline state (PSO/material/mesh). Step 4: collapse
        // the resulting identical-(mesh,material) runs into instanced batches. Applied to the
        // camera gbuffer view AND every shadow view — opaque draws are depth-tested, so the
        // reorder is invisible, and the grid instances in both the gbuffer and shadow passes.
        view.queue.SortOpaque();
        view.queue.BuildInstancedBatches(view.type == SceneView::Type::Camera);
    };

    // main + shore + cascades + up to kMaxShadowedSpotLights spot views + up to
    // 6*kMaxShadowedPointLights point cube-face views.
    tc::inl_vector<SceneView*, 48> viewsToCull;
    auto enqueueView = [&viewsToCull, &prepareQueue](SceneView& view)
    {
        if (view.type == SceneView::Type::Shadow && !view.frustum.IsValid())
        {
            prepareQueue(view);
            return;
        }

        if (viewsToCull.size() < viewsToCull.capacity())
        {
            viewsToCull.push_back(&view);
        }
        else
        {
            prepareQueue(view);
        }
    };

    enqueueView(mainView);
    if (shoreViewPtr)
    {
        enqueueView(*shoreViewPtr);
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

    if (!viewsToCull.empty())
    {
        TaskSystem::Get().DispatchWait(viewsToCull.size(), [prepareQueue, &viewsToCull](std::size_t index)
        {
            if (index >= viewsToCull.size())
            {
                return;
            }

            prepareQueue(*viewsToCull[index]);
        }, 1);
    }
}

void Scene::Render(Renderer* renderer) {
    if (!renderer) {
        return;
    }
    CPU_SCOPE(ProfilerScopes::kSceneRender);

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

    PrepareViews(renderer);

    // Rung 0 / Step 2: upload the active shadow views' frustum planes (the per-view cull input)
    // into this frame's ring region. Fixed slot layout [cascades | spots | point-faces] so a
    // view's slot index is stable for the future cull; inactive slots pass null → zeroed.
    {
        constexpr size_t kCascadeSlots = static_cast<size_t>(kCascades);
        constexpr size_t kSpotSlots = LightManager::kMaxShadowedSpotLights;
        constexpr size_t kPointFaceSlots = LightManager::kMaxShadowedPointLights * 6;
        // Single source of truth: the indirect buffers (ShadowGpuData) size per view against
        // render::kMaxShadowViews; keep it equal to the real cap sum so they can't drift.
        static_assert(kCascadeSlots + kSpotSlots + kPointFaceSlots == render::kMaxShadowViews,
                      "render::kMaxShadowViews must equal the shadow-view slot layout");
        // Direct constant base offsets (not a running index) so the array writes are
        // provably in-bounds — [0,kCascadeSlots) | [kCascadeSlots,+kSpotSlots) | rest.
        std::array<const Frustum*, kCascadeSlots + kSpotSlots + kPointFaceSlots> frustums{};
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
    objects_.clear();
#if WITH_EDITOR
    objectIds_.clear();
    selectedEditorObjectId_ = 0;
    selectionOutlineRadius_ = 1;
#endif
    camera_.GetView().queue.Clear();
    for (auto& view : cascadeViews_)
    {
        view.queue.Clear();
    }
    skyBox_.reset();
}
