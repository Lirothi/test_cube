// R2 (docs/scene_renderer_refactor_plan.md): everything that renders depth from a light (or from above): shadows, VSM, the shore map.
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
#include "rendering/meshes/LodSelect.h" // S3.6: render::EffectiveDrawLod (receiver LOD for casters)

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
#include "rendering/shadows/ShadowSettings.h"
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
using namespace scene_internal;

// ---- Pass_ShadowCull + the two VSM passes ----
void SceneRenderer::Pass_ShadowCull(Renderer* renderer, RenderGraphPassContext ctx,
    const ShadowGpuData::CullDecisions& dec)
{
    // Rung 0 / Step 4: GPU cull of shadow casters -> indirect draw args, consumed by the shadow
    // passes' ExecuteIndirect. pass-flow S7a: no gates — the builder decided this pass runs, and
    // `dec` carries both the decisions and the points ShadowGpuData declared them under.
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassShadowCull);
        frame_->shadowGpu->RecordCull(renderer, t.cl, dec);
    }
    ctx.EndCL(t);
}

void SceneRenderer::Pass_VsmPageRequest(Renderer* renderer, RenderGraphPassContext ctx,
    const VirtualShadowMap::PageRequestPoints& pts)
{
    // Rung 2 / Step 19b: mark the virtual shadow pages the visible frame needs. Runs after the
    // GBuffer (needs camera depth); output is the request bitfield, consumed by Step 20 (unused
    // yet — so the pass is gated OFF by default, Ctrl+V to exercise/measure). LOCAL lights only:
    // the view slots are [spots | point-faces] (NO CSM cascades — directional stays on Pass_CSM
    // until Step 24). Per-view viewProj + a mip/refDist LOD param drive the request shader.
    // pass-flow S3c: no gates here — the AddPass2 builder decided this pass runs and declared.

    const auto& D = renderer->GetDeferredForFrame();
    const UINT rw = renderer->GetRenderWidth();
    const UINT rh = renderer->GetRenderHeight();

    vsm::PageRequestConstants cb{};
    const SceneView& mv = *frame_->mainView;
    cb.invView = mv.invView.m;
    cb.invProj = mv.invProj.m;
    cb.camPosWS = DirectX::XMFLOAT4(mv.position.x, mv.position.y, mv.position.z, 0.0f);
    cb.screen = DirectX::XMFLOAT4(static_cast<float>(rw), static_cast<float>(rh),
                                  rw ? 1.0f / rw : 0.0f, rh ? 1.0f / rh : 0.0f);
    cb.lodParams = DirectX::XMFLOAT4(vsm::g_refDist, static_cast<float>(vsm::kMaxMipLevel),
                                     static_cast<float>(vsm::g_requestDownscale),
                                     vsm::ClipmapBlendWidth());

    std::uint32_t slot = 0;
    auto addView = [&](const SceneView& v, bool active)
    {
        if (slot >= vsm::kMaxVirtualViews) { return; }
        cb.views[slot].viewProj = (v.view * v.proj).m;
        const float valid = (active && v.frustum.IsValid()) ? 1.0f : 0.0f;
        cb.views[slot].params = DirectX::XMFLOAT4(valid, v.zNear, v.zFar, 0.0f);
        ++slot;
    };
    // Slot layout must match vsm::kMaxVirtualViews / the page-table view indexing: spots first,
    // then point-light cube faces. Cascades are intentionally excluded (Step 19b local scope).
    const size_t spotCount = frame_->lightManager->GetShadowedSpotCount();
    { size_t i = 0; for (const SceneView& v : *frame_->spotShadowViews) { addView(v, i < spotCount); ++i; } }
    const size_t pointFaces = frame_->lightManager->GetShadowedPointCount() * 6;
    { size_t i = 0; for (const SceneView& v : *frame_->pointShadowViews) { addView(v, i < pointFaces); ++i; } }
    // Step 24d: directional clipmap levels fill slots [32, 40) — the request shader picks the finest
    // level containing each receiver. Add-dormant: pages get requested + allocated but not yet
    // rendered/sampled (the setup shader skips clipmap views; directional still uses CSM).
    if (frame_->clipmapViews) { for (const SceneView& v : *frame_->clipmapViews) { addView(v, true); } }
    cb.numViews = slot;

    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassVsmPageRequest);
        // The base point moves camera depth to SRV (the G-buffer leaves it in DEPTH_WRITE) and
        // the request buffer to UAV, in one marker.
        renderer->EmitPoint(t.cl, pts.base);
        frame_->vsm->RecordPageRequest(renderer, t.cl, cb, D.depthSRV, rw, rh);
        // Step 20: allocate physical pages for the just-marked requests (same CL — request buffer
        // stays UAV between them). Add-dormant: nothing samples/renders the pages yet.
        frame_->vsm->RecordPageAllocate(renderer, t.cl, pts, frame_->clipmapSquares);
    }
    ctx.EndCL(t);
}

void SceneRenderer::Pass_VsmPageRender(Renderer* renderer, RenderGraphPassContext ctx,
    const VirtualShadowMap::PageRenderDecisions& dec)
{
    // Rung 2 / Step 22: render casters into the resident VSM pages. Builds the LOCAL shadow views
    // (spots then point faces — same slot layout as Pass_VsmPageRequest), then RecordPageRender
    // does the GPU per-page setup + per-page ExecuteIndirect into the pool (DEPTH_WRITE via graph).
    // pass-flow S3: no gates here — the AddPass2 builder decided this pass runs, made the
    // declarations, and captured `dec`; a second decision here could only disagree.

    std::array<vsm::ViewProjEntry, vsm::kMaxVirtualViews> views{};
    std::uint32_t slot = 0;
    auto addView = [&](const SceneView& v, bool active)
    {
        if (slot >= vsm::kMaxVirtualViews) { return; }
        views[slot].viewProj = (v.view * v.proj).m;
        const float valid = (active && v.frustum.IsValid()) ? 1.0f : 0.0f;
        views[slot].params = DirectX::XMFLOAT4(valid, v.zNear, v.zFar, 0.0f);
        ++slot;
    };
    const size_t spotCount = frame_->lightManager->GetShadowedSpotCount();
    { size_t i = 0; for (const SceneView& v : *frame_->spotShadowViews) { addView(v, i < spotCount); ++i; } }
    const size_t pointFaces = frame_->lightManager->GetShadowedPointCount() * 6;
    { size_t i = 0; for (const SceneView& v : *frame_->pointShadowViews) { addView(v, i < pointFaces); ++i; } }
    // Step 24e: clipmap views fill slots [32, 40) so the setup builds their per-page projection from
    // gViewProj[view]; they render directional casters via the Rung-0 clipmap cull slots (rung0View
    // = view + kNumCascades). Matches the request pass + cull frustum layout.
    if (frame_->clipmapViews) { for (const SceneView& v : *frame_->clipmapViews) { addView(v, true); } }

    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassVsmPageRender);
        frame_->vsm->RecordPageRender(renderer, t.cl, frame_->shadowGpu, views.data(), slot,
            frame_->wind, dec);
    }
    ctx.EndCL(t);
}

// ---- IndirectShadowDrawsActive ----
bool SceneRenderer::IndirectShadowDrawsActive() const
{
    return render::g_indirectShadowsEnabled && frame_ && frame_->shadowGpu &&
           frame_->shadowGpu->IndirectDrawReady();
}

// ---- Pass_ShoreDepth ----
void SceneRenderer::Pass_ShoreDepth(Renderer* renderer, RenderGraphPassContext ctx,
    const SceneView* view, const ShoreDepthPoints& pts)
{
    // pass-flow S6: no gates and no flag reads here. The builder decided drawDepth/buildSdf from
    // the SAME OceanSimulation flags this body used to re-read, validated every target and DSV it
    // declared for, and already committed MarkShoreSdfBuilt.
    OceanSimulation* oceanSimulation = Systems::GetOceanSimulation();

    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassShoreDepth);

        auto renderCascade = [&](const SceneView& cascadeView,
                                 ID3D12Resource* target,
                                 D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                                 std::uint32_t pointWrite,
                                 std::uint32_t pointRead)
        {
            renderer->EmitPoint(t.cl, pointWrite);
            t.cl->OMSetRenderTargets(0, nullptr, FALSE, &dsv);

            const auto desc = target->GetDesc();
            const float width = static_cast<float>(desc.Width);
            const float height = static_cast<float>(desc.Height);
            D3D12_VIEWPORT vp{ 0.0f, 0.0f, width, height, 0.0f, 1.0f };
            D3D12_RECT sc{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
            t.cl->RSSetViewports(1, &vp);
            t.cl->RSSetScissorRects(1, &sc);

            t.cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

            const D3D12_GPU_VIRTUAL_ADDRESS viewCB =
                BuildShadowViewCB(renderer, cascadeView.view, cascadeView.proj, frame_->wind);

            const auto& visibleBuckets = cascadeView.queue.VisibleBuckets();
            for (auto bucket : { SceneRenderQueue::BucketType::OpaqueSimple,
                                 SceneRenderQueue::BucketType::OpaqueComplex })
            {
                for (auto* obj : visibleBuckets[BucketIndex(bucket)])
                {
                    if (obj)
                    {
                        obj->RenderShadow(renderer, t.cl, cascadeView.view, cascadeView.proj, viewCB);
                    }
                }
            }

            renderer->EmitPoint(t.cl, pointRead);
        };

        if (pts.drawDepth)
        {
            renderCascade(*view, oceanSimulation->GetShoreDepthResource(),
                oceanSimulation->GetShoreDepthDsv(), pts.depthWrite, pts.depthRead);
        }

        // The SDF's source is the same top-down terrain render, just covering the whole level.
        // Once per load: rasterize it, then jump-flood it into a distance field.
        if (pts.buildSdf)
        {
            renderCascade(oceanSimulation->GetShoreSdfView(),
                oceanSimulation->GetShoreSdfSourceResource(),
                oceanSimulation->GetShoreSdfSourceDsv(), pts.sdfWrite, pts.sdfRead);
            // The flood's own transitions stay named (a legal mixed pass): its two UAV requests
            // were already covered by the point the marker above emitted, and its closing
            // `sdf -> shader-readable` matches the point the builder declared after them.
            oceanSimulation->BuildShoreSdf(renderer, t.cl);
        }
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

// ---- Pass_CSM + Pass_SpotShadows + Pass_PointShadows ----
namespace
{
    // S3.6: UE's rule for the CPU shadow paths. A caster draws at the LOD its RECEIVER draws -- the
    // one the CAMERA picked (UE: FProjectedShadowInfo::CalcAndUpdateLODToRender starts from
    // CurrentView.PrimitivesLODMask, and CurrentView is a camera view). These loops used to pass the
    // CASCADE INDEX as the LOD, which is the mismatch the whole contract exists to remove: a caster
    // FINER than its receiver puts the coarse receiver inside the finer caster's hull and it fails
    // its own depth test (the "black squares" family).
    //
    // Recomputed from the camera rather than read off the object, exactly like
    // ShadowGpuData::RefreshCasterLods: a stored per-object LOD is stale for anything the camera did
    // not select this frame, and these loops iterate the SHADOW view's visible set, not the camera's.
    UINT ReceiverCasterLod(const RenderableObjectBase* obj, const Math::float3& cameraPos)
    {
        const RenderableObject* ro = obj ? obj->AsRenderableObject() : nullptr;
        const Mesh* mesh = ro ? ro->GetMesh() : nullptr;
        if (!mesh) { return 0u; }
        return mesh->ClampExplicitLod(
            static_cast<UINT>(render::EffectiveDrawLod(ro->ComputeReceiverLodTier(cameraPos))));
    }
} // namespace

namespace
{
// S6: the four numbers a cascade's depth pass needs. Mirrors UE's UpdateShaderDepthBias:
//   ShaderDepthBias      = max(DepthBias, 0)                       (DepthBias itself already clamped)
//   ShaderSlopeDepthBias = max(DepthBias * SlopeScaleDepthBias, 0)
//   ShaderMaxSlopeDepthBias = r.Shadow.ShadowMaxSlopeScaleDepthBias
// including their `DepthBias = min(DepthBias, .1f)` clamp, whose comment reads: prevent a large
// depth bias due to low resolution from causing near plane clipping (ShadowRendering.cpp:1905).
struct CascadeDepthBias { float constBias, slopeBias, maxSlope, clampNear; };

// UpdateShaderDepthBias, transcribed. `depthBiasNDC` already carries UE's whole expression
// (CSMDepthBias / zRange * worldTexel -- see depthBiasInTexels), so it is fed RAW: no compatibility
// factor, because there is no longer a second arrangement to stay compatible with.
//   ShaderDepthBias      = max(DepthBias, 0)
//   ShaderSlopeDepthBias = max(DepthBias * SlopeScaleDepthBias, 0)
//   ShaderMaxSlopeDepthBias = r.Shadow.ShadowMaxSlopeScaleDepthBias
// plus their clamp (ShadowRendering.cpp:1905, "prevent a large depth bias due to low resolution
// from causing near plane clipping").
CascadeDepthBias ComputeCascadeDepthBias(const CascadeShadowConfig& cfg, float depthBiasNDC)
{
    const float b = std::max(0.0f, std::min(depthBiasNDC, 0.1f));
    return CascadeDepthBias{ b, std::max(0.0f, b * cfg.slopeScale), cfg.maxSlope,
                             cfg.pancakeCasters ? 1.0f : 0.0f };
}
} // namespace

void SceneRenderer::Pass_CSM(Renderer* renderer, RenderGraphPassContext ctx,
    const std::array<SceneView, kCascades>& cascadeViews,
    std::uint32_t atlasPoint, bool indirect)
{
    // Step 24f-2: only reached in Legacy mode — the graph omits the Main_CSM pass in VSM mode
    // (directional then comes from the clipmap), so no VSM gate is needed here.
    auto d = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(d.cl, ctx.pass);
    {
        GPU_SCOPE(d.cl, ProfilerScopes::kPassCSM);
        renderer->EmitPoint(d.cl, atlasPoint);
        renderer->BindShadowTarget(d.cl, 0, /*clear=*/true);
    }
    renderer->EndThreadCommandList(d, ctx.batchIndex);

#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
    const RenderPass passName = ctx.pass;
    // Step 6: GPU-driven indirect shadow submission (toggle, default off). The cull already ran
    // in Pass_ShadowCull; here each cascade issues ExecuteIndirect instead of the CPU loop.
    ShadowGpuData* shadowGpu = frame_->shadowGpu;
    // pass-flow S7c: `indirect` is the BUILDER's decision, passed in — the registration walk and
    // this draw walk have to agree about which objects go through the indirect path.
    const vfx::WindState* wind = frame_->wind; // W5: shadow casters sway with the gbuffer's params
    // Barrier plan step 5: carry the pass's transition log onto the fan-out workers, so the
    // comparator observes what they record. Captured HERE, on the pass thread, where the log is
    // installed; without it these passes look silent because they are unobserved, not correct.
    Renderer::TransitionLog* const cmpLog = Renderer::CurrentThreadTransitionLog();
    // Step 7: the compiled barriers travel with the log — a fan-out worker must emit its
    // pass's barriers too, or the flip loses exactly the passes that record in parallel.
    Renderer::CompiledBarriers* const cmpBarriers = Renderer::CurrentThreadCompiledBarriers();
    // S3.6: captured by value — the lambda has no `this`, and every caster in it needs the camera
    // position to resolve its RECEIVER's LOD.
    const Math::float3 csmCamPos = frame_->camera ? frame_->camera->GetPosition()
                                                  : Math::float3(0.0f, 0.0f, 0.0f);
    // S6: same reason -- copies, because the workers have no `this`. The per-cascade NDC bias is
    // the cascade's NDC bias, the only place a directional shadow is biased at all now.
    const CascadeShadowConfig csmCfg = frame_->cascadeConfig ? *frame_->cascadeConfig
                                                             : CascadeShadowConfig{};
    std::array<float, kCascades> csmDepthBiasNDC{};
    std::copy(std::begin(frame_->cascades.depthBiasNDC), std::end(frame_->cascades.depthBiasNDC),
              csmDepthBiasNDC.begin());
    // S11: the per-cascade view-cone scissor, copied for the workers like the biases. Passed as
    // null while the optimisation is off, so BindShadowTarget keeps its full-tile rect and the A/B
    // is one flag inside one binary.
    const bool csmUseScissor = csmCfg.scissorOptim;
    std::array<D3D12_RECT, kCascades> csmScissor{};
    for (std::size_t c = 0; c < kCascades; ++c)
    {
        const auto& r = frame_->cascades.scissor[c];
        csmScissor[c] = D3D12_RECT{ r.x0, r.y0, r.x1, r.y1 };
    }
    auto renderCascade = [renderer, &cascadeViews, batchIndex = ctx.batchIndex, passName, shadowGpu, indirect, wind, cmpLog, cmpBarriers, csmCamPos, &csmCfg, &csmDepthBiasNDC, csmUseScissor, &csmScissor](std::size_t cascadeIndex)
    {
        Renderer::TransitionLogScope cmpScope(cmpLog);
        Renderer::CompiledBarrierScope cmpBarrierScope(cmpBarriers);
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

        const CascadeDepthBias cb = ComputeCascadeDepthBias(csmCfg, csmDepthBiasNDC[cascadeIndex]);
        const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildShadowViewCB(renderer, view.view, view.proj, wind,
                                                                   cb.constBias, cb.slopeBias,
                                                                   cb.maxSlope, cb.clampNear);

        auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        SetCommandListName(t.cl, passName);
        {
            GPU_SCOPE(t.cl, ProfilerScopes::kPassCSM);
            renderer->BindShadowTarget(t.cl, static_cast<int>(cascadeIndex), /*clear=*/false,
                                       csmUseScissor ? &csmScissor[cascadeIndex] : nullptr);

            if (indirect)
            {
                // Cascade i -> shadow-view slot i (the frustum/args layout). Uses base-LOD
                // geometry (the cull's args carry the base index count).
                shadowGpu->RecordIndirectShadowDraws(renderer, t.cl, static_cast<std::uint32_t>(cascadeIndex), viewCB);
                // GPU-instanced casters: when the GI folding path is active (Ctrl+G, default on) they
                // cast via the indirect cull/scatter like everything else, so skip them here. Otherwise
                // (flag off, over the group cap, or scatter PSO failure) draw them through their own
                // instanced shadow path so they still cast — IsGiFoldedActive encodes exactly that.
                // GI casters register LOD0 geometry only (their mega rows past lod 0 are empty),
                // so LOD0 is both the receiver's and the only valid choice -- NOT the cascade index.
                const UINT giLod = 0u;
                for (auto* obj : opaqueSimple)  { if (obj && obj->IsGpuInstancedCaster() && !shadowGpu->IsGiFoldedActive(obj)) obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB, giLod); }
                for (auto* obj : opaqueComplex) { if (obj && obj->IsGpuInstancedCaster() && !shadowGpu->IsGiFoldedActive(obj)) obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB, giLod); }
            }
            else
            {
                // S3.6: per-object receiver LOD (see ReceiverCasterLod). Was the cascade index.
                // S1: the cascade's cull volume (S14 accurate frustum) rides along so a chunked
                // caster draws only the chunks that volume keeps -- the CPU twin of the GPU path's
                // per-chunk cull.
                const Math::float3& camPos = csmCamPos;
                for (auto* obj : opaqueSimple)
                {
                    if (obj)
                    {
                        obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB,
                            ReceiverCasterLod(obj, camPos), /*chunkCameraLods=*/true, &view.frustum);
                    }
                }

                for (auto* obj : opaqueComplex)
                {
                    if (obj)
                    {
                        obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB,
                            ReceiverCasterLod(obj, camPos), /*chunkCameraLods=*/true, &view.frustum);
                    }
                }
            }
        }

        // localOrder: the clear list (recorded above) is 0; cascades follow in
        // index order so the atlas clear always precedes every cascade's draws.
        renderer->EndThreadCommandList(t, batchIndex, static_cast<uint32_t>(cascadeIndex) + 1u);
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

        const CascadeDepthBias cb = ComputeCascadeDepthBias(frame_->cascadeConfig
                                                               ? *frame_->cascadeConfig
                                                               : CascadeShadowConfig{},
                                                           frame_->cascades.depthBiasNDC[idx]);
        const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildShadowViewCB(renderer, view.view, view.proj, frame_->wind,
                                                                   cb.constBias, cb.slopeBias,
                                                                   cb.maxSlope, cb.clampNear);

        auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        SetCommandListName(t.cl, ctx.pass);
        {
            GPU_SCOPE(t.cl, ProfilerScopes::kPassCSM);
            // S11: same view-cone scissor as the parallel path.
            const auto& sr = frame_->cascades.scissor[idx];
            const D3D12_RECT scRect{ sr.x0, sr.y0, sr.x1, sr.y1 };
            const bool useSc = frame_->cascadeConfig && frame_->cascadeConfig->scissorOptim;
            renderer->BindShadowTarget(t.cl, static_cast<int>(idx), /*clear=*/false,
                                       useSc ? &scRect : nullptr);

            // S3.6: per-object receiver LOD (see ReceiverCasterLod). Was the cascade index.
            const Math::float3 camPos = frame_->camera ? frame_->camera->GetPosition()
                                                       : Math::float3(0.0f, 0.0f, 0.0f);
            for (auto* obj : opaqueSimple)
            {
                if (obj)
                {
                    obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB,
                            ReceiverCasterLod(obj, camPos), /*chunkCameraLods=*/true, &view.frustum); // S1
                }
            }

            for (auto* obj : opaqueComplex)
            {
                if (obj)
                {
                    obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB,
                            ReceiverCasterLod(obj, camPos), /*chunkCameraLods=*/true, &view.frustum); // S1
                }
            }
        }

        // Matches the parallel path: clear list is 0, cascades follow in order.
        renderer->EndThreadCommandList(t, ctx.batchIndex, static_cast<uint32_t>(idx) + 1u);
    }
#endif
}

const Profiler::ScopeNameKey kShadows1 = Profiler::RegisterTraceLiteral(L"SpotShadows1");
const Profiler::ScopeNameKey kShadows2 = Profiler::RegisterTraceLiteral(L"SpotShadows2");
void SceneRenderer::Pass_SpotShadows(Renderer* renderer, RenderGraphPassContext ctx,
    const std::array<SceneView, LightManager::kMaxShadowedSpotLights>& spotViews,
    size_t viewCount, std::uint32_t atlasPoint, bool indirect)
{
    // pass-flow S6: the VSM-mode skip (Step 24c: in VSM mode the spot atlas is a 1x1 placeholder
    // and local shadows come from the VSM pool) and the light-count clamp are the BUILDER's — it
    // declares nothing on the frames this pass has no work, instead of the two sides agreeing by
    // discipline.
    const auto& D = renderer->GetDeferredForFrame();

#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
    ShadowGpuData* shadowGpu = frame_->shadowGpu;
    // pass-flow S7c: `indirect` is the BUILDER's decision, passed in — the registration walk and
    // this draw walk have to agree about which objects go through the indirect path.
    const vfx::WindState* wind = frame_->wind; // W5
    // Barrier plan step 5: carry the pass's transition log onto the fan-out workers, so the
    // comparator observes what they record. Captured HERE, on the pass thread, where the log is
    // installed; without it these passes look silent because they are unobserved, not correct.
    Renderer::TransitionLog* const cmpLog = Renderer::CurrentThreadTransitionLog();
    // Step 7: the compiled barriers travel with the log — a fan-out worker must emit its
    // pass's barriers too, or the flip loses exactly the passes that record in parallel.
    Renderer::CompiledBarriers* const cmpBarriers = Renderer::CurrentThreadCompiledBarriers();
    auto renderSpotShadow = [renderer, &D, &spotViews, batchIndex = ctx.batchIndex, shadowGpu, indirect, wind, cmpLog, cmpBarriers, atlasPoint](std::size_t lightIndex)
    {
        Renderer::TransitionLogScope cmpScope(cmpLog);
        Renderer::CompiledBarrierScope cmpBarrierScope(cmpBarriers);
        if (lightIndex >= spotViews.size())
        {
            return;
        }

        CPU_SCOPE(ProfilerScopes::kSpotShadowPerLight);
        auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        {
            GPU_SCOPE(t.cl, ProfilerScopes::kPassSpotShadow);
            // Only the FIRST light's list moves the atlas. Recording order across the fan-out is
            // nondeterministic; SUBMISSION order is localOrder = lightIndex. Letting every list
            // transition meant the barrier the flip actually emitted landed in whichever list
            // recorded first, i.e. possibly AFTER another list's ClearDepthStencilView had already
            // run on the queue — D3D12 errors 527/538 on the atlas. The tracker hid this by
            // stitching acquire barriers at submit time, where the real order is known.
            if (lightIndex == 0)
            {
                renderer->EmitPoint(t.cl, atlasPoint);
            }
            renderer->BindSpotShadowTarget(t.cl, static_cast<UINT>(lightIndex), /*clearDepth=*/true);

            const SceneView& view = spotViews[lightIndex];
            const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildShadowViewCB(renderer, view.view, view.proj, wind);
            const auto& visibleBuckets = view.queue.VisibleBuckets();
            const auto& opaqueSimple = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueSimple)];
            const auto& opaqueComplex = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueComplex)];

            if (indirect)
            {
                // Spot light i -> shadow-view slot kCascades + i.
                const std::uint32_t viewSlot = static_cast<std::uint32_t>(kCascades + lightIndex);
                shadowGpu->RecordIndirectShadowDraws(renderer, t.cl, viewSlot, viewCB);
                // GPU-instanced casters: skip when the GI folding path is active (Ctrl+G) — the
                // indirect cull draws them; otherwise (flag off / over-cap / PSO failure) draw here.
                for (auto* obj : opaqueSimple)  { if (obj && obj->IsGpuInstancedCaster() && !shadowGpu->IsGiFoldedActive(obj)) obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB); }
                for (auto* obj : opaqueComplex) { if (obj && obj->IsGpuInstancedCaster() && !shadowGpu->IsGiFoldedActive(obj)) obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB); }
            }
            else
            {
                for (auto* obj : opaqueSimple)
                {
                    if (obj)
                    {
                        obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB);
                    }
                }
                for (auto* obj : opaqueComplex)
                {
                    if (obj)
                    {
                        obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB);
                    }
                }
            }
        }
        // Per-light index is the deterministic submit order within the batch.
        renderer->EndThreadCommandList(t, batchIndex, static_cast<uint32_t>(lightIndex));
    };

    TaskSystem::Get().DispatchWait(viewCount, renderSpotShadow, 1);
#else
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSpotShadow);
        renderer->EmitPoint(t.cl, atlasPoint);

        for (size_t lightIndex = 0; lightIndex < viewCount; ++lightIndex)
        {
            renderer->BindSpotShadowTarget(t.cl, static_cast<UINT>(lightIndex), /*clearDepth=*/true);

            const SceneView& view = spotViews[lightIndex];
            const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildShadowViewCB(renderer, view.view, view.proj, frame_->wind);
            const auto& visibleBuckets = view.queue.VisibleBuckets();
            const auto& opaqueSimple = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueSimple)];
            const auto& opaqueComplex = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueComplex)];

            for (auto* obj : opaqueSimple)
            {
                if (obj)
                {
                    obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB);
                }
            }
            for (auto* obj : opaqueComplex)
            {
                if (obj)
                {
                    obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB);
                }
            }
        }
    }
    renderer->EndThreadCommandList(t, ctx.batchIndex);
#endif
}

void SceneRenderer::Pass_PointShadows(Renderer* renderer, RenderGraphPassContext ctx,
    const std::array<SceneView, LightManager::kMaxShadowedPointLights * 6>& pointViews,
    size_t viewCount, std::uint32_t atlasPoint, bool indirect)
{
    // pass-flow S6: same as Pass_SpotShadows — the VSM-mode skip (Step 24c: VSM renders point
    // shadows into the pool, the cube atlas is a 1x1 placeholder) and the 6-faces-per-light
    // clamp are the builder's single decision. 6 cube faces per shadowed point light; each face
    // is its own depth-array slice (its own DSV), so faces render independently, with the
    // flattened face index as the "slice" (cubeSlot = idx/6, face = idx%6).
    const auto& D = renderer->GetDeferredForFrame();

#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
    ShadowGpuData* shadowGpu = frame_->shadowGpu;
    // pass-flow S7c: `indirect` is the BUILDER's decision, passed in — the registration walk and
    // this draw walk have to agree about which objects go through the indirect path.
    const vfx::WindState* wind = frame_->wind; // W5
    // Barrier plan step 5: carry the pass's transition log onto the fan-out workers, so the
    // comparator observes what they record. Captured HERE, on the pass thread, where the log is
    // installed; without it these passes look silent because they are unobserved, not correct.
    Renderer::TransitionLog* const cmpLog = Renderer::CurrentThreadTransitionLog();
    // Step 7: the compiled barriers travel with the log — a fan-out worker must emit its
    // pass's barriers too, or the flip loses exactly the passes that record in parallel.
    Renderer::CompiledBarriers* const cmpBarriers = Renderer::CurrentThreadCompiledBarriers();
    auto renderPointShadow = [renderer, &D, &pointViews, batchIndex = ctx.batchIndex, shadowGpu, indirect, wind, cmpLog, cmpBarriers, atlasPoint](std::size_t faceIndex)
    {
        Renderer::TransitionLogScope cmpScope(cmpLog);
        Renderer::CompiledBarrierScope cmpBarrierScope(cmpBarriers);
        if (faceIndex >= pointViews.size())
        {
            return;
        }

        CPU_SCOPE(ProfilerScopes::kSpotShadowPerLight);
        auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        {
            GPU_SCOPE(t.cl, ProfilerScopes::kPassPointShadow);
            // See Pass_SpotShadows: the atlas barrier must be recorded into the list submitted
            // first (localOrder = faceIndex), not into whichever list records first.
            if (faceIndex == 0)
            {
                renderer->EmitPoint(t.cl, atlasPoint);
            }
            renderer->BindPointShadowTarget(t.cl, static_cast<UINT>(faceIndex / 6),
                static_cast<UINT>(faceIndex % 6), /*clear=*/true);

            const SceneView& view = pointViews[faceIndex];
            const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildShadowViewCB(renderer, view.view, view.proj, wind);
            const auto& visibleBuckets = view.queue.VisibleBuckets();
            const auto& opaqueSimple = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueSimple)];
            const auto& opaqueComplex = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueComplex)];

            if (indirect)
            {
                // Point cube face k -> shadow-view slot kCascades + kMaxShadowedSpotLights + k.
                const std::uint32_t viewSlot = static_cast<std::uint32_t>(
                    kCascades + LightManager::kMaxShadowedSpotLights + faceIndex);
                shadowGpu->RecordIndirectShadowDraws(renderer, t.cl, viewSlot, viewCB);
                // GPU-instanced casters: skip when the GI folding path is active (Ctrl+G) — the
                // indirect cull draws them; otherwise (flag off / over-cap / PSO failure) draw here.
                for (auto* obj : opaqueSimple)  { if (obj && obj->IsGpuInstancedCaster() && !shadowGpu->IsGiFoldedActive(obj)) obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB); }
                for (auto* obj : opaqueComplex) { if (obj && obj->IsGpuInstancedCaster() && !shadowGpu->IsGiFoldedActive(obj)) obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB); }
            }
            else
            {
                for (auto* obj : opaqueSimple)
                {
                    if (obj)
                    {
                        obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB);
                    }
                }
                for (auto* obj : opaqueComplex)
                {
                    if (obj)
                    {
                        obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB);
                    }
                }
            }
        }
        renderer->EndThreadCommandList(t, batchIndex, static_cast<uint32_t>(faceIndex));
    };

    TaskSystem::Get().DispatchWait(viewCount, renderPointShadow, 1);
#else
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSpotShadow);
        renderer->EmitPoint(t.cl, atlasPoint);

        for (size_t faceIndex = 0; faceIndex < viewCount; ++faceIndex)
        {
            renderer->BindPointShadowTarget(t.cl, static_cast<UINT>(faceIndex / 6),
                static_cast<UINT>(faceIndex % 6), /*clear=*/true);

            const SceneView& view = pointViews[faceIndex];
            const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildShadowViewCB(renderer, view.view, view.proj, frame_->wind);
            const auto& visibleBuckets = view.queue.VisibleBuckets();
            const auto& opaqueSimple = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueSimple)];
            const auto& opaqueComplex = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueComplex)];

            for (auto* obj : opaqueSimple)
            {
                if (obj)
                {
                    obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB);
                }
            }
            for (auto* obj : opaqueComplex)
            {
                if (obj)
                {
                    obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB);
                }
            }
        }
    }
    renderer->EndThreadCommandList(t, ctx.batchIndex);
#endif
}
