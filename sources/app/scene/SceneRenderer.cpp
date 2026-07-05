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
#include "rendering/renderables/RenderableObject.h"
#include "rendering/renderables/ShadowGpuData.h"
#include "rendering/renderables/VirtualShadowMap.h"
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
    RenderableObjectBase* FindSelectedObject(const SceneFrameData& frame, const SceneView& view, bool transparent)
    {
        if (frame.selectedEditorObjectId == 0 || !frame.objects)
        {
            return nullptr;
        }

        for (const auto& owned : *frame.objects)
        {
            RenderableObjectBase* obj = owned.get();
            if (!obj || obj->GetEditorObjectId() != frame.selectedEditorObjectId)
            {
                continue;
            }

            if (!obj->IsVisible() || obj->IsTransparent() != transparent)
            {
                return nullptr;
            }

            if ((obj->GetRenderLayerMask() & view.renderLayerMask) == 0)
            {
                return nullptr;
            }

            const AABB& bounds = obj->GetWorldBounds();
            if (view.frustum.IsValid() && bounds.IsValid() && !view.frustum.Intersects(bounds))
            {
                return nullptr;
            }

            return obj;
        }

        return nullptr;
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

    // Matches gbuffer_common.hlsl `cbuffer PerView : register(b1)`. The depth-only
    // shadow shaders consume only viewProj (the other two are left identity/unused).
    struct PerViewCB
    {
        mat4 viewProj;
        mat4 viewProjNoJitter;
        mat4 prevViewProjNoJitter;
    };

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

    D3D12_GPU_VIRTUAL_ADDRESS BuildGBufferViewCB(Renderer* renderer, const Camera& camera)
    {
        PerViewCB vc{};
        vc.viewProj = camera.GetViewProjMatrix();
        vc.viewProjNoJitter = camera.GetViewProjMatrixNoJitter();
        vc.prevViewProjNoJitter = camera.GetPrevViewProjMatrixNoJitter();
        return UploadFrameCB(renderer, vc);
    }

    D3D12_GPU_VIRTUAL_ADDRESS BuildShadowViewCB(Renderer* renderer, const mat4& lightView, const mat4& lightProj)
    {
        PerViewCB vc{};
        vc.viewProj = lightView * lightProj; // viewProjNoJitter/prevViewProjNoJitter unused by shadow shaders
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
        // z = glass-reflections-active flag: glass.hlsl samples GlassReflection when set (RT or
        // SSR mode), else uses skybox.
        vc.lightCounts = float4(pointCount, spotCount, glassReflActive ? 1.0f : 0.0f, 0.0f);

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

void SceneRenderer::Render(Renderer* renderer, const SceneFrameData& frame)
{
    if (!renderer)
    {
        return;
    }

    frame_ = &frame;

    // Reflection source (S8) + RT debug viz (S6), gated on hardware support. RT
    // reflections fall back to SSR on non-RT hardware (rtReflect stays false, so
    // the screen-space reflection source runs). The AS is built only when RT reflections or the debug
    // viz need it; otherwise the frame is byte-identical to the SSR/Off path.
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
    const bool reflectionsOff = frame.settings.reflectionSource == ReflectionSource::Off;
    const bool rtBuildAS = rtReflect || rtDebugView;
    rtReflectActive_ = rtReflect; // S15: RT reflections active this frame
    // S15b: glass gets off-screen reflections whenever opaque reflections do (source != Off) —
    // RT mode uses rt_reflections_cs, SSR mode (and RT's AS-failure fallback) uses ssr_cs. The
    // forward glass samples glassReflection when this is set (b1 flag), else skybox.
    glassReflActive_ = !reflectionsOff;
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

    using MainRenderGraph = RenderGraph<kMainRenderGraphPassCount>;
    MainRenderGraph rg;

    // RT acceleration-structure build (S5): the first pass when RT is enabled.
    // No consumer yet, so it's an independent node (no prereqs/dependents); a
    // future RT reflections pass (S7) will depend on it. The pass declares no
    // resource states and never transitions the AS buffers, so they bypass the
    // ResourceStateTracker and stay in RAYTRACING_ACCELERATION_STRUCTURE.
    size_t pBuildAS = (size_t)-1;
    if (rtBuildAS)
    {
        pBuildAS = rg.AddPass(RenderPass::Main_BuildAS, {},
            [this, renderer](RenderGraphPassContext ctx) {
                CPU_SCOPE(ProfilerScopes::kPassBuildAS);
                Pass_BuildAS(renderer, ctx);
            });
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

    auto pShoreDepth = rg.AddPass(RenderPass::Main_TerrainDepth, { pCompute },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassShoreDepth);
            OceanSimulation* oceanSim = Systems::GetOceanSimulation();
            const SceneView* shoreView = oceanSim ? &oceanSim->GetShoreDepthView() : nullptr;
            Pass_ShoreDepth(renderer, ctx, shoreView);
        });

    // Rung 0 / Step 4: GPU cull -> indirect shadow args, before the shadow passes (its output
    // is not consumed yet). Manages its own UAV states (declares none). Placed in the chain so
    // Step 6's ExecuteIndirect can consume it.
    auto pShadowCull = rg.AddPass(RenderPass::Main_ShadowCull, { pShoreDepth },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassShadowCull);
            Pass_ShadowCull(renderer, ctx);
        });

    auto pShadow = rg.AddPass(RenderPass::Main_CSM, { pShadowCull },
        { { D.shadow.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE } },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassCSM);
            Pass_CSM(renderer, ctx, *frame_->cascadeViews);
        });

    // No declarations: the per-light command lists are recorded in parallel with
    // no deterministic submit order inside the batch, so each list must register
    // the atlas state itself (first-use in whichever list lands first).
    auto pSpotShadow = rg.AddPass(RenderPass::Main_SpotShadows, { pShadow },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassSpotShadow);
            Pass_SpotShadows(renderer, ctx, *frame_->spotShadowViews);
        });

    // B2b: point cube shadows. Same per-CL atlas-state registration story as spot
    // shadows (parallel per-face lists, no declared states). Runs before Pass_PointLights
    // (which samples the cube atlas in B3).
    auto pPointShadow = rg.AddPass(RenderPass::Main_PointShadows, { pSpotShadow },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassPointShadow);
            Pass_PointShadows(renderer, ctx, *frame_->pointShadowViews);
        });

    auto pGbuf = rg.AddPass(RenderPass::Main_GBuffer, { pPointShadow },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassGBuffer);
            Pass_GBuffer(renderer, ctx, *frame_->camera, *frame_->mainView);
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
    (void)pVsmPageRequest;

    auto pLight = rg.AddPassMT(RenderPass::Main_Lighting, { pGbuf }, { pShadow },
        { { D.gb0.Get(), kSrvAll },
          { D.gb1.Get(), kSrvAll },
          { D.gb2.Get(), kSrvAll },
          { D.gbVelocity.Get(), kSrvAll },
          { D.depth.Get(), kSrvAll },
          { D.shadow.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassLighting);
            Pass_Lighting(renderer, ctx, *frame_->camera);
        });

    auto pSpotLights = rg.AddPassMT(RenderPass::Main_SpotLights, { pLight }, { pSpotShadow },
        { { D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
          { D.gb0.Get(), kSrvAll },
          { D.gb1.Get(), kSrvAll },
          { D.gb2.Get(), kSrvAll },
          { D.gbVelocity.Get(), kSrvAll },
          { D.depth.Get(), kSrvAll },
          { D.spotShadow.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassSpotLights);
            Pass_SpotLights(renderer, ctx, *frame_->camera);
        });

    // Depends on pPointShadow too: the cube must be rendered + transitioned to a
    // shader-readable state before this pass samples it (B3). kSrvAll keeps it readable
    // by both this compute pass and the later transparent (glass) pixel pass.
    auto pPointLights = rg.AddPass(RenderPass::Main_PointLights, { pSpotLights, pPointShadow },
        { { D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
          { D.gb0.Get(), kSrvAll },
          { D.gb1.Get(), kSrvAll },
          { D.gb2.Get(), kSrvAll },
          { D.gbVelocity.Get(), kSrvAll },
          { D.depth.Get(), kSrvAll },
          { D.pointShadow.Get(), kSrvAll } },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassPointLights);
            Pass_PointLights(renderer, ctx, *frame_->camera);
        });

    auto pSky = rg.AddPass(RenderPass::Main_Skybox, { pPointLights },
        { { D.light.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
          { D.gbVelocity.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
          { D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_READ } },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassSkybox);
            Pass_Skybox(renderer, ctx, *frame_->camera);
        });

    // CL group (step 5): reflection source -> reflection blur -> compose is a sequential single-dispatch
    // chain with no mtDeps. Grouping collapses its 3 command lists into 1 — the
    // per-CL prologue/acquire overhead dominates these passes' tiny record cost,
    // and the inter-pass acquire barriers become correctly-placed intra-CL barriers.
    rg.BeginCLGroup();
    // Reflection source (S8): whichever variant runs writes the same premultiplied
    // reflection buffer, so the blur + compose chain is identical. RT (S7) runs
    // instead of the screen-space source (mt-dep on Main_BuildAS; its TLAS SRV
    // bypasses the state tracker); Off just clears reflection so compose shows
    // skybox specular only.
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
    else if (reflectionsOff)
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

    // First-use states only; the blur ping-pongs reflection<->scratch states between
    // its two dispatches inside the pass body.
    auto pBlur = rg.AddPass(RenderPass::Main_ReflectionBlur, { pReflectionSource },
        { { D.reflection.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.reflectionScratch.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
          { D.gb0.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } }, // S16: roughness drives glossy blur
        [this, renderer](RenderGraphPassContext ctx) { CPU_SCOPE(ProfilerScopes::kPassReflectionBlur); Pass_ReflectionBlur(renderer, ctx); });

    // First-use states only; Compose transitions scene back to RENDER_TARGET
    // for the transparent pass at the end of its body.
    auto pCompose = rg.AddPass(RenderPass::Main_Compose, { pBlur },
        { { D.gb0.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.gb1.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.gb2.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.light.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.reflection.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.scene.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassCompose);
            Pass_Compose(renderer, ctx, *frame_->camera);
        });
    rg.EndCLGroup();

    // RT debug visualization (S6): runs AFTER the reflection group so it can overwrite
    // the already-consumed reflection target with ray-hit data for inspection via
    // TextureDebugViewer -> Reflection, without disturbing the composited scene. Needs
    // the TLAS (mtDep on Main_BuildAS) and reflection free (prereq/mtDep on Compose).
    // The TLAS SRV bypasses the state tracker (staged as a plain descriptor).
    if (rtDebugView && pBuildAS != (size_t)-1)
    {
        rg.AddPassMT(RenderPass::Main_RTDebug, { pCompose }, { pCompose, pBuildAS },
            { { D.reflection.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
              { D.gb1.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } },
            [this, renderer](RenderGraphPassContext ctx) {
                CPU_SCOPE(ProfilerScopes::kPassRTDebug);
                Pass_RTDebug(renderer, ctx, *frame_->camera);
            });
    }

    // Off-screen glass reflections (S15b): render a glass front-face G-buffer (normal+depth)
    // then compute reflections over it into glassReflection (sampled by the forward glass pass).
    // Active in RT mode (rt_reflections_cs, incl. off-screen recompute) AND SSR mode (ssr_cs).
    // Runs after Compose so the lit opaque `light` buffer is the on-screen color source. Off =>
    // these passes are absent and glass falls back to skybox by construction (b1 flag is 0).
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
    }

    auto pDebugDraw = rg.AddPass(RenderPass::Main_DebugDraw, { pObjectIdReadback },
        { { D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
          { D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE } },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassDebugDraw);
            Pass_DebugDraw(renderer, ctx, *frame_->camera);
        });

    size_t pSelectionOutline = pDebugDraw;
#if WITH_EDITOR
    if (frame_->selectedEditorObjectId != 0)
    {
        pSelectionOutline = rg.AddPass(RenderPass::Main_SelectionOutline, { pDebugDraw },
            { { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.scene.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
            [this, renderer](RenderGraphPassContext ctx) {
                Pass_SelectionOutline(renderer, ctx);
            });
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

    rg.AddPass(RenderPass::Main_Debug, { pTone },
        [this, renderer](RenderGraphPassContext ctx) { CPU_SCOPE(ProfilerScopes::kPassDebug); Pass_Debug(renderer, ctx); });
    rg.EndCLGroup();

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
        [this, renderer, &overlayPrepTask](RenderGraphPassContext ctx) { CPU_SCOPE(ProfilerScopes::kPassOverlay); Pass_Overlay(renderer, ctx, overlayPrepTask); });
    epilogueRG.Execute(renderer);
    renderer->EndFrame();
    renderer->ResolveObjectIdPickReadback();

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
    D3D12_GPU_VIRTUAL_ADDRESS viewCB)
{
    if (objects.empty()) {
        return;
    }

    //chunkSize = 16;

    auto& tasks = TaskSystem::Get();
    const size_t N = objects.size();

    auto renderJob = [renderer, &camera, &objects, useBundles, chunkSize, batchIndex, bindGbufOrScene, bindVelocity, viewCB](std::size_t jobIndex)
    {
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
            // Chunk index is the deterministic submit order within the batch's
            // bundle namespace (transparents must blend in sorted-queue order).
            renderer->EndThreadCommandBundle(b, batchIndex, static_cast<uint32_t>(jobIndex));
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
                        renderer->Transition(t.cl, D.objectID.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
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
            // Chunk index is the deterministic submit order within the batch's
            // direct namespace (transparents must blend in sorted-queue order).
            renderer->EndThreadCommandList(t, batchIndex, static_cast<uint32_t>(jobIndex));
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
            if (!obj) { continue; }
            // GetRtInstances appends one desc for a single-mesh object, or one per GPU
            // instance for instanced renderables (S14: instanced models reflect).
            descs.clear();
            obj->GetRtInstances(descs);
            for (const RtInstanceDesc& desc : descs)
            {
                rt::InstanceEntry entry;
                entry.mesh = desc.mesh;
                entry.world = desc.world.m; // Math::mat4 wraps a row-major XMFLOAT4X4
                // TLAS InstanceID = the mesh's bindless geometry index (S9), so a hit
                // can index the geometry/material table directly. Same mesh+material ->
                // same index (all instances of a cloud share one record). Falls back to
                // a running index if the bindless table isn't up.
                entry.instanceId = bindless_.Ready()
                    ? bindless_.GetOrRegisterMesh(desc.mesh, desc.albedoSrv, desc.mrSrv, &desc.baseColor.x,
                                                  /*roughness*/ desc.metalRough.y, /*metalness*/ desc.metalRough.x)
                    : instanceId;
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
        // AS buffers bypass the ResourceStateTracker: this pass declares no
        // resource states and never calls Transition on them, so the RenderGraph
        // never moves them out of RAYTRACING_ACCELERATION_STRUCTURE / UNORDERED_
        // ACCESS. Mesh VB/IB are read by first-frame BLAS builds via implicit
        // COMMON->NON_PIXEL_SHADER_RESOURCE promotion (this pass runs first, so
        // the buffers are fresh-decayed to COMMON).
        ID3D12GraphicsCommandList4* cl4 = renderer->AsCmdList4(t.cl);
        if (cl4 && !rtInstances_.empty())
        {
            asManager_.BuildTlas(rtInstances_, cl4, renderer->GetCurrentFrameIndex());
            if (asManager_.HasPendingScratch())
            {
                asScratchRetireFrame_ = frameNo + render::kFrameCount;
            }
            // S13: one-time AS VRAM accounting for visibility/budgeting.
            if (!asVramLogged_ && !asManager_.BuildFailed())
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
    if (!render::g_vsmPageRequestEnabled) { return; }
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
    cb.lodParams = DirectX::XMFLOAT4(vsm::kLodRefDist, static_cast<float>(vsm::kMaxMipLevel),
                                     static_cast<float>(vsm::kRequestDownscale), 0.0f);

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
    cb.numViews = slot;

    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassVsmPageRequest);
        frame_->vsm->RecordPageRequest(renderer, t.cl, cb, D.depthSRV, rw, rh);
        // Step 20: allocate physical pages for the just-marked requests (same CL — request buffer
        // stays UAV between them). Add-dormant: nothing samples/renders the pages yet.
        frame_->vsm->RecordPageAllocate(renderer, t.cl);
    }
    ctx.EndCL(t);
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

        const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildShadowViewCB(renderer, view->view, view->proj);

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
    auto renderCascade = [renderer, &cascadeViews, batchIndex = ctx.batchIndex, passName, shadowGpu, indirect](std::size_t cascadeIndex)
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

        const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildShadowViewCB(renderer, view.view, view.proj);

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
                // GPU-instanced casters aren't in the indirect buffer (one object -> many GPU
                // instances); draw them via their own instanced shadow path so they still cast.
                const UINT giLod = static_cast<UINT>(cascadeIndex);
                for (auto* obj : opaqueSimple)  { if (obj && obj->IsGpuInstancedCaster()) obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB, giLod); }
                for (auto* obj : opaqueComplex) { if (obj && obj->IsGpuInstancedCaster()) obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB, giLod); }
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

        const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildShadowViewCB(renderer, view.view, view.proj);

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
    auto renderSpotShadow = [renderer, &D, &spotViews, batchIndex = ctx.batchIndex, shadowGpu, indirect](std::size_t lightIndex)
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
            const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildShadowViewCB(renderer, view.view, view.proj);
            const auto& visibleBuckets = view.queue.VisibleBuckets();
            const auto& opaqueSimple = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueSimple)];
            const auto& opaqueComplex = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueComplex)];

            if (indirect)
            {
                // Spot light i -> shadow-view slot kCascades + i.
                const std::uint32_t viewSlot = static_cast<std::uint32_t>(kCascades + lightIndex);
                shadowGpu->RecordIndirectShadowDraws(renderer, t.cl, viewSlot, viewCB);
                // GPU-instanced casters draw via their own instanced shadow path (not indirect).
                for (auto* obj : opaqueSimple)  { if (obj && obj->IsGpuInstancedCaster()) obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB); }
                for (auto* obj : opaqueComplex) { if (obj && obj->IsGpuInstancedCaster()) obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB); }
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
            const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildShadowViewCB(renderer, view.view, view.proj);
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
    auto renderPointShadow = [renderer, &D, &pointViews, batchIndex = ctx.batchIndex, shadowGpu, indirect](std::size_t faceIndex)
    {
        if (faceIndex >= pointViews.size())
        {
            return;
        }

        CPU_SCOPE(ProfilerScopes::kSpotShadowPerLight);
        auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        {
            GPU_SCOPE(t.cl, ProfilerScopes::kPassPointShadow);
            renderer->Transition(t.cl, D.pointShadow.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
            renderer->BindPointShadowTarget(t.cl, static_cast<UINT>(faceIndex / 6),
                static_cast<UINT>(faceIndex % 6), /*clear=*/true);

            const SceneView& view = pointViews[faceIndex];
            const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildShadowViewCB(renderer, view.view, view.proj);
            const auto& visibleBuckets = view.queue.VisibleBuckets();
            const auto& opaqueSimple = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueSimple)];
            const auto& opaqueComplex = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueComplex)];

            if (indirect)
            {
                // Point cube face k -> shadow-view slot kCascades + kMaxShadowedSpotLights + k.
                const std::uint32_t viewSlot = static_cast<std::uint32_t>(
                    kCascades + LightManager::kMaxShadowedSpotLights + faceIndex);
                shadowGpu->RecordIndirectShadowDraws(renderer, t.cl, viewSlot, viewCB);
                // GPU-instanced casters draw via their own instanced shadow path (not indirect).
                for (auto* obj : opaqueSimple)  { if (obj && obj->IsGpuInstancedCaster()) obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB); }
                for (auto* obj : opaqueComplex) { if (obj && obj->IsGpuInstancedCaster()) obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB); }
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
            const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildShadowViewCB(renderer, view.view, view.proj);
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
    const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildGBufferViewCB(renderer, camera);

    RenderGraph<kGBufferRenderGraphPassCount> rgGB(ctx.batchIndex);
    const size_t pDriver = rgGB.AddPass(RenderPass::GBuffer_Driver, {},
        { { D.gb0.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
          { D.gb1.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
          { D.gb2.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
          { D.gbVelocity.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
          { D.objectID.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
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
    if (frame_->selectedEditorObjectId != 0)
    {
        RenderGraph<kGBufferRenderGraphPassCount>::DependencyList selectedDeps;
        selectedDeps.push_back(pOpaqueSimple);
        selectedDeps.push_back(pOpaqueComplex);
        rgGB.AddPass(RenderPass::GBuffer_Selected, selectedDeps, [this, renderer, &camera, &mainView, viewCB](RenderGraphPassContext sub) {
            RenderableObjectBase* selected = FindSelectedObject(*frame_, mainView, false);
            if (!selected)
            {
                return;
            }

            auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
            SetCommandListName(t.cl, sub.pass);
            {
                GPU_SCOPE(t.cl, ProfilerScopes::kRenderObjectBatchGpu);
                renderer->BindGBuffer(t.cl, Renderer::ClearMode::None);
                t.cl->OMSetStencilRef(kSelectionStencilBit);
                selected->Render(renderer, t.cl, camera, viewCB);
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
            deferred.depthSRV.ptr == 0 || deferred.shadowSRV.ptr == 0)
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

    if (!lightManager.EnsureSpotLightBuffer(renderer, spotLightCount))
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
            deferred.depthSRV.ptr == 0 || deferred.spotShadowSRV.ptr == 0)
        {
            return;
        }
    }

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

        const auto samplerDescs = std::array{ *SamplerManager::LinearClamp(), *SamplerManager::PointClamp(), *SamplerManager::ComparisonLinearClamp() };
        RecordComputeDispatch(renderer, t.cl, spotMaterial.get(), cbSize,
            [&](uint8_t* dest) { resources_.WriteSpotLightConstants(constants, dest); },
            { D.gbSRV[0], D.gbSRV[1], D.gbSRV[2], D.gbSRV[3], D.depthSRV, D.spotShadowSRV, spotLightSrvHandle },
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

    if (!lightManager.EnsurePointLightBuffer(renderer, pointLights.size()))
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
            deferred.depthSRV.ptr == 0)
        {
            return;
        }
    }

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

        // s2 = comparison sampler for the point shadow cube (B3).
        const auto samplerDescs = std::array{ *SamplerManager::LinearClamp(), *SamplerManager::PointClamp(), *SamplerManager::ComparisonLinearClamp() };
        RecordComputeDispatch(renderer, t.cl, pointMaterial.get(), cbSize,
            [&](uint8_t* dest) { resources_.WritePointLightConstants(constants, dest); },
            // t0-t5 as before; t6 = point shadow depth cube (B3).
            { D.gbSRV[0], D.gbSRV[1], D.gbSRV[2], D.gbSRV[3], D.depthSRV, pointLightSrvHandle, D.pointShadowSRV },
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
    // Reflection source = Off: zero the reflection target so the unchanged blur +
    // compose produce skybox-specular-only reflections. The target is per-frame, so it
    // must be cleared every frame, not once.
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
        if (!composeMaterial || cbSize == 0 || !skybox)
        {
            renderer->Transition(t.cl, D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            break;
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
            { D.lightSRV, D.gbSRV[2], D.gbSRV[0], D.gbSRV[1], D.depthSRV, skybox->GetTex()->GetSRVCPU(), D.reflectionSRV },
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
            renderer->Transition(driver.cl, D.dlssBias.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            renderer->Transition(driver.cl, D.objectID.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            // S15b: the glass refl (computed pre-transparent into UAV) is sampled by the forward
            // glass PS at t7. No-op when already PIXEL (RT off / non-RT HW: glass.hlsl won't read it).
            if (D.glassReflection.Get())
            {
                renderer->Transition(driver.cl, D.glassReflection.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            const float clearBias[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
            driver.cl->ClearRenderTargetView(D.dlssBiasRTV, clearBias, 0, nullptr);
            renderer->BindSceneColorWithVelocity(driver.cl, Renderer::ClearMode::None, true);
        }
        renderer->RegisterPassDriver(driver.cl, sub.batchIndex);
        });

    [[maybe_unused]] const size_t pTransparentSimple = rgTr.AddPass(RenderPass::Transparent_Simple, {}, [this, renderer, &camera, &mainView, viewCB](RenderGraphPassContext sub) {
        const auto& visibleBuckets = mainView.queue.VisibleBuckets();
        const auto& transparentSimple = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::TransparentSimple)];
        if (!transparentSimple.empty())
        {
            RenderObjectBatch(renderer, transparentSimple, sub.batchIndex, camera, /*useBundles=*/true, false, true, 32, viewCB);
        }
        });

    [[maybe_unused]] const size_t pTransparentComplex = rgTr.AddPass(RenderPass::Transparent_Complex, {}, [this, renderer, &camera, &mainView, viewCB](RenderGraphPassContext sub) {
        const auto& visibleBuckets = mainView.queue.VisibleBuckets();
        const auto& transparentComplex = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::TransparentComplex)];
        if (!transparentComplex.empty())
        {
            RenderObjectBatch(renderer, transparentComplex, sub.batchIndex, camera, /*useBundles=*/false, false, true, 32, viewCB);
        }
        });

#if WITH_EDITOR
    if (frame_ && frame_->selectedEditorObjectId != 0)
    {
        RenderGraph<kTransparentRenderGraphPassCount>::DependencyList selectedDeps;
        selectedDeps.push_back(pTransparentSimple);
        selectedDeps.push_back(pTransparentComplex);
        rgTr.AddPass(RenderPass::Transparent_Selected, selectedDeps, [this, renderer, &camera, &mainView](RenderGraphPassContext sub) {
            RenderableObjectBase* selected = FindSelectedObject(*frame_, mainView, true);
            auto material = resources_.GetSelectionStencilMaterial();
            if (!selected || !material)
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
                selected->RenderSelectionStencil(renderer, t.cl, material.get(), camera);
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
    if (!renderer || !frame_ || frame_->selectedEditorObjectId == 0)
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
            renderer->Transition(t.cl, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST);
            t.cl->CopyResource(backbuffer, resolveSource);
            renderer->Transition(t.cl, resolveSource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            renderer->Transition(t.cl, backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
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
        //rc.srvTable[0] = renderer->StageSrvUavTable({ D.dlssBiasSRV }).gpu; // t0
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

        if (auto* tm = renderer->GetTextManager())
        {
            tm->Draw(renderer, t.cl);
        }

        renderer->RenderImGui(t.cl);
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}
