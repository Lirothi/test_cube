#include "app/scene/SceneRenderer.h"
#include "core/logging/Log.h"

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

void SceneRenderer::InitializeCommonResources(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    resources_.Initialize(renderer, uploadCmdList, uploadKeepAlive);
    // R3: the bloom reads materials and CB sizes off the bootstrapper for the life of the
    // renderer, so it takes the pointer once rather than being handed it per call.
    bloom_.Initialize(&resources_);
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
    rtAs_.Reset();
    decisions_.reflectionTemporal = false;
    ssrHistoryValid_ = false;
    ssrHistoryFrames_ = 0u;
    ssrHistoryWidth_ = 0u;
    ssrHistoryHeight_ = 0u;
    ssrSceneColorHistoryValid_ = false;
    ssrSceneColorHistoryFrames_ = 0u;
    ssrSceneColorHistoryWidth_ = 0u;
    ssrSceneColorHistoryHeight_ = 0u;
    ssrSceneColorCameraRevision_ = 0u;
    frame_ = nullptr;
}

void SceneRenderer::InvalidateRaytracing()
{
    // RT-only subset of Reset() — keep materials/handles, rebuild the acceleration structures +
    // bindless geom-info next RT frame. The per-frame register loop (GetOrUpdateMesh) re-runs
    // and re-reads current material SRVs after this clear.
    rtAs_.Invalidate();
}

// Barrier plan step 4: everything the pass bodies used to create lazily, created here instead —
// once per frame, before the render graph is built and therefore before anything records.
//
// Two reasons. A pass's Prepare callback can only register a resource that already exists, which
// is what the rest of the plan is built on. And a lazy grow inside a recording body FREES the
// previous allocation while earlier in-flight frames may still be reading it — that is exactly
// how the spot/point light buffers produced the DXGI_DEVICE_HUNG the --scene-stress harness was
// written to catch. Growing here, before any command list is open, removes the class of bug
// rather than working around it per resource.
//
// Everything called here is idempotent and cheap when nothing changed.
void SceneRenderer::EnsureFrameResources(Renderer* renderer)
{
    if (!renderer || !frame_) { return; }

    if (frame_->shadowGpu)
    {
        // Was lazy inside RecordCull. Prepare needs the GI-scatter PSO to already exist,
        // because IsGiIndirectActive() gates on it and decides whether the GI instance
        // buffers get registered at all.
        frame_->shadowGpu->EnsureShaderResources(renderer);
    }

    if (frame_->vsm)
    {
        frame_->vsm->EnsureFrameResources(renderer, frame_->shadowGpu);
    }

    if (frame_->ocean)
    {
        // Was lazy inside OceanSimulation::Update, i.e. inside Main_ObjectCompute's RECORD body.
        frame_->ocean->EnsureSimulationResources(renderer);
    }

    if (frame_->lightManager)
    {
        // Same counts the passes derive; LightManager's light set does not change during Render.
        LightManager& lm = *frame_->lightManager;
        const size_t spots = lm.GetSpotLightCount();
        if (spots > 0) { lm.EnsureSpotLightBuffer(renderer, spots); }
        const size_t points = lm.PointLights().size();
        if (points > 0) { lm.EnsurePointLightBuffer(renderer, points); }

        // The GPU light buffers are CPU-filled HERE, before the graph runs, not in the lighting
        // pass bodies any more (async prep): Pass_RTTrace consumes them and must not depend on
        // the lighting passes' RECORD -- on the compute queue it could not. Everything read here
        // is settled by now: the light set is frozen for the frame and the shadow slots were
        // assigned in Scene's update (SelectShadowedSpots/Points). This frame slot's fence was
        // waited at BeginFrame, so writing the persistently-mapped ring is safe.
        const UINT frameIdx = renderer->GetCurrentFrameIndex();
        if (auto* spotCPU = lm.GetSpotLightBufferCPU(frameIdx); spotCPU && spots > 0)
        {
            const auto& spotLights = lm.SpotLights();
            for (size_t i = 0; i < spots; ++i)
            {
                const auto& light = spotLights[i];
                const auto& desc = light.GetDesc();
                spotCPU[i].positionRange = Math::float4(desc.position, desc.range);
                spotCPU[i].directionCosOuter = Math::float4(light.GetDirection(), light.GetCosOuter());
                spotCPU[i].colorIntensity =
                    Math::float4(desc.color, render::CandelaFromLumens(desc.luminousFluxLm)); // P16.5
                spotCPU[i].shadowParams = Math::float4(light.GetCosInner(),
                    static_cast<float>(lm.GetSpotShadowSlot(i)), light.GetInvAngleRange(),
                    light.GetShadowDepthBias());
                spotCPU[i].shadowParams2 = Math::float4(light.GetShadowNormalBias(), 0.0f, 0.0f, 0.0f);
                spotCPU[i].viewProj = light.GetViewProjMatrix();
            }
        }
        if (auto* pointCPU = lm.GetPointLightBufferCPU(frameIdx); pointCPU && points > 0)
        {
            const auto& pointLights = lm.PointLights();
            for (size_t i = 0; i < points; ++i)
            {
                const auto& desc = pointLights[i].GetDesc();
                pointCPU[i].position = desc.position;
                pointCPU[i].radius = desc.radius;
                pointCPU[i].color = desc.color;
                pointCPU[i].intensity = render::CandelaFromLumens(desc.luminousFluxLm); // P16.5
                // Per-light cube-shadow params = (slot/-1, worldDepthBias, near, far=radius).
                // near MUST match Scene.cpp's cube-face projection EXACTLY — PointShadowFactor
                // reconstructs the compare depth from it. Bias is WORLD-space (B4 tuning).
                const float pointShadowNear = std::max(0.2f, desc.radius * 0.02f);
                constexpr float kPointShadowBias = 0.10f; // world units
                pointCPU[i].shadowParams = Math::float4(
                    static_cast<float>(lm.GetPointShadowSlot(i)),
                    kPointShadowBias, pointShadowNear, desc.radius);
            }
        }
    }
}

// R6: every decision this frame takes, taken ONCE, here, before a single pass is registered.
//
// What used to be here was a mix: five members, eight locals, and three blocks of cross-frame
// history bookkeeping, interleaved with the graph construction that reads them. The split is by
// LIFETIME, not by topic — `decisions_` dies with the frame, the members it reads and updates
// (the SSR and VSM histories, the pre-exposure pair) are exactly the things that do not.
//
// Runs after EnsureFrameResources, so the deferred targets it queries are this frame's, and before
// the graph is built, so no builder can observe a half-decided frame.
void SceneRenderer::DecideFrame(Renderer* renderer, const SceneFrameData& frame)
{
    decisions_ = FrameDecisions{};

    // Reflection source (S8) + RT debug viz (S6), gated on hardware support. RT
    // reflections fall back to SSR on non-RT hardware (rtReflect stays false, so
    // the screen-space reflection source runs). The AS is built only when RT reflections or the debug
    // viz need it; otherwise the frame is byte-identical to the SSR/None/SkyOnly path.
    decisions_.rtSupported = renderer->IsRaytracingSupported();
    // S13: once an AS/table allocation has failed (low VRAM / descriptor exhaustion), disable RT for
    // the rest of this scene and fall back to SSR — cleanly, never a crash. Sticky
    // until the next level (rtAs_.Manager().Reset clears it).
    const bool rtFailed = rtAs_.Manager().BuildFailed() || rtAs_.Bindless().BuildFailed();
    if (rtFailed && !rtFailureLogged_)
    {
        // Once per scene via the flag (it resets with the AS manager on a level change), which is
        // narrower than LOG_WARNING_ONCE's once-per-process.
        LOG_WARNING(logging::LogCategory::RenderRt,
                    "acceleration-structure or bindless-table allocation failed; disabling RT, falling back to SSR");
        rtFailureLogged_ = true;
    }
    decisions_.rtDebugView = decisions_.rtSupported && !rtFailed && frame.settings.rtDebugView;
    decisions_.rtReflect = decisions_.rtSupported && !rtFailed &&
                           frame.settings.reflectionSource == ReflectionSource::RT;
    decisions_.clearReflections = frame.settings.reflectionSource == ReflectionSource::None ||
                                  frame.settings.reflectionSource == ReflectionSource::SkyOnly;
    decisions_.rtBuildAS = decisions_.rtReflect || decisions_.rtDebugView;
    // S15b: glass gets traced reflections in SSR/RT modes. SkyOnly and None skip
    // the glass reflection prepass; the forward shader uses the cubemap only in
    // SkyOnly and suppresses it in None via lightCounts.w.
    decisions_.glassRefl = !decisions_.clearReflections;
    // NOTHING READS THE CLOSEST PYRAMID ANY MORE, so it is not built. The stackless HiZ traversal
    // was its only consumer and it is gone: Unreal's own SSR marches the FURTHEST chain (the one
    // GTAO already builds) at a fixed mip, and a `max`-reduced chain answers a question no pass
    // now asks. The target, its descriptors and the shader's `writeClosest` path all stay --
    // P9's screen-space GI is the next consumer and wants exactly this chain.
    decisions_.ssrHiz = false;
    // P8 bloom. Gated on everything the body needs, not just the setting: the material and its CB
    // have to exist, the pyramid has to have been created, and a zero intensity is the plan's
    // "schedules no unnecessary active work" -- with it off nothing is dispatched and the tonemap
    // reads a literal 0 for the term.
    {
        // P16.1 -- PRE-EXPOSURE, decided here, once, for the same reason the bloom's method is:
        // several passes have to agree on it and a disagreement is a uniform brightness error that
        // reads as a tuning problem. It is the multiplier the tonemap applies just before the tone
        // curve, taken from the PREVIOUS frame's adapted exposure -- this frame's value is derived
        // FROM scene colour, and scene colour is what is about to be scaled by it.
        {
            prevPreExposure_ = preExposure_; // what last frame's scene colour was stored with
            preExposure_ = 1.0f;
            ExposureMetering& metering = renderer->Exposure();
            if (render::g_preExposureEnabled && frame.cameraExposure.enabled && metering.IsReady())
            {
                const float ev = metering.LatestReadback().adaptedEv100;
                if (std::isfinite(ev))
                {
                    const float m = render::ExposureMultiplierFromEv100(ev);
                    if (std::isfinite(m) && m > 0.0f) { preExposure_ = m; }
                }
            }
            render::g_preExposure = preExposure_;
        }

        // R3: the whole bloom decision — the kernel asset, the bokeh bake and the three flags —
        // belongs to the subsystem that reads it. It is still taken HERE, once, before the graph
        // is built, for the reason it always was: the tonemap builder declares a different set of
        // resources per method, and a body that disagreed would emit a barrier the compile never
        // registered.
        bloom_.Decide(renderer, frame);
    }
    // REFLECTION TEMPORAL RESOLVE. Born for SSR (a screen-space march is violently sensitive to
    // its jittered start). RT was assumed immune -- true on the mirror-on-flat-floor scenes it was
    // judged on, false the moment reflected FOLIAGE became subpixel at half reflection res: 1
    // sharp ray/px of sub-texel fronds boils under DLSS jitter exactly like the raw SSR buffer
    // did (user-visible on the ssr_bronze_palms mirror). One resolve, one toggle, both sources;
    // None/SkyOnly dispatch nothing to filter. The history is per-frame-set
    // like the GTAO one, so it is only valid once a previous frame at THIS reflection size has
    // written it -- a resize or a level switch has to seed instead of reading garbage.
    decisions_.reflectionTemporal = frame.settings.ssrTemporal && !decisions_.clearReflections;
    {
        const UINT rw = renderer->GetReflectionTextureWidth();
        const UINT rh = renderer->GetReflectionTextureHeight();
        ssrHistoryValid_ = decisions_.reflectionTemporal && ssrHistoryFrames_ > 0u &&
                           ssrHistoryWidth_ == rw && ssrHistoryHeight_ == rh;
        ssrHistoryFrames_ = decisions_.reflectionTemporal
            ? ((ssrHistoryWidth_ == rw && ssrHistoryHeight_ == rh) ? ssrHistoryFrames_ + 1u : 1u)
            : 0u;
        ssrHistoryWidth_ = rw;
        ssrHistoryHeight_ = rh;
    }
    // P7: hand the ocean this frame's medium. Once per frame, from the one place that owns the
    // render settings, so the water and the opaque compose pass are always given the same numbers.
    if (frame.ocean)
    {
        const AtmospherePacked fog =
            PackAtmosphere(frame.settings.atmosphere, frame.dirLight != nullptr);
        frame.ocean->SetAtmosphereParams(fog.params0, fog.params1, fog.params2);
        frame.ocean->SetAtmosphereDebugView(g_atmosphereDebugView);
    }
    // UE's SSRT color resolve is a separate temporal consumer: after finding a hit in CURRENT
    // depth it reprojects that hit into the PREVIOUS temporal SceneColor. Our Deferred.scene is
    // produced every frame, so validity must not depend on the optional reflection temporal pass.
    // A cut revision is explicit; a resize and the first frame seed from current Light instead.
    {
        const UINT rw = renderer->GetRenderWidth();
        const UINT rh = renderer->GetRenderHeight();
        const uint64_t cameraRevision = frame.camera ? frame.camera->GetHistoryRevision() : 0u;
        const bool sameHistory = frame.camera && ssrSceneColorHistoryFrames_ > 0u &&
            ssrSceneColorHistoryWidth_ == rw && ssrSceneColorHistoryHeight_ == rh &&
            ssrSceneColorCameraRevision_ == cameraRevision;
        ssrSceneColorHistoryValid_ = sameHistory;
        ssrSceneColorHistoryFrames_ = frame.camera ? (sameHistory ? ssrSceneColorHistoryFrames_ + 1u : 1u) : 0u;
        ssrSceneColorHistoryWidth_ = rw;
        ssrSceneColorHistoryHeight_ = rh;
        ssrSceneColorCameraRevision_ = cameraRevision;
    }
    if (decisions_.rtBuildAS)
    {
        rtAs_.EnsureInit(renderer);
    }
    // S11's hand-rolled RT denoise (ReflectionHistory ping-pong + rt_reflection_denoise_cs) was
    // retired in S12 and DELETED once the shared reflection temporal resolve took over the RT
    // source too -- one resolve, both sources, gated by settings.ssrTemporal. Glossy still waits
    // on DLSS-RR (S16).

    // Step 24f-2: in VSM mode directional shadows come from the clipmap and the CSM cascade atlas is
    // a 1x1 placeholder — the Main_CSM pass is OMITTED entirely. (Adding it but skipping its
    // per-cascade command lists breaks the parallel-execution CL timeline the graph expects → GPU
    // hang, and its declared D.shadow->DEPTH_WRITE transition would go unrecorded.) The light passes
    // still declare the 1x1 D.shadow NON_PIXEL for their (unused) bind.
    decisions_.vsmActive = render::VsmActive() && frame.vsm && frame.vsm->IsAllocated();

    // Rung 2 / Step 22 skip-when-still. Skip the VSM update (request + alloc + render) only when
    // NOTHING changed — the camera view is unchanged AND no shadow caster moved. Then the pool +
    // page table persist and last frame's content is still valid (saving the dominant cost, the
    // page-render draw loop). Gating on movers is essential: a rotating caster must re-render its
    // shadow every frame even with a still camera (otherwise its shadow freezes). The view matrix
    // carries the camera transform with NO jitter (jitter lives in proj), so it is bit-stable when
    // the camera is still.
    if (render::VsmActive() && frame.mainView)
    {
        const bool viewStill = vsmHasRendered_ &&
            std::memcmp(&frame.mainView->view, &vsmLastView_, sizeof(mat4)) == 0;
        // W5: a wind-swayed caster animates in the VERTEX shader, so its transform never changes and
        // MoverCount() stays 0 — without this the pool freezes after a few still frames and the palm
        // shadow stops swaying while the tree keeps going (the shadow visibly detaches).
        const bool windAnimating = frame.wind && frame.wind->swayAmplitude > 0.0f &&
                                   frame.shadowGpu && frame.shadowGpu->HasWindCasters();
        const bool contentStill = (!frame.shadowGpu || frame.shadowGpu->MoverCount() == 0) &&
                                  !windAnimating;
        if (viewStill && contentStill)
        {
            // Keep rendering for a few frames after everything goes still so the resident set + the
            // physOwner readback snapshot (kFrameCount-latent) catch up before we freeze the pool —
            // otherwise a just-stopped camera freezes a still-incomplete render.
            if (vsmStillFrames_ < 0xFFFFu) { ++vsmStillFrames_; }
            decisions_.vsmSkipUpdate = vsmStillFrames_ > render::kFrameCount + 1u;
        }
        else
        {
            vsmStillFrames_ = 0;
            vsmLastView_ = frame.mainView->view;
            vsmHasRendered_ = true;
        }
    }
    else
    {
        vsmHasRendered_ = false;
        vsmStillFrames_ = 0;
    }

    // DLSS-split: the PREDICTION, not the outcome. The evaluate itself can still decline in the
    // record (and tells the handler so, which backs the prediction off for the next frames); what
    // this decides is which image the tonemap is built to read.
    decisions_.willDlss = renderer->WillEvaluateDlss();
}

void SceneRenderer::Render(Renderer* renderer, const SceneFrameData& frame)
{
    if (!renderer)
    {
        return;
    }

    frame_ = &frame;
    EnsureFrameResources(renderer);
    DecideFrame(renderer, frame);

    renderer->BeginSubmitTimeline();

    const bool showProfilerOverlay = frame.settings.showProfiler;
    TaskSystem::TaskHandle overlayPrepTask = TaskSystem::Get().Submit([renderer, showProfilerOverlay]()
    {
        TextManager* tm = renderer->GetTextManager();
        if (!tm)
        {
            return;
        }

        if (showProfilerOverlay)
        {
            Profiler::Get().EmitOverlay(tm, /*x=*/16, /*y=*/64, /*maxLines=*/20);
        }

        tm->Build(renderer, nullptr);
    });

    // The deferred targets are stable between BeginFrame and Present, so pass
    // declarations capture the frame's resources directly. The declared states
    // are registered as first-use states on each pass's main command list; the
    // actual barriers are injected between command lists at submit time.
    const auto& D = renderer->GetDeferredForFrame();
    const auto& P = renderer->GetDeferredForPrevFrame();

    // The main graph is ~16 KB (MaxPasses x Pass, each with a std::function). Built as a
    // local it put that on Render's stack every frame and left no headroom — C6262 fired
    // the moment anything was added to Pass. Owned on the heap and Reset() per frame
    // instead: same semantics (a freshly empty graph), no per-frame stack cost, and no
    // per-frame allocation either.
    using MainRenderGraph = RenderGraph<kMainRenderGraphPassCount>;
    if (!mainRenderGraph_) { mainRenderGraph_ = std::make_unique<MainRenderGraph>(); }
    // The async switch is read by every AddPass2 below, so this is where a change to it first
    // matters and the only place it can be applied without splitting a frame between two queue
    // topologies. The developer window is drawn between BeginFrame and here, which is why the
    // drain cannot live in BeginFrame: it would always be one frame behind the click.
    renderer->SyncAsyncQueueMode();
    mainRenderGraph_->Reset();
    MainRenderGraph& rg = *mainRenderGraph_;

    // R5: the frame's SHAPE, in schedule order. Registration only — every builder these phases
    // attach runs later, inside ExecuteParallel. The bodies live in SceneRenderer_Graph.cpp.
    GraphBuild gb{ rg, D, P };
    BuildPrologue(renderer, gb);
    BuildShadows(renderer, gb);
    BuildGBufferAndAo(renderer, gb);
    BuildLighting(renderer, gb);
    BuildReflections(renderer, gb);
    BuildForwardAndEditor(renderer, gb);
    BuildPost(renderer, gb);

#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
    rg.ExecuteParallel(renderer, TaskSystem::Get());
#else
    rg.Execute(renderer);
#endif

    {
        CPU_SCOPE(ProfilerScopes::kFrameAsyncWait);
        TaskSystem::Get().WaitForTrackedAsyncTasks();
    }

    using EpilogueRenderGraph = RenderGraph<kEpilogueRenderGraphPassCount>;
    if (!epilogueRenderGraph_) { epilogueRenderGraph_ = std::make_unique<EpilogueRenderGraph>(); }
    epilogueRenderGraph_->Reset();
    EpilogueRenderGraph& epilogueRG = *epilogueRenderGraph_;
    // pass-flow S5: binds the backbuffer RTV/DSV and draws ImGui + text; comparator-verified to
    // perform no transitions, so the builder declares nothing and returns the body. The prep task
    // stays a REFERENCE capture on both levels: it is a handle the body consumes (Wait + Release),
    // not a frame decision, and it outlives this graph's Execute.
    epilogueRG.AddPass2(RenderPass::Epilogue_Overlay, {},
        [this, renderer, &overlayPrepTask](RenderGraphPassContext&) -> std::function<void(RenderGraphPassContext)> {
            return [this, renderer, &overlayPrepTask](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassOverlay);
                Pass_Overlay(renderer, c, overlayPrepTask);
            };
        });
    epilogueRG.Execute(renderer);
    renderer->EndFrame();
#if WITH_EDITOR
    renderer->ResolveObjectIdPickReadback();
#endif

    frame_ = nullptr;
}

void SceneRenderer::Pass_PrologueClear(Renderer* r, RenderGraphPassContext ctx, std::uint32_t point)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassPrologueClear);
        // The ocean maps' D7 release for Main_ObjectCompute on the compute queue (see the builder).
        // Empty, and therefore free, on a level without an ocean.
        r->EmitPoint(t.cl, point);
        r->RecordBindAndClear(t.cl);
    }
    ctx.EndCL(t);
}
