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
#include "rendering/shadows/LocalShadowCache.h" // local-light plan S7: the direct read's pool page size
#include "rendering/shadows/VirtualShadowMap.h"
#include "ocean/OceanSimulation.h"
#include "rendering/core/PhotographicSettings.h" // P16.1 pre-exposure
#include "rendering/core/UploadBatch.h" // the ghost sprite sheet is uploaded once, lazily
#include "rendering/shadows/ShadowSettings.h"
#include "ocean/OceanRenderable.h" // caustics: flipbook SRV + water level + shared clock
#include "vfx/WindState.h" // W3: fold WindState into the gbuffer per-view CB
#include "core/task/TaskSystem.h"
#include "rendering/streaming/TextureStreaming.h" // A3: manager tick
#include "text/TextManager.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include <optional>
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
    skyAtmosphere_.Reset();
    volumetricCloud_.Reset();
    for (DdgiVolume& volume : ddgi_) { volume.Reset(); }
    cloudHistoryFrames_ = 0u; cloudLogged_ = -1;
    lightShaftHistoryFrames_ = 0u; lightShaftLastSeconds_ = -1.0;
    fogDistantRevision_ = 0u;
    decisions_.reflectionTemporal = false;
    ssrHistoryValid_ = false;
    ssrHistoryFrames_ = 0u;
    ssrHistoryWidth_ = 0u;
    ssrHistoryHeight_ = 0u;
    oceanHistoryValid_ = false;
    oceanHistoryFrames_ = 0u;
    oceanHistoryWidth_ = 0u;
    oceanHistoryHeight_ = 0u;
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
    skyAtmosphere_.Prepare(renderer, frame_->settings.skyAtmosphere);
    // Plan part C: the clouds' fixed resources, once the level asks for them AND the sky is
    // procedural (their light and haze come from its LUTs; on a cubemap sky nothing is allocated).
    if (frame_->settings.skyAtmosphere.mode != 0u)
    {
        volumetricCloud_.Prepare(renderer, frame_->settings.volumetricCloud);
    }
    // Plan D: the GI probe volumes' fixed resources, once GI (and its far cascade) is wanted on RT hardware.
    if (frame_->settings.giEnabled && renderer->IsRaytracingSupported())
    {
        ddgi_[0].Prepare(renderer);
        if (frame_->settings.giFarCascade) { ddgi_[1].Prepare(renderer); }
    }

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
                    Math::float4(desc.color, render::CandelaFromLumens(light.EffectiveFluxLm())); // P16.5; plan L's level
                // .w = the OLD receiver bias (NDC): 0 under UE's, which lives in the depth pass (plan S1,
                // BuildSpotShadowViewCB) -- the receivers (spot pass, glass, fog) subtract whatever is here.
                // The TEXEL bias (render::LocalShadowTexelBias, local_shadow_bias.hlsli) is 0 here too: it moves the receiver.
                const bool texelBias = render::LocalShadowTexelBias();
                spotCPU[i].shadowParams = Math::float4(light.GetCosInner(),
                    static_cast<float>(lm.GetSpotShadowSlot(i)), light.GetInvAngleRange(),
                    ((render::g_localShadowBiasUe || texelBias) && !render::VsmActive()) ? 0.0f : light.GetShadowDepthBias()); // VSM: as before
                // .z = UE's shadow fade (plan S3, LightManager::SelectShadowedSpots); the VSM path does not read it.
                const float spotFade = (render::g_localShadowFadeUe && !render::VsmActive()) ? lm.GetSpotShadowFade(i) : 1.0f;
                // TEXEL bias: .x = push texels, .w = normal texels (> 0 marks the mode); otherwise .x = the world normal bias.
                spotCPU[i].shadowParams2 = texelBias
                    ? Math::float4(render::LocalShadowPushTexels(desc.shadowSlopeBias), std::max(desc.volumetricIntensity, 0.0f),
                                   spotFade, render::LocalShadowNormalTexels(desc.shadowBias)) // S8: the light's own knobs
                    : Math::float4(light.GetShadowNormalBias(), std::max(desc.volumetricIntensity, 0.0f), spotFade, 0.0f);
                // Plan S9: its square in its atlas page, in page UV (LightManager::PackShadowAtlases) -- corner, size,
                // page. A light without a place gets the whole first page (it has no slot then, nothing reads it).
                {
                    const float spotPage = static_cast<float>(std::max(renderer->GetDeferredForFrame().spotShadowRes, 1u));
                    const std::uint32_t spotRes = lm.GetSpotShadowResolution(i);
                    const LightManager::ShadowAtlasPlacement place = lm.GetSpotShadowPlacement(i);
                    spotCPU[i].shadowAtlas = (place.page >= 0 && spotRes != 0u)
                        ? Math::float4(static_cast<float>(place.x) / spotPage, static_cast<float>(place.y) / spotPage,
                                       std::min(static_cast<float>(spotRes) / spotPage, 1.0f), static_cast<float>(place.page))
                        : Math::float4(0.0f, 0.0f, 1.0f, 0.0f);
                    // Plan S7, the direct read: its square in the cache pool (a page of the same size, Scene checked) --
                    // the page negative, which is how the receivers know to read the pool (page -w - 1).
                    const LightManager::ShadowAtlasPlacement direct = lm.GetSpotShadowDirect(i);
                    if (direct.page >= 0 && spotRes != 0u && LocalShadowPoolResource())
                    {
                        spotCPU[i].shadowAtlas = Math::float4(static_cast<float>(direct.x) / spotPage,
                            static_cast<float>(direct.y) / spotPage, std::min(static_cast<float>(spotRes) / spotPage, 1.0f),
                            -1.0f - static_cast<float>(direct.page));
                    }
                }
                spotCPU[i].viewProj = light.GetViewProjMatrix();
                // .z = plan S9e: its VSM view (the receivers read it; they know no layout).
                const int spotSlot = lm.GetSpotShadowSlot(i);
                spotCPU[i].shadowParams4 = Math::float4(vsm::LocalMovingMipBias(lm.GetSpotMobility(i)), // S10e
                    render::SpotShadowLinearDepth() ? 1.0f / std::max(light.GetShadowFar() - light.GetShadowNear(), 1.0e-4f)
                                                    : 0.0f, // S4: the linear depth's scale, BuildSpotShadowViewCB's
                    spotSlot >= 0 ? static_cast<float>(vsm::SpotView(static_cast<std::uint32_t>(spotSlot))) : 0.0f, 0.0f);
                // Plan S2: the filter's inputs (local_shadow_filter.hlsli). UE's soft transition for a spot is
                // 1/SpotLightTransitionScale (60) x 2 x ShadowBias of its normalised depth (ShadowRendering.cpp:2059-2075),
                // which spans the light's range: the ramp is that many METRES here, (f - n) / 60 at the default bias.
                {
                    const float nearP = light.GetShadowNear(); // plan S4: the projection's own (render::SpotShadowNear)
                    const float farP = light.GetShadowFar();
                    const float transitionScale = 60.0f / std::max((farP - nearP) * 2.0f * std::max(desc.shadowBias, 0.0f), 1.0e-4f);
                    spotCPU[i].shadowParams3 = Math::float4(nearP, transitionScale, 7.0f * std::max(desc.shadowSharpen, 0.0f) + 1.0f,
                                                            render::g_localShadowFilterUe ? 1.0f : 0.0f);
                }
                // S3a.6: an occluded influence volume can light no visible pixel -- zero range and
                // intensity, keep the entry in place (the shadow slot mapping is by index). Plan L: a lamp
                // switched off (by day, or before it comes on) costs the same nothing.
                if ((frame_->spotLightOccluded && i < frame_->spotLightOccluded->size() && (*frame_->spotLightOccluded)[i]) ||
                    light.EffectiveFluxLm() <= 0.0f)
                {
                    spotCPU[i].positionRange.w = 0.0f;
                    spotCPU[i].colorIntensity.w = 0.0f;
                }
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
                // Per-light cube-shadow params = (slot/-1, bias, near, far=radius); near MUST match Scene.cpp's
                // cube-face projection EXACTLY -- PointShadowFactor reconstructs the compare depth from it.
                // The bias (local_shadow_bias.hlsli): UE's (plan S1) = -DepthBias, the fraction of the distance
                // 0.02 x 512/Res x 2 x shadowBias (ShadowRendering.cpp:1977-1979, r.Shadow.PointLightDepthBias 0.02),
                // its slope scale 3 x shadowSlopeBias (r.Shadow.PointLightSlopeScaleDepthBias 3) in volumetric.y;
                // or the old +0.10 world metres (B4) with shadow.localBiasUe off.
                const float pointShadowNear = render::PointShadowNear(desc.radius); // plan S4: Scene.cpp's faces
                const float pointShadowFar = std::max(desc.radius, pointShadowNear + 0.1f);  // ...and their far
                // Plan S4: the light's own cube-face resolution; UE's bias is per that.
                const float pointSlice = static_cast<float>(std::max(renderer->GetDeferredForFrame().pointShadowRes, 1u));
                const std::uint32_t pointResOwn = lm.GetPointShadowResolution(i);
                const float pointRes = pointResOwn ? std::min(static_cast<float>(pointResOwn), pointSlice) : pointSlice;
                // Plan S9: its 3f x 2f block in a point page (3 x 2 faces of pointSlice), in page UV.
                const LightManager::ShadowAtlasPlacement pointPlace = lm.GetPointShadowPlacement(i);
                const float pointPageW = 3.0f * pointSlice;
                const float pointPageH = 2.0f * pointSlice;
                pointCPU[i].shadowAtlas = (pointPlace.page >= 0)
                    ? Math::float4(static_cast<float>(pointPlace.x) / pointPageW, static_cast<float>(pointPlace.y) / pointPageH,
                                   pointRes / pointPageW, pointRes / pointPageH)
                    : Math::float4(0.0f, 0.0f, 1.0f / 3.0f, 0.5f);
                const float ueDepthBias = 0.02f * 512.0f / pointRes * 2.0f * std::max(desc.shadowBias, 0.0f);
                // The TEXEL bias (local_shadow_bias.hlsli): volumetric.y = push texels, shadowFade.w = normal texels
                // (> 0 marks the mode), shadowParams.y = 0 -- the receiver moves, the compare carries no bias.
                const bool texelBias = render::LocalShadowTexelBias();
                pointCPU[i].volumetric = Math::float4(std::max(desc.volumetricIntensity, 0.0f), // plan A4d
                                                      texelBias ? render::LocalShadowPushTexels(desc.shadowSlopeBias) // S8
                                                                : 3.0f * std::max(desc.shadowSlopeBias, 0.0f),
                                                      7.0f * std::max(desc.shadowSharpen, 0.0f) + 1.0f, // S2: UE's sharpen
                                                      render::g_localShadowFilterUe ? 1.0f : 0.0f);      // S2: UE's disc
                pointCPU[i].shadowParams = Math::float4(
                    static_cast<float>(lm.GetPointShadowSlot(i)),
                    texelBias ? 0.0f
                              : ((render::g_localShadowBiasUe && !render::VsmActive()) ? -std::max(ueDepthBias, 1.0e-9f) : 0.10f), // VSM: as before
                    pointShadowNear, desc.radius);
                // Plan S3: UE's shadow fade (LightManager::SelectShadowedPoints); the VSM path does not read it.
                // .y = plan S9: the atlas page; .z = plan S4: 1 / the face resolution (the filter's texel).
                pointCPU[i].shadowFade = Math::float4(
                    (render::g_localShadowFadeUe && !render::VsmActive()) ? lm.GetPointShadowFade(i) : 1.0f,
                    static_cast<float>(std::max(pointPlace.page, 0)), 1.0f / pointRes,
                    texelBias ? render::LocalShadowNormalTexels(desc.shadowBias) : 0.0f); // S8: the light's own knob
                // Plan S7, the direct read: its 3f x 2f block in the cache pool (a square page, in its UV), the page
                // negative -- the receivers then read the pool (page -y - 1).
                const LightManager::ShadowAtlasPlacement direct = lm.GetPointShadowDirect(i);
                const float poolPage = LocalShadowPoolResource()
                    ? static_cast<float>(frame_->localShadowCache->PageSize()) : 0.0f;
                if (direct.page >= 0 && poolPage > 0.0f)
                {
                    pointCPU[i].shadowAtlas = Math::float4(static_cast<float>(direct.x) / poolPage,
                        static_cast<float>(direct.y) / poolPage, pointRes / poolPage, pointRes / poolPage);
                    pointCPU[i].shadowFade.y = -1.0f - static_cast<float>(direct.page);
                }
                // .z = plan S9e: the VSM view of its +X face (the five others follow it).
                const int pointSlot = lm.GetPointShadowSlot(i);
                pointCPU[i].shadowParams4 = Math::float4(vsm::LocalMovingMipBias(lm.GetPointMobility(i)), // S10e
                    render::PointShadowLinearDepth() ? 1.0f / std::max(pointShadowFar - pointShadowNear, 1.0e-4f)
                                                     : 0.0f, // S4: the faces' linear depth scale, BuildPointShadowViewCB's
                    pointSlot >= 0 ? static_cast<float>(vsm::PointFaceView(static_cast<std::uint32_t>(pointSlot) * 6u)) : 0.0f,
                    0.0f);
                if ((frame_->pointLightOccluded && i < frame_->pointLightOccluded->size() && (*frame_->pointLightOccluded)[i]) ||
                    desc.luminousFluxLm <= 0.0f)
                {
                    pointCPU[i].radius = 0.0f;
                    pointCPU[i].intensity = 0.0f; // S3a.6 / plan L, see the spot loop
                }
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
    // Interior lighting plan S: the RT sky visibility traces the same TLAS, so it is one more reason
    // to build it. Without RT hardware (or after an AS failure) the sky stays open, as before.
    decisions_.skyVis = decisions_.rtSupported && !rtFailed && frame.settings.skyVisEnabled;
    // Plan D: the GI probe volume traces the same TLAS. Lighting reads it while some generation of
    // it was traced (a frame whose passes skip keeps the last atlases).
    decisions_.gi = decisions_.rtSupported && !rtFailed && frame.settings.giEnabled;
    decisions_.giApplied = decisions_.gi && ddgi_[0].Ready() && ddgi_[0].AppliedGeneration() != 0u;
    decisions_.giFar = decisions_.gi && frame.settings.giFarCascade && ddgi_[0].Ready() && ddgi_[1].Ready();
    decisions_.giFarApplied = decisions_.giFar && ddgi_[1].AppliedGeneration() != 0u;
    decisions_.rtBuildAS = decisions_.rtReflect || decisions_.rtDebugView || decisions_.skyVis || decisions_.gi;
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
        const HeightFogPacked fog =
            PackHeightFog(frame.settings.heightFog, frame.dirLight != nullptr);
        frame.ocean->SetFogParams(fog.params0, fog.params1, fog.params2);
        frame.ocean->SetFogDebugView(g_fogDebugView);
        frame.ocean->SetReflectionNormalCascades(frame.settings.waterReflectionNormalCascades);
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

    // S15: the same shape for SDSM. `frame.sdsm` already carries the mode + residency gate
    // (Scene::UpdateSdsmFrameParams); what is added here is the DRAW readiness, which depends on
    // EnsureFrameResources having built the permutation's PSO -- and Render() calls that before
    // DecideFrame, which is the ordering this relies on. False means Main_CSM stays in the graph
    // and the frame renders Legacy cascades, which is the right fallback: shadows, just not SDSM's.
    decisions_.sdsmActive = render::SdsmActive() && frame.sdsm != nullptr &&
                            frame.shadowGpu != nullptr && frame.shadowGpu->SdsmDrawReady();
    // The mode's own occlusion cull. Gated on the SAME knob as Legacy's (CascadeShadowConfig::
    // hzbCull) plus the indirect path, because it IS the same machinery -- the pyramids, the
    // deferred list and the pass-B pair are one set of resources serving whichever mode runs.
    decisions_.sdsmHzb = decisions_.sdsmActive && render::g_indirectShadowsEnabled &&
                         frame.cascadeConfig != nullptr && frame.cascadeConfig->hzbCull &&
                         frame.shadowGpu->CascadeHzbRef().Ready();
    // S16: EVSM4. Scene allocated the moments atlas (or failed to); this is the last gate, and
    // every consumer -- the conversion pass, the atlas SRV the light passes bind, the CB flag
    // that makes csm_sample take the Chebyshev arm -- asks THIS ONE, so they cannot disagree
    // about whether the slot holds depth or moments.
    decisions_.sdsmEvsm = decisions_.sdsmActive && render::sdsm::g_evsm && frame.sdsm->MomentsReady();

    // Rung 2 / Step 22 skip-when-still. Skip the VSM update (request + alloc + render) only when
    // NOTHING changed — the camera view is unchanged AND no shadow caster moved. Then the pool +
    // page table persist and last frame's content is still valid (saving the dominant cost, the
    // page-render draw loop). Gating on movers is essential: a rotating caster must re-render its
    // shadow every frame even with a still camera (otherwise its shadow freezes). The view matrix
    // carries the camera transform with NO jitter (jitter lives in proj), so it is bit-stable when
    // the camera is still.
    if (render::VsmActive() && frame.mainView)
    {
        // THE LIGHT IS PART OF THE VIEW (time-of-day plan T0): the clipmap's light-space frame is built
        // from the sun's direction, so a still camera under a moving sun is NOT still -- without this
        // the update was skipped after kFrameCount + 1 frames and the shadows froze while the sun went
        // on (any sun animation: the dev window, --scene-stress-sky, time of day).
        const std::uint32_t lightVersion = frame.dirLight ? frame.dirLight->GetTransformVersion() : 0u;
        const std::array<float, 7> requestKey = {
            vsm::g_localMipFromPixel ? 1.0f : 0.0f, vsm::LocalMipBias(), vsm::LocalRefDist(),
            static_cast<float>(vsm::g_requestDownscale), static_cast<float>(renderer->GetRenderWidth()),
            static_cast<float>(renderer->GetRenderHeight()), vsm::g_requestLightCull ? 1.0f : 0.0f };
        const bool viewStill = vsmHasRendered_ && lightVersion == vsmLastLightVersion_ &&
            std::memcmp(&frame.mainView->view, &vsmLastView_, sizeof(mat4)) == 0 &&
            requestKey == vsmLastRequestKey_;
        // W5: a wind-swayed caster animates in the VERTEX shader, so its transform never changes and
        // MoverCount() stays 0 — without this the pool freezes after a few still frames and the palm
        // shadow stops swaying while the tree keeps going (the shadow visibly detaches).
        const bool windAnimating = frame.wind && frame.wind->swayAmplitude > 0.0f &&
                                   frame.shadowGpu && frame.shadowGpu->HasWindCasters();
        // Local-light plan S10e: nor while the page pool is settling (refused requests, a moving dynamic-resolution
        // bias) -- frozen there, the pool keeps the pages the old bias asked for and the shadows stay missing.
        // Plan S10c: nor when the shadowed set changed (a light came, left or moved slot): its pages are requested,
        // allocated and moved only by an update.
        // Plan S10e: nor while a light's moving bias fades back (UE's mobility): the pages it asked coarser while it moved
        // must be asked again as it settles, or the stopped light keeps its coarse shadow.
        const bool lightsSettling = frame.lightManager && frame.lightManager->GetMobileLightCount() > 0;
        const bool poolSettling = frame.vsm && (frame.vsm->DynamicResSettling() || frame.vsm->ViewOwnersPending() ||
                                                lightsSettling);
        const bool contentStill = (!frame.shadowGpu || frame.shadowGpu->MoverCount() == 0) &&
                                  !windAnimating && !poolSettling;
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
            vsmLastLightVersion_ = lightVersion;
            vsmLastRequestKey_ = requestKey;
            vsmHasRendered_ = true;
        }
    }
    else
    {
        vsmHasRendered_ = false;
        vsmStillFrames_ = 0;
    }

    // THE LIGHT DOES NOT REACH THE GROUND (DirectionalLight::ReachesGround): the sun under a closed deck
    // (VolumetricCloudSettings.h CloudSunGroundVisibility is exactly 0 at coverage 1), or the time of
    // day's body wholly below the horizon at night -- nothing in the scene receives it, so its shadows
    // are not rendered at all (UDS: "Disable Directional Light Shadows when Fully Shadowed by Clouds").
    // Each sun-shadow builder returns empty on this flag; VSM goes through its own skip-when-still
    // path -- but only with no shadowed spot or point light, since their pages share the pool and
    // must keep updating. What is left in the atlas / pool is never read: the readers branch on
    // lightRgb == 0. Coming back, the VSM pool is stale (casters moved unrecorded), so every page is
    // dropped; the SDSM partition cache sees a frame it did not render and keeps nothing
    // (Scene::UpdateSdsmFrameParams); the cascade pyramids were not built last frame, so the
    // light-space cull tests nothing.
    decisions_.sunShadowsOff = frame.dirLight != nullptr && !frame.dirLight->ReachesGround();
    const bool localShadows = frame.lightManager != nullptr &&
        (frame.lightManager->GetShadowedSpotCount() > 0 || frame.lightManager->GetShadowedPointCount() > 0);
    const bool vsmSleeps = decisions_.sunShadowsOff && !localShadows;
    if (vsmSleeps)
    {
        decisions_.vsmSkipUpdate = true;
        vsmHasRendered_ = false;
        vsmStillFrames_ = 0;
    }
    else if (vsmSlept_ && render::VsmActive() && frame.vsm && frame.vsm->IsAllocated())
    {
        frame.vsm->InvalidateAllPages();
    }
    vsmSlept_ = vsmSleeps;
    if (decisions_.sunShadowsOff != sunShadowsWereOff_)
    {
        const Math::float3 t = frame.dirLight ? frame.dirLight->GetAtmosphereTransmittance() : Math::float3(1.0f, 1.0f, 1.0f);
        LOG_INFO(logging::LogCategory::RenderShadow,
                 "sun shadows: {} (cloud visibility {:.3f}, transmittance {:.4f} {:.4f} {:.4f}) (frame {})",
                 decisions_.sunShadowsOff ? "off" : "on", frame.dirLight ? frame.dirLight->GetCloudVisibility() : 1.0f,
                 t.x, t.y, t.z, renderer->GetTotalFrameNumber());
    }
    sunShadowsWereOff_ = decisions_.sunShadowsOff;

    // Volumetric fog (docs/volumetric_fog_sky_clouds_ssgi_plan.md, part A). ONE decision: the
    // pass exists and compose reads the volume, or neither -- a compose that read a volume nobody
    // wrote would be the silent failure this rule exists for. History validity is the GTAO shape:
    // frames written in a row at this grid size under this camera history (a cut voids it).
    {
        const auto& D = renderer->GetDeferredForFrame();
        const auto& P = renderer->GetDeferredForPrevFrame();
        const HeightFogSettings& a = frame.settings.heightFog;
        decisions_.volumetricFog = a.enabled && a.volumetric && a.density > 0.0f && frame.dirLight && frame.camera &&
            resources_.GetFogScatterMaterial() && resources_.GetFogIntegrateMaterial() &&
            resources_.GetFogScatterCBSizeBytes() != 0u && resources_.GetFogIntegrateCBSizeBytes() != 0u &&
            resources_.GetLightingCBSizeBytes() != 0u &&
            D.fogScatter.Get() != nullptr && D.fogIntegrated.Get() != nullptr && P.fogScatter.Get() != nullptr &&
            D.fogScatterUAV.ptr != 0 && D.fogIntegratedUAV.ptr != 0 && D.fogScatterSRV.ptr != 0 &&
            D.fogIntegratedSRV.ptr != 0 && P.fogScatterSRV.ptr != 0 && D.shadowSRV.ptr != 0;
        const UINT w = D.fogGridWidth;
        const UINT h = D.fogGridHeight;
        // A3 conservative depth: the furthest pyramid's base is half the render size, so the mip whose
        // texel covers one 16-pixel cell is log2(16) - 1 = 3; both slots must hold it (the history
        // check reads last frame's).
        {
            UINT hzbMip = 0u;
            for (UINT px = std::max(renderer->GetFogGridPixels(), 2u) / 2u; px > 1u; px >>= 1u) { ++hzbMip; }
            decisions_.fogHzbMip = hzbMip;
            decisions_.fogConservativeDepth = decisions_.volumetricFog && a.conservativeDepth &&
                D.hzb.Get() != nullptr && D.hzbSRV.ptr != 0 && D.hzbMips > hzbMip &&
                P.hzb.Get() != nullptr && P.hzbSRV.ptr != 0 && P.hzbMips > hzbMip;
        }
        // A4: local lights, when the level has any (their buffers are filled in EnsureFrameResources).
        decisions_.fogLocalLights = decisions_.volumetricFog && a.localLights && a.localLightScatter > 0.0f &&
            frame.lightManager != nullptr &&
            (frame.lightManager->GetSpotLightCount() > 0 || !frame.lightManager->PointLights().empty());
        // The lookup parameters every consumer shares (A5). Off = x 0: the shaders take the identity.
        {
            const float3 zp = frame.camera
                ? FogGridZParams(FogVolumeNear(a, *frame.camera), a.volumetricDistance, render::kFogDepthDistributionScale, D.fogGridDepth)
                : float3(1.0f, 0.0f, 1.0f);
            decisions_.fogVolumeZParams = Math::float4(zp.x, zp.y, zp.z, 0.0f);
            decisions_.fogVolumeParams = Math::float4(decisions_.volumetricFog ? 1.0f : 0.0f, a.volumetricDistance,
                                                      preExposure_ > 0.0f ? 1.0f / preExposure_ : 1.0f,
                                                      static_cast<float>(D.fogGridDepth));
            if (frame.ocean)
            {
                frame.ocean->SetFogVolumeParams(decisions_.fogVolumeParams, decisions_.fogVolumeZParams);
            }
        }
        const std::uint64_t rev = frame.camera ? frame.camera->GetHistoryRevision() : 0ull;
        const bool sameHistory = fogHistoryWidth_ == w && fogHistoryHeight_ == h && fogHistoryDepth_ == D.fogGridDepth && fogHistoryRevision_ == rev;
        decisions_.fogHistoryValid = decisions_.volumetricFog && a.temporal && fogHistoryFrames_ > 0u && sameHistory;
        fogHistoryFrames_ = decisions_.volumetricFog ? (sameHistory ? fogHistoryFrames_ + 1u : 1u) : 0u;
        fogHistoryWidth_ = w;
        fogHistoryHeight_ = h;
        fogHistoryDepth_ = D.fogGridDepth;
        fogHistoryRevision_ = rev;
        const int state = decisions_.volumetricFog
            ? (1 + (decisions_.fogHistoryValid ? 1 : 0) + (decisions_.fogConservativeDepth ? 2 : 0) + (decisions_.fogLocalLights ? 4 : 0))
            : 0;
        if (state != fogLogged_)
        {
            LOG_INFO(logging::LogCategory::Render, "volumetric fog: on={} history={} conservative={} local={} grid={}x{}x{} (frame {})",
                     decisions_.volumetricFog ? 1 : 0, decisions_.fogHistoryValid ? 1 : 0, decisions_.fogConservativeDepth ? 1 : 0,
                     decisions_.fogLocalLights ? 1 : 0, w, h, D.fogGridDepth, renderer->GetTotalFrameNumber());
            fogLogged_ = state;
        }
    }

    // Volumetric clouds (docs/volumetric_fog_sky_clouds_ssgi_plan.md, part C): the level has them,
    // the sky is procedural (the trace reads its distant light and aerial volume), the owner's
    // fixed resources built and the ring holds the half-res pair. History as the fog's: frames in
    // a row at this size under this camera history (a cut voids it).
    {
        const auto& D = renderer->GetDeferredForFrame();
        const auto& P = renderer->GetDeferredForPrevFrame();
        const VolumetricCloudSettings& c = frame.settings.volumetricCloud;
        const bool procedural = frame.settings.skyAtmosphere.mode != 0u;
        decisions_.volumetricCloud = c.enabled && procedural && volumetricCloud_.Ready() && frame.dirLight && frame.camera &&
            D.cloudTrace.Get() != nullptr && D.cloudTraceDepth.Get() != nullptr && D.cloudResolved.Get() != nullptr &&
            D.cloudResolvedDepth.Get() != nullptr && P.cloudResolved.Get() != nullptr && P.cloudResolvedDepth.Get() != nullptr &&
            D.cloudTraceUAV.ptr != 0 && D.cloudTraceDepthUAV.ptr != 0 && D.cloudResolvedUAV.ptr != 0 && D.cloudResolvedDepthUAV.ptr != 0 &&
            D.cloudTraceSRV.ptr != 0 && D.cloudTraceDepthSRV.ptr != 0 && D.cloudResolvedSRV.ptr != 0 && D.cloudResolvedDepthSRV.ptr != 0 &&
            P.cloudResolvedSRV.ptr != 0 && P.cloudResolvedDepthSRV.ptr != 0 && D.depthSRV.ptr != 0;
        const UINT w = D.cloudResolvedWidth, h = D.cloudResolvedHeight; // the history's own size
        const std::uint64_t rev = frame.camera ? frame.camera->GetHistoryRevision() : 0ull;
        const bool sameHistory = cloudHistoryWidth_ == w && cloudHistoryHeight_ == h && cloudHistoryRevision_ == rev;
        decisions_.cloudHistoryValid = decisions_.volumetricCloud && c.temporal && cloudHistoryFrames_ > 0u && sameHistory;
        cloudHistoryFrames_ = decisions_.volumetricCloud ? (sameHistory ? cloudHistoryFrames_ + 1u : 1u) : 0u;
        cloudHistoryWidth_ = w;
        cloudHistoryHeight_ = h;
        cloudHistoryRevision_ = rev;
        const int state = decisions_.volumetricCloud
            ? (1 + (decisions_.cloudHistoryValid ? 1 : 0) + (c.shadowMap ? 2 : 0))
            : ((c.enabled && !procedural) ? -2 : 0);
        if (state != cloudLogged_)
        {
            if (c.enabled && !procedural)
            {
                LOG_WARNING(logging::LogCategory::Render, "volumetric clouds: the level authors clouds but its sky is a cubemap; "
                            "they need the Sky Atmosphere object (procedural sky) and stay off");
            }
            LOG_INFO(logging::LogCategory::Render, "volumetric clouds: on={} history={} shadowMap={} trace={}x{} resolved={}x{} (frame {})",
                     decisions_.volumetricCloud ? 1 : 0, decisions_.cloudHistoryValid ? 1 : 0,
                     (decisions_.volumetricCloud && c.shadowMap) ? 1 : 0, D.cloudWidth, D.cloudHeight, w, h,
                     renderer->GetTotalFrameNumber());
            cloudLogged_ = state;
        }
    }

    // Plan A7 light shafts (UE LightShaftBloom). Registered only when the sun is in FRONT of the camera
    // (ShouldRenderLightShaftsForLight: the light's screen position has W > 0 -- LightShaftRendering.cpp:107-116)
    // and the bloom would be non-zero; off-screen but in front is fine, the shafts enter from the edge.
    {
        decisions_.lightShafts = false;
        decisions_.lightShaftHistoryValid = false;
        decisions_.lightShaftBlend = 1.0f;
        decisions_.lightShaftFade = 1.0f;
        decisions_.lightShaftFromMoon = false;
        // The SKY's sun (T2): the shafts are seeded by its disc and glow, whoever lights the ground.
        const DirectionalLight* sun = frame.skyLight;
        const auto& D = renderer->GetDeferredForFrame();
        const auto& P = renderer->GetDeferredForPrevFrame();
        const bool ready = sun != nullptr && frame.camera != nullptr && g_fogDebugView == 0u &&
            sun->GetLightShaftsEnabled() && // each light's bloom scale is checked with its fade below
            (sun->GetColor().x > 0.0f || sun->GetColor().y > 0.0f || sun->GetColor().z > 0.0f) &&
            resources_.GetLightShaftMaterial(0u) && resources_.GetLightShaftMaterial(1u) && resources_.GetLightShaftMaterial(2u) &&
            resources_.GetLightShaftCBSizeBytes(0u) != 0u && resources_.GetLightShaftCBSizeBytes(1u) != 0u &&
            resources_.GetLightShaftCBSizeBytes(2u) != 0u &&
            resources_.GetLightShaftMaterial(3u) && resources_.GetLightShaftCBSizeBytes(3u) != 0u &&
            D.lightShaftA.Get() != nullptr && D.lightShaftB.Get() != nullptr &&
            D.lightShaftASRV.ptr != 0 && D.lightShaftAUAV.ptr != 0 && D.lightShaftBSRV.ptr != 0 && D.lightShaftBUAV.ptr != 0 &&
            D.lightShaftHistory.Get() != nullptr && D.lightShaftHistoryUAV.ptr != 0 &&
            P.lightShaftHistory.Get() != nullptr && P.lightShaftHistorySRV.ptr != 0 &&
            D.gbVelocity.Get() != nullptr && D.gbSRV[3].ptr != 0 &&
            D.sceneSRV.ptr != 0 && D.sceneUAV.ptr != 0 && D.depthSRV.ptr != 0;
        if (ready)
        {
            // UE GetLightPositionForLightShafts for a directional light = ViewOrigin - Direction * WORLD_MAX
            // (DirectionalLightComponent.cpp:449-453): the point at infinity towards the sun, i.e. a homogeneous
            // DIRECTION (w = 0) through the unjittered view-projection. Row-vector convention, like the shaders.
            // NOT THE MOON (time-of-day N1, tried and measured 2026-10-13): radiating the shafts from the moon
            // at night turned its disc into a big BLOCKY glow. The shafts are a half-res radial blur of the
            // pixels over the threshold; for the sun those are the disc AND the bright sky around it, so the
            // blur's discrete steps vanish in a wide seed. At night the only seed is a 15-pixel disc on a
            // black sky, and every step of the blur shows as its own copy -- the "pixelated halo" the owner
            // asked about. With the shafts off it was gone and the bloom's own glow remained.
            // ...UNTIL N4 (owner, 2026-10-13: "make the moon's shafts light ones"): once the sun's are gone and
            // the moon lights the ground, the shafts radiate from the moon with the MOON'S OWN bloom properties at
            // Ultra Dynamic Sky's values (TimeOfDaySettings::moonLightShaftBloom*): the cap of 0.15 makes the disc
            // no brighter in the seed than its glow, and the halo smooth -- the copies were a disc seeded at 100.
            float3 toSun = (-sun->GetDirection()).Normalized();
            // BY THE SUN'S HEIGHT (time-of-day N3, ours): crepuscular rays are the shadows of clouds on air the
            // sun still lights directly, and a few degrees down nothing overhead is. UE gate on the screen
            // position alone, so the blur kept seeding on the twilight band -- lifted over the threshold by
            // the auto exposure -- and drew rays out of a sun 10 degrees under the sea (wind_test, 18:40-18:50,
            // 2026-10-13). Full while any of the disc is up, gone by -6 degrees, the end of civil twilight.
            const float elevationDeg = std::asin(std::clamp(toSun.y, -1.0f, 1.0f)) * 57.2957795f;
            const float fadeT = std::clamp((elevationDeg + 6.0f) / (6.0f - 0.27f), 0.0f, 1.0f);
            const float sunFade = fadeT * fadeT * (3.0f - 2.0f * fadeT);
            decisions_.lightShaftFade = sun->GetLightShaftBloomScale() > 0.0f ? sunFade : 0.0f;
            const SkyMoonFrame& moon = frame.skyMoon;
            if (sunFade <= 0.0f && moon.present && moon.lightsGround && moon.lightShaftBloomScale > 0.0f)
            {
                toSun = float3(moon.toward[0], moon.toward[1], moon.toward[2]).Normalized();
                // From the moon's disc on the horizon up to a few degrees, as the sun's from its own.
                const float moonElevationDeg = std::asin(std::clamp(toSun.y, -1.0f, 1.0f)) * 57.2957795f;
                const float moonT = std::clamp((moonElevationDeg + 0.27f) / 3.0f, 0.0f, 1.0f);
                // ...AND IN FROM ZERO after the sun's are out, over the next 3 degrees of the SUN's descent (full
                // at -9, where the twilight takes the sun out of the sky), out the same way before the sun's come
                // back at dawn. One source at a time, cross-faded: the moon's shafts used to start at full the
                // frame the sun's reached 0 -- a moon already 3 degrees up popped the frame +16..32 % on the
                // ground of a moonward view (owner, 2026-10-20: "a flicker on the water near the bodies when
                // dusk turns into night"; measured on wind_test, the pop gone with the moon's shafts off).
                const float inT = std::clamp((-6.0f - elevationDeg) / 3.0f, 0.0f, 1.0f);
                decisions_.lightShaftFade = moonT * moonT * (3.0f - 2.0f * moonT) * inT * inT * (3.0f - 2.0f * inT);
                decisions_.lightShaftFromMoon = true;
            }
            const float4 clip = frame.camera->GetViewProjMatrixNoJitter().Transform(float4(toSun.x, toSun.y, toSun.z, 0.0f));
            if (clip.w > 1e-6f && decisions_.lightShaftFade > 0.0f)
            {
                // LightShaftRendering.cpp:166-176: screen position -> uv -> aspect-corrected (v * H/W), so the blur
                // is the same pixel distance in x and y.
                const float invW = 1.0f / clip.w;
                const float u = 0.5f + 0.5f * clip.x * invW;
                const float v = 0.5f - 0.5f * clip.y * invW;
                const float invAspect = static_cast<float>(renderer->GetRenderHeight()) /
                                        static_cast<float>(std::max(renderer->GetRenderWidth(), 1u));
                decisions_.lightShaftOrigin = Math::float2(u, v * invAspect);
                decisions_.lightShafts = true;
            }
        }
        // The temporal step's history -- last frame's output, the previous slot's copy -- holds only
        // if the pass ran last frame at this size and camera history (the cloud's rule). Its blend
        // weight is a frame-rate-independent exponential of the sun's smoothing time constant, on a
        // delta capped like the exposure's so a pause does not resolve into one jump.
        const std::uint64_t rev = frame.camera ? frame.camera->GetHistoryRevision() : 0ull;
        const bool sameHistory = lightShaftHistoryWidth_ == D.lightShaftWidth &&
            lightShaftHistoryHeight_ == D.lightShaftHeight && lightShaftHistoryRevision_ == rev;
        decisions_.lightShaftHistoryValid = decisions_.lightShafts && lightShaftHistoryFrames_ > 0u && sameHistory;
        lightShaftHistoryFrames_ = decisions_.lightShafts ? (sameHistory ? lightShaftHistoryFrames_ + 1u : 1u) : 0u;
        lightShaftHistoryWidth_ = D.lightShaftWidth;
        lightShaftHistoryHeight_ = D.lightShaftHeight;
        lightShaftHistoryRevision_ = rev;
        const double now = GetTimeSeconds();
        const double dt = lightShaftLastSeconds_ >= 0.0 ? std::clamp(now - lightShaftLastSeconds_, 0.0, 0.1) : 0.0;
        lightShaftLastSeconds_ = now;
        const float smoothing = sun ? sun->GetLightShaftSmoothingSec() : 0.0f;
        decisions_.lightShaftBlend = smoothing > 0.0f
            ? std::max(1.0f - std::exp(-static_cast<float>(dt) / smoothing), 1.0e-3f) : 1.0f;
        const int state = decisions_.lightShafts ? 1 : 0;
        if (state != lightShaftsLogged_)
        {
            LOG_INFO(logging::LogCategory::Render, "light shafts: on={} origin=({:.3f}, {:.3f}) half-res {}x{} (frame {})",
                     state, decisions_.lightShaftOrigin.x, decisions_.lightShaftOrigin.y,
                     D.lightShaftWidth, D.lightShaftHeight, renderer->GetTotalFrameNumber());
            lightShaftsLogged_ = state;
        }
    }

    // DLSS-split: the PREDICTION, not the outcome. The evaluate itself can still decline in the
    // record (and tells the handler so, which backs the prediction off for the next frames); what
    // this decides is which image the tonemap is built to read.
    decisions_.willDlss = renderer->WillEvaluateDlss();

    // Occlusion plan S5: the camera's two-pass HZB occlusion inside the indirect G-buffer. ONE
    // decision, here, because three passes exist or not by it and every consumer of the G-buffer
    // chains off whichever pass finishes it. The PREVIOUS pyramid may be tested against when the
    // slot before this one built it LAST frame, at this render size, under the same camera
    // history (an explicit cut makes last frame's matrices meaningless) -- otherwise the main
    // cull defers nothing this frame and the three passes cost one pyramid build: the frame's
    // SHAPE does not depend on history, only its work does. The registry gets the matrices and
    // the two pyramids now, before any builder runs (its own builder applies the final gate).
    decisions_.camHzb = false;
    if (frame.shadowGpu)
    {
        ShadowGpuData::CameraHzbParams p{};
        ID3D12Resource* prevHzb = nullptr;
        ID3D12Resource* curHzb = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE prevSrv{};
        D3D12_CPU_DESCRIPTOR_HANDLE curSrv{};
        const auto& D = renderer->GetDeferredForFrame();
        const auto& P = renderer->GetDeferredForPrevFrame();
        // The same gates as the Main_Hzb builder's: a frame this passes is a frame Main_HzbA
        // cannot decline, so the post cull never retests against a pyramid nobody rebuilt.
        const bool hzbReady = resources_.GetHzbMaterial() && resources_.GetHzbCBSizeBytes() != 0u &&
                              D.depthSRV.ptr != 0 && D.hzb.Get() != nullptr && D.hzbClosest.Get() != nullptr &&
                              D.hzbMips != 0 && D.hzbSRV.ptr != 0 && P.hzb.Get() != nullptr && P.hzbSRV.ptr != 0;
        if (render::g_gbufferHzbCullEnabled && frame.gbufferIndirect && frame.camera && hzbReady)
        {
            const Camera& cam = *frame.camera;
            const UINT w = renderer->GetRenderWidth();
            const UINT h = renderer->GetRenderHeight();
            const UINT prevSlot = (renderer->GetCurrentFrameIndex() + render::kFrameCount - 1u) % render::kFrameCount;
            const std::uint64_t frameNo = renderer->GetTotalFrameNumber();
            const CamHzbBuilt& b = camHzbBuilt_[prevSlot];
            const bool prevValid = frameNo > 0 && b.frame == frameNo - 1u && b.width == w && b.height == h &&
                                   b.revision == cam.GetHistoryRevision() &&
                                   P.hzbWidth == D.hzbWidth && P.hzbHeight == D.hzbHeight;
            // The previous pyramid was reduced from last frame's JITTERED depth, so it is tested
            // with last frame's jittered pair (the plan text says no-jitter; exact beats close).
            p.prevViewProj = cam.GetPrevViewProjMatrix();
            p.prevViewToClip = cam.GetPrevProjMatrix();
            p.viewProj = cam.GetViewProjMatrix();
            p.viewToClip = cam.GetProjMatrix();
            p.viewRect[0] = 0;
            p.viewRect[1] = 0;
            p.viewRect[2] = static_cast<std::int32_t>(w);
            p.viewRect[3] = static_cast<std::int32_t>(h);
            p.hzbSize[0] = D.hzbWidth;
            p.hzbSize[1] = D.hzbHeight;
            p.prevValid = prevValid ? 1u : 0u;
            p.on = 1u;
            prevHzb = P.hzb.Get();
            prevSrv = P.hzbSRV;
            curHzb = D.hzb.Get();
            curSrv = D.hzbSRV;
            decisions_.camHzb = true;
        }
        frame.shadowGpu->SetCameraHzb(p, prevHzb, prevSrv, curHzb, curSrv);
        const int state = decisions_.camHzb ? (1 + static_cast<int>(p.prevValid)) : 0;
        if (state != camHzbLogged_)
        {
            LOG_INFO(logging::LogCategory::RenderShadow, "camera hzb cull: on={} prev-valid={} (frame {})",
                     decisions_.camHzb ? 1 : 0, p.prevValid, renderer->GetTotalFrameNumber());
            camHzbLogged_ = state;
        }
    }
}

void SceneRenderer::Render(Renderer* renderer, const SceneFrameData& frame)
{
    if (!renderer)
    {
        return;
    }

    frame_ = &frame;
    {
        CPU_SCOPE(ProfilerScopes::kSceneRendererEnsureResources);
        EnsureFrameResources(renderer);
    }
    {
        CPU_SCOPE(ProfilerScopes::kSceneRendererDecideFrame);
        DecideFrame(renderer, frame);
    }
    // Texture streaming A3: the manager's frame stage (snapshot / apply) before the graph is built,
    // so this frame's Main_TextureStreaming records what it asked for.
    if (streaming::TextureStreaming* ts = renderer->GetTextureStreaming())
    {
        CPU_SCOPE(ProfilerScopes::kTextureStreamingTick);
        ts->TickManager(renderer, frame);
    }

    // Until the graph's first AddPass: the submit timeline, the overlay task, the queue-mode sync, the graph reset.
    std::optional<Profiler::ScopedCpu> graphSetupScope(std::in_place, ProfilerScopes::kSceneRendererGraphSetup);
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
    graphSetupScope.reset();

    // R5: the frame's SHAPE, in schedule order. Registration only — every builder these phases
    // attach runs later, inside ExecuteParallel. The bodies live in SceneRenderer_Graph.cpp.
    GraphBuild gb{ rg, D, P };
    BuildPrologue(renderer, gb);
    BuildShadows(renderer, gb);
    BuildGBufferAndAo(renderer, gb);
    BuildGi(renderer, gb); // plan D: the GI probe volume (trace + update), read by lighting
    BuildLighting(renderer, gb);
    BuildReflections(renderer, gb);
    BuildForwardAndEditor(renderer, gb);
    gb.pSelectionOutline = skyAtmosphere_.BuildDebug(renderer, rg, frame_->settings.skyAtmosphere, *frame_->camera, frame_->settings.skyAtmosphere.aerialStartDepthMetres, gb.pSelectionOutline, gb.pSkyLuts);
    BuildPost(renderer, gb);

    // RenderObjectBatch's slots: every job of the last frame was joined by the wait below.
    objectBatchJobCount_.store(0, std::memory_order_relaxed);
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
