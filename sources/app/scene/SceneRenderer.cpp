#include "app/scene/SceneRenderer.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <memory>
#include <utility>

#include "app/camera/Camera.h"
#include "app/Systems.h"
#include "rendering/debug/DebugDraw.h"
#include "rendering/core/ComputeDispatch.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/RenderGraph.h"
#include "rendering/core/RenderPass.h"
#include "rendering/renderables/RenderableObject.h"
#include "ocean/OceanSimulation.h"
#include "core/task/TaskSystem.h"
#include "text/TextManager.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "core/containers/inl_vector.h"

static void SetCommandListName(ID3D12GraphicsCommandList* cl, RenderPass pass)
{
    const auto nameW = RenderPassToWString(pass);
    if (!nameW.empty() && cl)
    {
        cl->SetName(nameW.data());
    }
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

void SceneRenderer::InitializeCommonResources(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    resources_.Initialize(renderer, uploadCmdList, uploadKeepAlive);
}

void SceneRenderer::FinalizeLevelLoad(Renderer* renderer,
    const std::vector<std::unique_ptr<RenderableObjectBase>>& objects,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
    Skybox* skybox)
{
    resources_.Finalize(renderer, objects, uploadCmdList, uploadKeepAlive, skybox);
}

void SceneRenderer::RefreshMaterialHandles(Renderer* renderer,
    const std::vector<std::unique_ptr<RenderableObjectBase>>& objects,
    Skybox* skybox)
{
    resources_.RefreshMaterialHandles(renderer, objects, skybox);
}

void SceneRenderer::Reset()
{
    resources_ = SceneResourceBootstrapper{};
    frame_ = nullptr;
}

void SceneRenderer::Render(Renderer* renderer, const SceneFrameData& frame)
{
    if (!renderer)
    {
        return;
    }

    frame_ = &frame;

    renderer->BeginSubmitTimeline();

    using MainRenderGraph = RenderGraph<kMainRenderGraphPassCount>;
    MainRenderGraph rg;
    auto pClear = rg.AddPass(RenderPass::Main_PrologueClear, {},
        [this, renderer](RenderGraphPassContext ctx) { CPU_SCOPE(ProfilerScopes::kPassPrologueClear); Pass_PrologueClear(renderer, ctx); });

    auto pCompute = rg.AddPass(RenderPass::Main_ObjectCompute, { pClear },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassObjectCompute);
            Pass_ObjectCompute(renderer, ctx);
        });

    auto pShoreDepth = rg.AddPass(RenderPass::Main_TerrainDepth, { pCompute },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassShoreDepth);
            OceanSimulation* oceanSim = Systems::GetOceanSimulation();
            const SceneView* shoreView = oceanSim ? &oceanSim->GetShoreDepthView() : nullptr;
            Pass_ShoreDepth(renderer, ctx, shoreView);
        });

    auto pShadow = rg.AddPass(RenderPass::Main_CSM, { pShoreDepth },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassCSM);
            Pass_CSM(renderer, ctx, *frame_->cascadeViews);
        });

    auto pSpotShadow = rg.AddPass(RenderPass::Main_SpotShadows, { pShadow },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassSpotShadow);
            Pass_SpotShadows(renderer, ctx, *frame_->spotShadowViews);
        });

    auto pGbuf = rg.AddPass(RenderPass::Main_GBuffer, { pSpotShadow },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassGBuffer);
            Pass_GBuffer(renderer, ctx, *frame_->camera, *frame_->mainView);
        });

    auto pLight = rg.AddPassMT(RenderPass::Main_Lighting, { pGbuf }, { pShadow },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassLighting);
            Pass_Lighting(renderer, ctx, *frame_->camera);
        });

    auto pSpotLights = rg.AddPassMT(RenderPass::Main_SpotLights, { pLight }, { pSpotShadow },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassSpotLights);
            Pass_SpotLights(renderer, ctx, *frame_->camera);
        });

    auto pPointLights = rg.AddPass(RenderPass::Main_PointLights, { pSpotLights },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassPointLights);
            Pass_PointLights(renderer, ctx, *frame_->camera);
        });

    auto pSky = rg.AddPass(RenderPass::Main_Skybox, { pPointLights },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassSkybox);
            Pass_Skybox(renderer, ctx, *frame_->camera);
        });

    auto pSSR = rg.AddPass(RenderPass::Main_SSR, { pSky },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassSSR);
            Pass_SSR(renderer, ctx, *frame_->camera);
        });

    auto pBlur = rg.AddPass(RenderPass::Main_SSRBlur, { pSSR },
        [this, renderer](RenderGraphPassContext ctx) { CPU_SCOPE(ProfilerScopes::kPassSSRBlur); Pass_SSR_Blur(renderer, ctx); });

    auto pCompose = rg.AddPass(RenderPass::Main_Compose, { pBlur },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassCompose);
            Pass_Compose(renderer, ctx, *frame_->camera);
        });

    auto pTransp = rg.AddPass(RenderPass::Main_Transparent, { pCompose },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassTransparent);
            Pass_Transparent(renderer, ctx, *frame_->camera, *frame_->mainView);
        });

    auto pDebugDraw = rg.AddPass(RenderPass::Main_DebugDraw, { pTransp },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassDebugDraw);
            Pass_DebugDraw(renderer, ctx, *frame_->camera);
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

    frame_ = nullptr;
}

void SceneRenderer::RenderObjectBatch(Renderer* renderer,
    const std::vector<RenderableObjectBase*>& objects,
    size_t batchIndex,
    const Camera& camera,
    bool useBundles,
    bool bindGbufOrScene,
    bool bindVelocity,
    size_t chunkSize)
{
    if (objects.empty()) {
        return;
    }

    //chunkSize = 16;

    auto& tasks = TaskSystem::Get();
    const size_t N = objects.size();

    auto renderJob = [renderer, &camera, &objects, useBundles, chunkSize, batchIndex, bindGbufOrScene, bindVelocity](std::size_t jobIndex)
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
                    if (bindVelocity)
                    {
                        const auto& D = renderer->GetDeferredForFrame();
                        renderer->Transition(t.cl, D.gbVelocity.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
                        renderer->BindSceneColorWithVelocity(t.cl, Renderer::ClearMode::None, true);
                    }
                    else
                    {
                        renderer->BindSceneColor(t.cl, Renderer::ClearMode::None, true);
                    }
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

void SceneRenderer::RenderShadowBatch(Renderer* renderer,
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

void SceneRenderer::Pass_PrologueClear(Renderer* r, RenderGraphPassContext ctx)
{
    auto t = r->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassPrologueClear);
        r->RecordBindAndClear(t.cl);
    }
    r->EndThreadCommandList(t, ctx.batchIndex);
}

void SceneRenderer::Pass_ObjectCompute(Renderer* renderer, RenderGraphPassContext ctx)
{
    if (!renderer || !frame_->objects || frame_->objects->empty())
    {
        return;
    }

    auto compute = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(compute.cl, ctx.pass);
    {
        GPU_SCOPE(compute.cl, ProfilerScopes::kPassObjectCompute);
        for (const auto& obj : *frame_->objects)
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

void SceneRenderer::Pass_ShoreDepth(Renderer* renderer, RenderGraphPassContext ctx,
    const SceneView* view)
{
    if (!renderer || !view)
    {
        return;
    }

    OceanSimulation* oceanSimulation = Systems::GetOceanSimulation();
    if (!oceanSimulation || !oceanSimulation->ShouldRenderShoreDepth())
    {
        return;
    }

    const auto& visibleBuckets = view->queue.VisibleBuckets();
    const auto& opaqueSimple = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueSimple)];
    const auto& opaqueComplex = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueComplex)];

    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassShoreDepth);
        ID3D12Resource* shoreDepth = oceanSimulation->GetShoreDepthResource();
        if (shoreDepth)
        {
            renderer->Transition(t.cl, shoreDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }

        const D3D12_CPU_DESCRIPTOR_HANDLE shoreDepthDsv = oceanSimulation->GetShoreDepthDsv();
        if (shoreDepthDsv.ptr == 0)
        {
            renderer->EndThreadCommandList(t, ctx.batchIndex);
            return;
        }

        t.cl->OMSetRenderTargets(0, nullptr, FALSE, &shoreDepthDsv);

        float width = static_cast<float>(oceanSimulation->GetShoreDepthWidth());
        float height = static_cast<float>(oceanSimulation->GetShoreDepthHeight());
        if (shoreDepth)
        {
            const auto desc = shoreDepth->GetDesc();
            width = static_cast<float>(desc.Width);
            height = static_cast<float>(desc.Height);
        }

        D3D12_VIEWPORT vp{ 0.0f, 0.0f, width, height, 0.0f, 1.0f };
        D3D12_RECT sc{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
        t.cl->RSSetViewports(1, &vp);
        t.cl->RSSetScissorRects(1, &sc);

        t.cl->ClearDepthStencilView(shoreDepthDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        for (auto* obj : opaqueSimple)
        {
            if (obj)
            {
                obj->RenderShadow(renderer, t.cl, view->view, view->proj);
            }
        }

        for (auto* obj : opaqueComplex)
        {
            if (obj)
            {
                obj->RenderShadow(renderer, t.cl, view->view, view->proj);
            }
        }

        if (shoreDepth)
        {
            renderer->Transition(t.cl, shoreDepth,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void SceneRenderer::Pass_CSM(Renderer* renderer, RenderGraphPassContext ctx,
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
void SceneRenderer::Pass_SpotShadows(Renderer* renderer, RenderGraphPassContext ctx,
    const std::array<SceneView, LightManager::kMaxSpotLights>& spotViews)
{
    if (!renderer)
    {
        return;
    }

    const size_t availableLights = frame_->lightManager->GetSpotLightCount();
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

void SceneRenderer::Pass_GBuffer(Renderer* renderer, RenderGraphPassContext ctx,
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
            renderer->Transition(driver.cl, D.gbVelocity.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
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
            RenderObjectBatch(renderer, opaqueSimple, sub.batchIndex, camera, /*useBundles=*/true, true, true, 32);
        }
        });

    // 1.3 Opaque complex → direct command list, no clears
    rgGB.AddPass(RenderPass::GBuffer_OpaqueComplex, {}, [this, renderer, &camera, &mainView](RenderGraphPassContext sub) {
        const auto& visibleBuckets = mainView.queue.VisibleBuckets();
        const auto& opaqueComplex = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueComplex)];
        if (!opaqueComplex.empty())
        {
            RenderObjectBatch(renderer, opaqueComplex, sub.batchIndex, camera, /*useBundles=*/false, true, true, 32);
        }
        });

    rgGB.Execute(renderer);
}

void SceneRenderer::Pass_Lighting(Renderer* renderer, RenderGraphPassContext ctx,
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
        renderer->Transition(t.cl, D.gbVelocity.Get(), srvState);
        renderer->Transition(t.cl, D.depth.Get(), srvState);
        renderer->Transition(t.cl, D.shadow.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        const DirectionalLight& dirLight = *frame_->dirLight;
        LightingPassConstants constants{};
        const mat4& view = camera.GetViewMatrix();
        const mat4& invView = camera.GetInvViewMatrix();
        const mat4& invProj = camera.GetInvProjMatrix();
        const float3 camDir = camera.GetDirection();
        constants.sunDir = dirLight.GetDirection();
        constants.ambient = dirLight.GetAmbient();
        constants.lightRgb = dirLight.GetColor();
        constants.exposure = dirLight.GetExposure();
        constants.camPos = camera.GetPosition();
        constants.camDir = camDir;
        constants.invView = invView;
        constants.invProj = invProj;
        const SceneFrameData::CascadeData& cascades = frame_->cascades;
        for (size_t i = 0; i < constants.lightViewProj.size(); ++i)
        {
            constants.lightViewProj[i] = cascades.lightView[i] * cascades.lightProj[i];
            constants.cascadeScaleBias[i] = float4(cascades.atlasScale[i].x, cascades.atlasScale[i].y, cascades.atlasBias[i].x, cascades.atlasBias[i].y);
        }
        constants.cascadeSplits = float4(cascades.splitsVS[0], cascades.splitsVS[1], cascades.splitsVS[2], cascades.splitsVS[3]);
        const float shadowRes = static_cast<float>(renderer->GetDeferredForFrame().shadowRes);
        constants.shadowAtlasSize = float2(shadowRes, shadowRes);
        constants.shadowBiasNDC = float4(cascades.depthBiasNDC[0], cascades.depthBiasNDC[1], cascades.depthBiasNDC[2], cascades.depthBiasNDC[3]);
        constants.normalBiasWS = float4(cascades.normalBiasWS[0], cascades.normalBiasWS[1], cascades.normalBiasWS[2], cascades.normalBiasWS[3]);
        const float width = static_cast<float>(std::max(renderer->GetRenderWidth(), 1u));
        const float height = static_cast<float>(std::max(renderer->GetRenderHeight(), 1u));
        constants.screenSize = float2(width, height);
        constants.invScreenSize = float2(width > 0.f ? (1.0f / width) : 0.0f, height > 0.f ? (1.0f / height) : 0.0f);

        const auto samplerDescs = std::array{ *SamplerManager::PointClamp(), *SamplerManager::ComparisonLinearClamp() };
        RecordComputeDispatch(renderer, t.cl, lighting.get(), cbSize,
            [&](uint8_t* dest) { resources_.WriteLightingConstants(constants, dest); },
            { D.gbSRV[0], D.gbSRV[1], D.gbSRV[2], D.gbSRV[3], D.depthSRV, D.shadowSRV },
            { D.lightUAV },
            renderer->GetSamplerManager()->GetTable(renderer, samplerDescs),
            renderer->GetRenderWidth(), renderer->GetRenderHeight(),
            D.light.Get());
    }
    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void SceneRenderer::Pass_SpotLights(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera)
{
    LightManager& lightManager = *frame_->lightManager;
    const size_t spotLightCount = lightManager.GetSpotLightCount();
    if (spotLightCount == 0)
    {
        return;
    }

    lightManager.EnsureSpotLightBuffer(renderer, spotLightCount);
    auto* spotLightBufferCPU = lightManager.GetSpotLightBufferCPU();
    const D3D12_CPU_DESCRIPTOR_HANDLE spotLightSrvHandle = lightManager.GetSpotLightSrv();
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
        renderer->Transition(t.cl, D.gbVelocity.Get(), srvState);
        renderer->Transition(t.cl, D.depth.Get(), srvState);
        renderer->Transition(t.cl, D.spotShadow.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        const auto& spotLights = lightManager.SpotLights();
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

        SpotLightPassConstants constants{};
        constants.invView = camera.GetInvViewMatrix();
        constants.invProj = camera.GetInvProjMatrix();
        constants.camPos = camera.GetPosition();
        const float width = static_cast<float>(std::max(renderer->GetRenderWidth(), 1u));
        const float height = static_cast<float>(std::max(renderer->GetRenderHeight(), 1u));
        constants.screenSize = float2(width, height);
        constants.invScreenSize = float2(width > 0.f ? (1.0f / width) : 0.0f, height > 0.f ? (1.0f / height) : 0.0f);
        const float shadowRes = static_cast<float>(renderer->GetDeferredForFrame().spotShadowRes);
        const float invRes = shadowRes > 0.0f ? 1.0f / shadowRes : 0.0f;
        constants.invShadowSize = float2(invRes, invRes);
        constants.lightCount = static_cast<uint32_t>(spotLightCount);

        const auto samplerDescs = std::array{ *SamplerManager::LinearClamp(), *SamplerManager::PointClamp(), *SamplerManager::ComparisonLinearClamp() };
        RecordComputeDispatch(renderer, t.cl, spotMaterial.get(), cbSize,
            [&](uint8_t* dest) { resources_.WriteSpotLightConstants(constants, dest); },
            { D.gbSRV[0], D.gbSRV[1], D.gbSRV[2], D.gbSRV[3], D.depthSRV, D.spotShadowSRV, spotLightSrvHandle },
            { D.lightUAV },
            renderer->GetSamplerManager()->GetTable(renderer, samplerDescs),
            renderer->GetRenderWidth(), renderer->GetRenderHeight(),
            D.light.Get());
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void SceneRenderer::Pass_PointLights(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera)
{
    LightManager& lightManager = *frame_->lightManager;
    auto& pointLights = lightManager.PointLights();
    if (pointLights.empty()) { return; }

    lightManager.EnsurePointLightBuffer(renderer, pointLights.size());
    auto* pointLightBufferCPU = lightManager.GetPointLightBufferCPU();
    const D3D12_CPU_DESCRIPTOR_HANDLE pointLightSrvHandle = lightManager.GetPointLightSrv();
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
        renderer->Transition(t.cl, D.gbVelocity.Get(), srvState);
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

        PointLightPassConstants constants{};
        constants.invView = camera.GetInvViewMatrix();
        constants.invProj = camera.GetInvProjMatrix();
        constants.camPos = camera.GetPosition();
        const float width = static_cast<float>(std::max(renderer->GetRenderWidth(), 1u));
        const float height = static_cast<float>(std::max(renderer->GetRenderHeight(), 1u));
        constants.screenSize = float2(width, height);
        constants.invScreenSize = float2(width > 0.f ? (1.0f / width) : 0.0f, height > 0.f ? (1.0f / height) : 0.0f);
        constants.lightCount = static_cast<uint32_t>(pointLights.size());

        const auto samplerDescs = std::array{ *SamplerManager::LinearClamp(), *SamplerManager::PointClamp() };
        RecordComputeDispatch(renderer, t.cl, pointMaterial.get(), cbSize,
            [&](uint8_t* dest) { resources_.WritePointLightConstants(constants, dest); },
            { D.gbSRV[0], D.gbSRV[1], D.gbSRV[2], D.gbSRV[3], D.depthSRV, pointLightSrvHandle },
            { D.lightUAV },
            renderer->GetSamplerManager()->GetTable(renderer, samplerDescs),
            renderer->GetRenderWidth(), renderer->GetRenderHeight(),
            D.light.Get());
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void SceneRenderer::Pass_Skybox(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera)
{
    if (!frame_->skybox) { return; }
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSkybox);

        const auto& D = renderer->GetDeferredForFrame();
        renderer->Transition(t.cl, D.light.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        renderer->Transition(t.cl, D.gbVelocity.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        renderer->Transition(t.cl, D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_READ);

        // RTVs = Light + Velocity, DSV = GBuffer Depth (read-only), no clears
        renderer->BindLightTargetWithVelocity(t.cl, Renderer::ClearMode::None, true);

        frame_->skybox->Render(renderer, t.cl, camera);
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void SceneRenderer::Pass_SSR(Renderer* renderer, RenderGraphPassContext ctx,
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
        constants.screenSize = float2(static_cast<float>(renderer->GetRenderWidth()), static_cast<float>(renderer->GetRenderHeight()));
        constants.invScreenSize = float2(
            constants.screenSize.x > 0.0f ? 1.0f / constants.screenSize.x : 0.0f,
            constants.screenSize.y > 0.0f ? 1.0f / constants.screenSize.y : 0.0f);
        constants.technique = static_cast<uint32_t>(frame_->settings.ssrTechnique);

        const auto samplerDescs = std::array{ *SamplerManager::LinearClamp(), *SamplerManager::PointClamp() };
        RecordComputeDispatch(renderer, t.cl, ssrMaterial.get(), cbSize,
            [&](uint8_t* dest) { resources_.WriteSsrConstants(constants, dest); },
            { D.lightSRV, D.gbSRV[1], D.depthSRV }, // t0 Light, t1 GB1, t2 Depth
            { D.ssrUAV },                           // u0 output
            renderer->GetSamplerManager()->GetTable(renderer, samplerDescs),
            renderer->GetSsrTextureWidth(), renderer->GetSsrTextureHeight(),
            D.ssr.Get());
    }
    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void SceneRenderer::Pass_SSR_Blur(Renderer* renderer, RenderGraphPassContext ctx)
{
    //return;
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSSRBlur);
        const auto& D = renderer->GetDeferredForFrame();
        const UINT ssrWidth = renderer->GetSsrTextureWidth();
        const UINT ssrHeight = renderer->GetSsrTextureHeight();

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

        const auto samplerDescs = std::array{ *SamplerManager::LinearClamp() };
        const D3D12_GPU_DESCRIPTOR_HANDLE samplerTable = renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);

        const float invSsrWidth = ssrWidth > 0 ? (1.0f / static_cast<float>(ssrWidth)) : 0.0f;
        BlurPassConstants blurConstants{};
        blurConstants.direction = float2(invSsrWidth, 0.0f);
        blurConstants.radius = 1.0f;
        RecordComputeDispatch(renderer, t.cl, blurMaterial.get(), cbSize,
            [&](uint8_t* dest) { resources_.WriteBlurConstants(blurConstants, dest); },
            { D.ssrSRV }, { D.ssrBlurUAV }, samplerTable,
            ssrWidth, ssrHeight,
            D.ssrBlur.Get());

        // Vertical pass
        renderer->Transition(t.cl, D.ssrBlur.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.ssr.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        const float invSsrHeight = ssrHeight > 0 ? (1.0f / static_cast<float>(ssrHeight)) : 0.0f;
        blurConstants.direction = float2(0.0f, invSsrHeight);
        RecordComputeDispatch(renderer, t.cl, blurMaterial.get(), cbSize,
            [&](uint8_t* dest) { resources_.WriteBlurConstants(blurConstants, dest); },
            { D.ssrBlurSRV }, { D.ssrUAV }, samplerTable,
            ssrWidth, ssrHeight,
            D.ssr.Get());
    }
    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void SceneRenderer::Pass_Compose(Renderer* renderer, RenderGraphPassContext ctx,
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

        const float width = static_cast<float>(renderer->GetRenderWidth());
        const float height = static_cast<float>(renderer->GetRenderHeight());
        if (width <= 0.0f || height <= 0.0f)
        {
            renderer->Transition(t.cl, D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            renderer->EndThreadCommandList(t, ctx.batchIndex);
            return;
        }

        auto composeMaterial = resources_.GetComposeMaterial();
        const UINT cbSize = resources_.GetComposeCBSizeBytes();
        Skybox* skybox = frame_->skybox;
        if (!composeMaterial || cbSize == 0 || !skybox)
        {
            renderer->Transition(t.cl, D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            renderer->EndThreadCommandList(t, ctx.batchIndex);
            return;
        }

        ComposePassConstants constants{};
        constants.invView = camera.GetInvViewMatrix();
        constants.invProj = camera.GetInvProjMatrix();
        constants.skyboxIntensity = skybox->GetExposure();
        constants.camPos = camera.GetPosition();
        constants.screenSize = float2(width, height);
        constants.invScreenSize = float2(1.0f / width, 1.0f / height);

        const auto samplerDescs = std::array{ *SamplerManager::LinearClamp(), *SamplerManager::PointClamp() };
        RecordComputeDispatch(renderer, t.cl, composeMaterial.get(), cbSize,
            [&](uint8_t* dest) { resources_.WriteComposeConstants(constants, dest); },
            { D.lightSRV, D.gbSRV[2], D.gbSRV[0], D.gbSRV[1], D.depthSRV, skybox->GetTex()->GetSRVCPU(), D.ssrSRV },
            { D.sceneUAV },
            renderer->GetSamplerManager()->GetTable(renderer, samplerDescs),
            renderer->GetRenderWidth(), renderer->GetRenderHeight(),
            D.scene.Get());

        renderer->Transition(t.cl, D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void SceneRenderer::Pass_Transparent(Renderer* renderer, RenderGraphPassContext ctx,
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
            renderer->Transition(driver.cl, D.gbVelocity.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            renderer->Transition(driver.cl, D.dlssBias.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            const float clearBias[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
            driver.cl->ClearRenderTargetView(D.dlssBiasRTV, clearBias, 0, nullptr);
            renderer->BindSceneColorWithVelocity(driver.cl, Renderer::ClearMode::None, true);
        }
        renderer->RegisterPassDriver(driver.cl, sub.batchIndex);
        });

    rgTr.AddPass(RenderPass::Transparent_Simple, {}, [this, renderer, &camera, &mainView](RenderGraphPassContext sub) {
        const auto& visibleBuckets = mainView.queue.VisibleBuckets();
        const auto& transparentSimple = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::TransparentSimple)];
        if (!transparentSimple.empty())
        {
            RenderObjectBatch(renderer, transparentSimple, sub.batchIndex, camera, /*useBundles=*/true, false, true, 32);
        }
        });

    rgTr.AddPass(RenderPass::Transparent_Complex, {}, [this, renderer, &camera, &mainView](RenderGraphPassContext sub) {
        const auto& visibleBuckets = mainView.queue.VisibleBuckets();
        const auto& transparentComplex = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::TransparentComplex)];
        if (!transparentComplex.empty())
        {
            RenderObjectBatch(renderer, transparentComplex, sub.batchIndex, camera, /*useBundles=*/false, false, true, 32);
        }
        });

    rgTr.Execute(renderer);
}

void SceneRenderer::Pass_DebugDraw(Renderer* renderer, RenderGraphPassContext ctx, const Camera& camera)
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

void SceneRenderer::Pass_Tonemap(Renderer* renderer, RenderGraphPassContext ctx)
{
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassTonemap);
        const auto& D = renderer->GetDeferredForFrame();
        bool ranDlss = false;
        if (renderer->IsDlssActive())
        {
            ranDlss = renderer->EvaluateDLSS(t.cl);
            if (ranDlss)
            {
                renderer->Transition(t.cl, D.dlssOutput.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
        }
        if (!ranDlss)
        {
            renderer->Transition(t.cl, D.scene.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        renderer->Transition(t.cl, D.tonemap.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        renderer->Transition(t.cl, D.fxaa.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        renderer->BindDescriptorHeaps(t.cl);

        auto tonemapMaterial = resources_.GetTonemapMaterial();
        if (!tonemapMaterial)
        {
            renderer->EndThreadCommandList(t, ctx.batchIndex);
            return;
        }

        const D3D12_CPU_DESCRIPTOR_HANDLE tonemapSrc = ranDlss ? D.dlssOutputSRV : D.sceneSRV;
        const auto tonemapSamplers = std::array{ *SamplerManager::LinearClamp() };
        const D3D12_GPU_DESCRIPTOR_HANDLE samplerTable = renderer->GetSamplerManager()->GetTable(renderer, tonemapSamplers);
        RecordComputeDispatch(renderer, t.cl, tonemapMaterial.get(),
            { tonemapSrc }, { D.tonemapUAV }, samplerTable,
            renderer->GetWidth(), renderer->GetHeight(),
            D.tonemap.Get());

        bool ranFxaa = false;
        auto fxaaMaterial = resources_.GetFxaaMaterial();
        const UINT fxaaCbSize = resources_.GetFxaaCBSizeBytes();
        if (fxaaMaterial && fxaaCbSize > 0 && renderer->GetWidth() > 0 && renderer->GetHeight() > 0 && frame_->settings.doFxaa)
        {
            renderer->Transition(t.cl, D.tonemap.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

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

            RecordComputeDispatch(renderer, t.cl, fxaaMaterial.get(), fxaaCbSize,
                [&](uint8_t* dest) { resources_.WriteFxaaConstants(fxaaConstants, dest); },
                { D.tonemapSRV }, { D.fxaaUAV }, samplerTable,
                renderer->GetWidth(), renderer->GetHeight(),
                D.fxaa.Get());
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

void SceneRenderer::Pass_Debug(Renderer* renderer, RenderGraphPassContext ctx)
{
    if (!frame_->settings.debugTexMode)
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
        //rc.table[0] = renderer->StageSrvUavTable({ D.gbSRV[3] }).gpu; // t0
        //rc.table[0] = renderer->StageSrvUavTable({ D.dlssBiasSRV }).gpu; // t0
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

void SceneRenderer::Pass_Overlay(Renderer* renderer, RenderGraphPassContext ctx)
{
    if (auto* tm = renderer->GetTextManager()) {
        auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        SetCommandListName(t.cl, ctx.pass);
        {
            GPU_SCOPE(t.cl, ProfilerScopes::kPassOverlay);
            renderer->RecordBindDefaultsNoClear(t.cl);
            if (frame_->settings.showProfiler)
            {
                Profiler::Get().EmitOverlay(tm, /*x=*/16, /*y=*/64, /*maxLines=*/20);
            }
            tm->Build(renderer, t.cl);
            tm->Draw(renderer, t.cl);
        }
        renderer->EndThreadCommandList(t, ctx.batchIndex);
    }
}
