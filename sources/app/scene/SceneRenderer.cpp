#include "app/scene/SceneRenderer.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

#include "rendering/core/RenderConstants.h"

#include "app/camera/Camera.h"
#include "app/Systems.h"
#include "rendering/debug/DebugDraw.h"
#include "rendering/core/ComputeDispatch.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/RenderGraph.h"
#include "rendering/core/RenderPass.h"
#include "rendering/renderables/GBufferRenderable.h" // per-slot RT materials (B3 follow-up)
#include "rendering/renderables/RenderableObject.h"
#include "rendering/shadows/ShadowGpuData.h"
#include "rendering/shadows/VirtualShadowMap.h"
#include "ocean/OceanSimulation.h"
#include "ocean/OceanRenderable.h" // caustics: flipbook SRV + water level + shared clock
#include "vfx/WindState.h" // W3: fold WindState into the gbuffer per-view CB
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
#if WITH_EDITOR
    constexpr UINT kSelectionStencilBit = 0x80u;
    constexpr uint32_t kSelectionStencilGBufferLocalOrder = 0xfffffffeu;
    constexpr uint32_t kSelectionStencilTransparentLocalOrder = 0xfffffffeu;
#endif

    constexpr size_t BucketIndex(SceneRenderQueue::BucketType type)
    {
        return static_cast<size_t>(type);
    }

#if WITH_EDITOR
    bool IsSelectedEditorObject(const SceneFrameData& frame, std::uint64_t id)
    {
        if (id == 0)
        {
            return false;
        }
        for (std::uint32_t i = 0; i < frame.selectedEditorObjectCount; ++i)
        {
            if (frame.selectedEditorObjectIds[i] == id)
            {
                return true;
            }
        }
        return false;
    }

    bool ShouldRenderSelectionStencil(const SceneFrameData& frame,
        const SceneView& view,
        const RenderableObjectBase& object,
        bool transparent)
    {
        if (!IsSelectedEditorObject(frame, object.GetEditorObjectId()) ||
            !object.IsVisible() || object.IsTransparent() != transparent)
        {
            return false;
        }

        if ((object.GetRenderLayerMask() & view.renderLayerMask) == 0)
        {
            return false;
        }

        const AABB& bounds = object.GetWorldBounds();
        if (view.frustum.IsValid() && bounds.IsValid() && !view.frustum.Intersects(bounds))
        {
            return false;
        }
        return true;
    }
#endif

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

    // ---- Step 2: shared per-view/per-frame constant buffers (b1) ----
    // Filled once per pass and bound for every object in that pass, replacing the
    // old per-object duplication of view/light/cascade data.

    // Matches gbuffer_common.hlsli `cbuffer PerView : register(b1)`. The depth-only
    // shadow shaders consume only viewProj (the other two are left identity/unused).
    struct PerViewCB
    {
        mat4 viewProj;
        mat4 viewProjNoJitter;
        mat4 prevViewProjNoJitter;
        // W3: global wind, consumed by the gbuffer VS (W4) and — since W5 — by the shadow VS too
        // (shadow_indirect_csm.hlsl declares the same tail at offset 192). Layout matches the HLSL
        // `cbuffer PerView` in gbuffer_common.hlsli: time, prevTime, float2 windDirXZ, then
        // swayAmp, swayFreq, gustMul, prevGustMul.
        float windTime = 0.0f;
        float windPrevTime = 0.0f;
        float windDirX = 1.0f;
        float windDirZ = 0.0f;
        float windSwayAmp = 0.0f;
        float windSwayFreq = 0.0f;
        float windGustMul = 1.0f;
        float windPrevGustMul = 1.0f;
    };
    static_assert(sizeof(PerViewCB) == 224, "PerViewCB must match the gbuffer/shadow HLSL layout");

    // Matches glass.hlsl `cbuffer GlassView : register(b1)`.
    struct GlassViewCB
    {
        mat4   view;
        mat4   proj;
        mat4   viewProj;
        mat4   viewProjNoJitter;
        mat4   prevViewProjNoJitter;
        mat4   invView;
        mat4   invProj;
        float4 camPosSky;
        float4 sunDirAmbient;
        float4 sunColorExposure;
        float4 camDirWS;
        float4 screenSizeInv;
        float4 shadowAtlasSizeInv;
        float4 shadowBiasNDC;
        float4 normalBiasWS;
        float4 cascadeSplitsVS;
        float4 cascadeScaleBias[4];
        float4 spotShadowInfo;
        float4 lightCounts;
        mat4   lightViewProj[4];
        float4 vsmParams;             // Rung 2 / Step 21: x = useVsm, y = vsmRefDist
        float4 clipmapParams;         // Step 24f: x = baseExtent, y = normalBias (texels), z = depthBias (NDC)
        mat4   clipmapViewProj[8];    // Step 24f: directional clipmap level viewProjs
    };

    struct OceanReflectionConstants
    {
        mat4 view{};
        mat4 proj{};
        mat4 invView{};
        mat4 invProj{};
        float depthA = 0.0f;
        float depthB = 0.0f;
        float2 screenSize{};
        float2 invScreenSize{};
        float2 outputSize{};
        float3 camPosWS{};
        float waterHeight = 0.0f;
    };

    template <typename T>
    D3D12_GPU_VIRTUAL_ADDRESS UploadFrameCB(Renderer* renderer, const T& data)
    {
        constexpr UINT kAlign = render::kConstantBufferAlignment;
        const UINT sizeBytes = static_cast<UINT>((sizeof(T) + (kAlign - 1)) & ~(kAlign - 1));
        auto alloc = renderer->GetFrameResource()->AllocDynamic(sizeBytes, kAlign);
        std::memcpy(alloc.cpu, &data, sizeof(T));
        return alloc.gpu;
    }

    // W3/W5: the ONE place the wind tail of PerView is filled. The gbuffer and the shadow views
    // must carry identical wind values, or the shadow bends out of step with the tree it belongs to.
    void ApplyWind(PerViewCB& vc, const vfx::WindState* wind)
    {
        if (!wind) { return; } // no wind state -> defaults (swayAmp 0 => WindOffset returns 0)
        vc.windTime = wind->time;
        vc.windPrevTime = wind->prevTime;
        vc.windDirX = wind->windDirXZ.x;
        vc.windDirZ = wind->windDirXZ.y;
        vc.windSwayAmp = wind->swayAmplitude;
        vc.windSwayFreq = wind->swayFrequency;
        vc.windGustMul = wind->gustMul;
        vc.windPrevGustMul = wind->prevGustMul;
    }

    D3D12_GPU_VIRTUAL_ADDRESS BuildGBufferViewCB(Renderer* renderer, const Camera& camera,
                                                 const vfx::WindState* wind)
    {
        PerViewCB vc{};
        vc.viewProj = camera.GetViewProjMatrix();
        vc.viewProjNoJitter = camera.GetViewProjMatrixNoJitter();
        vc.prevViewProjNoJitter = camera.GetPrevViewProjMatrixNoJitter();
        ApplyWind(vc, wind); // W3: W4's VS reads it for the sway + the prev-position motion vectors
        return UploadFrameCB(renderer, vc);
    }

    D3D12_GPU_VIRTUAL_ADDRESS BuildShadowViewCB(Renderer* renderer, const mat4& lightView, const mat4& lightProj,
                                                const vfx::WindState* wind)
    {
        PerViewCB vc{};
        vc.viewProj = lightView * lightProj; // viewProjNoJitter/prevViewProjNoJitter unused by shadow shaders
        ApplyWind(vc, wind); // W5: the shadow VS sways casters with the SAME params as the gbuffer
        return UploadFrameCB(renderer, vc);
    }

    D3D12_GPU_VIRTUAL_ADDRESS BuildGlassViewCB(Renderer* renderer, const Camera& camera, const SceneFrameData& frame,
                                               bool glassReflActive)
    {
        GlassViewCB vc{};
        vc.view = camera.GetViewMatrix();
        vc.proj = camera.GetProjMatrix();
        vc.viewProj = camera.GetViewProjMatrix();
        vc.viewProjNoJitter = camera.GetViewProjMatrixNoJitter();
        vc.prevViewProjNoJitter = camera.GetPrevViewProjMatrixNoJitter();
        vc.invView = camera.GetInvViewMatrix();
        vc.invProj = camera.GetInvProjMatrix();

        float skyIntensity = 1.0f;
        if (frame.skybox) { skyIntensity = frame.skybox->GetExposure(); }
        vc.camPosSky = float4(camera.GetPosition(), skyIntensity);

        if (frame.dirLight)
        {
            const DirectionalLight& dirLight = *frame.dirLight;
            vc.sunDirAmbient = float4(dirLight.GetDirection(), dirLight.GetAmbient());
            vc.sunColorExposure = float4(dirLight.GetColor(), dirLight.GetExposure());
        }

        float3 camDir = camera.GetDirection();
        if (camDir.Length() > Math::EPS) { camDir = camDir.Normalized(); }
        else { camDir = float3(0.0f, 0.0f, 1.0f); }
        vc.camDirWS = float4(camDir, 0.0f);

        const float width = static_cast<float>(std::max(renderer->GetRenderWidth(), 1u));
        const float height = static_cast<float>(std::max(renderer->GetRenderHeight(), 1u));
        vc.screenSizeInv = float4(width, height, width > 0.0f ? 1.0f / width : 0.0f, height > 0.0f ? 1.0f / height : 0.0f);

        const auto& deferred = renderer->GetDeferredForFrame();
        const float shadowRes = static_cast<float>(std::max(deferred.shadowRes, 1u));
        const float invShadow = shadowRes > 0.0f ? 1.0f / shadowRes : 0.0f;
        vc.shadowAtlasSizeInv = float4(shadowRes, shadowRes, invShadow, invShadow);

        const SceneFrameData::CascadeData& cascades = frame.cascades;
        vc.shadowBiasNDC = float4(cascades.depthBiasNDC[0], cascades.depthBiasNDC[1], cascades.depthBiasNDC[2], cascades.depthBiasNDC[3]);
        vc.normalBiasWS = float4(cascades.normalBiasWS[0], cascades.normalBiasWS[1], cascades.normalBiasWS[2], cascades.normalBiasWS[3]);
        vc.cascadeSplitsVS = float4(cascades.splitsVS[0], cascades.splitsVS[1], cascades.splitsVS[2], cascades.splitsVS[3]);
        for (int i = 0; i < 4; ++i)
        {
            vc.cascadeScaleBias[i] = float4(cascades.atlasScale[i].x, cascades.atlasScale[i].y, cascades.atlasBias[i].x, cascades.atlasBias[i].y);
            vc.lightViewProj[i] = cascades.lightView[i] * cascades.lightProj[i];
        }

        const float spotRes = static_cast<float>(std::max(deferred.spotShadowRes, 1u));
        const float invSpot = spotRes > 0.0f ? 1.0f / spotRes : 0.0f;
        vc.spotShadowInfo = float4(spotRes, spotRes, invSpot, invSpot);

        const float pointCount = frame.lightManager ? static_cast<float>(frame.lightManager->PointLights().size()) : 0.0f;
        const float spotCount = frame.lightManager ? static_cast<float>(frame.lightManager->GetSpotLightCount()) : 0.0f;
        // z = traced glass reflections active (SSR/RT); w = skybox surface
        // reflection enabled (SkyOnly/SSR/RT, but not None).
        const bool skySpecularActive = frame.settings.reflectionSource != ReflectionSource::None;
        vc.lightCounts = float4(pointCount, spotCount,
            glassReflActive ? 1.0f : 0.0f,
            skySpecularActive ? 1.0f : 0.0f);

        // Step 21: VSM sampling for glass — on when the gate is on and the pool is allocated.
        const bool vsmOn = render::VsmActive() && frame.vsm && frame.vsm->IsAllocated();
        vc.vsmParams = float4(vsmOn ? 1.0f : 0.0f, vsm::g_refDist, 0.0f, 0.0f);
        // Step 24f: directional clipmap for glass (matches lighting_cs). Same tunables + level viewProjs.
        vc.clipmapParams = float4(vsm::g_clipmapBaseExtent, vsm::g_clipmapNormalBias, vsm::g_clipmapDepthBias, 0.0f);
        if (frame.clipmapViews)
        {
            for (size_t i = 0; i < 8 && i < frame.clipmapViews->size(); ++i)
            {
                const SceneView& cv = (*frame.clipmapViews)[i];
                vc.clipmapViewProj[i] = cv.view * cv.proj;
            }
        }

        return UploadFrameCB(renderer, vc);
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
    // Drop cached BLAS/TLAS — their Mesh* keys become dangling across a level
    // reload. Re-inited lazily on the next RT-enabled frame.
    asManager_.Reset();
    bindless_.Reset();
    reflectionHistory_.Reset();
    asManagerInited_ = false;
    asScratchRetireFrame_ = 0;
    rtInstances_.clear();
    frame_ = nullptr;
}

void SceneRenderer::InvalidateRaytracing()
{
    // RT-only subset of Reset() — keep materials/handles, rebuild the acceleration structures +
    // bindless geom-info next RT frame. The per-frame register loop (GetOrRegisterMesh) re-runs
    // and re-reads current material SRVs after this clear.
    asManager_.Reset();
    bindless_.Reset();
    reflectionHistory_.Reset();
    asManagerInited_ = false;
    asScratchRetireFrame_ = 0;
    rtInstances_.clear();
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
    }
}

void SceneRenderer::Render(Renderer* renderer, const SceneFrameData& frame)
{
    if (!renderer)
    {
        return;
    }

    frame_ = &frame;
    EnsureFrameResources(renderer);

    // Reflection source (S8) + RT debug viz (S6), gated on hardware support. RT
    // reflections fall back to SSR on non-RT hardware (rtReflect stays false, so
    // the screen-space reflection source runs). The AS is built only when RT reflections or the debug
    // viz need it; otherwise the frame is byte-identical to the SSR/None/SkyOnly path.
    const bool rtSupported = renderer->IsRaytracingSupported();
    // S13: once an AS allocation has failed (low VRAM / device lost), disable RT for
    // the rest of this scene and fall back to SSR — cleanly, never a crash. Sticky
    // until the next level (asManager_.Reset clears it).
    const bool rtFailed = asManager_.BuildFailed();
    if (rtFailed && !rtFailureLogged_)
    {
        OutputDebugStringA("[RT] Acceleration-structure allocation failed; disabling RT, "
                           "falling back to SSR.\n");
        rtFailureLogged_ = true;
    }
    const bool rtDebugView = rtSupported && !rtFailed && frame.settings.rtDebugView;
    const bool rtReflect = rtSupported && !rtFailed && frame.settings.reflectionSource == ReflectionSource::RT;
    const bool clearReflections = frame.settings.reflectionSource == ReflectionSource::None ||
                                  frame.settings.reflectionSource == ReflectionSource::SkyOnly;
    const bool rtBuildAS = rtReflect || rtDebugView;
    rtReflectActive_ = rtReflect; // S15: RT reflections active this frame
    // S15b: glass gets traced reflections in SSR/RT modes. SkyOnly and None skip
    // the glass reflection prepass; the forward shader uses the cubemap only in
    // SkyOnly and suppresses it in None via lightCounts.w.
    glassReflActive_ = !clearReflections;
    if (rtBuildAS && !asManagerInited_)
    {
        asManager_.Init(renderer->GetDevice5());
        bindless_.Init(renderer->GetDevice());
        asManagerInited_ = true;
    }
    // S11 temporal-accumulation history is retired (S12): the hand-rolled denoise pass
    // it fed was an inert pass-through once glossy was parked, so it was removed and these
    // history textures are no longer allocated. The infra (ReflectionHistory / Pass_RTDenoise
    // / rt_reflection_denoise_cs.hlsl) is kept dormant; a future glossy path uses DLSS-RR (S16).

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
    constexpr D3D12_RESOURCE_STATES kSrvAll =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // The main graph is ~16 KB (MaxPasses x Pass, each with a std::function). Built as a
    // local it put that on Render's stack every frame and left no headroom — C6262 fired
    // the moment anything was added to Pass. Owned on the heap and Reset() per frame
    // instead: same semantics (a freshly empty graph), no per-frame stack cost, and no
    // per-frame allocation either.
    using MainRenderGraph = RenderGraph<kMainRenderGraphPassCount>;
    if (!mainRenderGraph_) { mainRenderGraph_ = std::make_unique<MainRenderGraph>(); }
    mainRenderGraph_->Reset();
    MainRenderGraph& rg = *mainRenderGraph_;

    // RT acceleration-structure build (S5): the first pass when RT is enabled.
    // No consumer yet, so it's an independent node (no prereqs/dependents); a
    // future RT reflections pass (S7) will depend on it. The pass declares no
    // resource states and never transitions the AS buffers, so they bypass the
    // the barrier compile entirely and stay in RAYTRACING_ACCELERATION_STRUCTURE.
    size_t pBuildAS = (size_t)-1;
    if (rtBuildAS)
    {
        pBuildAS = rg.AddPass(RenderPass::Main_BuildAS, {},
            [this, renderer](RenderGraphPassContext ctx) {
                CPU_SCOPE(ProfilerScopes::kPassBuildAS);
                Pass_BuildAS(renderer, ctx);
            });
        // Measured: performs no transitions (the AS build bypasses the tracker entirely).
        rg.SetPassPrepare(pBuildAS, [](RenderGraphPassContext&) {});
    }

    // CL group (step 5): the prologue clear and the object-compute dispatches are
    // two tiny back-to-back lists with no mtDeps; share one command list.
    rg.BeginCLGroup();
    auto pClear = rg.AddPass(RenderPass::Main_PrologueClear, {},
        [this, renderer](RenderGraphPassContext ctx) { CPU_SCOPE(ProfilerScopes::kPassPrologueClear); Pass_PrologueClear(renderer, ctx); });

    auto pCompute = rg.AddPass(RenderPass::Main_ObjectCompute, { pClear },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassObjectCompute);
            Pass_ObjectCompute(renderer, ctx);
        });
    rg.EndCLGroup();
    // Measured: the prologue clear performs no transitions.
    rg.SetPassPrepare(pClear, [](RenderGraphPassContext&) {});
    // Walks exactly the list the body walks, calling the PrepareCompute mirror of
    // ExecuteCompute — so an object added later cannot silently skip registration.
    rg.SetPassPrepare(pCompute, [this](RenderGraphPassContext& p) {
        if (!frame_->objects) { return; }
        for (const auto& obj : *frame_->objects)
        {
            if (!obj) { continue; }
            obj->PrepareCompute(p);
        }
    });

    auto pShoreDepth = rg.AddPass(RenderPass::Main_TerrainDepth, { pCompute },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassShoreDepth);
            OceanSimulation* oceanSim = Systems::GetOceanSimulation();
            const SceneView* shoreView = oceanSim ? &oceanSim->GetShoreDepthView() : nullptr;
            Pass_ShoreDepth(renderer, ctx, shoreView);
        });
    rg.SetPassPrepare(pShoreDepth, [](RenderGraphPassContext& p) {
        OceanSimulation* oceanSim = Systems::GetOceanSimulation();
        ID3D12Resource* shoreDepth = oceanSim ? oceanSim->GetShoreDepthResource() : nullptr;
        if (!shoreDepth) { return; }
        // Step 7: same gate the body uses — registering on a frame it will skip advances the
        // compile past barriers nobody emits.
        if (!oceanSim->ShouldRenderShoreDepth()) { return; }
        p.Use(shoreDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        p.NextPoint();
        p.Use(shoreDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    });

    // Rung 0 / Step 4: GPU cull -> indirect shadow args, before the shadow passes (its output
    // is not consumed yet). Manages its own UAV states (declares none). Placed in the chain so
    // Step 6's ExecuteIndirect can consume it.
    auto pShadowCull = rg.AddPass(RenderPass::Main_ShadowCull, { pShoreDepth },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassShadowCull);
            Pass_ShadowCull(renderer, ctx);
        });
    rg.SetPassPrepare(pShadowCull, [this](RenderGraphPassContext& p) {
        if (frame_->shadowGpu) { frame_->shadowGpu->PrepareCullPass(p); }
    });

    // Step 24f-2: in VSM mode directional shadows come from the clipmap and the CSM cascade atlas is a
    // 1x1 placeholder — OMIT the Main_CSM pass entirely. (Adding it but skipping its per-cascade
    // command lists breaks the parallel-execution CL timeline the graph expects → GPU hang, and its
    // declared D.shadow->DEPTH_WRITE transition would go unrecorded.) Downstream passes chain off the
    // cull instead; the light passes still declare the 1x1 D.shadow NON_PIXEL for their (unused) bind.
    const bool vsmDirectional = render::VsmActive() && frame_->vsm && frame_->vsm->IsAllocated();
    size_t pShadow;
    if (vsmDirectional)
    {
        pShadow = pShadowCull;
    }
    else
    {
        pShadow = rg.AddPass(RenderPass::Main_CSM, { pShadowCull },
            { { D.shadow.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE } },
            [this, renderer](RenderGraphPassContext ctx) {
                CPU_SCOPE(ProfilerScopes::kPassCSM);
                Pass_CSM(renderer, ctx, *frame_->cascadeViews);
            });
        rg.SetPassPrepare(pShadow, [this](RenderGraphPassContext& p) {
            p.UseDeclared(); // the CSM atlas -> DEPTH_WRITE
            p.NextPoint();
            if (frame_->cascadeViews) { PrepareOpaqueDrawStates(p, frame_->cascadeViews->data(), frame_->cascadeViews->size(), /*shadowDraw=*/true); }
        });
    }

    // No declarations: the per-light command lists are recorded in parallel with
    // no deterministic submit order inside the batch, so each list must register
    // the atlas state itself (first-use in whichever list lands first).
    auto pSpotShadow = rg.AddPass(RenderPass::Main_SpotShadows, { pShadow },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassSpotShadow);
            Pass_SpotShadows(renderer, ctx, *frame_->spotShadowViews);
        });
    // Every per-light list re-registers the atlas itself, so one registration of the state
    // covers all of them (the comparator matches by state, not by count).
    rg.SetPassPrepare(pSpotShadow, [this](RenderGraphPassContext& p) {
        // Step 7: register NOTHING when the body will early-out. Under the tracker an
        // over-registration was a benign "INFO extra"; under the flip the compile advances its
        // model past a barrier nobody emits, and every later use of that resource gets a wrong
        // before-state. Gate on conditions that cannot change between Prepare and Record.
        if (render::VsmActive()) { return; } // Pass_SpotShadows returns immediately in VSM mode
        if (!frame_->spotShadowViews) { return; }
        // Only the ACTIVE views: the arrays are fixed-size and their tail entries keep queues from
        // earlier frames, whose object pointers a level switch has already freed. Pass_SpotShadows
        // dispatches over exactly this count for the same reason — reading past it crashed the
        // stress harness inside the very first SwitchLevel.
        const size_t n = std::min(frame_->spotShadowViews->size(), frame_->lightManager->GetShadowedSpotCount());
        if (n == 0) { return; }
        p.Use(p.renderer->GetDeferredForFrame().spotShadow.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        p.NextPoint();
        PrepareOpaqueDrawStates(p, frame_->spotShadowViews->data(), n, /*shadowDraw=*/true);
    });

    // B2b: point cube shadows. Same per-CL atlas-state registration story as spot
    // shadows (parallel per-face lists, no declared states). Runs before Pass_PointLights
    // (which samples the cube atlas in B3).
    auto pPointShadow = rg.AddPass(RenderPass::Main_PointShadows, { pSpotShadow },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassPointShadow);
            Pass_PointShadows(renderer, ctx, *frame_->pointShadowViews);
        });
    rg.SetPassPrepare(pPointShadow, [this](RenderGraphPassContext& p) {
        if (render::VsmActive()) { return; } // same early-out as the spot pass
        if (!frame_->pointShadowViews) { return; }
        // Pass_PointShadows returns before opening any list when no point light is shadowed —
        // mirror it, or the atlas transition below is compiled and never emitted.
        const size_t n = std::min(frame_->pointShadowViews->size(),
                                  frame_->lightManager->GetShadowedPointCount() * 6);
        if (n == 0) { return; }
        p.Use(p.renderer->GetDeferredForFrame().pointShadow.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        p.NextPoint();
        PrepareOpaqueDrawStates(p, frame_->pointShadowViews->data(), n, /*shadowDraw=*/true);
    });

    auto pGbuf = rg.AddPass(RenderPass::Main_GBuffer, { pPointShadow },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassGBuffer);
            Pass_GBuffer(renderer, ctx, *frame_->camera, *frame_->mainView);
        });
    // Pass_GBuffer's states live on its INNER graph's driver pass, which applies them itself;
    // the outer pass owns none. Mirror that declaration here so the outer list is complete.
    rg.SetPassPrepare(pGbuf, [this](RenderGraphPassContext& p) {
        const auto& DG = p.renderer->GetDeferredForFrame();
        p.Use(DG.gb0.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        p.Use(DG.gb1.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        p.Use(DG.gb2.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        p.Use(DG.gbVelocity.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
#if WITH_EDITOR
        p.Use(DG.objectID.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
#endif
        p.Use(DG.gbAux.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        p.Use(DG.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);

        // Objects transition their own buffers inside Render, on the fan-out worker (GPU-
        // instanced clouds flip the instance buffer back to SRV).
        p.NextPoint();
        if (frame_->mainView) { PrepareOpaqueDrawStates(p, frame_->mainView, 1, /*shadowDraw=*/false); }
    });

    // Rung 2 / Step 19: VSM page-request pass — reads the camera depth (after GBuffer), marks the
    // virtual pages the frame needs. Independent consumer of depth (its output is unused for now),
    // so it doesn't gate lighting. Manages the request-buffer UAV state itself.
    auto pVsmPageRequest = rg.AddPass(RenderPass::Main_VsmPageRequest, { pGbuf },
        { { D.depth.Get(), kSrvAll } },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassVsmPageRequest);
            Pass_VsmPageRequest(renderer, ctx);
        });
    rg.SetPassPrepare(pVsmPageRequest, [this](RenderGraphPassContext& p) {
        // Step 7: mirror BOTH of the body's gates. `vsmSkipUpdate_` is decided before the
        // graph is built and does not change during the frame, so this is exact.
        if (!render::VsmActive() || vsmSkipUpdate_) { return; }
        // IsAllocated BEFORE UseDeclared: the body returns on it too, so declaring the depth read
        // above this line registered a barrier the body would never emit.
        if (!frame_->vsm || !frame_->vsm->IsAllocated()) { return; }
        // The depth read is registered HERE rather than via UseDeclared, and the body performs the
        // matching transition. It used to do NEITHER — the pass read the depth SRV and relied on
        // some later pass having already moved depth to a read state. That reliance was wrong:
        // this pass runs right after the G-buffer, which leaves depth in DEPTH_WRITE, and
        // GPU-based validation reported the SRV-over-a-DEPTH_WRITE-resource every frame
        // (id=1358, on all three Deferred[N].Depth). Reading a resource the graph has not
        // transitioned is undefined however the barriers are emitted.
        // UseDeclared is still not used: it would register the WHOLE declare list, and the point
        // structure below belongs to VSM's own buffers.
        p.Use(p.renderer->GetDeferredForFrame().depth.Get(), kSrvAll);
        // VSM owns the buffers its Record* functions transition, so it registers them itself.
        frame_->vsm->PrepareRequestPass(p);
    });
    (void)pVsmPageRequest;

    // Rung 2 / Step 22: render shadow casters into the resident physical pages (depth-only into the
    // VSM pool). Only wired when the VSM path is on, so the default render is untouched. Consumes
    // Step 20's page table + Rung 0's per-view cull; the light passes (Step 21) read the pool.
    // Perf: skip the VSM update (request + alloc + render) only when NOTHING changed — the camera
    // view is unchanged AND no shadow caster moved. Then the pool + page table persist and last
    // frame's content is still valid (saving the dominant cost, the page-render draw loop). Gating
    // on movers is essential: a rotating caster must re-render its shadow every frame even with a
    // still camera (otherwise its shadow freezes). The view matrix carries the camera transform
    // with NO jitter (jitter lives in proj), so it is bit-stable when the camera is still.
    vsmSkipUpdate_ = false;
    if (render::VsmActive() && frame_->mainView)
    {
        const bool viewStill = vsmHasRendered_ &&
            std::memcmp(&frame_->mainView->view, &vsmLastView_, sizeof(mat4)) == 0;
        // W5: a wind-swayed caster animates in the VERTEX shader, so its transform never changes and
        // MoverCount() stays 0 — without this the pool freezes after a few still frames and the palm
        // shadow stops swaying while the tree keeps going (the shadow visibly detaches).
        const bool windAnimating = frame_->wind && frame_->wind->swayAmplitude > 0.0f &&
                                   frame_->shadowGpu && frame_->shadowGpu->HasWindCasters();
        const bool contentStill = (!frame_->shadowGpu || frame_->shadowGpu->MoverCount() == 0) &&
                                  !windAnimating;
        if (viewStill && contentStill)
        {
            // Keep rendering for a few frames after everything goes still so the resident set + the
            // physOwner readback snapshot (kFrameCount-latent) catch up before we freeze the pool —
            // otherwise a just-stopped camera freezes a still-incomplete render.
            if (vsmStillFrames_ < 0xFFFFu) { ++vsmStillFrames_; }
            vsmSkipUpdate_ = vsmStillFrames_ > render::kFrameCount + 1u;
        }
        else
        {
            vsmStillFrames_ = 0;
            vsmLastView_ = frame_->mainView->view;
            vsmHasRendered_ = true;
        }
    }
    else
    {
        vsmHasRendered_ = false;
        vsmStillFrames_ = 0;
    }

    size_t pVsmPageRender = static_cast<size_t>(-1);
    const bool vsmActive = render::VsmActive() && frame_->vsm && frame_->vsm->IsAllocated();
    if (vsmActive)
    {
        // No declared pool state: RecordPageRender transitions the pool DEPTH_WRITE itself (the
        // light passes declare it back to SRV). Ordering to the light passes is via their prereq.
        pVsmPageRender = rg.AddPass(RenderPass::Main_VsmPageRender, { pVsmPageRequest },
            [this, renderer](RenderGraphPassContext ctx) {
                CPU_SCOPE(ProfilerScopes::kPassVsmPageRender);
                Pass_VsmPageRender(renderer, ctx);
            });
        rg.SetPassPrepare(pVsmPageRender, [this](RenderGraphPassContext& p) {
            // Same two gates as the body — see the request pass.
            if (!render::VsmActive() || vsmSkipUpdate_) { return; }
            if (!frame_->shadowGpu) { return; }
            frame_->vsm->PrepareRenderPass(p, frame_->shadowGpu, frame_->wind);
        });
    }
    (void)pVsmPageRender;

    auto lightFn = [this, renderer](RenderGraphPassContext ctx) {
        CPU_SCOPE(ProfilerScopes::kPassLighting);
        Pass_Lighting(renderer, ctx, *frame_->camera);
    };
    size_t pLight;
    // Step 24f: in VSM mode the directional shader samples the clipmap (VSM page pool + table), so it
    // must order AFTER the page render and declare those SRV-readable. Legacy = the CSM-only decls.
    if (vsmActive && pVsmPageRender != static_cast<size_t>(-1))
    {
        ID3D12Resource* vpool = frame_->vsm->PagePool();
        ID3D12Resource* vpt = frame_->vsm->PageTable();
        pLight = rg.AddPassMT(RenderPass::Main_Lighting, { pGbuf, pVsmPageRender }, { pShadow },
            { { D.gb0.Get(), kSrvAll }, { D.gb1.Get(), kSrvAll }, { D.gb2.Get(), kSrvAll },
              { D.gbVelocity.Get(), kSrvAll }, { D.gbAux.Get(), kSrvAll }, { D.depth.Get(), kSrvAll },
              { D.shadow.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { vpool, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { vpt, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
            lightFn);
    }
    else
    {
        pLight = rg.AddPassMT(RenderPass::Main_Lighting, { pGbuf }, { pShadow },
            { { D.gb0.Get(), kSrvAll },
              { D.gb1.Get(), kSrvAll },
              { D.gb2.Get(), kSrvAll },
              { D.gbVelocity.Get(), kSrvAll },
              { D.gbAux.Get(), kSrvAll },
              { D.depth.Get(), kSrvAll },
              { D.shadow.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
            lightFn);
    }

    // Step 21: the spot lighting shader always binds the VSM page-table (t7) + pool (t8) SRVs, so
    // when the VSM is allocated they must be in a readable state on entry (declared here). When VSM
    // sampling is active, also order after the page render (fresh page content this frame).
    auto spotFn = [this, renderer](RenderGraphPassContext ctx) {
        CPU_SCOPE(ProfilerScopes::kPassSpotLights);
        Pass_SpotLights(renderer, ctx, *frame_->camera);
    };
    const bool vsmAlloc = frame_->vsm && frame_->vsm->IsAllocated();
    size_t pSpotLights;
    if (vsmAlloc)
    {
        ID3D12Resource* vpool = frame_->vsm->PagePool();
        ID3D12Resource* vpt = frame_->vsm->PageTable();
        const std::initializer_list<ResourceStateDecl> spotDecls = {
            { D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
            { D.gb0.Get(), kSrvAll }, { D.gb1.Get(), kSrvAll }, { D.gb2.Get(), kSrvAll },
            { D.gbVelocity.Get(), kSrvAll }, { D.gbAux.Get(), kSrvAll }, { D.depth.Get(), kSrvAll },
            { D.spotShadow.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
            { vpool, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
            { vpt, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } };
        if (vsmActive && pVsmPageRender != static_cast<size_t>(-1))
        {
            pSpotLights = rg.AddPassMT(RenderPass::Main_SpotLights, { pLight, pVsmPageRender }, { pSpotShadow }, spotDecls, spotFn);
        }
        else
        {
            pSpotLights = rg.AddPassMT(RenderPass::Main_SpotLights, { pLight }, { pSpotShadow }, spotDecls, spotFn);
        }
    }
    else
    {
        pSpotLights = rg.AddPassMT(RenderPass::Main_SpotLights, { pLight }, { pSpotShadow },
            { { D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
              { D.gb0.Get(), kSrvAll }, { D.gb1.Get(), kSrvAll }, { D.gb2.Get(), kSrvAll },
              { D.gbVelocity.Get(), kSrvAll }, { D.gbAux.Get(), kSrvAll }, { D.depth.Get(), kSrvAll },
              { D.spotShadow.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } },
            spotFn);
    }

    // Depends on pPointShadow too: the cube must be rendered + transitioned to a
    // shader-readable state before this pass samples it (B3). kSrvAll keeps it readable
    // by both this compute pass and the later transparent (glass) pixel pass. Step 21: the point
    // shader also binds the VSM page-table (t7) + pool (t8) SRVs; ordering after the page render is
    // transitive (this pass depends on pSpotLights, which depends on pVsmPageRender when active).
    auto pointFn = [this, renderer](RenderGraphPassContext ctx) {
        CPU_SCOPE(ProfilerScopes::kPassPointLights);
        Pass_PointLights(renderer, ctx, *frame_->camera);
    };
    size_t pPointLights;
    if (vsmAlloc)
    {
        pPointLights = rg.AddPass(RenderPass::Main_PointLights, { pSpotLights, pPointShadow },
            { { D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
              { D.gb0.Get(), kSrvAll }, { D.gb1.Get(), kSrvAll }, { D.gb2.Get(), kSrvAll },
              { D.gbVelocity.Get(), kSrvAll }, { D.gbAux.Get(), kSrvAll }, { D.depth.Get(), kSrvAll },
              { D.pointShadow.Get(), kSrvAll },
              { frame_->vsm->PagePool(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { frame_->vsm->PageTable(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } },
            pointFn);
    }
    else
    {
        pPointLights = rg.AddPass(RenderPass::Main_PointLights, { pSpotLights, pPointShadow },
            { { D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
              { D.gb0.Get(), kSrvAll }, { D.gb1.Get(), kSrvAll }, { D.gb2.Get(), kSrvAll },
              { D.gbVelocity.Get(), kSrvAll }, { D.gbAux.Get(), kSrvAll }, { D.depth.Get(), kSrvAll },
              { D.pointShadow.Get(), kSrvAll } },
            pointFn);
    }

    // The three lighting passes each have several AddPass variants (VSM vs Legacy, with/without
    // spot or point shadows) but every variant declares its own first-use set, so one Prepare
    // each covers all of them. All three always create a real pass — none falls back to a
    // previous index, which would otherwise attach a second Prepare to somebody else's pass.
    rg.SetPassPrepare(pLight, [](RenderGraphPassContext& p) { p.UseDeclared(); });
    rg.SetPassPrepare(pSpotLights, [this](RenderGraphPassContext& p) {
        if (frame_->lightManager->GetSpotLightCount() == 0) { return; } // body early-outs
        p.UseDeclared();
    });
    rg.SetPassPrepare(pPointLights, [this](RenderGraphPassContext& p) {
        if (frame_->lightManager->PointLights().empty()) { return; } // body early-outs
        p.UseDeclared();
    });

    auto pSky = rg.AddPass(RenderPass::Main_Skybox, { pPointLights },
        { { D.light.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
          { D.gbVelocity.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
          { D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_READ } },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassSkybox);
            Pass_Skybox(renderer, ctx, *frame_->camera);
        });
    rg.SetPassPrepare(pSky, [](RenderGraphPassContext& p) { p.UseDeclared(); });

    // CL group (step 5): reflection source -> reflection blur -> compose is a sequential single-dispatch
    // chain with no mtDeps. Grouping collapses its 3 command lists into 1 — the
    // per-CL prologue/acquire overhead dominates these passes' tiny record cost,
    // and the inter-pass acquire barriers become correctly-placed intra-CL barriers.
    rg.BeginCLGroup();
    // Reflection source (S8): whichever variant runs writes the same premultiplied
    // reflection buffer, so the blur + compose chain is identical. RT (S7) runs
    // instead of the screen-space source (mt-dep on Main_BuildAS; its TLAS SRV
    // bypasses the state tracker); None/SkyOnly clear the reflection buffer and
    // compose decides whether the skybox fallback is enabled.
    const bool useRtReflections = rtReflect && pBuildAS != (size_t)-1;
    const std::initializer_list<ResourceStateDecl> reflectDecls = {
        { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { D.gb1.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { D.light.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { D.reflection.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } };
    size_t pReflectionSource; // node the blur depends on (reflection chain end)
    if (useRtReflections)
    {
        // RT (S7/S10): trace one sharp reflection ray per surface and shade the hit;
        // write the premultiplied reflection straight into the main reflection target
        // (S12: the old temporal-denoise pass was an inert pass-through once glossy was
        // parked, so it was removed -- blur + compose consume `reflection` directly).
        pReflectionSource = rg.AddPassMT(RenderPass::Main_RTReflections, { pSky }, { pSky, pBuildAS },
            { { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.gb1.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.light.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.reflection.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
            [this, renderer](RenderGraphPassContext ctx) {
                CPU_SCOPE(ProfilerScopes::kPassRTReflections);
                Pass_RTReflections(renderer, ctx, *frame_->camera);
            });
    }
    else if (clearReflections)
    {
        pReflectionSource = rg.AddPass(RenderPass::Main_ReflectionSource, { pSky },
            { { D.reflection.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
            [this, renderer](RenderGraphPassContext ctx) {
                CPU_SCOPE(ProfilerScopes::kPassReflectionSource);
                Pass_ClearReflections(renderer, ctx);
            });
    }
    else
    {
        pReflectionSource = rg.AddPass(RenderPass::Main_ReflectionSource, { pSky }, reflectDecls,
            [this, renderer](RenderGraphPassContext ctx) {
                CPU_SCOPE(ProfilerScopes::kPassReflectionSource);
                Pass_ScreenSpaceReflections(renderer, ctx, *frame_->camera);
            });
    }
    // Whichever variant was added above (RT / clear / SSR) declares its own first-use set,
    // so one Prepare covers all three.
    rg.SetPassPrepare(pReflectionSource, [](RenderGraphPassContext& p) { p.UseDeclared(); });

    // First-use states only; the blur ping-pongs reflection<->scratch states between
    // its two dispatches inside the pass body.
    auto pBlur = rg.AddPass(RenderPass::Main_ReflectionBlur, { pReflectionSource },
        { { D.reflection.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.reflectionScratch.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
          { D.gb0.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } }, // S16: roughness drives glossy blur
        [this, renderer](RenderGraphPassContext ctx) { CPU_SCOPE(ProfilerScopes::kPassReflectionBlur); Pass_ReflectionBlur(renderer, ctx); });

    // Barrier plan step 3: the first pass converted to a Prepare, as the comparator's test
    // subject. Chosen because it exercises the hard cases in one pass — `reflection` and
    // `reflectionScratch` each take TWO states inside the body (census category C), and the
    // second pair is behind a predicate the body evaluates (category B).
    //
    // The body is UNCHANGED at this step: it still calls ApplyDeclaredStates + Transition, and
    // the comparator only watches. `ctx.Barrier` starts replacing them at step 5.
    // Note the predicate is evaluated here AND in the body — that duplication is exactly what
    // D1.1 forbids once this goes authoritative; step 5 hoists it into pass state.
    {
        ID3D12Resource* const blurRefl = D.reflection.Get();
        ID3D12Resource* const blurScratch = D.reflectionScratch.Get();
        ID3D12Resource* const blurGb0 = D.gb0.Get();
        rg.SetPassPrepare(pBlur, [this, blurRefl, blurScratch, blurGb0](RenderGraphPassContext& p) {
            p.Use(blurRefl, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            p.Use(blurScratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            p.Use(blurGb0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (resources_.GetBlurMaterial() && resources_.GetBlurCBSizeBytes() != 0) {
                p.NextPoint(); // the vertical dispatch ping-pongs the two targets
                p.Use(blurScratch, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                p.Use(blurRefl, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
        });
    }

    // First-use states only; Compose transitions scene back to RENDER_TARGET
    // for the transparent pass at the end of its body.
    auto pCompose = rg.AddPass(RenderPass::Main_Compose, { pBlur },
        { { D.gb0.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.gb1.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.gb2.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.gbAux.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.light.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.reflection.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.scene.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassCompose);
            Pass_Compose(renderer, ctx, *frame_->camera);
        });
    // Compose hands `scene` back to the transparent pass as a render target on EVERY path —
    // both early-outs and the success tail — so it is a second unconditional point.
    {
        ID3D12Resource* const composeScene = D.scene.Get();
        rg.SetPassPrepare(pCompose, [composeScene](RenderGraphPassContext& p) {
            p.UseDeclared();
            p.NextPoint();
            p.Use(composeScene, D3D12_RESOURCE_STATE_RENDER_TARGET);
        });
    }
    rg.EndCLGroup();

    // RT debug visualization (S6): runs AFTER the reflection group so it can overwrite
    // the already-consumed reflection target with ray-hit data for inspection via
    // TextureDebugViewer -> Reflection, without disturbing the composited scene. Needs
    // the TLAS (mtDep on Main_BuildAS) and reflection free (prereq/mtDep on Compose).
    // The TLAS SRV bypasses the state tracker (staged as a plain descriptor).
    if (rtDebugView && pBuildAS != (size_t)-1)
    {
        const size_t pRtDebug = rg.AddPassMT(RenderPass::Main_RTDebug, { pCompose }, { pCompose, pBuildAS },
            { { D.reflection.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
              { D.gb1.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } },
            [this, renderer](RenderGraphPassContext ctx) {
                CPU_SCOPE(ProfilerScopes::kPassRTDebug);
                Pass_RTDebug(renderer, ctx, *frame_->camera);
            });
        // The TLAS SRV is staged as a plain descriptor and never transitioned, so the
        // declared trio is the whole list.
        rg.SetPassPrepare(pRtDebug, [](RenderGraphPassContext& p) { p.UseDeclared(); });
    }

    // Off-screen glass reflections (S15b): render a glass front-face G-buffer (normal+depth)
    // then compute reflections over it into glassReflection (sampled by the forward glass pass).
    // Active in RT mode (rt_reflections_cs, incl. off-screen recompute) AND SSR mode (ssr_cs).
    // Runs after Compose so the lit opaque `light` buffer is the on-screen color source.
    // None/SkyOnly skip these passes; glass.hlsl independently suppresses or samples
    // its skybox fallback through the second b1 flag.
    size_t pGlassReflect = (size_t)-1;
    if (glassReflActive_)
    {
        size_t pGlassGbuf = rg.AddPass(RenderPass::Main_GlassReflGbuffer, { pCompose },
            { { D.glassReflNormal.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
              { D.glassReflDepth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE } },
            [this, renderer](RenderGraphPassContext ctx) {
                CPU_SCOPE(ProfilerScopes::kPassGlassReflGbuffer);
                Pass_GlassReflGbuffer(renderer, ctx, *frame_->camera, *frame_->mainView);
            });
        const std::initializer_list<ResourceStateDecl> glassReflDecls = {
            { D.glassReflNormal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
            { D.glassReflDepth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
            { D.light.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
            { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
            { D.glassReflection.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } };
        if (useRtReflections && pBuildAS != (size_t)-1)
        {
            // RT mode: dispatch rt_reflections_cs (needs the TLAS, so mt-dep on pBuildAS).
            pGlassReflect = rg.AddPassMT(RenderPass::Main_GlassReflections, { pGlassGbuf }, { pGlassGbuf, pBuildAS },
                glassReflDecls,
                [this, renderer](RenderGraphPassContext ctx) {
                    CPU_SCOPE(ProfilerScopes::kPassGlassReflections);
                    Pass_GlassReflections(renderer, ctx, *frame_->camera);
                });
        }
        else
        {
            // SSR mode: dispatch ssr_cs over the glass G-buffer (no TLAS, works on all HW).
            pGlassReflect = rg.AddPass(RenderPass::Main_GlassReflections, { pGlassGbuf },
                glassReflDecls,
                [this, renderer](RenderGraphPassContext ctx) {
                    CPU_SCOPE(ProfilerScopes::kPassGlassReflections);
                    Pass_GlassReflectionsSSR(renderer, ctx, *frame_->camera);
                });
        }
        rg.SetPassPrepare(pGlassGbuf, [](RenderGraphPassContext& p) { p.UseDeclared(); });
        rg.SetPassPrepare(pGlassReflect, [](RenderGraphPassContext& p) { p.UseDeclared(); });
    }

    // No declarations: the driver sequences depth/scene copies (COPY_SOURCE/DEST flips mid-list)
    // before rebinding the targets — inherently ordered work. When glass reflections are active,
    // order the transparent pass after the glass-reflection compute (it samples glassReflection;
    // pCompose + the AS build are covered transitively through it).
    const std::initializer_list<size_t> transpDeps = glassReflActive_
        ? std::initializer_list<size_t>{ pCompose, pGlassReflect }
        : std::initializer_list<size_t>{ pCompose };
    auto pTransp = rg.AddPass(RenderPass::Main_Transparent, transpDeps,
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassTransparent);
            Pass_Transparent(renderer, ctx, *frame_->camera, *frame_->mainView);
        });
    rg.SetPassPrepare(pTransp, [this](RenderGraphPassContext& p) {
        const auto& DT = p.renderer->GetDeferredForFrame();
        // 1. Snapshot depth + opaque colour for the refraction/reflection reads.
        if (DT.depthCopy.Get())
        {
            p.Use(DT.depth.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            p.Use(DT.depthCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        }
        if (DT.sceneOpaque.Get())
        {
            p.Use(DT.scene.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            p.Use(DT.sceneOpaque.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        }
        // 2. RecordOceanReflection's compute. Registered even when it early-outs to
        // makePixelReadable: the early-out path is chosen inside the body.
        p.NextPoint();
        p.Use(DT.sceneOpaque.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        p.Use(DT.depthCopy.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        p.Use(DT.oceanReflection.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // 3. makePixelReadable: all three become PS-readable for the forward draws.
        p.NextPoint();
        p.Use(DT.sceneOpaque.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        p.Use(DT.depthCopy.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        p.Use(DT.oceanReflection.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        // 4. Rebind the forward targets. The fan-out chunks re-apply the velocity/objectID
        // pair per chunk; same states, so one registration covers them.
        p.NextPoint();
        p.Use(DT.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        p.Use(DT.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        p.Use(DT.gbVelocity.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
#if WITH_EDITOR
        p.Use(DT.objectID.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
#endif
        p.Use(DT.glassReflection.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        // 5. Per-object reads (ocean displacement, particle sim buffers), on fan-out workers.
        if (!frame_->objects) { return; }
        p.NextPoint();
        for (const auto& obj : *frame_->objects)
        {
            if (!obj || !obj->IsTransparent()) { continue; }
            obj->PrepareRender(p);
        }
    });

#if WITH_EDITOR
    size_t pObjectIdReadback = pTransp;
    if (renderer->HasPendingObjectIdPick())
    {
        pObjectIdReadback = rg.AddPass(RenderPass::Main_ObjectIdReadback, { pTransp },
            { { D.objectID.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE } },
            [renderer](RenderGraphPassContext ctx) {
                auto t = ctx.BeginCL();
                SetCommandListName(t.cl, ctx.pass);
                ctx.ApplyDeclaredStates(t.cl);
                renderer->RecordObjectIdPickReadback(t.cl);
                ctx.EndCL(t);
            });
        // Inside the `if` on purpose: without a pending pick pObjectIdReadback aliases
        // pTransp, and a Prepare set here would attach to the transparent pass instead.
        rg.SetPassPrepare(pObjectIdReadback, [](RenderGraphPassContext& p) { p.UseDeclared(); });
    }
#else
    const size_t pObjectIdReadback = pTransp;
#endif

    auto pDebugDraw = rg.AddPass(RenderPass::Main_DebugDraw, { pObjectIdReadback },
        { { D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
          { D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE } },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassDebugDraw);
            Pass_DebugDraw(renderer, ctx, *frame_->camera);
        });
    rg.SetPassPrepare(pDebugDraw, [renderer](RenderGraphPassContext& p) {
        DebugDrawSystem* dd = renderer->GetDebugDrawSystem();
        if (!dd || !dd->HasCommands()) { return; } // body early-outs
        p.UseDeclared();
    });

    size_t pSelectionOutline = pDebugDraw;
#if WITH_EDITOR
    if (frame_->selectedEditorObjectCount != 0)
    {
        pSelectionOutline = rg.AddPass(RenderPass::Main_SelectionOutline, { pDebugDraw },
            { { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.scene.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
            [this, renderer](RenderGraphPassContext ctx) {
                Pass_SelectionOutline(renderer, ctx);
            });
        rg.SetPassPrepare(pSelectionOutline, [](RenderGraphPassContext& p) { p.UseDeclared(); });
    }
#endif

    // Ensure tonemapping runs after the debug draw pass so the resolved backbuffer
    // always includes any debug geometry submitted during rendering.
    // Only the unconditional outputs are declared; the tonemap source (scene or
    // DLSS output) and the backbuffer copy are handled inside the pass body.
    // CL group (step 5): the optional debug-texture draw follows tonemap on the
    // same target with no mtDeps; share one command list (Debug usually early-outs).
    rg.BeginCLGroup();
    auto pTone = rg.AddPass(RenderPass::Main_Tonemap, { pSelectionOutline },
        { { D.tonemap.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
          { D.fxaa.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
        [this, renderer](RenderGraphPassContext ctx) { CPU_SCOPE(ProfilerScopes::kPassTonemap); Pass_Tonemap(renderer, ctx); });
    // Tonemap also drives the whole DLSS evaluate and the backbuffer resolve. The resolve
    // SOURCE (fxaa vs tonemap) and `ranDlss` are both decided inside the body, so BOTH
    // alternatives are registered — a state the body skips is one redundant barrier, the
    // one it takes and never registered is a missing barrier.
    rg.SetPassPrepare(pTone, [this](RenderGraphPassContext& p) {
        constexpr D3D12_RESOURCE_STATES kNps = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        p.UseDeclared(); // tonemap + fxaa -> UAV
        const auto& DTM = p.renderer->GetDeferredForFrame();
        p.NextPoint();
        if (p.renderer->IsDlssActive())
        {
            // Inside EvaluateDLSS (DlssHandler): the three inputs plus the upscaled output.
            p.Use(DTM.scene.Get(), kNps);
            p.Use(DTM.gbVelocity.Get(), kNps);
            p.Use(DTM.depth.Get(), kNps);
            p.Use(DTM.dlssOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            p.NextPoint();
            p.Use(DTM.dlssOutput.Get(), kNps);
        }
        p.Use(DTM.scene.Get(), kNps); // the non-DLSS tonemap source
        p.NextPoint();
        // The body needs ALL of these, not just the setting — gating on the setting alone
        // registered the FXAA resolve source on frames the FXAA pass could not run.
        const bool fxaa = frame_->settings.doFxaa && resources_.GetFxaaMaterial() &&
                          resources_.GetFxaaCBSizeBytes() > 0 &&
                          p.renderer->GetWidth() > 0 && p.renderer->GetHeight() > 0;
        if (fxaa) { p.Use(DTM.tonemap.Get(), kNps); } // FXAA input
        // Backbuffer resolve. Gated on the SAME things the body needs: without the tonemap
        // material it breaks out before the resolve, and the pass's trailing
        // `Transition(tonemap, UAV)` would then fire this point's restore barrier against a
        // resource that never went to COPY_SOURCE.
        if (!resources_.GetTonemapMaterial() || !p.renderer->GetCurrentBackbuffer()) { return; }
        p.NextPoint();
        // The resolve reads whichever of the two actually produced this frame.
        // The backbuffer is NOT registered: it is driven from outside the graph (present
        // epilogue + RecordBindAndClear both write it with hand-rolled barriers), so the body
        // resolves it with Renderer::TransitionExplicit and the compile models only the source.
        p.Use(fxaa ? DTM.fxaa.Get() : DTM.tonemap.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        p.NextPoint();
        p.Use(fxaa ? DTM.fxaa.Get() : DTM.tonemap.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    });

    const size_t pDebug = rg.AddPass(RenderPass::Main_Debug, { pTone },
        [this, renderer](RenderGraphPassContext ctx) { CPU_SCOPE(ProfilerScopes::kPassDebug); Pass_Debug(renderer, ctx); });
    rg.EndCLGroup();
    // The debug blit binds the backbuffer RTV/DSV and draws a triangle; RecordBindDefaultsNoClear
    // sets targets and viewport only, so the pass performs no transitions at all.
    rg.SetPassPrepare(pDebug, [](RenderGraphPassContext&) {});

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
    const size_t pOverlay = epilogueRG.AddPass(RenderPass::Epilogue_Overlay, {},
        [this, renderer, &overlayPrepTask](RenderGraphPassContext ctx) { CPU_SCOPE(ProfilerScopes::kPassOverlay); Pass_Overlay(renderer, ctx, overlayPrepTask); });
    // Binds the backbuffer RTV/DSV and draws ImGui + text; comparator-verified to perform
    // no transitions.
    epilogueRG.SetPassPrepare(pOverlay, [](RenderGraphPassContext&) {});
    epilogueRG.Execute(renderer);
    renderer->EndFrame();
#if WITH_EDITOR
    renderer->ResolveObjectIdPickReadback();
#endif

    frame_ = nullptr;
}

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

void SceneRenderer::Pass_BuildAS(Renderer* renderer, RenderGraphPassContext ctx)
{
    if (!renderer)
    {
        return;
    }

    // Retire scratch from earlier (one-time) BLAS builds once their command
    // list's frame has surely completed — kFrameCount frames later, when that
    // frame slot is reused and BeginFrame has waited on its fence.
    const uint64_t frameNo = renderer->GetTotalFrameNumber();
    if (asManager_.HasPendingScratch() && frameNo >= asScratchRetireFrame_)
    {
        asManager_.ReleaseCompletedScratch();
    }

    // Gather opaque, single-mesh, CPU-placed instances. Ocean (GPU-displaced) is
    // excluded by design — kept on its planar-reflection path (S13). Instanced clouds
    // (S14) and transparent/glass (S15) also return false from GetRtInstance today;
    // bringing each into RT is its own step.
    rtInstances_.clear();
    if (frame_->objects)
    {
        uint32_t instanceId = 0;
        std::vector<RtInstanceDesc> descs; // reused across objects this frame
        for (const auto& obj : *frame_->objects)
        {
            // The editor's Enabled command maps to visibility. In no-editor
            // builds ordinary level objects retain their default visible state,
            // so this only removes explicitly disabled editor objects from RT.
            if (!obj || !obj->IsVisible()) { continue; }
            // GetRtInstances appends one desc for a single-mesh object, or one per GPU
            // instance for instanced renderables (S14: instanced models reflect).
            descs.clear();
            obj->GetRtInstances(descs);
            // Per-slot RT materials (B3 follow-up): a multi-slot object registers one record per
            // submesh with THAT slot's albedo/MR/params, so palms reflect bark + green fronds
            // instead of slot-0 everywhere. Hit shaders already index per (InstanceID +
            // GeometryIndex). Single-slot objects and GI instance clouds keep the slot-0 path.
            GBufferRenderable* gb = obj->AsGBufferRenderable();
            const bool perSlot = bindless_.Ready() && gb && gb->MultiSlotDraw();
            std::vector<rt::BindlessTable::SlotMaterial> slotMats;
            if (perSlot)
            {
                slotMats.resize(gb->SlotCount());
                for (size_t s = 0; s < slotMats.size(); ++s)
                {
                    MaterialData* md = gb->GetMaterialDataForSlot(s);
                    const MaterialParams* p = gb->InstanceSlotParams(s);
                    if (md && md->hasAlbedo) { slotMats[s].albedoSrv = md->albedo.GetSRVCPU(); }
                    if (md && md->hasMR && p && p->texFlags.y > 0.5f) { slotMats[s].mrSrv = md->mr.GetSRVCPU(); }
                    if (p)
                    {
                        slotMats[s].baseColor4 = &p->baseColor.x;
                        slotMats[s].roughness = p->metalRough.y;
                        slotMats[s].metalness = p->metalRough.x;
                        slotMats[s].mrMultiply = p->mrMultiply > 0.5f;
                    }
                }
            }
            for (const RtInstanceDesc& desc : descs)
            {
                rt::InstanceEntry entry;
                entry.mesh = desc.mesh;
                entry.world = desc.world.m; // Math::mat4 wraps a row-major XMFLOAT4X4
                // TLAS InstanceID = the mesh's bindless geometry index (S9), so a hit
                // can index the geometry/material table directly. Same mesh+material ->
                // same index (all instances of a cloud share one record). Falls back to
                // a running index if the bindless table isn't up.
                if (perSlot && desc.mesh == gb->GetMesh())
                {
                    entry.instanceId = bindless_.GetOrRegisterMesh(desc.mesh, slotMats.data(), slotMats.size());
                }
                else
                {
                    entry.instanceId = bindless_.Ready()
                        ? bindless_.GetOrRegisterMesh(desc.mesh, desc.albedoSrv, desc.mrSrv, &desc.baseColor.x,
                                                      /*roughness*/ desc.metalRough.y, /*metalness*/ desc.metalRough.x,
                                                      desc.mrMultiply)
                        : instanceId;
                }
                rtInstances_.push_back(entry);
                ++instanceId;
            }
        }
    }
    if (bindless_.Ready())
    {
        bindless_.UploadGeometryInfo();
    }

    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassBuildAS);
        // AS buffers bypass the barrier compile: this pass declares no
        // resource states and never calls Transition on them, so the RenderGraph
        // never moves them out of RAYTRACING_ACCELERATION_STRUCTURE / UNORDERED_
        // ACCESS. Mesh VB/IB are read by first-frame BLAS builds via implicit
        // COMMON->NON_PIXEL_SHADER_RESOURCE promotion (this pass runs first, so
        // the buffers are fresh-decayed to COMMON).
        ID3D12GraphicsCommandList4* cl4 = renderer->AsCmdList4(t.cl);
        if (cl4)
        {
            // BuildTlas records the zero count as well. That prevents a reused
            // frame slot from exposing a previous frame's TLAS after the last
            // visible RT instance is disabled.
            asManager_.BuildTlas(rtInstances_, cl4, renderer->GetCurrentFrameIndex());
            if (asManager_.HasPendingScratch())
            {
                asScratchRetireFrame_ = frameNo + render::kFrameCount;
            }
            // S13: one-time AS VRAM accounting for visibility/budgeting.
            if (!rtInstances_.empty() && !asVramLogged_ && !asManager_.BuildFailed())
            {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "[RT] Acceleration structures: %.2f MB VRAM, %zu instances.\n",
                              asManager_.GetAsMemoryBytes() / (1024.0 * 1024.0), rtInstances_.size());
                OutputDebugStringA(buf);
                asVramLogged_ = true;
            }
        }
    }
    ctx.EndCL(t);
}

void SceneRenderer::Pass_PrologueClear(Renderer* r, RenderGraphPassContext ctx)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassPrologueClear);
        r->RecordBindAndClear(t.cl);
    }
    ctx.EndCL(t);
}

void SceneRenderer::Pass_ShadowCull(Renderer* renderer, RenderGraphPassContext ctx)
{
    // Rung 0 / Step 4: GPU cull of shadow casters -> indirect draw args. Produced here but not
    // yet consumed by any draw. ShadowGpuData manages its own UAV state transitions (this pass
    // declares none), mirroring the ComputeDispatch "transitions at the call site" convention.
    if (!renderer || !frame_->shadowGpu) { return; }
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassShadowCull);
        frame_->shadowGpu->RecordCull(renderer, t.cl);
    }
    ctx.EndCL(t);
}

void SceneRenderer::Pass_VsmPageRequest(Renderer* renderer, RenderGraphPassContext ctx)
{
    // Rung 2 / Step 19b: mark the virtual shadow pages the visible frame needs. Runs after the
    // GBuffer (needs camera depth); output is the request bitfield, consumed by Step 20 (unused
    // yet — so the pass is gated OFF by default, Ctrl+V to exercise/measure). LOCAL lights only:
    // the view slots are [spots | point-faces] (NO CSM cascades — directional stays on Pass_CSM
    // until Step 24). Per-view viewProj + a mip/refDist LOD param drive the request shader.
    if (!render::VsmActive() || vsmSkipUpdate_) { return; }
    if (!renderer || !frame_->vsm || !frame_->vsm->IsAllocated()) { return; }

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
                                     static_cast<float>(vsm::g_requestDownscale), 0.0f);

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
        // Matches the Prepare above: this pass reads camera depth as an SRV, so it must be the one
        // to move it there. It previously read it in whatever state the G-buffer left behind.
        renderer->Transition(t.cl, D.depth.Get(),
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        frame_->vsm->RecordPageRequest(renderer, t.cl, cb, D.depthSRV, rw, rh);
        // Step 20: allocate physical pages for the just-marked requests (same CL — request buffer
        // stays UAV between them). Add-dormant: nothing samples/renders the pages yet.
        frame_->vsm->RecordPageAllocate(renderer, t.cl);
    }
    ctx.EndCL(t);
}

void SceneRenderer::Pass_VsmPageRender(Renderer* renderer, RenderGraphPassContext ctx)
{
    // Rung 2 / Step 22: render casters into the resident VSM pages. Builds the LOCAL shadow views
    // (spots then point faces — same slot layout as Pass_VsmPageRequest), then RecordPageRender
    // does the GPU per-page setup + per-page ExecuteIndirect into the pool (DEPTH_WRITE via graph).
    if (!render::VsmActive() || vsmSkipUpdate_) { return; }
    if (!renderer || !frame_->vsm || !frame_->vsm->IsAllocated() || !frame_->shadowGpu) { return; }

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
        frame_->vsm->RecordPageRender(renderer, t.cl, frame_->shadowGpu, views.data(), slot, frame_->wind);
    }
    ctx.EndCL(t);
}

void SceneRenderer::PrepareOpaqueDrawStates(RenderGraphPassContext& p, const SceneView* views,
                                            size_t viewCount, bool shadowDraw)
{
    if (!frame_ || !views || viewCount == 0) { return; }
    ShadowGpuData* const shadowGpu = frame_->shadowGpu;
    const bool indirect = shadowDraw && render::g_indirectShadowsEnabled &&
                          shadowGpu && shadowGpu->IndirectDrawReady();

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

void SceneRenderer::Pass_ObjectCompute(Renderer* renderer, RenderGraphPassContext ctx)
{
    if (!renderer || !frame_->objects || frame_->objects->empty())
    {
        return;
    }

    auto compute = ctx.BeginCL();
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

    ctx.EndCL(compute);
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
    do
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
            break;
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

        const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildShadowViewCB(renderer, view->view, view->proj, frame_->wind);

        for (auto* obj : opaqueSimple)
        {
            if (obj)
            {
                obj->RenderShadow(renderer, t.cl, view->view, view->proj, viewCB);
            }
        }

        for (auto* obj : opaqueComplex)
        {
            if (obj)
            {
                obj->RenderShadow(renderer, t.cl, view->view, view->proj, viewCB);
            }
        }

        if (shoreDepth)
        {
            renderer->Transition(t.cl, shoreDepth,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    } while (false);

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void SceneRenderer::Pass_CSM(Renderer* renderer, RenderGraphPassContext ctx,
    const std::array<SceneView, kCascades>& cascadeViews)
{
    if (!renderer)
    {
        return;
    }
    // Step 24f-2: only reached in Legacy mode — the graph omits the Main_CSM pass in VSM mode
    // (directional then comes from the clipmap), so no VSM gate is needed here.
    auto d = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(d.cl, ctx.pass);
    {
        GPU_SCOPE(d.cl, ProfilerScopes::kPassCSM);
        ctx.ApplyDeclaredStates(d.cl);
        renderer->BindShadowTarget(d.cl, 0, /*clear=*/true);
    }
    renderer->EndThreadCommandList(d, ctx.batchIndex);

#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
    const RenderPass passName = ctx.pass;
    // Step 6: GPU-driven indirect shadow submission (toggle, default off). The cull already ran
    // in Pass_ShadowCull; here each cascade issues ExecuteIndirect instead of the CPU loop.
    ShadowGpuData* shadowGpu = frame_->shadowGpu;
    const bool indirect = render::g_indirectShadowsEnabled && shadowGpu && shadowGpu->IndirectDrawReady();
    const vfx::WindState* wind = frame_->wind; // W5: shadow casters sway with the gbuffer's params
    // Barrier plan step 5: carry the pass's transition log onto the fan-out workers, so the
    // comparator observes what they record. Captured HERE, on the pass thread, where the log is
    // installed; without it these passes look silent because they are unobserved, not correct.
    Renderer::TransitionLog* const cmpLog = Renderer::CurrentThreadTransitionLog();
    // Step 7: the compiled barriers travel with the log — a fan-out worker must emit its
    // pass's barriers too, or the flip loses exactly the passes that record in parallel.
    Renderer::CompiledBarriers* const cmpBarriers = Renderer::CurrentThreadCompiledBarriers();
    auto renderCascade = [renderer, &cascadeViews, batchIndex = ctx.batchIndex, passName, shadowGpu, indirect, wind, cmpLog, cmpBarriers](std::size_t cascadeIndex)
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

        const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildShadowViewCB(renderer, view.view, view.proj, wind);

        auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        SetCommandListName(t.cl, passName);
        {
            GPU_SCOPE(t.cl, ProfilerScopes::kPassCSM);
            renderer->BindShadowTarget(t.cl, static_cast<int>(cascadeIndex), /*clear=*/false);

            if (indirect)
            {
                // Cascade i -> shadow-view slot i (the frustum/args layout). Uses base-LOD
                // geometry (the cull's args carry the base index count).
                shadowGpu->RecordIndirectShadowDraws(renderer, t.cl, static_cast<std::uint32_t>(cascadeIndex), viewCB);
                // GPU-instanced casters: when the GI folding path is active (Ctrl+G, default on) they
                // cast via the indirect cull/scatter like everything else, so skip them here. Otherwise
                // (flag off, over the group cap, or scatter PSO failure) draw them through their own
                // instanced shadow path so they still cast — IsGiFoldedActive encodes exactly that.
                const UINT giLod = static_cast<UINT>(cascadeIndex);
                for (auto* obj : opaqueSimple)  { if (obj && obj->IsGpuInstancedCaster() && !shadowGpu->IsGiFoldedActive(obj)) obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB, giLod); }
                for (auto* obj : opaqueComplex) { if (obj && obj->IsGpuInstancedCaster() && !shadowGpu->IsGiFoldedActive(obj)) obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB, giLod); }
            }
            else
            {
                // Step 6c: far cascades cast coarse LODs (texels are huge there; silhouette error
                // invisible). Cascade 0 (near, sharp shadows) stays full detail. Mesh clamps.
                const UINT shadowLod = static_cast<UINT>(cascadeIndex);
                for (auto* obj : opaqueSimple)
                {
                    if (obj)
                    {
                        obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB, shadowLod);
                    }
                }

                for (auto* obj : opaqueComplex)
                {
                    if (obj)
                    {
                        obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB, shadowLod);
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

        const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildShadowViewCB(renderer, view.view, view.proj, frame_->wind);

        auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        SetCommandListName(t.cl, ctx.pass);
        {
            GPU_SCOPE(t.cl, ProfilerScopes::kPassCSM);
            renderer->BindShadowTarget(t.cl, static_cast<int>(idx), /*clear=*/false);

            const UINT shadowLod = static_cast<UINT>(idx); // Step 6c: cascade-index LOD floor
            for (auto* obj : opaqueSimple)
            {
                if (obj)
                {
                    obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB, shadowLod);
                }
            }

            for (auto* obj : opaqueComplex)
            {
                if (obj)
                {
                    obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB, shadowLod);
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
    const std::array<SceneView, LightManager::kMaxShadowedSpotLights>& spotViews)
{
    if (!renderer)
    {
        return;
    }
    // Step 24c: in VSM mode the spot atlas is a 1x1 placeholder and local shadows come from the VSM
    // pool — skip rendering into it (saves the per-light submission + avoids touching the tiny atlas).
    if (render::VsmActive())
    {
        return;
    }

    const size_t shadowedLights = frame_->lightManager->GetShadowedSpotCount();
    const size_t viewCount = std::min(spotViews.size(), shadowedLights);
    if (viewCount == 0)
    {
        return;
    }

    const auto& D = renderer->GetDeferredForFrame();

#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
    ShadowGpuData* shadowGpu = frame_->shadowGpu;
    const bool indirect = render::g_indirectShadowsEnabled && shadowGpu && shadowGpu->IndirectDrawReady();
    const vfx::WindState* wind = frame_->wind; // W5
    // Barrier plan step 5: carry the pass's transition log onto the fan-out workers, so the
    // comparator observes what they record. Captured HERE, on the pass thread, where the log is
    // installed; without it these passes look silent because they are unobserved, not correct.
    Renderer::TransitionLog* const cmpLog = Renderer::CurrentThreadTransitionLog();
    // Step 7: the compiled barriers travel with the log — a fan-out worker must emit its
    // pass's barriers too, or the flip loses exactly the passes that record in parallel.
    Renderer::CompiledBarriers* const cmpBarriers = Renderer::CurrentThreadCompiledBarriers();
    auto renderSpotShadow = [renderer, &D, &spotViews, batchIndex = ctx.batchIndex, shadowGpu, indirect, wind, cmpLog, cmpBarriers](std::size_t lightIndex)
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
                renderer->Transition(t.cl, D.spotShadow.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
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
        renderer->Transition(t.cl, D.spotShadow.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);

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
    const std::array<SceneView, LightManager::kMaxShadowedPointLights * 6>& pointViews)
{
    if (!renderer)
    {
        return;
    }
    // Step 24c: VSM mode renders point shadows into the VSM pool; the cube atlas is a 1x1 placeholder.
    if (render::VsmActive())
    {
        return;
    }

    // 6 cube faces per shadowed point light; each face is its own depth-array slice
    // (its own DSV), so faces render independently — mirror Pass_SpotShadows exactly,
    // treating the flattened face index as the "slice" (cubeSlot = idx/6, face = idx%6).
    const size_t viewCount = std::min(pointViews.size(),
        frame_->lightManager->GetShadowedPointCount() * 6);
    if (viewCount == 0)
    {
        return;
    }

    const auto& D = renderer->GetDeferredForFrame();

#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
    ShadowGpuData* shadowGpu = frame_->shadowGpu;
    const bool indirect = render::g_indirectShadowsEnabled && shadowGpu && shadowGpu->IndirectDrawReady();
    const vfx::WindState* wind = frame_->wind; // W5
    // Barrier plan step 5: carry the pass's transition log onto the fan-out workers, so the
    // comparator observes what they record. Captured HERE, on the pass thread, where the log is
    // installed; without it these passes look silent because they are unobserved, not correct.
    Renderer::TransitionLog* const cmpLog = Renderer::CurrentThreadTransitionLog();
    // Step 7: the compiled barriers travel with the log — a fan-out worker must emit its
    // pass's barriers too, or the flip loses exactly the passes that record in parallel.
    Renderer::CompiledBarriers* const cmpBarriers = Renderer::CurrentThreadCompiledBarriers();
    auto renderPointShadow = [renderer, &D, &pointViews, batchIndex = ctx.batchIndex, shadowGpu, indirect, wind, cmpLog, cmpBarriers](std::size_t faceIndex)
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
                renderer->Transition(t.cl, D.pointShadow.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
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
        renderer->Transition(t.cl, D.pointShadow.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);

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

void SceneRenderer::Pass_GBuffer(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera, const SceneView& mainView)
{
    const auto& D = renderer->GetDeferredForFrame();

    // Shared per-view CB (b1) for every opaque object in this pass.
    const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildGBufferViewCB(renderer, camera, frame_->wind);

    RenderGraph<kGBufferRenderGraphPassCount> rgGB(ctx.batchIndex);
    const size_t pDriver = rgGB.AddPass(RenderPass::GBuffer_Driver, {},
        { { D.gb0.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
          { D.gb1.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
          { D.gb2.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
          { D.gbVelocity.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
#if WITH_EDITOR
          { D.objectID.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
#endif
          { D.gbAux.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
          { D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE } },
        [this, renderer](RenderGraphPassContext sub) {
        auto driver = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        SetCommandListName(driver.cl, sub.pass);
        {
            GPU_SCOPE(driver.cl, ProfilerScopes::kGBufferDriver);
            sub.ApplyDeclaredStates(driver.cl);
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
            RenderObjectBatch(renderer, opaqueSimple, sub.batchIndex, camera, /*useBundles=*/true, true, true, 32, viewCB);
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

    // Defensive: skip the frame if a deferred SRV handle staged below is null
    // (see Pass_SpotLights note); avoids a null CopyDescriptorsSimple source.
    {
        const auto& deferred = renderer->GetDeferredForFrame();
        if (deferred.gbSRV[0].ptr == 0 || deferred.gbSRV[1].ptr == 0 ||
            deferred.gbSRV[2].ptr == 0 || deferred.gbSRV[3].ptr == 0 ||
            deferred.gbAuxSRV.ptr == 0 || deferred.depthSRV.ptr == 0 ||
            deferred.shadowSRV.ptr == 0)
        {
            return;
        }
    }

    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassLighting);
        const auto& D = renderer->GetDeferredForFrame();
        ctx.ApplyDeclaredStates(t.cl);

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
        constants.sunMetalSpec = frame_->settings.sunMetalSpecInfluence;
        constants.sunAngularSize = frame_->settings.sunAngularSize;

        // Underwater caustics. Everything comes from the ocean: no water in the level means the
        // whole block stays zeroed and the shader skips it (causticsTint.w == 0).
        D3D12_CPU_DESCRIPTOR_HANDLE causticsSrv{};
        if (frame_->ocean)
        {
            if (const OceanSimulation* oceanSim = frame_->ocean->GetSimulation())
            {
                const OceanRenderConfig& oc = oceanSim->GetRenderConfig();
                causticsSrv = frame_->ocean->GetCausticsSrvCPU();
                if (oc.causticsEnabled && oc.causticsIntensity > 0.0f && causticsSrv.ptr != 0)
                {
                    // World metres covered by one screen pixel at one metre of view depth; the
                    // shader turns it into a mip level so distant caustics stop aliasing. Pixels
                    // are square, so the horizontal FOV over the render width gives both axes.
                    const float pixelWorldScale = width > 0.0f
                        ? (2.0f * std::tan(0.5f * camera.GetHFov()) / width)
                        : 0.0f;
                    constants.causticsTint =
                        float4(oc.causticsTint.x, oc.causticsTint.y, oc.causticsTint.z, 1.0f);
                    constants.causticsParams0 = float4(oc.causticsIntensity, oc.causticsScale,
                        oc.causticsSpeed, frame_->ocean->GetWaterLevel());
                    constants.causticsParams1 = float4(oc.causticsDepthFade, oc.causticsSurfaceFade,
                        oc.causticsUpFacing, oc.causticsBias);
                    constants.causticsParams2 = float4(oc.causticsDispersion, oc.causticsLayerBlend,
                        frame_->ocean->GetElapsedTime(), pixelWorldScale);
                }
            }
        }

        // Step 24f: sample directional shadows from the VSM clipmap in VSM mode (else CSM cascades).
        // t6/t7 bind valid dummy SRVs when VSM isn't resident (Legacy) — useVsm=0 never samples them.
        const bool vsmDir = render::VsmActive() && frame_->vsm && frame_->vsm->IsAllocated() &&
                            frame_->vsm->PageTableSrv().ptr != 0 && frame_->vsm->PagePoolSrv().ptr != 0;
        constants.useVsm = vsmDir ? 1u : 0u;
        // S0.3: cascade-tint debug. Forced off whenever the clipmap is the shadow source — the
        // tint visualizes CSM cascades, which that path does not sample.
        constants.csmDebugMode = vsmDir ? 0u : static_cast<uint32_t>(render::g_csmDebugMode);
        constants.vsmDepthBias = vsm::g_clipmapDepthBias;
        constants.clipmapBaseExtent = vsm::g_clipmapBaseExtent;
        constants.clipmapNormalBias = vsm::g_clipmapNormalBias;
        if (frame_->clipmapViews)
        {
            for (size_t i = 0; i < constants.clipmapViewProj.size() && i < frame_->clipmapViews->size(); ++i)
            {
                const SceneView& cv = (*frame_->clipmapViews)[i];
                constants.clipmapViewProj[i] = cv.view * cv.proj;
            }
        }

        const auto samplerDescs = std::array{ *SamplerManager::PointClamp(),
                                              *SamplerManager::ComparisonLinearClamp(),
                                              *SamplerManager::LinearWrap() };
        RecordComputeDispatch(renderer, t.cl, lighting.get(), cbSize,
            [&](uint8_t* dest) { resources_.WriteLightingConstants(constants, dest); },
            { D.gbSRV[0], D.gbSRV[1], D.gbSRV[2], D.gbSRV[3], D.depthSRV, D.shadowSRV,
              vsmDir ? frame_->vsm->PageTableSrv() : renderer->VsmDummyBufferSrv(),  // t6 (inert in Legacy)
              vsmDir ? frame_->vsm->PagePoolSrv()  : renderer->VsmDummyTexSrv(),     // t7 (inert in Legacy)
              D.gbAuxSRV,                                                             // t8
              causticsSrv.ptr != 0 ? causticsSrv : renderer->VsmDummyTexSrv() },      // t9 (inert without water)
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

    // Buffer creation/growth moved to EnsureFrameResources (barrier plan step 4) — growing it
    // here freed the previous allocation while earlier frames were still reading it.
    if (!lightManager.HasSpotLightBuffer(spotLightCount))
    {
        return;
    }
    const UINT frameIdx = renderer->GetCurrentFrameIndex();
    auto* spotLightBufferCPU = lightManager.GetSpotLightBufferCPU(frameIdx);
    const D3D12_CPU_DESCRIPTOR_HANDLE spotLightSrvHandle = lightManager.GetSpotLightSrv(frameIdx);
    if (!spotLightBufferCPU || spotLightSrvHandle.ptr == 0)
    {
        return;
    }

    // Defensive: if any deferred-target SRV handle staged below is still null
    // (observed at startup; CopyDescriptorsSimple rejects a null source and trips
    // the D3D12 debug layer), skip this pass for the frame. It recovers next frame.
    {
        const auto& deferred = renderer->GetDeferredForFrame();
        if (deferred.gbSRV[0].ptr == 0 || deferred.gbSRV[1].ptr == 0 ||
            deferred.gbSRV[2].ptr == 0 || deferred.gbSRV[3].ptr == 0 ||
            deferred.gbAuxSRV.ptr == 0 || deferred.depthSRV.ptr == 0 ||
            deferred.spotShadowSRV.ptr == 0)
        {
            return;
        }
    }

    // Rung 2 / Step 21+24b: the shader's root sig always binds t7 (VSM page table) + t8 (VSM pool).
    // In VSM mode they must be resident; in Legacy mode the pool is freed, so bind inert dummy SRVs
    // (the shader's useVsm=0 branch never samples them). Skip only when VSM SAMPLING is requested but
    // the pool isn't ready (startup / OOM) — never in Legacy mode, which must still light via the atlas.
    const bool vsmSample = render::VsmActive();
    const bool vsmReady = frame_->vsm && frame_->vsm->IsAllocated() &&
                          frame_->vsm->PageTableSrv().ptr != 0 && frame_->vsm->PagePoolSrv().ptr != 0;
    if (vsmSample && !vsmReady) { return; }

    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSpotLights);
        const auto& D = renderer->GetDeferredForFrame();
        ctx.ApplyDeclaredStates(t.cl);

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
            spotLightBufferCPU[i].shadowParams = float4(light.GetCosInner(), static_cast<float>(lightManager.GetSpotShadowSlot(i)), light.GetInvAngleRange(), light.GetShadowDepthBias());
            spotLightBufferCPU[i].shadowParams2 = float4(light.GetShadowNormalBias(), 0.0f, 0.0f, 0.0f);
            spotLightBufferCPU[i].viewProj = viewProj;
        }

        auto spotMaterial = resources_.GetSpotLightMaterial();
        const UINT cbSize = resources_.GetSpotLightCBSizeBytes();
        if (!spotMaterial || cbSize == 0)
        {
            break;
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
        constants.useVsm = vsmSample ? 1u : 0u;
        constants.vsmRefDist = vsm::g_refDist;
        constants.localLateralTexels = vsm::g_localLateralTexels;
        constants.localDepthPushTexels = vsm::g_localDepthPushTexels;

        const auto samplerDescs = std::array{ *SamplerManager::LinearClamp(), *SamplerManager::PointClamp(), *SamplerManager::ComparisonLinearClamp() };
        RecordComputeDispatch(renderer, t.cl, spotMaterial.get(), cbSize,
            [&](uint8_t* dest) { resources_.WriteSpotLightConstants(constants, dest); },
            { D.gbSRV[0], D.gbSRV[1], D.gbSRV[2], D.gbSRV[3], D.depthSRV, D.spotShadowSRV, spotLightSrvHandle,
              vsmReady ? frame_->vsm->PageTableSrv() : renderer->VsmDummyBufferSrv(),  // t7 (inert in Legacy)
              vsmReady ? frame_->vsm->PagePoolSrv()  : renderer->VsmDummyTexSrv(),     // t8 (inert in Legacy)
              D.gbAuxSRV },                                                             // t9
            { D.lightUAV },
            renderer->GetSamplerManager()->GetTable(renderer, samplerDescs),
            renderer->GetRenderWidth(), renderer->GetRenderHeight(),
            D.light.Get());
    } while (false);

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void SceneRenderer::Pass_PointLights(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera)
{
    LightManager& lightManager = *frame_->lightManager;
    auto& pointLights = lightManager.PointLights();
    if (pointLights.empty()) { return; }

    // Growth moved to EnsureFrameResources (barrier plan step 4); see Pass_SpotLights.
    if (!lightManager.HasPointLightBuffer(pointLights.size()))
    {
        return;
    }
    const UINT frameIdx = renderer->GetCurrentFrameIndex();
    auto* pointLightBufferCPU = lightManager.GetPointLightBufferCPU(frameIdx);
    const D3D12_CPU_DESCRIPTOR_HANDLE pointLightSrvHandle = lightManager.GetPointLightSrv(frameIdx);
    if (!pointLightBufferCPU || pointLightSrvHandle.ptr == 0) { return; }

    // Defensive: skip the frame if a deferred SRV handle staged below is null
    // (see Pass_SpotLights note); avoids a null CopyDescriptorsSimple source.
    {
        const auto& deferred = renderer->GetDeferredForFrame();
        if (deferred.gbSRV[0].ptr == 0 || deferred.gbSRV[1].ptr == 0 ||
            deferred.gbSRV[2].ptr == 0 || deferred.gbSRV[3].ptr == 0 ||
            deferred.gbAuxSRV.ptr == 0 || deferred.depthSRV.ptr == 0)
        {
            return;
        }
    }

    // Rung 2 / Step 21+24b: the shader always binds t7 (VSM page table) + t8 (VSM pool). Bind inert
    // dummy SRVs in Legacy mode (freed pool) — useVsm=0 never samples them. Skip only when VSM
    // sampling is requested but the pool isn't ready; Legacy must still light via the cube atlas.
    const bool vsmSample = render::VsmActive();
    const bool vsmReady = frame_->vsm && frame_->vsm->IsAllocated() &&
                          frame_->vsm->PageTableSrv().ptr != 0 && frame_->vsm->PagePoolSrv().ptr != 0;
    if (vsmSample && !vsmReady) { return; }

    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassPointLights);

        const auto& D = renderer->GetDeferredForFrame();
        ctx.ApplyDeclaredStates(t.cl);

        for (size_t i = 0; i < pointLights.size(); ++i)
        {
            const auto& desc = pointLights[i].GetDesc();
            pointLightBufferCPU[i].position = desc.position;
            pointLightBufferCPU[i].radius = desc.radius;
            pointLightBufferCPU[i].color = desc.color;
            pointLightBufferCPU[i].intensity = desc.intensity;
            // Per-light cube-shadow params = (slot/-1, worldDepthBias, near, far=radius).
            // near MUST match Scene.cpp's cube-face projection EXACTLY — PointShadowFactor
            // reconstructs the compare depth from it. Bias is WORLD-space (subtracted from the
            // compare distance before projecting); a constant NDC bias is unusable in the
            // crushed far region of a perspective depth buffer (B4 tuning).
            const float pointShadowNear = std::max(0.2f, desc.radius * 0.02f);
            constexpr float kPointShadowBias = 0.10f; // world units
            pointLightBufferCPU[i].shadowParams = float4(
                static_cast<float>(lightManager.GetPointShadowSlot(i)),
                kPointShadowBias, pointShadowNear, desc.radius);
        }

        auto pointMaterial = resources_.GetPointLightMaterial();
        const UINT cbSize = resources_.GetPointLightCBSizeBytes();
        if (!pointMaterial || cbSize == 0)
        {
            break;
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
        constants.invPointShadowSize = 1.0f / static_cast<float>(std::max(D.pointShadowRes, 1u)); // cube-face texel for PCF
        constants.useVsm = vsmSample ? 1u : 0u;
        constants.vsmRefDist = vsm::g_refDist;
        constants.localLateralTexels = vsm::g_localLateralTexels;
        constants.localDepthPushTexels = vsm::g_localDepthPushTexels;

        // s2 = comparison sampler for the point shadow cube (B3).
        const auto samplerDescs = std::array{ *SamplerManager::LinearClamp(), *SamplerManager::PointClamp(), *SamplerManager::ComparisonLinearClamp() };
        RecordComputeDispatch(renderer, t.cl, pointMaterial.get(), cbSize,
            [&](uint8_t* dest) { resources_.WritePointLightConstants(constants, dest); },
            // t0-t5 as before; t6 = point shadow depth cube; t7/t8 = VSM page table + pool;
            // t9 = GBAux appended for material-model lighting.
            { D.gbSRV[0], D.gbSRV[1], D.gbSRV[2], D.gbSRV[3], D.depthSRV, pointLightSrvHandle, D.pointShadowSRV,
              vsmReady ? frame_->vsm->PageTableSrv() : renderer->VsmDummyBufferSrv(),  // t7 (inert in Legacy)
              vsmReady ? frame_->vsm->PagePoolSrv()  : renderer->VsmDummyTexSrv(),     // t8 (inert in Legacy)
              D.gbAuxSRV },                                                             // t9
            { D.lightUAV },
            renderer->GetSamplerManager()->GetTable(renderer, samplerDescs),
            renderer->GetRenderWidth(), renderer->GetRenderHeight(),
            D.light.Get());
    } while (false);

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

        ctx.ApplyDeclaredStates(t.cl);

        // RTVs = Light + Velocity, DSV = GBuffer Depth (read-only), no clears
        renderer->BindLightTargetWithVelocity(t.cl, Renderer::ClearMode::None, true);

        frame_->skybox->Render(renderer, t.cl, camera, 0); // skybox has no b1; viewCB ignored
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void SceneRenderer::Pass_ScreenSpaceReflections(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera)
{
    //return;
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassReflectionSource);
        const auto& D = renderer->GetDeferredForFrame();
        ctx.ApplyDeclaredStates(t.cl);

        auto ssrMaterial = resources_.GetSsrMaterial();
        const UINT cbSize = resources_.GetSsrCBSizeBytes();
        if (!ssrMaterial || cbSize == 0)
        {
            break;
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
            { D.lightSRV, D.gbSRV[1], D.depthSRV, D.depthSRV }, // t0 Light, t1 GB1, t2 march depth, t3 origin depth (== t2 for opaque)
            { D.reflectionUAV },                           // u0 output
            renderer->GetSamplerManager()->GetTable(renderer, samplerDescs),
            renderer->GetReflectionTextureWidth(), renderer->GetReflectionTextureHeight(),
            D.reflection.Get());
    } while (false);
    ctx.EndCL(t);
}

// Matches the `Probe` cbuffer in rt_reflections_cs.hlsl (row-major; 4x mat4,
// then light params, then bindless descriptor indices).
namespace {
struct RtReflectConstants
{
    Math::mat4 view;
    Math::mat4 proj;
    Math::mat4 invView;
    Math::mat4 invProj;
    Math::float3 sunDirWS;  float ambientIntensity = 0.0f;
    Math::float3 lightRgb;  float exposure = 1.0f;
    float depthA = 0.0f;    float depthB = 0.0f;   uint32_t outWidth = 0;  uint32_t outHeight = 0;
    uint32_t tlasIndex = 0; uint32_t lightIndex = 0; uint32_t gb1Index = 0; uint32_t depthIndex = 0;
    uint32_t reflectionUavIndex = 0; uint32_t geomInfoIndex = 0; uint32_t skyboxIndex = 0; float skyboxIntensity = 1.0f;
    uint32_t spotLightIndex = 0; uint32_t spotCount = 0; uint32_t pointLightIndex = 0; uint32_t pointCount = 0;
    uint32_t screenDepthIndex = 0; uint32_t _padS0 = 0; uint32_t _padS1 = 0; uint32_t _padS2 = 0;
};

// Matches the `Denoise` cbuffer in rt_reflection_denoise_cs.hlsl.
struct RtDenoiseConstants
{
    uint32_t rawIndex = 0;
    uint32_t histPrevIndex = 0;
    uint32_t velocityIndex = 0;
    uint32_t reflectionUavIndex = 0;
    uint32_t histCurrUavIndex = 0;
    uint32_t outWidth = 0;
    uint32_t outHeight = 0;
    float    alpha = 0.1f;
};
} // namespace

void SceneRenderer::Pass_RTReflections(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassRTReflections);
        const auto& D = renderer->GetDeferredForFrame();
        ctx.ApplyDeclaredStates(t.cl); // depth/gb1/light -> NPS, reflection scratch -> UAV

        auto reflectMaterial = resources_.GetRtReflectMaterial();
        const UINT frameIndex = renderer->GetCurrentFrameIndex();
        const D3D12_CPU_DESCRIPTOR_HANDLE tlasSrv = asManager_.TlasSrvCpu(frameIndex);
        Skybox* skybox = frame_->skybox;
        if (!reflectMaterial || !bindless_.Ready() || tlasSrv.ptr == 0 ||
            asManager_.TlasInstanceCount(frameIndex) == 0 || !frame_->dirLight || !skybox)
        {
            // No usable TLAS/bindless/light/skybox this frame: leave reflection as is.
            break;
        }

        // Per-frame scene descriptors into the bindless heap (geometry VB/IB +
        // geometry-info are persistent, populated in Pass_BuildAS). Scene slots
        // 0-7 are this pass's; the debug pass uses 13-16 (distinct, so passes in
        // the same frame never alias heap slots). The RT reflection is written
        // straight into the main reflection target -- the denoise pass was removed
        // in S12 (it was an inert pass-through once glossy was parked).
        bindless_.WriteSceneDescriptor(frameIndex, 0, tlasSrv);     // TLAS
        bindless_.WriteSceneDescriptor(frameIndex, 1, D.lightSRV);  // lit HDR (fast path)
        bindless_.WriteSceneDescriptor(frameIndex, 2, D.gbSRV[1]);  // GB1 (normal)
        bindless_.WriteSceneDescriptor(frameIndex, 3, D.depthSRV);  // Depth
        bindless_.WriteSceneDescriptor(frameIndex, 4, D.reflectionUAV); // reflection out -> blur/compose
        bindless_.WriteSceneDescriptor(frameIndex, 5, skybox->GetTex()->GetSRVCPU()); // skybox cube (env reflection)

        // Spot/point light buffers (filled earlier this frame by Pass_SpotLights /
        // Pass_PointLights) so off-screen reflected surfaces are lit by the same
        // local lights as the base pass. Slots 6-7.
        LightManager* lm = frame_->lightManager;
        const UINT spotCount = lm ? static_cast<UINT>(lm->GetSpotLightCount()) : 0u;
        const UINT pointCount = lm ? static_cast<UINT>(lm->PointLights().size()) : 0u;
        const D3D12_CPU_DESCRIPTOR_HANDLE spotSrv = lm ? lm->GetSpotLightSrv(frameIndex) : D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
        const D3D12_CPU_DESCRIPTOR_HANDLE pointSrv = lm ? lm->GetPointLightSrv(frameIndex) : D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
        const bool haveSpots = spotCount > 0u && spotSrv.ptr != 0;
        const bool havePoints = pointCount > 0u && pointSrv.ptr != 0;
        if (haveSpots)  { bindless_.WriteSceneDescriptor(frameIndex, 6, spotSrv); }
        if (havePoints) { bindless_.WriteSceneDescriptor(frameIndex, 7, pointSrv); }

        RtReflectConstants c{};
        const float zNear = camera.GetZNear();
        const float zFar = camera.GetZFar();
        c.view = camera.GetViewMatrix();
        c.proj = camera.GetProjMatrix();
        c.invView = camera.GetInvViewMatrix();
        c.invProj = camera.GetInvProjMatrix();
        const DirectionalLight& dl = *frame_->dirLight;
        c.sunDirWS = dl.GetDirection();
        c.ambientIntensity = dl.GetAmbient();
        c.lightRgb = dl.GetColor();
        c.exposure = dl.GetExposure();
        c.depthA = zNear / (zNear - zFar);
        c.depthB = (zNear * zFar) / (zFar - zNear);
        c.outWidth = renderer->GetReflectionTextureWidth();
        c.outHeight = renderer->GetReflectionTextureHeight();
        c.tlasIndex = bindless_.SceneIndex(frameIndex, 0);
        c.lightIndex = bindless_.SceneIndex(frameIndex, 1);
        c.gb1Index = bindless_.SceneIndex(frameIndex, 2);
        c.depthIndex = bindless_.SceneIndex(frameIndex, 3);
        c.screenDepthIndex = c.depthIndex; // opaque: primary == on-screen depth (no change)
        c.reflectionUavIndex = bindless_.SceneIndex(frameIndex, 4);
        c.skyboxIndex = bindless_.SceneIndex(frameIndex, 5);
        c.skyboxIntensity = skybox->GetExposure();
        c.geomInfoIndex = bindless_.GeomInfoIndex();
        c.spotLightIndex = bindless_.SceneIndex(frameIndex, 6);
        c.spotCount = haveSpots ? spotCount : 0u;
        c.pointLightIndex = bindless_.SceneIndex(frameIndex, 7);
        c.pointCount = havePoints ? pointCount : 0u;

        auto cb = renderer->GetFrameResource()->AllocDynamic(sizeof(RtReflectConstants), render::kConstantBufferAlignment);
        std::memcpy(cb.cpu, &c, sizeof(c));

        // Bespoke bindless dispatch (binds the persistent heap, not the frame heap).
        ID3D12DescriptorHeap* heaps[] = { bindless_.Heap() };
        t.cl->SetDescriptorHeaps(1, heaps);
        t.cl->SetComputeRootSignature(reflectMaterial->GetRootSignature());
        t.cl->SetPipelineState(reflectMaterial->GetPipelineState());
        t.cl->SetComputeRootConstantBufferView(0, cb.gpu);
        const UINT gx = (c.outWidth + 7u) / 8u;
        const UINT gy = (c.outHeight + 7u) / 8u;
        if (gx > 0 && gy > 0)
        {
            t.cl->Dispatch(gx, gy, 1);
        }
        renderer->UAVBarrier(t.cl, D.reflection.Get()); // reflection -> consumed by the blur pass

        // Restore the frame heap: this pass shares its command list with the
        // grouped blur + compose passes, which bind into the per-frame heap.
        renderer->BindDescriptorHeaps(t.cl);
    } while (false);
    ctx.EndCL(t);
}

// S15b: rasterize transparent (glass) front faces into a reflection-res G-buffer
// (world normal RTV + depth DSV) so the next pass can ray-trace their reflections.
void SceneRenderer::Pass_GlassReflGbuffer(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera, const SceneView& mainView)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassGlassReflGbuffer);
        const auto& D = renderer->GetDeferredForFrame();
        auto prepassMat = resources_.GetGlassReflPrepassMaterial();
        if (!prepassMat) { break; }
        ctx.ApplyDeclaredStates(t.cl); // glassReflNormal -> RTV, glassReflDepth -> DEPTH_WRITE

        const UINT w = renderer->GetReflectionTextureWidth();
        const UINT h = renderer->GetReflectionTextureHeight();
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = D.glassReflNormalRTV;
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = D.glassReflDepthDSV;
        t.cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        const float clearN[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // alpha 0 = "no glass" sentinel
        t.cl->ClearRenderTargetView(rtv, clearN, 0, nullptr);
        t.cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr); // reverse-Z far
        D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
        D3D12_RECT sr{ 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };
        t.cl->RSSetViewports(1, &vp);
        t.cl->RSSetScissorRects(1, &sr);

        Math::mat4 viewProj = camera.GetViewProjMatrix();
        auto vcb = renderer->GetFrameResource()->AllocDynamic(sizeof(viewProj), render::kConstantBufferAlignment);
        std::memcpy(vcb.cpu, &viewProj, sizeof(viewProj));

        t.cl->SetGraphicsRootSignature(prepassMat->GetRootSignature());
        t.cl->SetPipelineState(prepassMat->GetPipelineState());
        t.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        t.cl->SetGraphicsRootConstantBufferView(1, vcb.gpu); // b1 = viewProj (shared)

        auto drawBucket = [&](SceneRenderQueue::BucketType bt) {
            const auto& bucket = mainView.queue.VisibleBuckets()[BucketIndex(bt)];
            for (RenderableObjectBase* base : bucket)
            {
                // Only glass (TransparentStaticMesh) samples glassReflection — skip the ocean
                // and any other transparent renderable so they don't flood the glass G-buffer.
                if (!base || !base->UsesGlassReflection()) { continue; }
                auto* ro = base->AsRenderableObject();
                if (!ro || !ro->GetMesh()) { continue; }
                Math::mat4 world = ro->GetModelMatrix();
                auto ocb = renderer->GetFrameResource()->AllocDynamic(sizeof(world), render::kConstantBufferAlignment);
                std::memcpy(ocb.cpu, &world, sizeof(world));
                t.cl->SetGraphicsRootConstantBufferView(0, ocb.gpu); // b0 = per-object world
                ro->GetMesh()->Draw(t.cl, 0);
            }
        };
        drawBucket(SceneRenderQueue::BucketType::TransparentSimple);
        drawBucket(SceneRenderQueue::BucketType::TransparentComplex);
    } while (false);
    ctx.EndCL(t);
}

// S15b: dispatch rt_reflections_cs over the glass G-buffer -> glassReflection. Reuses the
// opaque reflection shader (incl. its off-screen recompute); the on-screen color source is
// the lit opaque buffer (lightIndex) and the depth-match uses the opaque depth (screenDepth).
void SceneRenderer::Pass_GlassReflections(Renderer* renderer, RenderGraphPassContext ctx, const Camera& camera)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassGlassReflections);
        const auto& D = renderer->GetDeferredForFrame();
        ctx.ApplyDeclaredStates(t.cl); // glassReflNormal/Depth/light/depth -> NPS, glassReflection -> UAV

        auto reflectMaterial = resources_.GetRtReflectMaterial();
        const UINT frameIndex = renderer->GetCurrentFrameIndex();
        const D3D12_CPU_DESCRIPTOR_HANDLE tlasSrv = asManager_.TlasSrvCpu(frameIndex);
        Skybox* skybox = frame_->skybox;
        if (!reflectMaterial || !bindless_.Ready() || tlasSrv.ptr == 0 ||
            asManager_.TlasInstanceCount(frameIndex) == 0 || !frame_->dirLight || !skybox)
        {
            break;
        }

        // Glass scene-descriptor range (17-25): distinct from reflections 0-7 / denoise 8-12
        // / debug 13-16 so same-frame passes never alias bindless heap slots.
        constexpr UINT B = 17;
        bindless_.WriteSceneDescriptor(frameIndex, B + 0, tlasSrv);                          // TLAS
        bindless_.WriteSceneDescriptor(frameIndex, B + 1, D.lightSRV);                        // on-screen lit (light buffer)
        bindless_.WriteSceneDescriptor(frameIndex, B + 2, D.glassReflNormalSRV);              // glass normal (gb1)
        bindless_.WriteSceneDescriptor(frameIndex, B + 3, D.glassReflDepthSRV);               // glass depth (primary)
        bindless_.WriteSceneDescriptor(frameIndex, B + 4, D.glassReflectionUAV);              // output
        bindless_.WriteSceneDescriptor(frameIndex, B + 5, skybox->GetTex()->GetSRVCPU());     // skybox
        bindless_.WriteSceneDescriptor(frameIndex, B + 8, D.depthSRV);                        // screen (opaque) depth for the match

        LightManager* lm = frame_->lightManager;
        const UINT spotCount = lm ? static_cast<UINT>(lm->GetSpotLightCount()) : 0u;
        const UINT pointCount = lm ? static_cast<UINT>(lm->PointLights().size()) : 0u;
        const D3D12_CPU_DESCRIPTOR_HANDLE spotSrv = lm ? lm->GetSpotLightSrv(frameIndex) : D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
        const D3D12_CPU_DESCRIPTOR_HANDLE pointSrv = lm ? lm->GetPointLightSrv(frameIndex) : D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
        const bool haveSpots = spotCount > 0u && spotSrv.ptr != 0;
        const bool havePoints = pointCount > 0u && pointSrv.ptr != 0;
        if (haveSpots)  { bindless_.WriteSceneDescriptor(frameIndex, B + 6, spotSrv); }
        if (havePoints) { bindless_.WriteSceneDescriptor(frameIndex, B + 7, pointSrv); }

        RtReflectConstants c{};
        const float zNear = camera.GetZNear();
        const float zFar = camera.GetZFar();
        c.view = camera.GetViewMatrix();
        c.proj = camera.GetProjMatrix();
        c.invView = camera.GetInvViewMatrix();
        c.invProj = camera.GetInvProjMatrix();
        const DirectionalLight& dl = *frame_->dirLight;
        c.sunDirWS = dl.GetDirection();
        c.ambientIntensity = dl.GetAmbient();
        c.lightRgb = dl.GetColor();
        c.exposure = dl.GetExposure();
        c.depthA = zNear / (zNear - zFar);
        c.depthB = (zNear * zFar) / (zFar - zNear);
        c.outWidth = renderer->GetReflectionTextureWidth();
        c.outHeight = renderer->GetReflectionTextureHeight();
        c.tlasIndex = bindless_.SceneIndex(frameIndex, B + 0);
        c.lightIndex = bindless_.SceneIndex(frameIndex, B + 1);
        c.gb1Index = bindless_.SceneIndex(frameIndex, B + 2);
        c.depthIndex = bindless_.SceneIndex(frameIndex, B + 3);        // primary = glass depth
        c.screenDepthIndex = bindless_.SceneIndex(frameIndex, B + 8);  // visibility match = opaque depth
        c.reflectionUavIndex = bindless_.SceneIndex(frameIndex, B + 4);
        c.skyboxIndex = bindless_.SceneIndex(frameIndex, B + 5);
        c.skyboxIntensity = skybox->GetExposure();
        c.geomInfoIndex = bindless_.GeomInfoIndex();
        c.spotLightIndex = bindless_.SceneIndex(frameIndex, B + 6);
        c.spotCount = haveSpots ? spotCount : 0u;
        c.pointLightIndex = bindless_.SceneIndex(frameIndex, B + 7);
        c.pointCount = havePoints ? pointCount : 0u;

        auto cb = renderer->GetFrameResource()->AllocDynamic(sizeof(RtReflectConstants), render::kConstantBufferAlignment);
        std::memcpy(cb.cpu, &c, sizeof(c));

        ID3D12DescriptorHeap* heaps[] = { bindless_.Heap() };
        t.cl->SetDescriptorHeaps(1, heaps);
        t.cl->SetComputeRootSignature(reflectMaterial->GetRootSignature());
        t.cl->SetPipelineState(reflectMaterial->GetPipelineState());
        t.cl->SetComputeRootConstantBufferView(0, cb.gpu);
        const UINT gx = (c.outWidth + 7u) / 8u;
        const UINT gy = (c.outHeight + 7u) / 8u;
        if (gx > 0 && gy > 0)
        {
            t.cl->Dispatch(gx, gy, 1);
        }
        renderer->UAVBarrier(t.cl, D.glassReflection.Get());
        renderer->BindDescriptorHeaps(t.cl); // restore the frame heap
    } while (false);
    ctx.EndCL(t);
}

// S15b (SSR mode): screen-space reflections for glass — reuse ssr_cs over the glass G-buffer.
// Origin = glass front depth/normal (t3/t1); the ray marches against the opaque depth (t2) and
// samples the lit opaque color (t0), writing into glassReflection (the forward glass samples it).
void SceneRenderer::Pass_GlassReflectionsSSR(Renderer* renderer, RenderGraphPassContext ctx, const Camera& camera)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassGlassReflections);
        const auto& D = renderer->GetDeferredForFrame();
        ctx.ApplyDeclaredStates(t.cl);

        auto ssrMaterial = resources_.GetSsrMaterial();
        const UINT cbSize = resources_.GetSsrCBSizeBytes();
        if (!ssrMaterial || cbSize == 0)
        {
            break;
        }

        SsrPassConstants constants{};
        const float zNear = camera.GetZNear();
        const float zFar = camera.GetZFar();
        constants.view = camera.GetViewMatrix();
        constants.proj = camera.GetProjMatrix();
        constants.invView = camera.GetInvViewMatrix();
        constants.invProj = camera.GetInvProjMatrix();
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
            { D.lightSRV, D.glassReflNormalSRV, D.depthSRV, D.glassReflDepthSRV }, // t0 lit, t1 glass normal, t2 opaque(march), t3 glass(origin)
            { D.glassReflectionUAV },
            renderer->GetSamplerManager()->GetTable(renderer, samplerDescs),
            renderer->GetReflectionTextureWidth(), renderer->GetReflectionTextureHeight(),
            D.glassReflection.Get());
    } while (false);
    ctx.EndCL(t);
}

void SceneRenderer::Pass_RTDenoise(Renderer* renderer, RenderGraphPassContext ctx)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassRTDenoise);
        const auto& D = renderer->GetDeferredForFrame();
        ctx.ApplyDeclaredStates(t.cl); // scratch(raw)/velocity/histPrev -> NPS, reflection/histCurr -> UAV

        auto denoiseMaterial = resources_.GetRtDenoiseMaterial();
        const UINT frameIndex = renderer->GetCurrentFrameIndex();
        if (!denoiseMaterial || !bindless_.Ready() || !reflectionHistory_.Ready())
        {
            break;
        }

        const uint64_t parity = renderer->GetTotalFrameNumber();
        // Scene slots 8-12 (distinct from the reflection pass's 0-7).
        bindless_.WriteSceneDescriptor(frameIndex, 8, D.reflectionScratchSRV);                 // raw reflection (this frame)
        bindless_.WriteSceneDescriptor(frameIndex, 9, reflectionHistory_.PrevSrv(parity)); // accumulated (prev frame)
        bindless_.WriteSceneDescriptor(frameIndex, 10, D.gbSRV[3]);                  // gbVelocity (motion)
        bindless_.WriteSceneDescriptor(frameIndex, 11, D.reflectionUAV);                    // denoised out -> blur/compose
        bindless_.WriteSceneDescriptor(frameIndex, 12, reflectionHistory_.CurrUav(parity)); // history (this frame)

        RtDenoiseConstants c{};
        c.rawIndex = bindless_.SceneIndex(frameIndex, 8);
        c.histPrevIndex = bindless_.SceneIndex(frameIndex, 9);
        c.velocityIndex = bindless_.SceneIndex(frameIndex, 10);
        c.reflectionUavIndex = bindless_.SceneIndex(frameIndex, 11);
        c.histCurrUavIndex = bindless_.SceneIndex(frameIndex, 12);
        c.outWidth = renderer->GetReflectionTextureWidth();
        c.outHeight = renderer->GetReflectionTextureHeight();
        // alpha = 1 -> pass-through (no accumulation). The reflection is currently
        // sharp + stable, so temporal accumulation isn't needed and would only add
        // ghosting under motion. (Re-enable < 1 together with jittered glossy + a
        // proper denoiser, e.g. DLSS Ray Reconstruction.)
        c.alpha = 1.0f;

        auto cb = renderer->GetFrameResource()->AllocDynamic(sizeof(RtDenoiseConstants), render::kConstantBufferAlignment);
        std::memcpy(cb.cpu, &c, sizeof(c));

        ID3D12DescriptorHeap* heaps[] = { bindless_.Heap() };
        t.cl->SetDescriptorHeaps(1, heaps);
        t.cl->SetComputeRootSignature(denoiseMaterial->GetRootSignature());
        t.cl->SetPipelineState(denoiseMaterial->GetPipelineState());
        t.cl->SetComputeRootConstantBufferView(0, cb.gpu);
        const UINT gx = (c.outWidth + 7u) / 8u;
        const UINT gy = (c.outHeight + 7u) / 8u;
        if (gx > 0 && gy > 0)
        {
            t.cl->Dispatch(gx, gy, 1);
        }
        renderer->UAVBarrier(t.cl, D.reflection.Get());

        // Restore the frame heap for the grouped blur + compose passes.
        renderer->BindDescriptorHeaps(t.cl);
    } while (false);
    ctx.EndCL(t);
}

void SceneRenderer::Pass_ClearReflections(Renderer* renderer, RenderGraphPassContext ctx)
{
    // Reflection source = None/SkyOnly: zero the traced/screen reflection target.
    // Compose separately gates the skybox fallback, so SkyOnly retains it while
    // None produces no opaque environment reflection. The target is per-frame, so
    // it must be cleared every frame, not once.
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassReflectionSource);
        const auto& D = renderer->GetDeferredForFrame();
        ctx.ApplyDeclaredStates(t.cl); // reflection -> UNORDERED_ACCESS

        renderer->BindDescriptorHeaps(t.cl);
        const GpuDescHandle uav = renderer->StageSrvUavTable({ D.reflectionUAV });
        const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        t.cl->ClearUnorderedAccessViewFloat(uav.gpu, D.reflectionUAV, D.reflection.Get(), zero, 0, nullptr);
    }
    ctx.EndCL(t);
}

void SceneRenderer::Pass_ReflectionBlur(Renderer* renderer, RenderGraphPassContext ctx)
{
    //return;
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassReflectionBlur);
        const auto& D = renderer->GetDeferredForFrame();
        const UINT ssrWidth = renderer->GetReflectionTextureWidth();
        const UINT ssrHeight = renderer->GetReflectionTextureHeight();

        // Horizontal pass (first-use states come from the pass declarations)
        ctx.ApplyDeclaredStates(t.cl);

        auto blurMaterial = resources_.GetBlurMaterial();
        const UINT cbSize = resources_.GetBlurCBSizeBytes();
        if (!blurMaterial || cbSize == 0)
        {
            break;
        }

        const auto samplerDescs = std::array{ *SamplerManager::LinearClamp() };
        const D3D12_GPU_DESCRIPTOR_HANDLE samplerTable = renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);

        const float invSsrWidth = ssrWidth > 0 ? (1.0f / static_cast<float>(ssrWidth)) : 0.0f;
        BlurPassConstants blurConstants{};
        blurConstants.direction = float2(invSsrWidth, 0.0f);
        blurConstants.radius = 1.0f;
        // S16 glossy: scale the per-pixel blur by the reflector's roughness (gb0). 0 = sharp.
        blurConstants.glossyScale = std::max(0.0f, frame_->settings.reflectionGlossyScale);
        RecordComputeDispatch(renderer, t.cl, blurMaterial.get(), cbSize,
            [&](uint8_t* dest) { resources_.WriteBlurConstants(blurConstants, dest); },
            { D.reflectionSRV, D.gbSRV[0] }, { D.reflectionScratchUAV }, samplerTable, // t0 reflection, t1 GB0 (roughness)
            ssrWidth, ssrHeight,
            D.reflectionScratch.Get());

        // Vertical pass
        renderer->Transition(t.cl, D.reflectionScratch.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.reflection.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        const float invSsrHeight = ssrHeight > 0 ? (1.0f / static_cast<float>(ssrHeight)) : 0.0f;
        blurConstants.direction = float2(0.0f, invSsrHeight);
        RecordComputeDispatch(renderer, t.cl, blurMaterial.get(), cbSize,
            [&](uint8_t* dest) { resources_.WriteBlurConstants(blurConstants, dest); },
            { D.reflectionScratchSRV, D.gbSRV[0] }, { D.reflectionUAV }, samplerTable, // t0 reflection, t1 GB0
            ssrWidth, ssrHeight,
            D.reflection.Get());
    } while (false);
    ctx.EndCL(t);
}

void SceneRenderer::Pass_Compose(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassCompose);
        const auto& D = renderer->GetDeferredForFrame();
        ctx.ApplyDeclaredStates(t.cl);

        const float width = static_cast<float>(renderer->GetRenderWidth());
        const float height = static_cast<float>(renderer->GetRenderHeight());
        if (width <= 0.0f || height <= 0.0f)
        {
            renderer->Transition(t.cl, D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            break;
        }

        auto composeMaterial = resources_.GetComposeMaterial();
        const UINT cbSize = resources_.GetComposeCBSizeBytes();
        Skybox* skybox = frame_->skybox;
        if (!composeMaterial || cbSize == 0 || !skybox || D.gbAuxSRV.ptr == 0)
        {
            renderer->Transition(t.cl, D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            break;
        }

        ComposePassConstants constants{};
        constants.invView = camera.GetInvViewMatrix();
        constants.invProj = camera.GetInvProjMatrix();
        constants.skyboxIntensity = skybox->GetExposure();
        constants.camPos = camera.GetPosition();
        constants.enableSkySpecular =
            frame_->settings.reflectionSource != ReflectionSource::None ? 1u : 0u;
        constants.screenSize = float2(width, height);
        constants.invScreenSize = float2(1.0f / width, 1.0f / height);

        const auto samplerDescs = std::array{ *SamplerManager::LinearClamp(), *SamplerManager::PointClamp() };
        RecordComputeDispatch(renderer, t.cl, composeMaterial.get(), cbSize,
            [&](uint8_t* dest) { resources_.WriteComposeConstants(constants, dest); },
            { D.lightSRV, D.gbSRV[2], D.gbSRV[0], D.gbSRV[1], D.depthSRV,
              skybox->GetTex()->GetSRVCPU(), D.reflectionSRV, D.gbAuxSRV },
            { D.sceneUAV },
            renderer->GetSamplerManager()->GetTable(renderer, samplerDescs),
            renderer->GetRenderWidth(), renderer->GetRenderHeight(),
            D.scene.Get());

        renderer->Transition(t.cl, D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    } while (false);

    ctx.EndCL(t);
}

// Matches the `Probe` cbuffer in rt_debug_cs.hlsl (row-major; 2x mat4 then indices).
namespace {
struct RtDebugConstants
{
    Math::mat4 invView;
    Math::mat4 invProj;
    uint32_t tlasIndex = 0;
    uint32_t gb1Index = 0;
    uint32_t depthIndex = 0;
    uint32_t reflectionUavIndex = 0;
    uint32_t geomInfoIndex = 0;
    uint32_t outWidth = 0;
    uint32_t outHeight = 0;
    uint32_t _pad = 0;
};
} // namespace

void SceneRenderer::Pass_RTDebug(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassRTDebug);
        const auto& D = renderer->GetDeferredForFrame();
        ctx.ApplyDeclaredStates(t.cl); // reflection -> UAV, gb1/depth -> NON_PIXEL_SHADER_RESOURCE

        auto debugMaterial = resources_.GetRtDebugMaterial();
        const UINT frameIndex = renderer->GetCurrentFrameIndex();
        const D3D12_CPU_DESCRIPTOR_HANDLE tlasSrv = asManager_.TlasSrvCpu(frameIndex);
        if (!debugMaterial || !bindless_.Ready() || tlasSrv.ptr == 0 ||
            asManager_.TlasInstanceCount(frameIndex) == 0)
        {
            break; // no TLAS / bindless table this frame - leave reflection as compose left it
        }

        // Copy this frame's scene descriptors into the persistent bindless heap so
        // the shader can reach them via ResourceDescriptorHeap[]. (Geometry VB/IB +
        // geometry-info live in the heap persistently, populated in Pass_BuildAS.)
        // Scene slots 13-16 (distinct from the reflection 0-7 / denoise 8-12
        // ranges, so the debug pass never aliases their heap slots in a frame).
        bindless_.WriteSceneDescriptor(frameIndex, 13, tlasSrv);    // TLAS SRV
        bindless_.WriteSceneDescriptor(frameIndex, 14, D.gbSRV[1]); // GB1 (normal)
        bindless_.WriteSceneDescriptor(frameIndex, 15, D.depthSRV); // Depth
        bindless_.WriteSceneDescriptor(frameIndex, 16, D.reflectionUAV);   // reflection UAV (output)

        RtDebugConstants c{};
        c.invView = camera.GetInvViewMatrix();
        c.invProj = camera.GetInvProjMatrix();
        c.tlasIndex = bindless_.SceneIndex(frameIndex, 13);
        c.gb1Index = bindless_.SceneIndex(frameIndex, 14);
        c.depthIndex = bindless_.SceneIndex(frameIndex, 15);
        c.reflectionUavIndex = bindless_.SceneIndex(frameIndex, 16);
        c.geomInfoIndex = bindless_.GeomInfoIndex();
        c.outWidth = renderer->GetReflectionTextureWidth();
        c.outHeight = renderer->GetReflectionTextureHeight();

        auto cb = renderer->GetFrameResource()->AllocDynamic(sizeof(RtDebugConstants), render::kConstantBufferAlignment);
        std::memcpy(cb.cpu, &c, sizeof(c));

        // Bespoke dispatch: bind the bindless heap (not the per-frame heap) and the
        // shader's root sig (RootFlags CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED + CBV(b0) +
        // static samplers). Root param 0 is the b0 CBV.
        ID3D12DescriptorHeap* heaps[] = { bindless_.Heap() };
        t.cl->SetDescriptorHeaps(1, heaps);
        t.cl->SetComputeRootSignature(debugMaterial->GetRootSignature());
        t.cl->SetPipelineState(debugMaterial->GetPipelineState());
        t.cl->SetComputeRootConstantBufferView(0, cb.gpu);

        const UINT gx = (c.outWidth + 7u) / 8u;
        const UINT gy = (c.outHeight + 7u) / 8u;
        if (gx > 0 && gy > 0)
        {
            t.cl->Dispatch(gx, gy, 1);
        }
        renderer->UAVBarrier(t.cl, D.reflection.Get());

        // Leave reflection in the same frame-end state compose does, so the texture
        // inspector reads it exactly as it would the normal SSR result.
        renderer->Transition(t.cl, D.reflection.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    } while (false);
    ctx.EndCL(t);
}

void SceneRenderer::RecordOceanReflection(Renderer* renderer, ID3D12GraphicsCommandList* cl,
    const Camera& camera)
{
    if (!renderer || !cl)
    {
        return;
    }

    GPU_SCOPE(cl, ProfilerScopes::kPassOceanReflection);

    const auto& D = renderer->GetDeferredForFrame();
    auto makePixelReadable = [&]()
    {
        renderer->Transition(cl, D.sceneOpaque.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        renderer->Transition(cl, D.depthCopy.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        renderer->Transition(cl, D.oceanReflection.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    };

    if (!D.sceneOpaque.Get() || !D.depthCopy.Get() || !D.oceanReflection.Get())
    {
        makePixelReadable();
        return;
    }

    renderer->Transition(cl, D.sceneOpaque.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    renderer->Transition(cl, D.depthCopy.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    renderer->Transition(cl, D.oceanReflection.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    auto material = resources_.GetOceanReflectionMaterial();
    const UINT cbSize = resources_.GetOceanReflectionCBSizeBytes();
    if (!material || cbSize == 0 || D.sceneOpaqueSRV.ptr == 0 || D.depthCopySRV.ptr == 0 || D.oceanReflectionUAV.ptr == 0)
    {
        makePixelReadable();
        return;
    }

    OceanReflectionConstants constants{};
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
    constants.screenSize = float2(static_cast<float>(std::max(renderer->GetRenderWidth(), 1u)),
        static_cast<float>(std::max(renderer->GetRenderHeight(), 1u)));
    constants.invScreenSize = float2(
        constants.screenSize.x > 0.0f ? 1.0f / constants.screenSize.x : 0.0f,
        constants.screenSize.y > 0.0f ? 1.0f / constants.screenSize.y : 0.0f);
    constants.outputSize = float2(static_cast<float>(renderer->GetOceanReflectionTextureWidth()),
        static_cast<float>(renderer->GetOceanReflectionTextureHeight()));
    constants.camPosWS = camera.GetPosition();
    constants.waterHeight = 0.0f;

    const auto samplerDescs = std::array{ *SamplerManager::LinearClamp(), *SamplerManager::PointClamp() };
    RecordComputeDispatch(renderer, cl, material.get(), cbSize,
        [&](uint8_t* dest) { std::memcpy(dest, &constants, sizeof(constants)); },
        { D.sceneOpaqueSRV, D.depthCopySRV },
        { D.oceanReflectionUAV },
        renderer->GetSamplerManager()->GetTable(renderer, samplerDescs),
        renderer->GetOceanReflectionTextureWidth(), renderer->GetOceanReflectionTextureHeight(),
        D.oceanReflection.Get());

    makePixelReadable();
}

void SceneRenderer::Pass_Transparent(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera, const SceneView& mainView)
{
    // Shared per-view/per-frame CB (b1) for every transparent object in this pass.
    const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildGlassViewCB(renderer, camera, *frame_, glassReflActive_);

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
    rgTr.AddPass(RenderPass::Transparent_Driver, {}, [this, renderer, &camera](RenderGraphPassContext sub) {
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
            }
            if (D.sceneOpaque.Get())
            {
                driver.cl->CopyResource(D.sceneOpaque.Get(), D.scene.Get());
            }

            RecordOceanReflection(renderer, driver.cl, camera);

            renderer->Transition(driver.cl, D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            renderer->Transition(driver.cl, D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
            renderer->Transition(driver.cl, D.gbVelocity.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
#if WITH_EDITOR
            renderer->Transition(driver.cl, D.objectID.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
#endif
            // S15b: the glass refl (computed pre-transparent into UAV) is sampled by the forward
            // glass PS at t7. No-op when already PIXEL (RT off / non-RT HW: glass.hlsl won't read it).
            if (D.glassReflection.Get())
            {
                renderer->Transition(driver.cl, D.glassReflection.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
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
        ctx.ApplyDeclaredStates(t.cl);
        renderer->BindSceneColor(t.cl, Renderer::ClearMode::None, true);

        debugDraw->Render(renderer, t.cl, camera.GetViewMatrix(), camera.GetProjMatrix());
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

#if WITH_EDITOR
void SceneRenderer::Pass_SelectionOutline(Renderer* renderer, RenderGraphPassContext ctx)
{
    if (!renderer || !frame_ || frame_->selectedEditorObjectCount == 0)
    {
        return;
    }

    auto material = resources_.GetSelectionOutlineMaterial();
    const UINT cbSize = resources_.GetSelectionOutlineCBSizeBytes();
    if (!material || cbSize == 0)
    {
        return;
    }

    const auto& D = renderer->GetDeferredForFrame();
    if (D.stencilSRV.ptr == 0 || D.sceneUAV.ptr == 0)
    {
        return;
    }

    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        ctx.ApplyDeclaredStates(t.cl);

        SelectionOutlinePassConstants constants{};
        constants.screenSize = float2(
            static_cast<float>(std::max(renderer->GetRenderWidth(), 1u)),
            static_cast<float>(std::max(renderer->GetRenderHeight(), 1u)));
        constants.selectedBit = kSelectionStencilBit;
        constants.outlineRadius = std::clamp<std::uint32_t>(frame_->selectionOutlineRadius, 1u, 8u);
        constants.outlineColor = float4(1.0f, 0.82f, 0.12f, 0.92f);

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

void SceneRenderer::Pass_Tonemap(Renderer* renderer, RenderGraphPassContext ctx)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassTonemap);
        const auto& D = renderer->GetDeferredForFrame();
        ctx.ApplyDeclaredStates(t.cl);
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

        renderer->BindDescriptorHeaps(t.cl);

        auto tonemapMaterial = resources_.GetTonemapMaterial();
        if (!tonemapMaterial)
        {
            break;
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
            // The backbuffer's state cycle is owned OUTSIDE the graph and is fully determined:
            // RecordBindAndClear takes it PRESENT -> RENDER_TARGET at the top of the frame and the
            // present epilogue takes it back, both with hand-rolled barriers. So the resolve knows
            // its own before-states and needs no state tracking -- this pair was the LAST client of
            // ResourceStateTracker, and converting it is what let the tracker be deleted.
            Renderer::TransitionExplicit(t.cl, backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                         D3D12_RESOURCE_STATE_COPY_DEST);
            t.cl->CopyResource(backbuffer, resolveSource);
            renderer->Transition(t.cl, resolveSource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Renderer::TransitionExplicit(t.cl, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                         D3D12_RESOURCE_STATE_RENDER_TARGET);
        }

        renderer->Transition(t.cl, D.tonemap.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    } while (false);

    ctx.EndCL(t);
}

void SceneRenderer::Pass_Debug(Renderer* renderer, RenderGraphPassContext ctx)
{
    if (!frame_->settings.debugTexMode)
    {
        return;
    }
    const auto& D = renderer->GetDeferredForFrame();
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassDebug);
        renderer->RecordBindDefaultsNoClear(t.cl);

        auto h = renderer->GetRenderContextPool()->Acquire();
        auto& rc = h.ref();

        rc.srvTable[0] = renderer->StageSrvUavTable({ D.shadowSRV }).gpu; // t0
        //rc.srvTable[0] = renderer->StageSrvUavTable({ D.gbSRV[3] }).gpu; // t0
        const auto debugSamplers = std::array{ *SamplerManager::LinearClamp() };
        rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, debugSamplers);

        auto debugMaterial = resources_.GetDebugMaterial();
        if (!debugMaterial)
        {
            break;
        }

        debugMaterial->Bind(t.cl, rc);
        t.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        t.cl->DrawInstanced(3, 1, 0, 0);
    } while (false);

    ctx.EndCL(t);
}

void SceneRenderer::Pass_Overlay(Renderer* renderer, RenderGraphPassContext ctx, TaskSystem::TaskHandle& overlayPrepTask)
{
    if (overlayPrepTask)
    {
        CPU_SCOPE(ProfilerScopes::kOverlayAsyncWait);
        TaskSystem::Get().Wait(overlayPrepTask);
        TaskSystem::Get().Release(overlayPrepTask);
    }

    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassOverlay);
        renderer->RecordBindDefaultsNoClear(t.cl);

        renderer->RenderImGui(t.cl);
        renderer->RestoreGraphicsStateAfterExternalDraw(t.cl);

        if (auto* tm = renderer->GetTextManager())
        {
            tm->Draw(renderer, t.cl);
        }

        //renderer->RenderImGui(t.cl);
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}
