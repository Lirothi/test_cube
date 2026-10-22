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
#include "rendering/shadows/LocalShadowCache.h" // local-light plan S7
#include "rendering/shadows/VirtualShadowMap.h"
#include "ocean/OceanSimulation.h"
#include "rendering/core/PhotographicSettings.h" // P16.1 pre-exposure
#include "rendering/core/UploadBatch.h" // the ghost sprite sheet is uploaded once, lazily
#include "rendering/shadows/ShadowSettings.h"
#include "rendering/shadows/SdsmShadows.h" // S15: the partition buffer + the analyze record
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
namespace
{
// Local-light plan S10b: VSM's view list, by the layout's slot for each light (vsm::SpotView / PointFaceView /
// ClipmapView = the cull slot minus the cascades) -- the request and the render pass must agree, so both build it here.
// Every slot of the prefix (spots, faces, clipmap) is written, valid or not; past it, up to the last shadowed light's
// view. Returns the count: the request shader, the scatter and the setup walk exactly these. Plan S9e: `views` holds
// the VSM's ViewCount (the capacity it was made for); a light past it -- the capacity grew this frame, VSM is re-created
// at the next ReconcileShadowMode -- has no view yet.
std::uint32_t BuildVsmViews(const SceneFrameData& frame, std::vector<vsm::ViewProjEntry>& views)
{
    std::uint32_t count = vsm::kClipmapFirstView + vsm::kNumClipmapLevels;
    views.assign(frame.vsm ? frame.vsm->ViewCount() : 0u, vsm::ViewProjEntry{});
    const std::uint32_t viewSlots = static_cast<std::uint32_t>(views.size());
    // `face` = a point face's cube face (0..5), -1 for a spot or a clipmap level.
    const auto put = [&](std::uint32_t slot, const SceneView& v, bool active, float face)
    {
        if (slot >= viewSlots) { return; }
        views[slot].viewProj = (v.view * v.proj).m;
        const float valid = (active && v.frustum.IsValid()) ? 1.0f : 0.0f;
        // .w = the projection's x scale (plan S10e: the request's pixel-footprint mip; a spot's 1 / tan(outer), a face's 1).
        views[slot].params = DirectX::XMFLOAT4(valid, face, v.zFar, v.proj.m._11);
        // Plan S10e: the light's reach (SceneView::lightSphere, S9e) -- the request marks nothing for receivers past it
        // (when localMip.w says so: vsm.requestLightCull); plan S10d classifies distant lights by it.
        views[slot].cull = v.lightSphere.xf();
        // S10e: UE's moving bias, this light's; y = render::g_localShadowWindRange's verdict (Scene::PrepareViews, both modes).
        views[slot].mip = DirectX::XMFLOAT4(v.vsmMipBias, v.windRigid ? 1.0f : 0.0f, 0.0f, 0.0f);
        if (active) { count = std::max(count, slot + 1u); }
    };
    const size_t spotCount = std::min(frame.lightManager->GetShadowedSpotCount(), frame.spotShadowViews->size());
    for (size_t i = 0; i < std::max<size_t>(spotCount, LightManager::kVsmShadowedSpotLights) &&
                       i < frame.spotShadowViews->size(); ++i)
    {
        put(vsm::SpotView(static_cast<std::uint32_t>(i)), (*frame.spotShadowViews)[i], i < spotCount, -1.0f);
    }
    const size_t pointFaces = std::min(frame.lightManager->GetShadowedPointCount() * 6, frame.pointShadowViews->size());
    for (size_t i = 0; i < std::max<size_t>(pointFaces, 6u * LightManager::kVsmShadowedPointLights) &&
                       i < frame.pointShadowViews->size(); ++i)
    {
        put(vsm::PointFaceView(static_cast<std::uint32_t>(i)), (*frame.pointShadowViews)[i], i < pointFaces,
            static_cast<float>(i % 6u));
    }
    // Step 24d/24e: the directional clipmap levels -- the request shader picks the finest level containing each
    // receiver; the setup renders their pages through the cull's clipmap slots (rung0View = view + kNumCascades).
    if (frame.clipmapViews)
    {
        std::uint32_t level = 0;
        for (const SceneView& v : *frame.clipmapViews)
        {
            if (level < vsm::kNumClipmapLevels) { put(vsm::ClipmapView(level), v, true, -1.0f); }
            ++level;
        }
    }
    return count;
}
} // namespace

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
    cb.lodParams = DirectX::XMFLOAT4(vsm::LocalRefDist(), static_cast<float>(vsm::kMaxMipLevel), // S10e: dynamic res
                                     static_cast<float>(vsm::g_requestDownscale),
                                     vsm::ClipmapBlendWidth());
    cb.localMip = vsm::LocalMipParams(mv.invProj.m, static_cast<float>(rw), static_cast<float>(rh)); // S10e: UE's mip
    cb.localMip.w = vsm::g_requestLightCull ? 1.0f : 0.0f; // S10e: test the receiver against the light's sphere

    // The page-table view indexing (plan S10b: vsm::SpotView / PointFaceView / ClipmapView, the cull layout minus its
    // cascades). Cascades are intentionally excluded (Step 19b local scope).
    std::vector<vsm::ViewProjEntry> views;
    cb.numViews = BuildVsmViews(*frame_, views);
    // Plan S9e: the request walks the shadowed lights (BuildVsmViews made their views valid), as many as have a view.
    cb.spotLights = static_cast<std::uint32_t>(std::min(frame_->lightManager->GetShadowedSpotCount(),
                                                        frame_->spotShadowViews->size()));
    cb.pointLights = static_cast<std::uint32_t>(std::min(frame_->lightManager->GetShadowedPointCount(),
                                                         frame_->pointShadowViews->size() / 6u));
    cb.pointExtraView = vsm::PointFaceView(6u * render::kShadowViewVsmPoints);

    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassVsmPageRequest);
        // The base point moves camera depth to SRV (the G-buffer leaves it in DEPTH_WRITE) and
        // the request buffer to UAV, in one marker.
        renderer->EmitPoint(t.cl, pts.base);
        frame_->vsm->RecordPageRequest(renderer, t.cl, cb, views.data(), cb.numViews, pts, D.depthSRV, rw, rh);
        // Step 20: allocate physical pages for the just-marked requests (same CL — request buffer
        // stays UAV between them). Add-dormant: nothing samples/renders the pages yet.
        frame_->vsm->RecordPageAllocate(renderer, t.cl, pts, frame_->clipmapSquares);
    }
    ctx.EndCL(t);
}

// Plan S10d: which lights are distant this frame and which of them the round robin redraws -- from the view list both
// VSM passes walk, once a frame, in the page-render pass's builder before PrepareRenderPass.
void SceneRenderer::PlanVsmDistantLights(Renderer* renderer)
{
    if (!frame_ || !frame_->vsm || !frame_->mainView || !renderer) { return; }
    std::vector<vsm::ViewProjEntry> views;
    const std::uint32_t count = BuildVsmViews(*frame_, views);
    const SceneView& mv = *frame_->mainView;
    const DirectX::XMFLOAT3 camPos(mv.position.x, mv.position.y, mv.position.z);
    const DirectX::XMFLOAT3 camForward(mv.invView.m._31, mv.invView.m._32, mv.invView.m._33); // the request's depth axis
    const float pixelRadiusPerDepth = vsm::PixelRadiusPerDepth(mv.invProj.m, static_cast<float>(renderer->GetRenderWidth()),
                                                                static_cast<float>(renderer->GetRenderHeight()));
    frame_->vsm->PlanDistantLights(views.data(), count, camPos, camForward, pixelRadiusPerDepth,
                                   renderer->GetTotalFrameNumber());
}

void SceneRenderer::Pass_VsmPageRender(Renderer* renderer, RenderGraphPassContext ctx,
    const VirtualShadowMap::PageRenderDecisions& dec)
{
    // Rung 2 / Step 22: render casters into the resident VSM pages. Builds the LOCAL shadow views
    // (spots then point faces — same slot layout as Pass_VsmPageRequest), then RecordPageRender
    // does the GPU per-page setup + per-page ExecuteIndirect into the pool (DEPTH_WRITE via graph).
    // pass-flow S3: no gates here — the AddPass2 builder decided this pass runs, made the
    // declarations, and captured `dec`; a second decision here could only disagree.

    // Step 24e: clipmap views sit at vsm::ClipmapView(level) so the setup builds their per-page projection from
    // gViewProj[view]; every view renders its casters via its Rung-0 cull slot (rung0View = view + kNumCascades).
    // Plan S10b: the same list the request pass built (BuildVsmViews).
    std::vector<vsm::ViewProjEntry> views;
    const std::uint32_t slot = BuildVsmViews(*frame_, views);

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

ID3D12Resource* SceneRenderer::LocalShadowPoolResource() const
{
    return (frame_ && frame_->localShadowCache) ? frame_->localShadowCache->Pool() : nullptr;
}

D3D12_CPU_DESCRIPTOR_HANDLE SceneRenderer::LocalShadowPoolSrv(D3D12_CPU_DESCRIPTOR_HANDLE fallback) const
{
    if (!LocalShadowPoolResource()) { return fallback; }
    const D3D12_CPU_DESCRIPTOR_HANDLE srv = frame_->localShadowCache->PoolSrv();
    return srv.ptr != 0 ? srv : fallback;
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

// ---- S15 (SDSM): analyze, cull, draw --------------------------------------------------------
void SceneRenderer::Pass_SdsmAnalyze(Renderer* renderer, RenderGraphPassContext ctx,
    const SdsmShadows::AnalyzeDecisions& dec)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSdsmAnalyze);
        if (frame_->sdsm) { frame_->sdsm->RecordAnalyze(renderer, t.cl, dec); }
    }
    ctx.EndCL(t);
}

void SceneRenderer::Pass_SdsmCull(Renderer* renderer, RenderGraphPassContext ctx,
    const ShadowGpuData::DirectionalCullDecisions& dec)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassShadowCull);
        if (frame_->shadowGpu) { frame_->shadowGpu->RecordDirectionalCull(renderer, t.cl, dec); }
    }
    ctx.EndCL(t);
}

void SceneRenderer::Pass_SdsmMoments(Renderer* renderer, RenderGraphPassContext ctx,
    const SdsmShadows::MomentsDecisions& dec)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSdsmMoments);
        if (frame_->sdsm) { frame_->sdsm->RecordMoments(renderer, t.cl, dec); }
    }
    ctx.EndCL(t);
}

void SceneRenderer::Pass_SdsmCullPost(Renderer* renderer, RenderGraphPassContext ctx,
    const ShadowGpuData::CullPostDecisions& dec)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassShadowCullPost);
        if (frame_->shadowGpu && frame_->sdsm)
        {
            frame_->shadowGpu->RecordSdsmCullPost(renderer, t.cl, dec, frame_->sdsm->CullCbAddress());
        }
    }
    ctx.EndCL(t);
}

void SceneRenderer::Pass_SdsmShadow(Renderer* renderer, RenderGraphPassContext ctx,
    std::uint32_t atlasPoint, bool passB)
{
    ShadowGpuData* shadowGpu = frame_->shadowGpu;
    SdsmShadows* sdsm = frame_->sdsm;
    // NO EARLY RETURN before the point below: this pass declared the atlas DEPTH_WRITE, and a
    // body that skips its own marker leaves that transition unrecorded while the compile believes
    // it happened. The builder already gated on both pointers, so this is belt and braces -- but
    // it is the belt the comparator checks.

    // ONE list for all four partitions, unlike Pass_CSM's fan-out. The per-cascade workers exist
    // there because the CPU tail (RenderShadow per object) is the expensive half; here every draw
    // is an ExecuteIndirect whose instance count the GPU decided, so the recording is four binds
    // and a handful of indirect calls -- nothing to spread over threads.
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSdsmShadow);
        renderer->EmitPoint(t.cl, atlasPoint);
        // S20: only the partitions that RENDER this frame; a cached one keeps its tile as it is,
        // and the analyze already gave its cull rows nothing to draw. ~0 with the cache off.
        const std::uint32_t renderMask = sdsm ? sdsm->FrameParams().updateMask : ~0u;
        // Pass A clears what it renders, exactly as Pass_CSM clears the whole atlas: the S5 gutter
        // has to keep the clear value (1.0 = lit) or a filter tap that lands in it reads a stale
        // shadow. Every tile re-rendered = the whole-atlas clear, as before. Pass B draws into the
        // SAME tiles and must not clear them.
        if (!passB) { renderer->ClearShadowTiles(t.cl, renderMask); }

        const std::uint32_t partitions = (shadowGpu && sdsm)
            ? std::min<std::uint32_t>(sdsm->FrameParams().partitions, render::sdsm::kMaxPartitions)
            : 0u;
        for (std::uint32_t p = 0; p < partitions; ++p)
        {
            if (((renderMask >> p) & 1u) == 0u) { continue; }
            // Partition p -> atlas tile p -> args row p: the same identity a cascade index carries,
            // which is what lets the whole indirect path stay unchanged.
            renderer->BindShadowTarget(t.cl, static_cast<int>(p), /*clear=*/false);
            // No scissor (S11): the partition box IS the tight rect, so there is nothing left for a
            // view-cone scissor to cut -- and a rect computed from a CPU cascade would be wrong here.
            //
            // The projection and the bias come from the block the ANALYSIS wrote, bound by address
            // as an ordinary root CBV. Pass B uses the same block: a different bias between the two
            // halves of one tile would seam the shadow down the middle.
            shadowGpu->RecordIndirectShadowDraws(renderer, t.cl, p, sdsm->ViewCbAddress(p), passB);
        }
    }
    ctx.EndCL(t);
}

const Profiler::ScopeNameKey kShadows1 = Profiler::RegisterTraceLiteral(L"SpotShadows1");
const Profiler::ScopeNameKey kShadows2 = Profiler::RegisterTraceLiteral(L"SpotShadows2");
// ---- Occlusion plan S5b: the cascades' light-space two-pass HZB cull ----
// Pass A is Pass_CSM as before. Then: the tile pyramids from the atlas (this), the deferred
// casters retested against them (Pass_ShadowCullPost), and pass B into the same tiles
// (Pass_CSMPost). Every point the builder declared is emitted whatever the data says.
void SceneRenderer::Pass_CsmHzb(Renderer* renderer, RenderGraphPassContext ctx, std::uint32_t point)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassCsmHzb);
        const auto& D = renderer->GetDeferredForFrame();
        // The atlas readable, the pyramids writable.
        renderer->EmitPoint(t.cl, point);
        if (frame_->shadowGpu)
        {
            const UINT tile = D.shadowRes / 2u;
            frame_->shadowGpu->CascadeHzbRef().RecordBuild(renderer, t.cl, D.shadowSRV, tile, render::kCascadeAtlasBorder);
        }
        // ...and the pyramids readable for the post cull and next frame's main cull.
        renderer->EmitPoint(t.cl, point + 1u);
    }
    ctx.EndCL(t);
}

void SceneRenderer::Pass_ShadowCullPost(Renderer* renderer, RenderGraphPassContext ctx,
    const ShadowGpuData::CullPostDecisions& dec)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassShadowCullPost);
        if (frame_->shadowGpu) { frame_->shadowGpu->RecordCullPost(renderer, t.cl, dec); }
    }
    ctx.EndCL(t);
}

void SceneRenderer::Pass_CSMPost(Renderer* renderer, RenderGraphPassContext ctx,
    const std::array<SceneView, kCascades>& cascadeViews, std::uint32_t atlasPoint)
{
    // One list for all four cascades: pass B is the bad guesses of the main cull, a handful of
    // casters on a settled frame, so there is nothing to fan out.
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassCSMPost);
        renderer->EmitPoint(t.cl, atlasPoint);
        ShadowGpuData* shadowGpu = frame_->shadowGpu;
        const CascadeShadowConfig csmCfg = frame_->cascadeConfig ? *frame_->cascadeConfig : CascadeShadowConfig{};
        for (std::size_t c = 0; c < cascadeViews.size(); ++c)
        {
            const SceneView& view = cascadeViews[c];
            if (!view.frustum.IsValid()) { continue; }
            // The same bias and scissor as pass A: pass B draws into the same tile with the
            // same projection, and a different bias would seam the two halves of one shadow.
            const CascadeDepthBias cb = ComputeCascadeDepthBias(csmCfg, frame_->cascades.depthBiasNDC[c]);
            const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildShadowViewCB(renderer, view.view, view.proj, frame_->wind,
                                                                       cb.constBias, cb.slopeBias, cb.maxSlope, cb.clampNear);
            const auto& sr = frame_->cascades.scissor[c];
            const D3D12_RECT scRect{ sr.x0, sr.y0, sr.x1, sr.y1 };
            renderer->BindShadowTarget(t.cl, static_cast<int>(c), /*clear=*/false,
                                       csmCfg.scissorOptim ? &scRect : nullptr);
            if (shadowGpu)
            {
                shadowGpu->RecordIndirectShadowDraws(renderer, t.cl, static_cast<std::uint32_t>(c), viewCB, /*passB=*/true);
            }
        }
    }
    ctx.EndCL(t);
}

namespace
{
// Local-light plan S7: a view copies its statics from the cache pool when the pass was told the cache is usable this
// frame and the view hit, or missed and the cache pass filled it.
bool LocalViewCached(const SceneView& view, const render::LocalShadowCache* cache, bool cached)
{
    return cached && cache && view.cachePage >= 0 &&
           (view.cacheMode == 1 || (view.cacheMode == 2 && cache->FillsRan()));
}

// S7: the view's rectangle as the target, then its depth before the movable casters -- the cached statics copied in
// (UE CopyCachedShadowMap) or a clear to far.
void PrepareLocalShadowTarget(Renderer* renderer, ID3D12GraphicsCommandList* cl, render::LocalShadowCache* cache,
                              const SceneView& view, bool pointPool, UINT res, bool copyStatics)
{
    renderer->BindLocalShadowTarget(cl, pointPool, view.shadowPage, view.shadowX, view.shadowY, res,
                                    /*clearDepth=*/!copyStatics);
    if (!copyStatics) { return; }
    Material* mat = cache->CopyMaterial(nullptr); // built by the pass's builder (main thread); never created here
    if (!mat) { return; }
    struct CopyCB { std::int32_t dx, dy; std::uint32_t page, pad; };
    const CopyCB c{ static_cast<std::int32_t>(view.cacheX) - static_cast<std::int32_t>(view.shadowX),
                    static_cast<std::int32_t>(view.cacheY) - static_cast<std::int32_t>(view.shadowY),
                    static_cast<std::uint32_t>(view.cachePage), 0u };
    auto cb = renderer->GetFrameResource()->AllocDynamic(static_cast<UINT>(sizeof(CopyCB)), render::kConstantBufferAlignment);
    if (!cb.cpu) { return; }
    std::memcpy(cb.cpu, &c, sizeof(c));
    auto h = renderer->GetRenderContextPool()->Acquire();
    RenderContext& rc = h.ref();
    rc.cbv[0] = cb.gpu;
    rc.srvTable[0] = renderer->StageSrvUavTable({ cache->PoolSrv() }).gpu;
    mat->Bind(cl, rc, false);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->DrawInstanced(3u, 1u, 0u, 0u);
}
} // namespace

void SceneRenderer::Pass_LocalShadowCache(Renderer* renderer, RenderGraphPassContext ctx, size_t spotCount,
    size_t faceCount, std::uint32_t fillPoint, std::uint32_t readPoint)
{
    // Local-light plan S7: every miss's STATIC casters (the cull's split rows) into its square of the pool, cleared
    // first -- UE SDCM_StaticPrimitivesOnly. One list: the pool is one resource, and the spot / point lists that copy
    // from it record in parallel after this one is submitted.
    render::LocalShadowCache* cache = frame_->localShadowCache;
    ShadowGpuData* shadowGpu = frame_->shadowGpu;
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassLocalShadowCache);
        renderer->EmitPoint(t.cl, fillPoint);
        if (cache && shadowGpu)
        {
            const auto& D = renderer->GetDeferredForFrame();
            const auto fill = [&](const SceneView& view, std::uint32_t slot, D3D12_GPU_VIRTUAL_ADDRESS viewCB, bool linear)
            {
                render::LocalShadowCache::Rect r{};
                r.page = view.cachePage;
                r.x = view.cacheX;
                r.y = view.cacheY;
                r.size = view.shadowRes;
                cache->BindTarget(t.cl, r, /*clear=*/true);
                shadowGpu->RecordIndirectShadowDraws(renderer, t.cl, slot, viewCB, /*passB=*/false, /*staticRows=*/true,
                                                     linear);
            };
            const bool spotLinear = render::SpotShadowLinearDepth();   // plan S4: the spot pass's own encoding
            const bool pointLinear = render::PointShadowLinearDepth(); // ...and the point pass's
            // render::g_localShadowWindRange: a light past it caches its swaying casters too -- rigid (no wind state).
            const auto viewWind = [this](const SceneView& view) { return view.windRigid ? nullptr : frame_->wind; };
            for (size_t i = 0; i < spotCount; ++i)
            {
                const SceneView& view = (*frame_->spotShadowViews)[i];
                if (view.cacheMode != 2 || view.cachePage < 0) { continue; }
                const UINT res = ShadowViewRes(view, D.spotShadowRes);
                fill(view, render::ShadowViewSpotSlot(static_cast<unsigned>(i)),
                     BuildSpotShadowViewCB(renderer, view, viewWind(view), res, spotLinear), spotLinear); // the spot pass's bias
            }
            for (size_t i = 0; i < faceCount; ++i)
            {
                const SceneView& view = (*frame_->pointShadowViews)[i];
                if (view.cacheMode != 2 || view.cachePage < 0) { continue; }
                fill(view, render::ShadowViewPointFaceSlot(static_cast<unsigned>(i)),
                     BuildPointShadowViewCB(renderer, view, viewWind(view), pointLinear), pointLinear);
            }
        }
        renderer->EmitPoint(t.cl, readPoint);
    }
    ctx.EndCL(t);
}

void SceneRenderer::Pass_SpotShadows(Renderer* renderer, RenderGraphPassContext ctx,
    const std::vector<SceneView>& spotViews,
    size_t viewCount, std::uint32_t atlasPoint, bool indirect, bool cached)
{
    // pass-flow S6: the VSM-mode skip (Step 24c: in VSM mode the spot atlas is a 1x1 placeholder
    // and local shadows come from the VSM pool) and the light-count clamp are the BUILDER's — it
    // declares nothing on the frames this pass has no work, instead of the two sides agreeing by
    // discipline.
    const auto& D = renderer->GetDeferredForFrame();
    // Plan S8: the CPU path's casters at their RECEIVER's LOD, chunks at their camera tier and culled by the view --
    // Pass_CSM's rule (ReceiverCasterLod); they drew LOD 0 here. By value: the workers have no `this`.
    const Math::float3 localCamPos = frame_->camera ? frame_->camera->GetPosition() : Math::float3(0.0f, 0.0f, 0.0f);

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
    render::LocalShadowCache* const cache = frame_->localShadowCache; // S7
    const bool split = indirect && shadowGpu && shadowGpu->LocalSplitFrame();
    // Local-light plan S6b: several views per command list (render::g_localShadowViewsPerList). A list of one tiny view
    // cost the GPU ~8 us on its own -- its boundary, its heaps, its profiler timestamps -- which on demo (39 views) was
    // most of what local shadows cost; UE renders a whole atlas in one pass.
    const size_t perList = std::max<size_t>(render::g_localShadowViewsPerList, 1u);
    const size_t lists = (viewCount + perList - 1) / perList;
    // Plan S4: read ONCE -- the view CBs and the PSOs of every list must agree (and the light buffer's flag with them).
    const bool linear = render::SpotShadowLinearDepth();
    auto renderSpotShadow = [renderer, &D, &spotViews, batchIndex = ctx.batchIndex, shadowGpu, indirect, wind, cmpLog, cmpBarriers, atlasPoint,
                             cache, cached, split, perList, viewCount, localCamPos, linear](std::size_t listIndex)
    {
        Renderer::TransitionLogScope cmpScope(cmpLog);
        Renderer::CompiledBarrierScope cmpBarrierScope(cmpBarriers);
        const size_t first = listIndex * perList;
        const size_t last = std::min(std::min(first + perList, viewCount), spotViews.size());
        if (first >= last)
        {
            return;
        }

        CPU_SCOPE(ProfilerScopes::kSpotShadowPerLight);
        auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        SetCommandListName(t.cl, RenderPass::Main_SpotShadows);
        {
            GPU_SCOPE(t.cl, ProfilerScopes::kPassSpotShadow);
            // Only the FIRST list moves the atlas. Recording order across the fan-out is
            // nondeterministic; SUBMISSION order is localOrder = listIndex. Letting every list
            // transition meant the barrier the flip actually emitted landed in whichever list
            // recorded first, i.e. possibly AFTER another list's ClearDepthStencilView had already
            // run on the queue — D3D12 errors 527/538 on the atlas. The tracker hid this by
            // stitching acquire barriers at submit time, where the real order is known.
            if (listIndex == 0)
            {
                renderer->EmitPoint(t.cl, atlasPoint);
            }
            for (size_t lightIndex = first; lightIndex < last; ++lightIndex)
            {
                const SceneView& view = spotViews[lightIndex];
                if (view.directRead) { continue; } // S7: its receivers read the cache pool; nothing to draw here
                // Plan S4: the view's own square of the slice, and the S1 bias for that resolution.
                const UINT res = ShadowViewRes(view, D.spotShadowRes);
                // Plan S9: the view's square of its atlas page. S7: its cached statics copied in, or a clear.
                const bool copyStatics = LocalViewCached(view, cache, cached);
                PrepareLocalShadowTarget(renderer, t.cl, cache, view, /*pointPool=*/false, res, copyStatics);
                // S1, S4; render::g_localShadowWindRange: a light past it draws its casters rigid.
                const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildSpotShadowViewCB(renderer, view, view.windRigid ? nullptr : wind,
                                                                               res, linear);
                const auto& visibleBuckets = view.queue.VisibleBuckets();
                const auto& opaqueSimple = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueSimple)];
                const auto& opaqueComplex = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueComplex)];

                if (indirect)
                {
                    // Spot slot i -> its cull-view slot (plan S9: VSM's prefix, then the atlas modes' extra spots).
                    const std::uint32_t viewSlot = render::ShadowViewSpotSlot(static_cast<unsigned>(lightIndex));
                    // S7: on a split frame the main rows hold the movables only; an uncached view draws its statics too.
                    if (split && !copyStatics) { shadowGpu->RecordIndirectShadowDraws(renderer, t.cl, viewSlot, viewCB, false, true, linear); }
                    shadowGpu->RecordIndirectShadowDraws(renderer, t.cl, viewSlot, viewCB, false, false, linear);
                    // GPU-instanced casters: skip when the GI folding path is active (Ctrl+G) — the
                    // indirect cull draws them; otherwise (flag off / over-cap / PSO failure) draw here.
                    for (auto* obj : opaqueSimple)  { if (obj && obj->IsGpuInstancedCaster() && !shadowGpu->IsGiFoldedActive(obj)) obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB, 0, false, nullptr, linear); }
                    for (auto* obj : opaqueComplex) { if (obj && obj->IsGpuInstancedCaster() && !shadowGpu->IsGiFoldedActive(obj)) obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB, 0, false, nullptr, linear); }
                }
                else
                {
                    for (auto* obj : opaqueSimple)
                    {
                        if (obj)
                        {
                            obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB,
                                ReceiverCasterLod(obj, localCamPos), /*chunkCameraLods=*/true, &view.frustum, linear); // S8
                        }
                    }
                    for (auto* obj : opaqueComplex)
                    {
                        if (obj)
                        {
                            obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB,
                                ReceiverCasterLod(obj, localCamPos), /*chunkCameraLods=*/true, &view.frustum, linear); // S8
                        }
                    }
                }
            }
        }
        // The list index is the deterministic submit order within the batch.
        renderer->EndThreadCommandList(t, batchIndex, static_cast<uint32_t>(listIndex));
    };

    TaskSystem::Get().DispatchWait(lists, renderSpotShadow, 1);
#else
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSpotShadow);
        renderer->EmitPoint(t.cl, atlasPoint);
        const bool linear = render::SpotShadowLinearDepth(); // plan S4

        for (size_t lightIndex = 0; lightIndex < viewCount; ++lightIndex)
        {
            const SceneView& view = spotViews[lightIndex];
            if (view.directRead) { continue; } // S7: read from the cache pool
            const UINT res = ShadowViewRes(view, D.spotShadowRes); // S4
            renderer->BindLocalShadowTarget(t.cl, /*pointPool=*/false, view.shadowPage, view.shadowX, view.shadowY, res,
                                            /*clearDepth=*/true); // S9
            const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildSpotShadowViewCB(renderer, view,
                view.windRigid ? nullptr : frame_->wind, res, linear); // S1; the wind range
            const auto& visibleBuckets = view.queue.VisibleBuckets();
            const auto& opaqueSimple = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueSimple)];
            const auto& opaqueComplex = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueComplex)];

            for (auto* obj : opaqueSimple)
            {
                if (obj)
                {
                    obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB,
                        ReceiverCasterLod(obj, localCamPos), /*chunkCameraLods=*/true, &view.frustum, linear); // S8
                }
            }
            for (auto* obj : opaqueComplex)
            {
                if (obj)
                {
                    obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB,
                        ReceiverCasterLod(obj, localCamPos), /*chunkCameraLods=*/true, &view.frustum, linear); // S8
                }
            }
        }
    }
    renderer->EndThreadCommandList(t, ctx.batchIndex);
#endif
}

void SceneRenderer::Pass_PointShadows(Renderer* renderer, RenderGraphPassContext ctx,
    const std::vector<SceneView>& pointViews,
    size_t viewCount, std::uint32_t atlasPoint, bool indirect, bool cached)
{
    // pass-flow S6: same as Pass_SpotShadows — the VSM-mode skip (Step 24c: VSM renders point
    // shadows into the pool, the cube atlas is a 1x1 placeholder) and the 6-faces-per-light
    // clamp are the builder's single decision. 6 cube faces per shadowed point light; each face
    // is its own depth-array slice (its own DSV), so faces render independently, with the
    // flattened face index as the "slice" (cubeSlot = idx/6, face = idx%6).
    const auto& D = renderer->GetDeferredForFrame();
    // Plan S8: the CPU path's casters at their receiver's LOD, as Pass_SpotShadows.
    const Math::float3 localCamPos = frame_->camera ? frame_->camera->GetPosition() : Math::float3(0.0f, 0.0f, 0.0f);

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
    render::LocalShadowCache* const cache = frame_->localShadowCache; // S7
    const bool split = indirect && shadowGpu && shadowGpu->LocalSplitFrame();
    // Plan S6b: several faces per command list, as the spot pass.
    const size_t perList = std::max<size_t>(render::g_localShadowViewsPerList, 1u);
    const size_t lists = (viewCount + perList - 1) / perList;
    // Plan S4: read ONCE, as the spot pass -- the face CBs, the PSOs and the light buffer's flag agree.
    const bool linear = render::PointShadowLinearDepth();
    auto renderPointShadow = [renderer, &D, &pointViews, batchIndex = ctx.batchIndex, shadowGpu, indirect, wind, cmpLog, cmpBarriers, atlasPoint,
                              cache, cached, split, perList, viewCount, localCamPos, linear](std::size_t listIndex)
    {
        Renderer::TransitionLogScope cmpScope(cmpLog);
        Renderer::CompiledBarrierScope cmpBarrierScope(cmpBarriers);
        const size_t first = listIndex * perList;
        const size_t last = std::min(std::min(first + perList, viewCount), pointViews.size());
        if (first >= last)
        {
            return;
        }

        CPU_SCOPE(ProfilerScopes::kSpotShadowPerLight);
        auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        SetCommandListName(t.cl, RenderPass::Main_PointShadows);
        {
            GPU_SCOPE(t.cl, ProfilerScopes::kPassPointShadow);
            // See Pass_SpotShadows: the atlas barrier must be recorded into the list submitted
            // first (localOrder = listIndex), not into whichever list records first.
            if (listIndex == 0)
            {
                renderer->EmitPoint(t.cl, atlasPoint);
            }
            for (size_t faceIndex = first; faceIndex < last; ++faceIndex)
            {
                const SceneView& view = pointViews[faceIndex];
                if (view.directRead) { continue; } // S7: its receivers read the cache pool; nothing to draw here
                // Plan S9: the face's square inside its light's 3f x 2f block of an atlas page. S7: its cached statics
                // copied in, or a clear.
                const bool copyStatics = LocalViewCached(view, cache, cached);
                PrepareLocalShadowTarget(renderer, t.cl, cache, view, /*pointPool=*/true, ShadowViewRes(view, D.pointShadowRes),
                                         copyStatics);
                // S4; render::g_localShadowWindRange: a light past it draws its casters rigid.
                const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildPointShadowViewCB(renderer, view, view.windRigid ? nullptr : wind,
                                                                                linear);
                const auto& visibleBuckets = view.queue.VisibleBuckets();
                const auto& opaqueSimple = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueSimple)];
                const auto& opaqueComplex = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueComplex)];

                if (indirect)
                {
                    // Point cube face k -> its cull-view slot (plan S9: VSM's prefix, then the extra faces).
                    const std::uint32_t viewSlot = render::ShadowViewPointFaceSlot(static_cast<unsigned>(faceIndex));
                    // S7: on a split frame the main rows hold the movables only; an uncached face draws its statics too.
                    if (split && !copyStatics) { shadowGpu->RecordIndirectShadowDraws(renderer, t.cl, viewSlot, viewCB, false, true, linear); }
                    shadowGpu->RecordIndirectShadowDraws(renderer, t.cl, viewSlot, viewCB, false, false, linear);
                    // GPU-instanced casters: skip when the GI folding path is active (Ctrl+G) — the
                    // indirect cull draws them; otherwise (flag off / over-cap / PSO failure) draw here.
                    for (auto* obj : opaqueSimple)  { if (obj && obj->IsGpuInstancedCaster() && !shadowGpu->IsGiFoldedActive(obj)) obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB, 0, false, nullptr, linear); }
                    for (auto* obj : opaqueComplex) { if (obj && obj->IsGpuInstancedCaster() && !shadowGpu->IsGiFoldedActive(obj)) obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB, 0, false, nullptr, linear); }
                }
                else
                {
                    for (auto* obj : opaqueSimple)
                    {
                        if (obj)
                        {
                            obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB,
                                ReceiverCasterLod(obj, localCamPos), /*chunkCameraLods=*/true, &view.frustum, linear); // S8
                        }
                    }
                    for (auto* obj : opaqueComplex)
                    {
                        if (obj)
                        {
                            obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB,
                                ReceiverCasterLod(obj, localCamPos), /*chunkCameraLods=*/true, &view.frustum, linear); // S8
                        }
                    }
                }
            }
        }
        renderer->EndThreadCommandList(t, batchIndex, static_cast<uint32_t>(listIndex));
    };

    TaskSystem::Get().DispatchWait(lists, renderPointShadow, 1);
#else
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSpotShadow);
        renderer->EmitPoint(t.cl, atlasPoint);
        const bool linear = render::PointShadowLinearDepth(); // plan S4

        for (size_t faceIndex = 0; faceIndex < viewCount; ++faceIndex)
        {
            const SceneView& view = pointViews[faceIndex];
            if (view.directRead) { continue; } // S7: read from the cache pool
            renderer->BindLocalShadowTarget(t.cl, /*pointPool=*/true, view.shadowPage, view.shadowX, view.shadowY,
                                            ShadowViewRes(view, D.pointShadowRes), /*clearDepth=*/true); // S9
            const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildPointShadowViewCB(renderer, view,
                view.windRigid ? nullptr : frame_->wind, linear); // S4; the wind range
            const auto& visibleBuckets = view.queue.VisibleBuckets();
            const auto& opaqueSimple = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueSimple)];
            const auto& opaqueComplex = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueComplex)];

            for (auto* obj : opaqueSimple)
            {
                if (obj)
                {
                    obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB,
                        ReceiverCasterLod(obj, localCamPos), /*chunkCameraLods=*/true, &view.frustum, linear); // S8
                }
            }
            for (auto* obj : opaqueComplex)
            {
                if (obj)
                {
                    obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB,
                        ReceiverCasterLod(obj, localCamPos), /*chunkCameraLods=*/true, &view.frustum, linear); // S8
                }
            }
        }
    }
    renderer->EndThreadCommandList(t, ctx.batchIndex);
#endif
}
