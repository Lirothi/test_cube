// R2 (docs/scene_renderer_refactor_plan.md): object submission: the draw walks and the two passes that own an inner render graph.
//
// Moved out of SceneRenderer.cpp VERBATIM — same class, same methods, one subject per
// file. The include block is the one the original file carries; trimming it per TU is
// deliberately NOT part of this step, because an unused include is not a defect and a
// trimmed one is a second thing to review.

#include "app/scene/SceneRenderer.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <fstream>
#include <DirectXPackedVector.h> // P8C-2o: the kernel is FP16 on disk
#include <memory>
#include <utility>
#include <vector>

#include "rendering/core/RenderConstants.h"

#include "app/camera/Camera.h"
#include "app/Systems.h"
#include "rendering/debug/DebugDraw.h"
#include "core/Helpers.h" // GetTimeSeconds (P2 adaptation delta)
#include "core/diagnostics/DiagPaths.h" // P8C-2o kernel survey verdict
#include "rendering/core/ComputeDispatch.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/RenderGraph.h"
#include "rendering/core/RenderPass.h"
#include "rendering/renderables/GBufferRenderable.h" // per-slot RT materials (B3 follow-up)
#include "rendering/renderables/RenderableObject.h"
#include "rendering/shadows/ShadowGpuData.h"
#include "rendering/shadows/VirtualShadowMap.h"
#include "ocean/OceanSimulation.h"
#include "rendering/core/PhotographicSettings.h" // P16.1 pre-exposure
#include "rendering/core/UploadBatch.h" // the ghost sprite sheet is uploaded once, lazily
#include "ocean/OceanRenderable.h" // caustics: flipbook SRV + water level + shared clock
#include "vfx/WindState.h" // W3: fold WindState into the gbuffer per-view CB
#include "core/task/TaskSystem.h"
#include "text/TextManager.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "core/containers/inl_vector.h"

// R1 (docs/scene_renderer_refactor_plan.md): the helpers every pass body uses moved to an
// INTERNAL header, verbatim, so the bodies can be split across translation units. The
// using-directive keeps every call site spelled exactly as it was.
#include "app/scene/SceneRenderInternal.h"
#include "rendering/visibility/OcclusionQueries.h" // occlusion plan S3a: Pass_OcclusionQueries
using namespace scene_internal;

// ---- RenderObjectBatch ----
void SceneRenderer::RenderObjectBatch(Renderer* renderer,
    const std::vector<RenderableObjectBase*>& objects,
    size_t batchIndex,
    const Camera& camera,
    bool useBundles,
    bool bindGbufOrScene,
    bool bindVelocity,
    size_t chunkSize,
    D3D12_GPU_VIRTUAL_ADDRESS viewCB,
    uint32_t localOrderBase)
{
    if (objects.empty()) {
        return;
    }

    //chunkSize = 16;

    auto& tasks = TaskSystem::Get();
    const size_t N = objects.size();

    // Barrier plan step 5: carry the pass's transition log onto the fan-out workers, so the
    // comparator observes what they record. Captured HERE, on the pass thread, where the log is
    // installed; without it these passes look silent because they are unobserved, not correct.
    Renderer::TransitionLog* const cmpLog = Renderer::CurrentThreadTransitionLog();
    // Step 7: the compiled barriers travel with the log — a fan-out worker must emit its
    // pass's barriers too, or the flip loses exactly the passes that record in parallel.
    Renderer::CompiledBarriers* const cmpBarriers = Renderer::CurrentThreadCompiledBarriers();
    auto renderJob = [renderer, &camera, &objects, useBundles, chunkSize, batchIndex, bindGbufOrScene, bindVelocity, viewCB, localOrderBase, cmpLog, cmpBarriers](std::size_t jobIndex)
    {
        Renderer::TransitionLogScope cmpScope(cmpLog);
        Renderer::CompiledBarrierScope cmpBarrierScope(cmpBarriers);
        CPU_SCOPE(ProfilerScopes::kRenderObjectBatchAsync);
        const size_t begin = jobIndex * chunkSize;
        const size_t end = std::min(begin + chunkSize, objects.size());

        if (useBundles) {
            auto b = renderer->BeginThreadCommandBundle(nullptr);
            for (size_t i = begin; i < end; ++i) {
                if (auto* obj = objects[i]) {
                    obj->Render(renderer, b.cl, camera, viewCB);
                }
            }
            // Base + chunk index is the deterministic submit order within the batch's
            // bundle namespace (transparents must blend in sorted-queue order).
            renderer->EndThreadCommandBundle(b, batchIndex,
                localOrderBase + static_cast<uint32_t>(jobIndex));
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
#if WITH_EDITOR
                        renderer->Transition(t.cl, D.objectID.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
#endif
                        renderer->BindSceneColorWithVelocity(t.cl, Renderer::ClearMode::None, true);
                    }
                    else
                    {
                        renderer->BindSceneColor(t.cl, Renderer::ClearMode::None, true);
                    }
                }

                for (size_t i = begin; i < end; ++i) {
                    if (auto* obj = objects[i]) {
                        obj->Render(renderer, t.cl, camera, viewCB);
                    }
                }
            }
            // Base + chunk index is the deterministic submit order within the batch's
            // direct namespace (transparents must blend in sorted-queue order).
            renderer->EndThreadCommandList(t, batchIndex,
                localOrderBase + static_cast<uint32_t>(jobIndex));
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

// ---- PrepareOpaqueDrawStates + Pass_ObjectCompute ----
void SceneRenderer::PrepareOpaqueDrawStates(RenderGraphPassContext& p, const SceneView* views,
                                            size_t viewCount, bool indirect)
{
    if (!frame_ || !views || viewCount == 0) { return; }
    ShadowGpuData* const shadowGpu = frame_->shadowGpu;

    // The VISIBLE buckets of the pass's own views, not every opaque object. This used to be the
    // whole object list on the reasoning that "a culled object's redundant barrier is free" —
    // true under the tracker, exactly backwards under the flip. A compiled barrier that no body
    // ever asks for stalls the rest of the pass's points (a request may only match the CURRENT
    // one) and leaves the compile's model one transition ahead of the GPU. Measured: the GI cloud
    // registered here while the GPU-driven path drew it through the cull instead, which put
    // D3D12 error 527 on the SpotShadow/PointShadow atlases across half the frame's lists.
    tc::inl_vector<const RenderableObjectBase*, 16> registered;
    auto prepareOne = [&](RenderableObjectBase* obj) {
        if (!obj) { return; }
        // The bodies' own gate (Pass_CSM / Pass_SpotShadows / Pass_PointShadows): with GPU-driven
        // shadows on, only the GPU-instanced casters the GI fold did NOT take are drawn here —
        // everything else casts through the indirect cull and its RenderShadow is never called.
        const bool gpuInstanced = obj->IsGpuInstancedCaster();
        if (indirect && (!gpuInstanced || shadowGpu->IsGiFoldedActive(obj))) { return; }
        if (gpuInstanced)
        {
            // The only kind that registers PER-OBJECT state (its instance buffer), so the only
            // kind worth de-duplicating across views against kResourceUsesPerPassBudget. Everything
            // else's PrepareRender is empty, so repeats there cost nothing.
            for (const RenderableObjectBase* seen : registered) { if (seen == obj) { return; } }
            if (registered.size() < registered.capacity()) { registered.push_back(obj); }
        }
        obj->PrepareRender(p);
    };

    for (size_t v = 0; v < viewCount; ++v)
    {
        const auto& visibleBuckets = views[v].queue.VisibleBuckets();
        for (auto* obj : visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueSimple)])  { prepareOne(obj); }
        for (auto* obj : visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueComplex)]) { prepareOne(obj); }
    }
}

void SceneRenderer::Pass_ObjectCompute(Renderer* renderer, RenderGraphPassContext ctx,
    const ObjectComputeList& objects, Profiler::ScopeNameKey gpuScope)
{
    // pass-flow S7b: `objects` is what the builder declared for — no second walk of the scene and
    // no second copy of the "does this object's compute record anything" test.
    auto compute = ctx.BeginCL();
    SetCommandListName(compute.cl, ctx.pass);
    {
        // Step 9: the scope NAME comes from the caller. Both halves of the split run this same
        // body, and a hard-coded name would put them on one trace row — which is exactly what the
        // first cut did: two "Pass_ObjectCompute" events per frame and no GpuInstanceCompute at all.
        GPU_SCOPE(compute.cl, gpuScope);
        for (RenderableObjectBase* obj : objects)
        {
            obj->ExecuteCompute(renderer, compute.cl);
        }
    }

    ctx.EndCL(compute);
}

// ---- Pass_GBuffer (+ its inner graph) ----
void SceneRenderer::Pass_GBuffer(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera, const SceneView& mainView, std::uint32_t bindPoint)
{
    const auto& D = renderer->GetDeferredForFrame();

    // Shared per-view CB (b1) for every opaque object in this pass.
    const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildGBufferViewCB(renderer, camera, frame_->wind);

    // pass-flow S7d: the inner driver DECLARES NOTHING. Its states are the outer pass's — declared
    // once by the Main_GBuffer builder — and it emits that point as a marker. The inner graph runs
    // its bodies inline on this thread, which is where the outer pass's compiled barriers are
    // installed, so the marker resolves to exactly those barriers.
    RenderGraph<kGBufferRenderGraphPassCount> rgGB(ctx.batchIndex);
    const size_t pDriver = rgGB.AddPass(RenderPass::GBuffer_Driver, {},
        [this, renderer, bindPoint](RenderGraphPassContext sub) {
        auto driver = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        SetCommandListName(driver.cl, sub.pass);
        {
            GPU_SCOPE(driver.cl, ProfilerScopes::kGBufferDriver);
            renderer->EmitPoint(driver.cl, bindPoint);
            renderer->BindGBuffer(driver.cl, Renderer::ClearMode::ColorDepth);
        }
        renderer->RegisterPassDriver(driver.cl, sub.batchIndex);
        });

    // 1.2 Opaque simple → bundles
    const size_t pOpaqueSimple = rgGB.AddPass(RenderPass::GBuffer_OpaqueSimple, { pDriver }, [this, renderer, &camera, &mainView, viewCB](RenderGraphPassContext sub) {
        const auto& visibleBuckets = mainView.queue.VisibleBuckets();
        const auto& opaqueSimple = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueSimple)];
        if (!opaqueSimple.empty())
        {
            // Auto-instancing leaves one heavyweight object per mesh/material run. Small chunks
            // let the three palm species record concurrently without paying one bundle per tiny
            // terrain object. localOrder preserves deterministic execution order.
            RenderObjectBatch(renderer, opaqueSimple, sub.batchIndex, camera, /*useBundles=*/true, true, true, 2, viewCB);
        }
        });

    // 1.3 Opaque complex → direct command list, no clears
    const size_t pOpaqueComplex = rgGB.AddPass(RenderPass::GBuffer_OpaqueComplex, { pDriver }, [this, renderer, &camera, &mainView, viewCB](RenderGraphPassContext sub) {
        const auto& visibleBuckets = mainView.queue.VisibleBuckets();
        const auto& opaqueComplex = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueComplex)];
        if (!opaqueComplex.empty())
        {
            RenderObjectBatch(renderer, opaqueComplex, sub.batchIndex, camera, /*useBundles=*/false, true, true, 32, viewCB);
        }
        });

#if WITH_EDITOR
    if (frame_->selectedEditorObjectCount != 0)
    {
        RenderGraph<kGBufferRenderGraphPassCount>::DependencyList selectedDeps;
        selectedDeps.push_back(pOpaqueSimple);
        selectedDeps.push_back(pOpaqueComplex);
        rgGB.AddPass(RenderPass::GBuffer_Selected, selectedDeps, [this, renderer, &camera, &mainView](RenderGraphPassContext sub) {
            auto material = resources_.GetSelectionStencilMaterial();
            if (!frame_->objects || !material)
            {
                return;
            }

            auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
            SetCommandListName(t.cl, sub.pass);
            {
                GPU_SCOPE(t.cl, ProfilerScopes::kRenderObjectBatchGpu);

                const auto& D = renderer->GetDeferredForFrame();
                t.cl->OMSetRenderTargets(0, nullptr, FALSE, &D.dsv);

                const D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(renderer->GetRenderWidth()), static_cast<float>(renderer->GetRenderHeight()), 0.0f, 1.0f };
                const D3D12_RECT sr{ 0, 0, static_cast<LONG>(renderer->GetRenderWidth()), static_cast<LONG>(renderer->GetRenderHeight()) };
                t.cl->RSSetViewports(1, &vp);
                t.cl->RSSetScissorRects(1, &sr);

                t.cl->OMSetStencilRef(kSelectionStencilBit);
                for (const std::unique_ptr<RenderableObjectBase>& owned : *frame_->objects)
                {
                    RenderableObjectBase* object = owned.get();
                    if (object && ShouldRenderSelectionStencil(*frame_, mainView, *object, false))
                    {
                        object->RenderSelectionStencil(renderer, t.cl, material.get(), camera);
                    }
                }
                t.cl->OMSetStencilRef(0);
            }
            renderer->EndThreadCommandList(t, sub.batchIndex, kSelectionStencilGBufferLocalOrder);
            });
    }
#endif

    rgGB.Execute(renderer);
}

// The inspector preview. See shaders/debug_preview_cs.hlsl for why this pass exists at all:
// ImGui can only multiply an image by an 8-bit tint, so brightening has to happen before ImGui.

// ---- Pass_Transparent (+ its inner graph) ----
void SceneRenderer::Pass_Transparent(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera, const SceneView& mainView, const TransparentPoints& pts)
{
    // Shared per-view/per-frame CB (b1) for every transparent object in this pass.
    const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildGlassViewCB(renderer, camera, *frame_, decisions_.glassRefl);

    // Step 21: publish the VSM page-table + pool SRVs (t9/t10) for the glass draws, which lack frame
    // access. Valid once a level is loaded; the pool/page-table are already SRV here (the light
    // passes declared them). glass.hlsl only reads them when vsmParams.x != 0.
    if (frame_->vsm && frame_->vsm->IsAllocated())
    {
        renderer->SetVsmShadowSrvs(frame_->vsm->PageTableSrv(), frame_->vsm->PagePoolSrv());
    }
    else
    {
        renderer->SetVsmShadowSrvs({}, {});
    }

    RenderGraph<kTransparentRenderGraphPassCount> rgTr(ctx.batchIndex);

    // Driver: RTV=SceneColor, DSV=GBuffer. No clear. Do NOT close the driver list.
    // pass-flow S7d: the driver names no resource and no state. Its four points were declared by
    // the Main_Transparent builder from the same decisions it captured here, and the inner graph
    // runs inline on this thread, so the markers resolve to the outer pass's compiled barriers.
    rgTr.AddPass(RenderPass::Transparent_Driver, {}, [this, renderer, &camera, pts](RenderGraphPassContext sub) {
        auto driver = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        SetCommandListName(driver.cl, sub.pass);
        {
            GPU_SCOPE(driver.cl, ProfilerScopes::kTransparentDriver);
            const auto& D = renderer->GetDeferredForFrame();
            // 1. Snapshot depth + opaque colour for the refraction/reflection reads.
            renderer->EmitPoint(driver.cl, pts.copy);
            if (pts.copyDepth)
            {
                driver.cl->CopyResource(D.depthCopy.Get(), D.depth.Get());
            }
            if (pts.copyScene)
            {
                driver.cl->CopyResource(D.sceneOpaque.Get(), D.scene.Get());
            }

            // 2/3. The ocean reflection compute and the pixel-readable hand-off it always ends on.
            RecordOceanReflection(renderer, driver.cl, camera, pts);

            // 4. Rebind the forward targets.
            renderer->EmitPoint(driver.cl, pts.rebind);
            renderer->BindSceneColorWithVelocity(driver.cl, Renderer::ClearMode::None, true);
        }
        renderer->RegisterPassDriver(driver.cl, sub.batchIndex);
        });

    // Draw the COMPLEX bucket (ocean, glass) BEFORE the SIMPLE bucket (particles). Both buckets
    // must use direct lists here: bundles are executed inside the pass driver before every direct
    // list, regardless of render-graph dependencies, which made the direct-list ocean composite
    // over particle bundles. Reserve the first local-order range for complex chunks and place the
    // simple chunks immediately after it so SubmitTimeline preserves this order on the GPU.
    constexpr size_t kTransparentChunkSize = 32;
    const auto& visibleBuckets = mainView.queue.VisibleBuckets();
    const auto& transparentComplex = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::TransparentComplex)];
    const auto& transparentSimple = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::TransparentSimple)];
    const size_t complexChunkCount =
        (transparentComplex.size() + kTransparentChunkSize - 1) / kTransparentChunkSize;
    const size_t simpleChunkCount =
        (transparentSimple.size() + kTransparentChunkSize - 1) / kTransparentChunkSize;
    assert(complexChunkCount + simpleChunkCount <= UINT32_MAX);
    const uint32_t simpleLocalOrderBase = static_cast<uint32_t>(complexChunkCount);

    [[maybe_unused]] const size_t pTransparentComplex = rgTr.AddPass(RenderPass::Transparent_Complex, {}, [this, renderer, &camera, &mainView, viewCB](RenderGraphPassContext sub) {
        const auto& visibleBuckets = mainView.queue.VisibleBuckets();
        const auto& transparentComplex = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::TransparentComplex)];
        if (!transparentComplex.empty())
        {
            RenderObjectBatch(renderer, transparentComplex, sub.batchIndex, camera, /*useBundles=*/false,
                false, true, kTransparentChunkSize, viewCB);
        }
        });

    [[maybe_unused]] const size_t pTransparentSimple = rgTr.AddPass(RenderPass::Transparent_Simple, { pTransparentComplex }, [this, renderer, &camera, &mainView, viewCB, simpleLocalOrderBase](RenderGraphPassContext sub) {
        const auto& visibleBuckets = mainView.queue.VisibleBuckets();
        const auto& transparentSimple = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::TransparentSimple)];
        if (!transparentSimple.empty())
        {
            RenderObjectBatch(renderer, transparentSimple, sub.batchIndex, camera, /*useBundles=*/false,
                false, true, kTransparentChunkSize, viewCB, simpleLocalOrderBase);
        }
        });

#if WITH_EDITOR
    if (frame_ && frame_->selectedEditorObjectCount != 0)
    {
        RenderGraph<kTransparentRenderGraphPassCount>::DependencyList selectedDeps;
        selectedDeps.push_back(pTransparentSimple);
        selectedDeps.push_back(pTransparentComplex);
        rgTr.AddPass(RenderPass::Transparent_Selected, selectedDeps, [this, renderer, &camera, &mainView](RenderGraphPassContext sub) {
            auto material = resources_.GetSelectionStencilMaterial();
            if (!frame_->objects || !material)
            {
                return;
            }

            auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
            SetCommandListName(t.cl, sub.pass);
            {
                GPU_SCOPE(t.cl, ProfilerScopes::kRenderObjectBatchGpu);

                const auto& D = renderer->GetDeferredForFrame();
                renderer->Transition(t.cl, D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
                t.cl->OMSetRenderTargets(0, nullptr, FALSE, &D.dsv);

                const D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(renderer->GetRenderWidth()), static_cast<float>(renderer->GetRenderHeight()), 0.0f, 1.0f };
                const D3D12_RECT sr{ 0, 0, static_cast<LONG>(renderer->GetRenderWidth()), static_cast<LONG>(renderer->GetRenderHeight()) };
                t.cl->RSSetViewports(1, &vp);
                t.cl->RSSetScissorRects(1, &sr);

                t.cl->OMSetStencilRef(kSelectionStencilBit);
                for (const std::unique_ptr<RenderableObjectBase>& owned : *frame_->objects)
                {
                    RenderableObjectBase* object = owned.get();
                    if (object && ShouldRenderSelectionStencil(*frame_, mainView, *object, true))
                    {
                        object->RenderSelectionStencil(renderer, t.cl, material.get(), camera);
                    }
                }
                t.cl->OMSetStencilRef(0);
            }
            renderer->EndThreadCommandList(t, sub.batchIndex, kSelectionStencilTransparentLocalOrder);
            });
    }
#endif

    rgTr.Execute(renderer);
}

// ---- Pass_DebugDraw + Pass_SelectionOutline ----
void SceneRenderer::Pass_DebugDraw(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera, std::uint32_t point)
{
    // pass-flow S6: the "has anything been submitted this frame" gate is the builder's; it used
    // to be asked here and again in the Prepare.
    DebugDrawSystem* debugDraw = renderer->GetDebugDrawSystem();

    const auto& D = renderer->GetDeferredForFrame();
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassDebugDraw);
        renderer->EmitPoint(t.cl, point);
        renderer->BindSceneColor(t.cl, Renderer::ClearMode::None, true);

        debugDraw->Render(renderer, t.cl, camera.GetViewMatrix(), camera.GetProjMatrix());
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

#if WITH_EDITOR
void SceneRenderer::Pass_SelectionOutline(Renderer* renderer, RenderGraphPassContext ctx,
    std::uint32_t point)
{
    // pass-flow S5: no gates here — the builder decided this pass runs (selection count at Add
    // time, material/CB/handles at Prepare time) and declared from that decision. Returning after
    // the declarations were made is what leaves depth and scene one transition behind the compile.
    auto material = resources_.GetSelectionOutlineMaterial();
    const UINT cbSize = resources_.GetSelectionOutlineCBSizeBytes();
    const auto& D = renderer->GetDeferredForFrame();

    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        renderer->EmitPoint(t.cl, point);

        SelectionOutlinePassConstants constants{};
        constants.screenSize = float2(
            static_cast<float>(std::max(renderer->GetRenderWidth(), 1u)),
            static_cast<float>(std::max(renderer->GetRenderHeight(), 1u)));
        constants.selectedBit = kSelectionStencilBit;
        constants.outlineRadius = std::clamp<std::uint32_t>(frame_->selectionOutlineRadius, 1u, 8u);
        // P16.1: an authored, DISPLAY-REFERRED colour written into scene colour BEFORE the tone
        // curve. What the curve finally sees is `outlineColor * whatever the tonemap still applies`,
        // so the authored value has to be divided by exactly that -- and nothing else:
        //   * pre-exposed (the default), the WRITERS applied the exposure and the tonemap applies
        //     1.0, so the authored value goes in unchanged;
        //   * with pre-exposure off, scene colour is still physical -- P16 put it in cd/m^2, and a
        //     sunny beach sits near 1e4 -- and the tonemap scales it by the exposure multiplier, so
        //     the outline has to be scaled UP by the inverse.
        // Multiplying BY the pre-exposure, which is what this did, is that backwards: measured at
        // EV100 14.3 the factor is 7.1e-5, so the outline was written as 7e-5 into a buffer whose
        // sand sits around 0.3, and a 0.92 blend toward it painted the contour BLACK.
        // Alpha is the blend weight and stays put.
        float tonemapExposure = 1.0f;
        if (!render::g_preExposureEnabled)
        {
            ExposureMetering& metering = renderer->Exposure();
            if (frame_->cameraExposure.enabled && metering.IsReady())
            {
                const float ev = metering.LatestReadback().adaptedEv100;
                if (std::isfinite(ev))
                {
                    const float m = render::ExposureMultiplierFromEv100(ev);
                    if (std::isfinite(m) && m > 0.0f) { tonemapExposure = m; }
                }
            }
        }
        const float outlineScale = 1.0f / std::max(tonemapExposure, 1.0e-8f);
        constants.outlineColor = float4(1.0f * outlineScale, 0.82f * outlineScale,
                                        0.12f * outlineScale, 0.92f);

        RecordComputeDispatch(renderer, t.cl, material.get(), cbSize,
            [&](uint8_t* dest) { resources_.WriteSelectionOutlineConstants(constants, dest); },
            { D.stencilSRV },
            { D.sceneUAV },
            {},
            renderer->GetRenderWidth(), renderer->GetRenderHeight(),
            D.scene.Get());
    }
    ctx.EndCL(t);
}
#endif

// ---- Pass_OcclusionQueries (occlusion plan S3a) ----
// The camera prepare's box queries against the G-buffer depth: depth bound read-only (the
// binding the skybox uses under DEPTH_READ), no colour target, render-resolution viewport. The
// heap records the batches and resolves the counts into this frame slot's readback region.
void SceneRenderer::Pass_OcclusionQueries(Renderer* renderer, RenderGraphPassContext ctx, uint32_t point)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassOcclusionQueries);
        const auto& D = renderer->GetDeferredForFrame();
        renderer->EmitPoint(t.cl, point);

        t.cl->OMSetRenderTargets(0, nullptr, FALSE, &D.dsv);
        const D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(renderer->GetRenderWidth()),
                                 static_cast<float>(renderer->GetRenderHeight()), 0.0f, 1.0f };
        const D3D12_RECT sr{ 0, 0, static_cast<LONG>(renderer->GetRenderWidth()), static_cast<LONG>(renderer->GetRenderHeight()) };
        t.cl->RSSetViewports(1, &vp);
        t.cl->RSSetScissorRects(1, &sr);

        if (frame_->occlusionPlan && frame_->occlusionQueries)
        {
            frame_->occlusionQueries->Record(renderer, t.cl, *frame_->occlusionPlan, renderer->GetCurrentFrameIndex());
        }
    }
    ctx.EndCL(t);
}
