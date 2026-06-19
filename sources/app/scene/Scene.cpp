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
    objects_.push_back(std::move(obj));
}

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
    frameData_.lightManager = &lightManager_;
    frameData_.skybox = skyBox_.get();
    frameData_.objects = &objects_;
    frameData_.dirLight = &dirLight_;
    frameData_.settings = renderSettings_;

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

    const size_t spotLightCount = lightManager_.GetSpotLightCount();
    const auto& spotLights = lightManager_.SpotLights();
    for (size_t i = 0; i < spotLightCount; ++i)
    {
        const auto& light = spotLights[i];
        SceneView& view = spotShadowViews_[i];
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
        else
        {
            view.queue.Bucketize(objects_, view.renderLayerMask, view.type == SceneView::Type::Shadow);
            view.queue.Cull(view.frustum);
        }
        if (view.type == SceneView::Type::Camera)
        {
            view.queue.SortTransparent(view.view);
            view.queue.SortOpaque(); // Step 3: group opaque draws by pipeline state
        }
    };

    tc::inl_vector<SceneView*, 32> viewsToCull;
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
    for (int i = 0; i < lightManager_.GetSpotLightCount(); ++i)
    {
        enqueueView(spotShadowViews_[i]);
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

    PrepareViews(renderer);

    sceneRenderer_.Render(renderer, frameData_);
}

void Scene::Clear()
{
    sceneRenderer_.Reset();
    frameData_ = SceneFrameData{}; // drop pointers into objects we are about to destroy
    lightManager_.Reset();
    objects_.clear();
    camera_.GetView().queue.Clear();
    for (auto& view : cascadeViews_)
    {
        view.queue.Clear();
    }
    skyBox_.reset();
}
