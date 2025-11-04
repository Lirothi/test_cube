#include "app/scene/Scene.h"

#include <memory>
#include <algorithm>
#include <array>
#include <cmath>
#include <cassert>
#include <string>
#include <utility>

#include "input/InputManager.h"
#include "app/camera/Camera.h"
#include "app/Systems.h"
#include "rendering/debug/DebugDraw.h"
#include "core/Helpers.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/RenderGraph.h"
#include "rendering/core/RenderPass.h"
#include "rendering/renderables/RenderableObject.h"
#include "core/task/TaskSystem.h"
#include "text/TextManager.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "core/math/AABB.h"
#include "core/math/Frustum.h"
#include "core/containers/inl_vector.h"

static void SetCommandListName(ID3D12GraphicsCommandList* cl, RenderPass pass)
{
    const auto nameW = RenderPassToWString(pass);
    if (!nameW.empty() && cl)
    {
        cl->SetName(nameW.data());
    }
}

const mat4& Scene::GetCascadeView(size_t index) const
{
    assert(index < static_cast<size_t>(kCascades));
    return cachedLightView_[index];
}

const mat4& Scene::GetCascadeProj(size_t index) const
{
    assert(index < static_cast<size_t>(kCascades));
    return cachedLightProj_[index];
}

float2 Scene::GetCascadeScale(size_t index) const
{
    assert(index < static_cast<size_t>(kCascades));
    return cachedScale_[index];
}

float2 Scene::GetCascadeBias(size_t index) const
{
    assert(index < static_cast<size_t>(kCascades));
    return cachedBias_[index];
}

float Scene::GetCascadeNormalBias(size_t index) const
{
    assert(index < static_cast<size_t>(kCascades));
    return cachedNormalBiasWS_[index];
}

float Scene::GetCascadeDepthBias(size_t index) const
{
    assert(index < static_cast<size_t>(kCascades));
    return cachedDepthBiasNDC_[index];
}

namespace
{
    constexpr size_t BucketIndex(SceneRenderQueue::BucketType type)
    {
        return static_cast<size_t>(type);
    }

    void FilterShadowCasters(SceneRenderQueue& queue)
    {
        auto filterBucket = [](SceneRenderQueue::ObjectBucket& bucket)
        {
            bucket.erase(std::remove_if(bucket.begin(), bucket.end(), [](RenderableObjectBase* obj)
            {
                return !obj || !obj->CastsShadow();
            }), bucket.end());
        };

        filterBucket(queue.GetBucket(SceneRenderQueue::BucketType::OpaqueSimple));
        filterBucket(queue.GetBucket(SceneRenderQueue::BucketType::OpaqueComplex));
        filterBucket(queue.GetBucket(SceneRenderQueue::BucketType::TransparentSimple));
        filterBucket(queue.GetBucket(SceneRenderQueue::BucketType::TransparentComplex));
    }
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
    resources_.Initialize(renderer, uploadCmdList, uploadKeepAlive);
}

void Scene::FinalizeLevelLoad(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    resources_.Finalize(renderer, objects_, uploadCmdList, uploadKeepAlive, skyBox_.get());
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
    const auto splits = cascadeConfig_.BuildSplitScheme(zNear, zFar);
    for (size_t i = 0; i < splits.size(); ++i)
    {
        cachedSplitsVS_[i] = splits[i];
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
            view.queue.Clear();
        }
        return;
    }

    const mat4& proj = camera.GetProjMatrix();
    const mat4& invView = camera.GetInvViewMatrix();
    const mat4& invProj = camera.GetInvProjMatrix();
    const float3 camDir = camera.GetDirection();
    const float3 camPos = camera.GetPosition();
    const float3 sunDirWS = dirLight_.GetDirection();

    const float tanH = 1.0f / proj.m._11;
    const float tanV = 1.0f / proj.m._22;

    for (size_t idx = 0; idx < cascadeViews_.size(); ++idx)
    {
        const float sliceNear = cachedSplitsVS_[idx];
        const float sliceFar = cachedSplitsVS_[idx + 1];

        std::array<float3, 8> cornersWS{};
        BuildFrustumSliceCornersWS(invView, invProj, sliceNear, sliceFar, cornersWS);

        const float halfSlice = 0.5f * (sliceFar - sliceNear);
        const float farCoef = (sliceFar * tanH) * (sliceFar * tanH) + (sliceFar * tanV) * (sliceFar * tanV);

        float delta = cascadeConfig_.forwardOffset * halfSlice;
        auto radiusFor = [&](float d)
        {
            const float rf2 = farCoef + (halfSlice - d) * (halfSlice - d);
            return std::sqrt(rf2);
        };

        float radius = radiusFor(delta) + cascadeConfig_.overlap;
        float3 center = camPos + camDir * (sliceNear + halfSlice + delta);
        if (cascadeConfig_.stabilizationStepFraction > 0.0f)
        {
            const float spatialStep = radius * cascadeConfig_.stabilizationStepFraction;
            if (spatialStep > 0.0f)
            {
                center = Floor(center / spatialStep) * spatialStep;
            }
        }

        const float3 up(0, 1, 0);
        const float lightDistance = std::max(1.0f, cascadeConfig_.maxDistance);
        const mat4 lightView = mat4::LookAtLH(center - sunDirWS * lightDistance, center, up);

        float2 centerLS = (lightView * float4(center, 1)).xy();
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
        radius = std::min(radius, rLS);

        const float unitsPerTexel = (2.0f * radius) / static_cast<float>(tileRes);
        if (unitsPerTexel > 0.0f)
        {
            centerLS.x = std::floor(centerLS.x / unitsPerTexel) * unitsPerTexel;
            centerLS.y = std::floor(centerLS.y / unitsPerTexel) * unitsPerTexel;
        }

        const float minX = centerLS.x - radius;
        const float maxX = centerLS.x + radius;
        const float minY = centerLS.y - radius;
        const float maxY = centerLS.y + radius;

        const float zPad = cascadeConfig_.zPadding;
        const float nearLS = std::max(0.001f, minZ - zPad);
        const float farLS = maxZ + zPad;

        const mat4 lightProj = mat4::OrthoOffCenterLH(minX, maxX, minY, maxY, nearLS, farLS);

        const float normalBiasInTexels = cascadeConfig_.normalBiasInTexels;
        const float depthBiasInTexels = cascadeConfig_.depthBiasInTexels;
        cachedNormalBiasWS_[idx] = normalBiasInTexels * unitsPerTexel;
        cachedDepthBiasNDC_[idx] = (depthBiasInTexels * unitsPerTexel) / (farLS - nearLS);

        const float2 scale = float2(static_cast<float>(tileRes) / static_cast<float>(deferred.shadowRes));
        const float2 bias = float2((idx % 2) * scale.x, (idx / 2) * scale.y);
        cachedScale_[idx] = scale;
        cachedBias_[idx] = bias;
        cachedLightView_[idx] = lightView;
        cachedLightProj_[idx] = lightProj;

        SceneView& cascadeView = cascadeViews_[idx];
        cascadeView.view = lightView;
        cascadeView.proj = lightProj;
        cascadeView.invView = mat4::Inverse(lightView);
        cascadeView.invProj = mat4::Inverse(lightProj);
        cascadeView.frustum = Frustum::FromCorners(cornersWS);
        cascadeView.renderLayerMask = camera.GetRenderLayerMask();
        cascadeView.position = center;
        cascadeView.type = SceneView::Type::Shadow;
        cascadeView.zNear = nearLS;
        cascadeView.zFar = farLS;
        cascadeView.hfov = 0.0f;
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

    auto& input = Systems::GetInput();
    camera_.UpdateFromInput(deltaTime);

    if (input.WasActionPressed("DebugTex"))
    {
        debugTexMode_ = !debugTexMode_;
    }
    if (input.WasActionPressed("ToggleProfiler"))
    {
        showProfiler_ = !showProfiler_;
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

    //for (auto& l : lightManager_.SpotLights())
    //{
    //    auto dbg = l.GetDebugConeParams();
    //    Systems::GetRenderer().GetDebugDrawSystem()->AddCone(dbg.apex, dbg.direction, dbg.height, dbg.radius, { 0.0f, 0.0f, 1.0f, 0.5f }, false);
    //    //const OBB& coneObb = l.GetConeOBB();
    //    //if (coneObb.IsValid())
    //    //{
    //    //    Systems::GetRenderer().GetDebugDrawSystem()->AddBox(coneObb, {1.0f, 1.0f, 0.0f, 0.25f}, false);
    //    //    Systems::GetRenderer().GetDebugDrawSystem()->AddBox(coneObb, { 1.0f, 1.0f, 0.0f, 0.5f }, true);
    //    //    Systems::GetRenderer().GetDebugDrawSystem()->AddSphere(l.GetDesc().position, 1.0f, { 0.0f, 1.0f, 0.0f, 0.25f }, false);
    //    //}

    //    //Systems::GetRenderer().GetDebugDrawSystem()->AddBox({ 0.0f, 0.0f, 0.0f }, { 2.0f, 2.0f, 2.0f }, { 1.0f, 1.0f, 0.0f, 0.5f }, true);
    //}
}

void Scene::PrepareViews(Renderer* renderer)
{
    CPU_SCOPE(ProfilerScopes::kPrepareViews);
    if (!renderer)
    {
        return;
    }

    constexpr float kHFovRadians = XMConvertToRadians(90.0f);
    const float zNear = 0.01f;
    const float zFar = 10000.0f;
    camera_.SetHFov(kHFovRadians);
    camera_.SetZNearFar(zNear, zFar);
    camera_.CalcMatrices(renderer);

    SceneView& mainView = camera_.GetView();
    mainView.renderLayerMask = camera_.GetRenderLayerMask();
    mainView.frustum = Frustum::FromInvViewProj(mainView.invView, mainView.proj, camera_.GetZNear(), camera_.GetZFar());
    mainView.type = SceneView::Type::Camera;

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
    }

    //if (DebugDrawSystem* debugDraw = renderer->GetDebugDrawSystem())
    //{
    //    const Math::float4 cameraColor(0.0f, 1.0f, 0.0f, 0.9f);
    //    if (mainView.frustum.IsValid())
    //    {
    //        debugDraw->AddFrustum(mainView.frustum, cameraColor);
    //    }

    //    const Math::float4 cascadeColor(1.0f, 0.8f, 0.0f, 0.9f);
    //    for (const SceneView& cascadeView : cascadeViews_)
    //    {
    //        if (cascadeView.frustum.IsValid())
    //        {
    //            debugDraw->AddFrustum(cascadeView.frustum, cascadeColor);
    //        }
    //    }

    //    const Math::float4 spotColor(1.0f, 0.2f, 0.2f, 0.9f);
    //    for (size_t i = 0; i < spotLightCount; ++i)
    //    {
    //        const SceneView& spotView = spotShadowViews_[i];
    //        if (spotView.frustum.IsValid())
    //        {
    //            debugDraw->AddFrustum(spotView.frustum, spotColor);
    //        }
    //    }
    //}

    auto prepareQueue = [this](SceneView& view)
    {
        CPU_SCOPE(ProfilerScopes::kPrepareQueue);
        if (view.type == SceneView::Type::Shadow && !view.frustum.IsValid())
        {
            view.queue.Clear();
            return;
        }

        {
            CPU_SCOPE(ProfilerScopes::kService1);
            view.queue.Bucketize(objects_, view.renderLayerMask, view.type == SceneView::Type::Shadow);
        }

        view.queue.Cull(view.frustum);
        if (view.type == SceneView::Type::Camera)
        {
            view.queue.SortTransparent(view.view);
        }
    };

    tc::inl_vector<SceneView*, 16> viewsToCull;
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

    if (Systems::GetInput().WasActionPressed("Wireframe")) {
        renderer->SetWireframeMode(!renderer->GetWireframeMode());
    }

    if (renderer->ConsumeMaterialHotReloadFlag())
    {
        resources_.RefreshMaterialHandles(renderer, objects_, skyBox_.get());
    }

    auto* tb = renderer->GetTextManager();
    tb->Begin(renderer->GetWidth(), renderer->GetHeight(), 1.0f);
    int y = 8;
    tb->AddTextfShadow(8, 8, 32.0f, float4(1, 1, 1, 0.6f), true, L"FPS:%.0f MS:%0.2f", renderer->GetFPS(), 1000.0f / renderer->GetFPS());
    auto& camPos = camera_.GetPosition();
    tb->AddTextfShadow(8, 8 + 32, 16.0f, float4(1, 1, 1, 0.9f), true, L"Cam: %0.2f %0.2f %0.2f, speed: %0.2f", camPos.x, camPos.y, camPos.z, camera_.GetMoveSpeedMult());
    //tb->AddText(8, 8 + 32 + 32, 10.0f, float4(1, 1, 1, 0.9f), L"The quick brown fox jumps over the lazy dog 0123456789", false);
    //tb->AddText(8, 8 + 32 + 32 + 32, 16.0f, float4(1, 1, 1, 0.9f), L"The quick brown fox jumps over the lazy dog 0123456789", true);
    //tb->AddText(8, 8 + 32 + 32 + 32 + 32, 64.0f, float4(1, 1, 1, 0.9f), L"The quick brown fox jumps over the lazy dog 0123456789", true);


    renderer->BeginSubmitTimeline();

    TaskSystem::Get().WaitForTrackedAsyncTasks();

    lightManager_.UpdateSpotLightCache();

    PrepareViews(renderer);

    using MainRenderGraph = RenderGraph<kMainRenderGraphPassCount>;
    MainRenderGraph rg;
    auto pClear = rg.AddPass(RenderPass::Main_PrologueClear, {},
        [this, renderer](RenderGraphPassContext ctx) { CPU_SCOPE(ProfilerScopes::kPassPrologueClear); Pass_PrologueClear(renderer, ctx); });

    auto pCompute = rg.AddPass(RenderPass::Main_ObjectCompute, { pClear },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassObjectCompute);
            Pass_ObjectCompute(renderer, ctx);
        });

    auto pShadow = rg.AddPass(RenderPass::Main_CSM, { pCompute },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassCSM);
            Pass_CSM(renderer, ctx, cascadeViews_);
        });

    auto pSpotShadow = rg.AddPass(RenderPass::Main_SpotShadows, { pShadow },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassSpotShadow);
            Pass_SpotShadows(renderer, ctx, spotShadowViews_);
        });

    auto pGbuf = rg.AddPass(RenderPass::Main_GBuffer, { pSpotShadow },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassGBuffer);
            Pass_GBuffer(renderer, ctx, camera_, camera_.GetView());
        });

    auto pLight = rg.AddPassMT(RenderPass::Main_Lighting, { pGbuf }, { pShadow },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassLighting);
            Pass_Lighting(renderer, ctx, camera_);
        });

    auto pSpotLights = rg.AddPassMT(RenderPass::Main_SpotLights, { pLight }, { pSpotShadow },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassSpotLights);
            Pass_SpotLights(renderer, ctx, camera_);
        });

    auto pPointLights = rg.AddPass(RenderPass::Main_PointLights, { pSpotLights },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassPointLights);
            Pass_PointLights(renderer, ctx, camera_);
        });

    auto pSky = rg.AddPass(RenderPass::Main_Skybox, { pPointLights },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassSkybox);
            Pass_Skybox(renderer, ctx, camera_);
        });

    auto pSSR = rg.AddPass(RenderPass::Main_SSR, { pSky },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassSSR);
            Pass_SSR(renderer, ctx, camera_);
        });

    auto pBlur = rg.AddPass(RenderPass::Main_SSRBlur, { pSSR },
        [this, renderer](RenderGraphPassContext ctx) { CPU_SCOPE(ProfilerScopes::kPassSSRBlur); Pass_SSR_Blur(renderer, ctx); });

    auto pCompose = rg.AddPass(RenderPass::Main_Compose, { pBlur },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassCompose);
            Pass_Compose(renderer, ctx, camera_);
        });

    auto pTransp = rg.AddPass(RenderPass::Main_Transparent, { pCompose },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassTransparent);
            Pass_Transparent(renderer, ctx, camera_, camera_.GetView());
        });

    auto pDebugDraw = rg.AddPass(RenderPass::Main_DebugDraw, { pTransp },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassDebugDraw);
            Pass_DebugDraw(renderer, ctx, camera_);
        });

    // Ensure tonemapping runs after the debug draw pass so the resolved backbuffer
    // always includes any debug geometry submitted during rendering.
    auto pTone = rg.AddPass(RenderPass::Main_Tonemap, { pDebugDraw },
        [this, renderer](RenderGraphPassContext ctx) { CPU_SCOPE(ProfilerScopes::kPassTonemap); Pass_Tonemap(renderer, ctx); });

    rg.AddPass(RenderPass::Main_Debug, { pTone },
        [this, renderer](RenderGraphPassContext ctx) { CPU_SCOPE(ProfilerScopes::kPassDebug); Pass_Debug(renderer, ctx); });

#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
    rg.ExecuteParallel(renderer, TaskSystem::Get());
#else
    rg.Execute(renderer);
#endif

    {
        CPU_SCOPE(ProfilerScopes::kFrameAsyncWait);
        TaskSystem::Get().WaitForTrackedAsyncTasks();
    }

    RenderGraph<kEpilogueRenderGraphPassCount> epilogueRG;
    epilogueRG.AddPass(RenderPass::Epilogue_Overlay, {},
        [this, renderer](RenderGraphPassContext ctx) { CPU_SCOPE(ProfilerScopes::kPassOverlay); Pass_Overlay(renderer, ctx); });
    epilogueRG.Execute(renderer);
    
    {
        CPU_SCOPE(ProfilerScopes::kOverlayAsyncWait);
        TaskSystem::Get().WaitForTrackedAsyncTasks();
    }
    renderer->EndFrame();
}

void Scene::RenderObjectBatch(Renderer* renderer,
    const std::vector<RenderableObjectBase*>& objects,
    size_t batchIndex,
    const Camera& camera,
    bool useBundles,
    bool bindGbufOrScene,
    size_t chunkSize)
{
    if (objects.empty()) {
        return;
    }

    //chunkSize = 16;

    auto& tasks = TaskSystem::Get();
    const size_t N = objects.size();

    auto renderJob = [renderer, &camera, &objects, useBundles, chunkSize, batchIndex, bindGbufOrScene](std::size_t jobIndex)
    {
        CPU_SCOPE(ProfilerScopes::kRenderObjectBatchAsync);
        const size_t begin = jobIndex * chunkSize;
        const size_t end = std::min(begin + chunkSize, objects.size());

        if (useBundles) {
            auto b = renderer->BeginThreadCommandBundle(nullptr);
            for (size_t i = begin; i < end; ++i) {
                if (auto* obj = objects[i]) {
                    obj->Render(renderer, b.cl, camera);
                }
            }
            renderer->EndThreadCommandBundle(b, batchIndex);
        }
        else {
            auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
            {
                GPU_SCOPE(t.cl, ProfilerScopes::kRenderObjectBatchGpu);
                if (bindGbufOrScene)
                {
                    renderer->BindGBuffer(t.cl, Renderer::ClearMode::None); // no clear!
                }
                else
                {
                    renderer->BindSceneColor(t.cl, Renderer::ClearMode::None, true);
                }

                for (size_t i = begin; i < end; ++i) {
                    if (auto* obj = objects[i]) {
                        obj->Render(renderer, t.cl, camera);
                    }
                }
            }
            renderer->EndThreadCommandList(t, batchIndex);
        }
    };

#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
    tasks.DispatchTrack((N + chunkSize - 1) / chunkSize, renderJob, 1);
#else
    (void)tasks;
    const size_t jobCount = (N + chunkSize - 1) / chunkSize;
    for (size_t jobIndex = 0; jobIndex < jobCount; ++jobIndex) {
        renderJob(jobIndex);
    }
#endif
}
void Scene::RenderShadowBatch(Renderer* renderer,
    const std::vector<RenderableObjectBase*>& objects,
    size_t batchIndex,
    const mat4& lightView, const mat4& lightProj,
    UINT cascadeIndex, size_t chunkSize)
{
    if (objects.empty())
    {
        return;
    }

    auto& tasks = TaskSystem::Get();
    const size_t N = objects.size();
    if (chunkSize == 0)
    {
        chunkSize = 16;
    }

    auto shadowJob = [renderer, &objects, &lightView, &lightProj, cascadeIndex, chunkSize, batchIndex](std::size_t jobIndex)
    {
        CPU_SCOPE(ProfilerScopes::kRenderShadowBatchAsync);
        const size_t begin = jobIndex * chunkSize;
        const size_t end = std::min(begin + chunkSize, objects.size());

        // Each chunk uses its own DIRECT command list
        auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        t.cl->SetName(L"RenderShadowBatch");
        {
            GPU_SCOPE(t.cl, ProfilerScopes::kRenderShadowBatchGpu);

            // Important: bind the correct atlas tile for the cascade without clearing
            renderer->BindShadowTarget(t.cl, cascadeIndex, /*clear=*/false);

            for (size_t i = begin; i < end; ++i) {
                if (auto* obj = objects[i]) {
                    obj->RenderShadow(renderer, t.cl, lightView, lightProj);
                }
            }
        }
        renderer->EndThreadCommandList(t, batchIndex);
    };

#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
    tasks.DispatchTrack((N + chunkSize - 1) / chunkSize, shadowJob, 1);
#else
    (void)tasks;
    const size_t jobCount = (N + chunkSize - 1) / chunkSize;
    for (size_t jobIndex = 0; jobIndex < jobCount; ++jobIndex) {
        shadowJob(jobIndex);
    }
#endif
}

void Scene::Pass_PrologueClear(Renderer* r, RenderGraphPassContext ctx)
{
    auto t = r->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassPrologueClear);
        r->RecordBindAndClear(t.cl);
    }
    r->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_ObjectCompute(Renderer* renderer, RenderGraphPassContext ctx)
{
    if (!renderer || objects_.empty())
    {
        return;
    }

    auto compute = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(compute.cl, ctx.pass);
    {
        GPU_SCOPE(compute.cl, ProfilerScopes::kPassObjectCompute);
        for (const auto& obj : objects_)
        {
            if (!obj)
            {
                continue;
            }

            obj->ExecuteCompute(renderer, compute.cl);
        }
    }

    renderer->EndThreadCommandList(compute, ctx.batchIndex);
}

void Scene::Pass_CSM(Renderer* renderer, RenderGraphPassContext ctx,
    const std::array<SceneView, kCascades>& cascadeViews)
{
    if (!renderer)
    {
        return;
    }

    auto d = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(d.cl, ctx.pass);
    {
        GPU_SCOPE(d.cl, ProfilerScopes::kPassCSM);
        const auto& D = renderer->GetDeferredForFrame();
        renderer->Transition(d.cl, D.shadow.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        renderer->BindShadowTarget(d.cl, 0, /*clear=*/true);
    }
    renderer->EndThreadCommandList(d, ctx.batchIndex);

#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
    const RenderPass passName = ctx.pass;
    auto renderCascade = [renderer, &cascadeViews, batchIndex = ctx.batchIndex, passName](std::size_t cascadeIndex)
    {
        if (cascadeIndex >= cascadeViews.size())
        {
            return;
        }

        CPU_SCOPE(ProfilerScopes::kCSMPerCascade);
        const SceneView& view = cascadeViews[cascadeIndex];
        const auto& visibleBuckets = view.queue.VisibleBuckets();
        const auto& opaqueSimple = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueSimple)];
        const auto& opaqueComplex = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueComplex)];
        if (opaqueSimple.empty() && opaqueComplex.empty())
        {
            return;
        }

        auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        SetCommandListName(t.cl, passName);
        {
            GPU_SCOPE(t.cl, ProfilerScopes::kPassCSM);
            renderer->BindShadowTarget(t.cl, static_cast<int>(cascadeIndex), /*clear=*/false);

            for (auto* obj : opaqueSimple)
            {
                if (obj)
                {
                    obj->RenderShadow(renderer, t.cl, view.view, view.proj);
                }
            }

            for (auto* obj : opaqueComplex)
            {
                if (obj)
                {
                    obj->RenderShadow(renderer, t.cl, view.view, view.proj);
                }
            }
        }

        renderer->EndThreadCommandList(t, batchIndex);
    };

    TaskSystem::Get().DispatchWait(cascadeViews.size(), renderCascade, 1);
#else
    for (size_t idx = 0; idx < cascadeViews.size(); ++idx)
    {
        CPU_SCOPE(ProfilerScopes::kCSMPerCascade);
        const SceneView& view = cascadeViews[idx];
        const auto& visibleBuckets = view.queue.VisibleBuckets();
        const auto& opaqueSimple = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueSimple)];
        const auto& opaqueComplex = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueComplex)];
        if (opaqueSimple.empty() && opaqueComplex.empty())
        {
            continue;
        }

        auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        SetCommandListName(t.cl, ctx.pass);
        {
            GPU_SCOPE(t.cl, ProfilerScopes::kPassCSM);
            renderer->BindShadowTarget(t.cl, static_cast<int>(idx), /*clear=*/false);

            for (auto* obj : opaqueSimple)
            {
                if (obj)
                {
                    obj->RenderShadow(renderer, t.cl, view.view, view.proj);
                }
            }

            for (auto* obj : opaqueComplex)
            {
                if (obj)
                {
                    obj->RenderShadow(renderer, t.cl, view.view, view.proj);
                }
            }
        }

        renderer->EndThreadCommandList(t, ctx.batchIndex);
    }
#endif
}

const Profiler::ScopeNameKey kShadows1 = Profiler::RegisterTraceLiteral(L"SpotShadows1");
const Profiler::ScopeNameKey kShadows2 = Profiler::RegisterTraceLiteral(L"SpotShadows2");
void Scene::Pass_SpotShadows(Renderer* renderer, RenderGraphPassContext ctx,
    const std::array<SceneView, LightManager::kMaxSpotLights>& spotViews)
{
    if (!renderer)
    {
        return;
    }

    const size_t availableLights = lightManager_.GetSpotLightCount();
    const size_t viewCount = std::min(spotViews.size(), availableLights);
    if (viewCount == 0)
    {
        return;
    }

    const auto& D = renderer->GetDeferredForFrame();

#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
    auto renderSpotShadow = [renderer, &D, &spotViews, batchIndex = ctx.batchIndex](std::size_t lightIndex)
    {
        if (lightIndex >= spotViews.size())
        {
            return;
        }

        CPU_SCOPE(ProfilerScopes::kSpotShadowPerLight);
        auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        {
            GPU_SCOPE(t.cl, ProfilerScopes::kPassSpotShadow);
            renderer->Transition(t.cl, D.spotShadow.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
            renderer->BindSpotShadowTarget(t.cl, static_cast<UINT>(lightIndex), /*clearDepth=*/true);

            const SceneView& view = spotViews[lightIndex];
            const auto& visibleBuckets = view.queue.VisibleBuckets();
            const auto& opaqueSimple = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueSimple)];
            const auto& opaqueComplex = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueComplex)];

            for (auto* obj : opaqueSimple)
            {
                if (obj)
                {
                    obj->RenderShadow(renderer, t.cl, view.view, view.proj);
                }
            }
            for (auto* obj : opaqueComplex)
            {
                if (obj)
                {
                    obj->RenderShadow(renderer, t.cl, view.view, view.proj);
                }
            }
        }
        renderer->EndThreadCommandList(t, batchIndex);
    };

    TaskSystem::Get().DispatchWait(viewCount, renderSpotShadow, 1);
#else
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSpotShadow);
        renderer->Transition(t.cl, D.spotShadow.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);

        for (size_t lightIndex = 0; lightIndex < viewCount; ++lightIndex)
        {
            renderer->BindSpotShadowTarget(t.cl, static_cast<UINT>(lightIndex), /*clearDepth=*/true);

            const SceneView& view = spotViews[lightIndex];
            const auto& visibleBuckets = view.queue.VisibleBuckets();
            const auto& opaqueSimple = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueSimple)];
            const auto& opaqueComplex = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueComplex)];

            for (auto* obj : opaqueSimple)
            {
                if (obj)
                {
                    obj->RenderShadow(renderer, t.cl, view.view, view.proj);
                }
            }
            for (auto* obj : opaqueComplex)
            {
                if (obj)
                {
                    obj->RenderShadow(renderer, t.cl, view.view, view.proj);
                }
            }
        }
    }
    renderer->EndThreadCommandList(t, ctx.batchIndex);
#endif
}

void Scene::Pass_GBuffer(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera, const SceneView& mainView)
{
    RenderGraph<kGBufferRenderGraphPassCount> rgGB(ctx.batchIndex);
    rgGB.AddPass(RenderPass::GBuffer_Driver, {}, [this, renderer](RenderGraphPassContext sub) {
        auto driver = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        SetCommandListName(driver.cl, sub.pass);
        {
            GPU_SCOPE(driver.cl, ProfilerScopes::kGBufferDriver);

            const auto& D = renderer->GetDeferredForFrame();
            renderer->Transition(driver.cl, D.gb0.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            renderer->Transition(driver.cl, D.gb1.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            renderer->Transition(driver.cl, D.gb2.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            renderer->Transition(driver.cl, D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);

            renderer->BindGBuffer(driver.cl, Renderer::ClearMode::ColorDepth);
        }
        renderer->RegisterPassDriver(driver.cl, sub.batchIndex);
        });

    // 1.2 Opaque simple → bundles
    rgGB.AddPass(RenderPass::GBuffer_OpaqueSimple, {}, [this, renderer, &camera, &mainView](RenderGraphPassContext sub) {
        const auto& visibleBuckets = mainView.queue.VisibleBuckets();
        const auto& opaqueSimple = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueSimple)];
        if (!opaqueSimple.empty())
        {
            RenderObjectBatch(renderer, opaqueSimple, sub.batchIndex, camera, /*useBundles=*/true, true, 32);
        }
        });

    // 1.3 Opaque complex → direct command list, no clears
    rgGB.AddPass(RenderPass::GBuffer_OpaqueComplex, {}, [this, renderer, &camera, &mainView](RenderGraphPassContext sub) {
        const auto& visibleBuckets = mainView.queue.VisibleBuckets();
        const auto& opaqueComplex = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueComplex)];
        if (!opaqueComplex.empty())
        {
            RenderObjectBatch(renderer, opaqueComplex, sub.batchIndex, camera, /*useBundles=*/false, true, 32);
        }
        });

    rgGB.Execute(renderer);
}

void Scene::Pass_Lighting(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera)
{
    auto lighting = resources_.GetLightingMaterial();
    if (!lighting)
    {
        return;
    }
    const UINT cbSize = resources_.GetLightingCBSizeBytes();
    if (cbSize == 0)
    {
        return;
    }

    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassLighting);
        const auto& D = renderer->GetDeferredForFrame();
        const D3D12_RESOURCE_STATES srvState =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        renderer->Transition(t.cl, D.gb0.Get(), srvState);
        renderer->Transition(t.cl, D.gb1.Get(), srvState);
        renderer->Transition(t.cl, D.gb2.Get(), srvState);
        renderer->Transition(t.cl, D.depth.Get(), srvState);
        renderer->Transition(t.cl, D.shadow.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        auto cb = renderer->GetFrameResource()->AllocDynamic(cbSize, Renderer::kConstantBufferAlignment);
        LightingPassConstants constants{};
        const mat4& view = camera.GetViewMatrix();
        const mat4& invView = camera.GetInvViewMatrix();
        const mat4& invProj = camera.GetInvProjMatrix();
        const float3 camDir = camera.GetDirection();
        constants.sunDir = dirLight_.GetDirection();
        constants.ambient = dirLight_.GetAmbient();
        constants.lightRgb = dirLight_.GetColor();
        constants.exposure = dirLight_.GetExposure();
        constants.camPos = camera.GetPosition();
        constants.camDir = camDir;
        constants.view = view;
        constants.invView = invView;
        constants.invProj = invProj;
        for (size_t i = 0; i < constants.lightViewProj.size(); ++i)
        {
            constants.lightViewProj[i] = cachedLightView_[i] * cachedLightProj_[i];
            constants.cascadeScaleBias[i] = float4(cachedScale_[i].x, cachedScale_[i].y, cachedBias_[i].x, cachedBias_[i].y);
        }
        constants.cascadeSplits = float4(cachedSplitsVS_[0], cachedSplitsVS_[1], cachedSplitsVS_[2], cachedSplitsVS_[3]);
        const float shadowRes = static_cast<float>(renderer->GetDeferredForFrame().shadowRes);
        constants.shadowAtlasSize = float2(shadowRes, shadowRes);
        constants.shadowBiasNDC = float4(cachedDepthBiasNDC_[0], cachedDepthBiasNDC_[1], cachedDepthBiasNDC_[2], cachedDepthBiasNDC_[3]);
        constants.normalBiasWS = float4(cachedNormalBiasWS_[0], cachedNormalBiasWS_[1], cachedNormalBiasWS_[2], cachedNormalBiasWS_[3]);
        const float width = static_cast<float>(std::max(renderer->GetWidth(), 1u));
        const float height = static_cast<float>(std::max(renderer->GetHeight(), 1u));
        constants.screenSize = float2(width, height);
        constants.invScreenSize = float2(width > 0.f ? (1.0f / width) : 0.0f, height > 0.f ? (1.0f / height) : 0.0f);

        resources_.WriteLightingConstants(constants, (uint8_t*)cb.cpu);

        auto h = renderer->GetRenderContextPool()->Acquire();
        auto& rc = h.ref();

        rc.cbv[0] = cb.gpu;
        const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 5> srvs = {
            D.gbSRV[0],
            D.gbSRV[1],
            D.gbSRV[2],
            D.depthSRV,
            D.shadowSRV
        };
        rc.table[0] = renderer->StageSrvUavTable(srvs).gpu;
        rc.table[1] = renderer->StageSrvUavTable({ D.lightUAV }).gpu;
        const auto samplerDescs = std::array{ *SamplerManager::PointClamp(), *SamplerManager::ComparisonLinearClamp() };
        rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);

        lighting->Bind(t.cl, rc);
        constexpr UINT kGroupSize = 8;
        const UINT groupsX = (renderer->GetWidth() + kGroupSize - 1u) / kGroupSize;
        const UINT groupsY = (renderer->GetHeight() + kGroupSize - 1u) / kGroupSize;
        if (groupsX > 0 && groupsY > 0)
        {
            t.cl->Dispatch(groupsX, groupsY, 1);
        }
        renderer->UAVBarrier(t.cl, D.light.Get());
    }
    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_SpotLights(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera)
{
    const size_t spotLightCount = lightManager_.GetSpotLightCount();
    if (spotLightCount == 0)
    {
        return;
    }

    lightManager_.EnsureSpotLightBuffer(renderer, spotLightCount);
    auto* spotLightBufferCPU = lightManager_.GetSpotLightBufferCPU();
    const D3D12_CPU_DESCRIPTOR_HANDLE spotLightSrvHandle = lightManager_.GetSpotLightSrv();
    if (!spotLightBufferCPU || spotLightSrvHandle.ptr == 0)
    {
        return;
    }

    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSpotLights);
        const auto& D = renderer->GetDeferredForFrame();
        const D3D12_RESOURCE_STATES srvState =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        renderer->Transition(t.cl, D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        renderer->Transition(t.cl, D.gb0.Get(), srvState);
        renderer->Transition(t.cl, D.gb1.Get(), srvState);
        renderer->Transition(t.cl, D.gb2.Get(), srvState);
        renderer->Transition(t.cl, D.depth.Get(), srvState);
        renderer->Transition(t.cl, D.spotShadow.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        const auto& spotLights = lightManager_.SpotLights();
        for (size_t i = 0; i < spotLightCount; ++i)
        {
            const auto& light = spotLights[i];
            const auto& desc = light.GetDesc();
            const mat4 viewProj = light.GetViewProjMatrix();
            const float3 dir = light.GetDirection();

            spotLightBufferCPU[i].positionRange = float4(desc.position, desc.range);
            spotLightBufferCPU[i].directionCosOuter = float4(dir, light.GetCosOuter());
            spotLightBufferCPU[i].colorIntensity = float4(desc.color, desc.intensity);
            spotLightBufferCPU[i].shadowParams = float4(light.GetCosInner(), static_cast<float>(i), light.GetInvAngleRange(), light.GetShadowDepthBias());
            spotLightBufferCPU[i].shadowParams2 = float4(light.GetShadowNormalBias(), 0.0f, 0.0f, 0.0f);
            spotLightBufferCPU[i].viewProj = viewProj;
        }

        auto spotMaterial = resources_.GetSpotLightMaterial();
        const UINT cbSize = resources_.GetSpotLightCBSizeBytes();
        if (!spotMaterial || cbSize == 0)
        {
            renderer->EndThreadCommandList(t, ctx.batchIndex);
            return;
        }

        auto cb = renderer->GetFrameResource()->AllocDynamic(cbSize, Renderer::kConstantBufferAlignment);
        SpotLightPassConstants constants{};
        constants.invView = camera.GetInvViewMatrix();
        constants.invProj = camera.GetInvProjMatrix();
        constants.camPos = camera.GetPosition();
        const float width = static_cast<float>(std::max(renderer->GetWidth(), 1u));
        const float height = static_cast<float>(std::max(renderer->GetHeight(), 1u));
        constants.screenSize = float2(width, height);
        constants.invScreenSize = float2(width > 0.f ? (1.0f / width) : 0.0f, height > 0.f ? (1.0f / height) : 0.0f);
        const float shadowRes = static_cast<float>(renderer->GetDeferredForFrame().spotShadowRes);
        const float invRes = shadowRes > 0.0f ? 1.0f / shadowRes : 0.0f;
        constants.shadowSize = float2(shadowRes, shadowRes);
        constants.invShadowSize = float2(invRes, invRes);
        constants.lightCount = static_cast<uint32_t>(spotLightCount);

        resources_.WriteSpotLightConstants(constants, (uint8_t*)cb.cpu);

        auto h = renderer->GetRenderContextPool()->Acquire();
        auto& rc = h.ref();

        rc.cbv[0] = cb.gpu;
        const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 6> srvs = {
            D.gbSRV[0],
            D.gbSRV[1],
            D.gbSRV[2],
            D.depthSRV,
            D.spotShadowSRV,
            spotLightSrvHandle
        };
        rc.table[0] = renderer->StageSrvUavTable(srvs).gpu;
        rc.table[1] = renderer->StageSrvUavTable({ D.lightUAV }).gpu;
        const auto samplerDescs = std::array{ *SamplerManager::LinearClamp(), *SamplerManager::PointClamp(), *SamplerManager::ComparisonLinearClamp() };
        rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);

        spotMaterial->Bind(t.cl, rc);
        constexpr UINT kGroupSize = 8;
        const UINT groupsX = (renderer->GetWidth() + kGroupSize - 1u) / kGroupSize;
        const UINT groupsY = (renderer->GetHeight() + kGroupSize - 1u) / kGroupSize;
        if (groupsX > 0 && groupsY > 0)
        {
            t.cl->Dispatch(groupsX, groupsY, 1);
        }
        renderer->UAVBarrier(t.cl, D.light.Get());
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_PointLights(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera)
{
    auto& pointLights = lightManager_.PointLights();
    if (pointLights.empty()) { return; }

    lightManager_.EnsurePointLightBuffer(renderer, pointLights.size());
    auto* pointLightBufferCPU = lightManager_.GetPointLightBufferCPU();
    const D3D12_CPU_DESCRIPTOR_HANDLE pointLightSrvHandle = lightManager_.GetPointLightSrv();
    if (!pointLightBufferCPU || pointLightSrvHandle.ptr == 0) { return; }

    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassPointLights);

        const auto& D = renderer->GetDeferredForFrame();
        const D3D12_RESOURCE_STATES srvState =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        renderer->Transition(t.cl, D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        renderer->Transition(t.cl, D.gb0.Get(), srvState);
        renderer->Transition(t.cl, D.gb1.Get(), srvState);
        renderer->Transition(t.cl, D.gb2.Get(), srvState);
        renderer->Transition(t.cl, D.depth.Get(), srvState);

        for (size_t i = 0; i < pointLights.size(); ++i)
        {
            const auto& desc = pointLights[i].GetDesc();
            pointLightBufferCPU[i].position = desc.position;
            pointLightBufferCPU[i].radius = desc.radius;
            pointLightBufferCPU[i].color = desc.color;
            pointLightBufferCPU[i].intensity = desc.intensity;
        }

        auto pointMaterial = resources_.GetPointLightMaterial();
        const UINT cbSize = resources_.GetPointLightCBSizeBytes();
        if (!pointMaterial || cbSize == 0)
        {
            renderer->EndThreadCommandList(t, ctx.batchIndex);
            return;
        }

        auto cb = renderer->GetFrameResource()->AllocDynamic(cbSize, Renderer::kConstantBufferAlignment);
        PointLightPassConstants constants{};
        constants.invView = camera.GetInvViewMatrix();
        constants.invProj = camera.GetInvProjMatrix();
        constants.camPos = camera.GetPosition();
        const float width = static_cast<float>(std::max(renderer->GetWidth(), 1u));
        const float height = static_cast<float>(std::max(renderer->GetHeight(), 1u));
        constants.screenSize = float2(width, height);
        constants.invScreenSize = float2(width > 0.f ? (1.0f / width) : 0.0f, height > 0.f ? (1.0f / height) : 0.0f);
        constants.lightCount = static_cast<uint32_t>(pointLights.size());

        resources_.WritePointLightConstants(constants, (uint8_t*)cb.cpu);

        auto h = renderer->GetRenderContextPool()->Acquire();
        auto& rc = h.ref();

        rc.cbv[0] = cb.gpu;
        const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 5> srvs = {
            D.gbSRV[0],
            D.gbSRV[1],
            D.gbSRV[2],
            D.depthSRV,
            pointLightSrvHandle
        };
        rc.table[0] = renderer->StageSrvUavTable(srvs).gpu;
        rc.table[1] = renderer->StageSrvUavTable({ D.lightUAV }).gpu;
        const auto samplerDescs = std::array{ *SamplerManager::LinearClamp(), *SamplerManager::PointClamp() };
        rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);

        pointMaterial->Bind(t.cl, rc);
        constexpr UINT kGroupSize = 8;
        const UINT groupsX = (renderer->GetWidth() + kGroupSize - 1u) / kGroupSize;
        const UINT groupsY = (renderer->GetHeight() + kGroupSize - 1u) / kGroupSize;
        if (groupsX > 0 && groupsY > 0)
        {
            t.cl->Dispatch(groupsX, groupsY, 1);
        }
        renderer->UAVBarrier(t.cl, D.light.Get());
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_Skybox(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera)
{
    if (!skyBox_) { return; }
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSkybox);

        const auto& D = renderer->GetDeferredForFrame();
        renderer->Transition(t.cl, D.light.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        renderer->Transition(t.cl, D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_READ);

        // RTV = SceneColor, DSV = GBuffer Depth (read-only), no clears
        renderer->BindLightTarget(t.cl, Renderer::ClearMode::None, true);

        skyBox_->Render(renderer, t.cl, camera);
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_SSR(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera)
{
    //return;
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSSR);
        const auto& D = renderer->GetDeferredForFrame();

        renderer->Transition(t.cl, D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.gb1.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.light.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.ssr.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        auto ssrMaterial = resources_.GetSsrMaterial();
        const UINT cbSize = resources_.GetSsrCBSizeBytes();
        if (!ssrMaterial || cbSize == 0)
        {
            renderer->EndThreadCommandList(t, ctx.batchIndex);
            return;
        }

        auto cb = renderer->GetFrameResource()->AllocDynamic(cbSize, Renderer::kConstantBufferAlignment);
        SsrPassConstants constants{};
        const mat4& view = camera.GetViewMatrix();
        const mat4& proj = camera.GetProjMatrix();
        const mat4& invView = camera.GetInvViewMatrix();
        const mat4& invProj = camera.GetInvProjMatrix();
        const float zNear = camera.GetZNear();
        const float zFar = camera.GetZFar();
        constants.view = view;
        constants.proj = proj;
        constants.invView = invView;
        constants.invProj = invProj;
        constants.depthA = zNear / (zNear - zFar);
        constants.depthB = (zNear * zFar) / (zFar - zNear);
        constants.zNear = zNear;
        constants.zFar = zFar;
        constants.screenSize = float2(static_cast<float>(renderer->GetWidth()), static_cast<float>(renderer->GetHeight()));

        resources_.WriteSsrConstants(constants, (uint8_t*)cb.cpu);

        auto h = renderer->GetRenderContextPool()->Acquire();
        auto& rc = h.ref();

        rc.cbv[0] = cb.gpu;
        rc.table[0] = renderer->StageSrvUavTable({ D.lightSRV, D.gbSRV[1], D.depthSRV }).gpu; // t0 Light, t1 GB1, t2 Depth
        rc.table[1] = renderer->StageSrvUavTable({ D.ssrUAV }).gpu; // u0 output
        const auto samplerDescs = std::array{ *SamplerManager::LinearClamp(), *SamplerManager::PointClamp() };
        rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);

        ssrMaterial->Bind(t.cl, rc);
        constexpr UINT kGroupSize = 8;
        const UINT ssrWidth = renderer->GetSsrTextureWidth();
        const UINT ssrHeight = renderer->GetSsrTextureHeight();
        const UINT groupsX = (ssrWidth + kGroupSize - 1u) / kGroupSize;
        const UINT groupsY = (ssrHeight + kGroupSize - 1u) / kGroupSize;
        t.cl->Dispatch(groupsX, groupsY, 1);
        renderer->UAVBarrier(t.cl, D.ssr.Get());
    }
    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_SSR_Blur(Renderer* renderer, RenderGraphPassContext ctx)
{
    //return;
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSSRBlur);
        const auto& D = renderer->GetDeferredForFrame();
        constexpr UINT kGroupSize = 8;
        const UINT ssrWidth = renderer->GetSsrTextureWidth();
        const UINT ssrHeight = renderer->GetSsrTextureHeight();
        const UINT groupsX = (ssrWidth + kGroupSize - 1u) / kGroupSize;
        const UINT groupsY = (ssrHeight + kGroupSize - 1u) / kGroupSize;

        // Horizontal pass
        renderer->Transition(t.cl, D.ssr.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.ssrBlur.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        auto blurMaterial = resources_.GetBlurMaterial();
        const UINT cbSize = resources_.GetBlurCBSizeBytes();
        if (!blurMaterial || cbSize == 0)
        {
            renderer->EndThreadCommandList(t, ctx.batchIndex);
            return;
        }

        auto cb = renderer->GetFrameResource()->AllocDynamic(cbSize, Renderer::kConstantBufferAlignment);
        const float invSsrWidth = ssrWidth > 0 ? (1.0f / static_cast<float>(ssrWidth)) : 0.0f;
        BlurPassConstants blurConstants{};
        blurConstants.direction = float2(invSsrWidth, 0.0f);
        blurConstants.radius = 1.0f;
        resources_.WriteBlurConstants(blurConstants, (uint8_t*)cb.cpu);

        auto h = renderer->GetRenderContextPool()->Acquire();
        auto& rc = h.ref();

        rc.cbv[0] = cb.gpu;
        rc.table[0] = renderer->StageSrvUavTable({ D.ssrSRV }).gpu;
        rc.table[1] = renderer->StageSrvUavTable({ D.ssrBlurUAV }).gpu;
        const auto samplerDescsX = std::array{ *SamplerManager::LinearClamp() };
        rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, samplerDescsX);

        blurMaterial->Bind(t.cl, rc);
        t.cl->Dispatch(groupsX, groupsY, 1);
        renderer->UAVBarrier(t.cl, D.ssrBlur.Get());

        // Vertical pass
        renderer->Transition(t.cl, D.ssrBlur.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.ssr.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        cb = renderer->GetFrameResource()->AllocDynamic(cbSize, Renderer::kConstantBufferAlignment);
        const float invSsrHeight = ssrHeight > 0 ? (1.0f / static_cast<float>(ssrHeight)) : 0.0f;
        blurConstants.direction = float2(0.0f, invSsrHeight);
        resources_.WriteBlurConstants(blurConstants, (uint8_t*)cb.cpu);

        rc.ClearFast();
        rc.cbv[0] = cb.gpu;
        rc.table[0] = renderer->StageSrvUavTable({ D.ssrBlurSRV }).gpu;
        rc.table[1] = renderer->StageSrvUavTable({ D.ssrUAV }).gpu;
        const auto samplerDescsY = std::array{ *SamplerManager::LinearClamp() };
        rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, samplerDescsY);

        blurMaterial->Bind(t.cl, rc);
        t.cl->Dispatch(groupsX, groupsY, 1);
        renderer->UAVBarrier(t.cl, D.ssr.Get());
    }
    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_Compose(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera)
{
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassCompose);
        const auto& D = renderer->GetDeferredForFrame();

        renderer->Transition(t.cl, D.gb0.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.gb1.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.gb2.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.light.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.ssr.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.scene.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        const float width = static_cast<float>(renderer->GetWidth());
        const float height = static_cast<float>(renderer->GetHeight());
        if (width <= 0.0f || height <= 0.0f)
        {
            renderer->Transition(t.cl, D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            renderer->EndThreadCommandList(t, ctx.batchIndex);
            return;
        }

        auto composeMaterial = resources_.GetComposeMaterial();
        const UINT cbSize = resources_.GetComposeCBSizeBytes();
        if (!composeMaterial || cbSize == 0)
        {
            renderer->Transition(t.cl, D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            renderer->EndThreadCommandList(t, ctx.batchIndex);
            return;
        }

        auto cb = renderer->GetFrameResource()->AllocDynamic(cbSize, Renderer::kConstantBufferAlignment);
        ComposePassConstants constants{};
        constants.invView = camera.GetInvViewMatrix();
        constants.invProj = camera.GetInvProjMatrix();
        constants.skyboxIntensity = skyBox_ ? skyBox_->GetExposure() : 1.0f;
        constants.camPos = camera.GetPosition();
        constants.screenSize = float2(width, height);
        constants.invScreenSize = float2(1.0f / width, 1.0f / height);

        resources_.WriteComposeConstants(constants, (uint8_t*)cb.cpu);

        const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 7> srvs = {
            D.lightSRV,
            D.gbSRV[2],
            D.gbSRV[0],
            D.gbSRV[1],
            D.depthSRV,
            skyBox_->GetTex()->GetSRVCPU(),
            D.ssrSRV
        };

        auto h = renderer->GetRenderContextPool()->Acquire();
        auto& rc = h.ref();
        rc.cbv[0] = cb.gpu;
        rc.table[0] = renderer->StageSrvUavTable(srvs).gpu;
        rc.table[1] = renderer->StageSrvUavTable({ D.sceneUAV }).gpu;
        const auto samplerDescs = std::array{ *SamplerManager::LinearClamp(), *SamplerManager::PointClamp() };
        rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);

        composeMaterial->Bind(t.cl, rc);
        constexpr UINT kGroupSize = 8;
        const UINT groupsX = (renderer->GetWidth() + kGroupSize - 1u) / kGroupSize;
        const UINT groupsY = (renderer->GetHeight() + kGroupSize - 1u) / kGroupSize;
        if (groupsX > 0 && groupsY > 0)
        {
            t.cl->Dispatch(groupsX, groupsY, 1);
        }

        renderer->UAVBarrier(t.cl, D.scene.Get());
        renderer->Transition(t.cl, D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_Transparent(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera, const SceneView& mainView)
{
    RenderGraph<kTransparentRenderGraphPassCount> rgTr(ctx.batchIndex);

    // Driver: RTV=SceneColor, DSV=GBuffer. No clear. Do NOT close the driver list.
    rgTr.AddPass(RenderPass::Transparent_Driver, {}, [this, renderer](RenderGraphPassContext sub) {
        auto driver = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        SetCommandListName(driver.cl, sub.pass);
        {
            GPU_SCOPE(driver.cl, ProfilerScopes::kTransparentDriver);
            const auto& D = renderer->GetDeferredForFrame();
            if (D.depthCopy.Get())
            {
                renderer->Transition(driver.cl, D.depth.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
                renderer->Transition(driver.cl, D.depthCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
            }
            if (D.sceneOpaque.Get())
            {
                renderer->Transition(driver.cl, D.scene.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
                renderer->Transition(driver.cl, D.sceneOpaque.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
            }
            if (D.depthCopy.Get())
            {
                driver.cl->CopyResource(D.depthCopy.Get(), D.depth.Get());
                renderer->Transition(driver.cl, D.depthCopy.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            if (D.sceneOpaque.Get())
            {
                driver.cl->CopyResource(D.sceneOpaque.Get(), D.scene.Get());
                renderer->Transition(driver.cl, D.sceneOpaque.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            renderer->Transition(driver.cl, D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            renderer->Transition(driver.cl, D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
            renderer->BindSceneColor(driver.cl, Renderer::ClearMode::None, true);
        }
        renderer->RegisterPassDriver(driver.cl, sub.batchIndex);
        });

    rgTr.AddPass(RenderPass::Transparent_Simple, {}, [this, renderer, &camera, &mainView](RenderGraphPassContext sub) {
        const auto& visibleBuckets = mainView.queue.VisibleBuckets();
        const auto& transparentSimple = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::TransparentSimple)];
        if (!transparentSimple.empty())
        {
            RenderObjectBatch(renderer, transparentSimple, sub.batchIndex, camera, /*useBundles=*/true, false, 32);
        }
        });

    rgTr.AddPass(RenderPass::Transparent_Complex, {}, [this, renderer, &camera, &mainView](RenderGraphPassContext sub) {
        const auto& visibleBuckets = mainView.queue.VisibleBuckets();
        const auto& transparentComplex = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::TransparentComplex)];
        if (!transparentComplex.empty())
        {
            RenderObjectBatch(renderer, transparentComplex, sub.batchIndex, camera, /*useBundles=*/false, false, 32);
        }
        });

    rgTr.Execute(renderer);
}

void Scene::Pass_DebugDraw(Renderer* renderer, RenderGraphPassContext ctx, const Camera& camera)
{
    if (!renderer)
    {
        return;
    }

    DebugDrawSystem* debugDraw = renderer->GetDebugDrawSystem();
    if (!debugDraw || !debugDraw->HasCommands())
    {
        return;
    }

    const auto& D = renderer->GetDeferredForFrame();
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassDebugDraw);
        renderer->Transition(t.cl, D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        renderer->Transition(t.cl, D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        renderer->BindSceneColor(t.cl, Renderer::ClearMode::None, true);

        debugDraw->Render(renderer, t.cl, camera.GetViewMatrix(), camera.GetProjMatrix());
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_Tonemap(Renderer* renderer, RenderGraphPassContext ctx)
{
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassTonemap);
        const auto& D = renderer->GetDeferredForFrame();
        renderer->Transition(t.cl, D.scene.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.tonemap.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        renderer->Transition(t.cl, D.fxaa.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        auto h = renderer->GetRenderContextPool()->Acquire();
        auto& rc = h.ref();

        rc.table[0] = renderer->StageSrvUavTable({ D.sceneSRV }).gpu;
        rc.table[1] = renderer->StageSrvUavTable({ D.tonemapUAV }).gpu;
        const auto tonemapSamplers = std::array{ *SamplerManager::LinearClamp() };
        rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, tonemapSamplers);

        auto tonemapMaterial = resources_.GetTonemapMaterial();
        if (!tonemapMaterial)
        {
            renderer->EndThreadCommandList(t, ctx.batchIndex);
            return;
        }

        tonemapMaterial->Bind(t.cl, rc);
        constexpr UINT kGroupSize = 8;
        const UINT groupsX = (renderer->GetWidth() + kGroupSize - 1u) / kGroupSize;
        const UINT groupsY = (renderer->GetHeight() + kGroupSize - 1u) / kGroupSize;
        if (groupsX > 0 && groupsY > 0)
        {
            t.cl->Dispatch(groupsX, groupsY, 1);
        }

        renderer->UAVBarrier(t.cl, D.tonemap.Get());

        bool ranFxaa = false;
        auto fxaaMaterial = resources_.GetFxaaMaterial();
        const UINT fxaaCbSize = resources_.GetFxaaCBSizeBytes();
        if (fxaaMaterial && fxaaCbSize > 0 && groupsX > 0 && groupsY > 0)
        {
            renderer->Transition(t.cl, D.tonemap.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            auto cb = renderer->GetFrameResource()->AllocDynamic(fxaaCbSize, Renderer::kConstantBufferAlignment);
            const float width = static_cast<float>(renderer->GetWidth());
            const float height = static_cast<float>(renderer->GetHeight());
            const float2 invResolution = float2(width > 0.0f ? 1.0f / width : 0.0f, height > 0.0f ? 1.0f / height : 0.0f);
            // FXAA 3.11 tuning parameters. These map 1:1 with the reference shader controls:
            //   * subpix: linear blend between the original color and FXAA output. 1.0 reproduces the
            //             stock look; lower values bias towards the unfiltered image for extra
            //             sharpness.
            //   * edgeThreshold: relative luminance contrast required to trigger FXAA (default 1/8).
            //   * edgeThresholdMin: absolute minimum contrast to treat as an edge (default 1/24).
            const float subpix = 1.0f;
            const float edgeThreshold = 0.125f;
            const float edgeThresholdMin = 0.0416667f;

            FxaaPassConstants fxaaConstants{};
            fxaaConstants.invResolution = invResolution;
            fxaaConstants.subpix = subpix;
            fxaaConstants.edgeThreshold = edgeThreshold;
            fxaaConstants.edgeThresholdMin = edgeThresholdMin;

            resources_.WriteFxaaConstants(fxaaConstants, (uint8_t*)cb.cpu);

            auto fxaaCtx = renderer->GetRenderContextPool()->Acquire();
            auto& fxaaRC = fxaaCtx.ref();
            fxaaRC.cbv[0] = cb.gpu;
            fxaaRC.table[0] = renderer->StageSrvUavTable({ D.tonemapSRV }).gpu;
            fxaaRC.table[1] = renderer->StageSrvUavTable({ D.fxaaUAV }).gpu;
            const auto fxaaSamplers = std::array{ *SamplerManager::LinearClamp() };
            fxaaRC.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, fxaaSamplers);

            fxaaMaterial->Bind(t.cl, fxaaRC);
            t.cl->Dispatch(groupsX, groupsY, 1);

            renderer->UAVBarrier(t.cl, D.fxaa.Get());
            ranFxaa = true;
        }

        ID3D12Resource* const backbuffer = renderer->GetCurrentBackbuffer();
        ID3D12Resource* const resolveSource = ranFxaa ? D.fxaa.Get() : D.tonemap.Get();
        if (backbuffer && resolveSource)
        {
            renderer->Transition(t.cl, resolveSource, D3D12_RESOURCE_STATE_COPY_SOURCE);
            renderer->Transition(t.cl, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST);
            t.cl->CopyResource(backbuffer, resolveSource);
            renderer->Transition(t.cl, resolveSource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            renderer->Transition(t.cl, backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }

        renderer->Transition(t.cl, D.tonemap.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_Debug(Renderer* renderer, RenderGraphPassContext ctx)
{
    if (!debugTexMode_)
    {
        return;
    }
    const auto& D = renderer->GetDeferredForFrame();
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassDebug);
        renderer->RecordBindDefaultsNoClear(t.cl);

        auto h = renderer->GetRenderContextPool()->Acquire();
        auto& rc = h.ref();

        rc.table[0] = renderer->StageSrvUavTable({ D.shadowSRV }).gpu; // t0
        const auto debugSamplers = std::array{ *SamplerManager::LinearClamp() };
        rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, debugSamplers);

        auto debugMaterial = resources_.GetDebugMaterial();
        if (!debugMaterial)
        {
            renderer->EndThreadCommandList(t, ctx.batchIndex);
            return;
        }

        debugMaterial->Bind(t.cl, rc);
        t.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        t.cl->DrawInstanced(3, 1, 0, 0);
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_Overlay(Renderer* renderer, RenderGraphPassContext ctx)
{
    if (auto* tm = renderer->GetTextManager()) {
        auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        SetCommandListName(t.cl, ctx.pass);
        {
            GPU_SCOPE(t.cl, ProfilerScopes::kPassOverlay);
            renderer->RecordBindDefaultsNoClear(t.cl);
            if (showProfiler_)
            {
                Profiler::Get().EmitOverlay(tm, /*x=*/8, /*y=*/48, /*maxLines=*/20);
            }
            tm->Build(renderer, t.cl);
            tm->Draw(renderer, t.cl);
        }
        renderer->EndThreadCommandList(t, ctx.batchIndex);
    }
}

void Scene::Clear()
{
    resources_ = SceneResourceBootstrapper{};
    lightManager_.Reset();
    objects_.clear();
    camera_.GetView().queue.Clear();
    for (auto& view : cascadeViews_)
    {
        view.queue.Clear();
    }
    skyBox_.reset();
}
