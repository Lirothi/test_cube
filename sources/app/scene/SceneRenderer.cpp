#include "app/scene/SceneRenderer.h"

#include <algorithm>
#include <array>
#include <cassert>
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

    D3D12_GPU_VIRTUAL_ADDRESS BuildGlassViewCB(Renderer* renderer, const Camera& camera, const SceneFrameData& frame)
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
        vc.lightCounts = float4(pointCount, spotCount, 0.0f, 0.0f);

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
    // the SSR pass runs). The AS is built only when RT reflections or the debug
    // viz need it; otherwise the frame is byte-identical to the SSR/Off path.
    const bool rtSupported = renderer->IsRaytracingSupported();
    const bool rtDebugView = rtSupported && frame.settings.rtDebugView;
    const bool rtReflect = rtSupported && frame.settings.reflectionSource == ReflectionSource::RT;
    const bool reflectionsOff = frame.settings.reflectionSource == ReflectionSource::Off;
    const bool rtBuildAS = rtReflect || rtDebugView;
    if (rtBuildAS && !asManagerInited_)
    {
        asManager_.Init(renderer->GetDevice5());
        bindless_.Init(renderer->GetDevice());
        asManagerInited_ = true;
    }
    // S11 temporal-accumulation history (RT reflections only). (Re)allocate to the
    // current SSR resolution; register both textures with the state tracker on a
    // (re)alloc so the denoise pass can transition them.
    if (rtReflect)
    {
        if (reflectionHistory_.EnsureSize(renderer->GetDevice(), renderer->GetSsrTextureWidth(),
                                          renderer->GetSsrTextureHeight(), renderer->GetSsrFormat()))
        {
            renderer->SetResourceState(reflectionHistory_.Curr(0), D3D12_RESOURCE_STATE_COMMON);
            renderer->SetResourceState(reflectionHistory_.Prev(0), D3D12_RESOURCE_STATE_COMMON);
        }
    }

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

    auto pShadow = rg.AddPass(RenderPass::Main_CSM, { pShoreDepth },
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

    auto pGbuf = rg.AddPass(RenderPass::Main_GBuffer, { pSpotShadow },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassGBuffer);
            Pass_GBuffer(renderer, ctx, *frame_->camera, *frame_->mainView);
        });

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

    auto pPointLights = rg.AddPass(RenderPass::Main_PointLights, { pSpotLights },
        { { D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
          { D.gb0.Get(), kSrvAll },
          { D.gb1.Get(), kSrvAll },
          { D.gb2.Get(), kSrvAll },
          { D.gbVelocity.Get(), kSrvAll },
          { D.depth.Get(), kSrvAll } },
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

    // CL group (step 5): SSR -> SSRBlur -> Compose is a sequential single-dispatch
    // chain with no mtDeps. Grouping collapses its 3 command lists into 1 — the
    // per-CL prologue/acquire overhead dominates these passes' tiny record cost,
    // and the inter-pass acquire barriers become correctly-placed intra-CL barriers.
    rg.BeginCLGroup();
    // Reflection source (S8): whichever variant runs writes the same premultiplied
    // ssr buffer, so the blur + compose chain is identical. RT (S7) runs instead
    // of SSR (mt-dep on Main_BuildAS; its TLAS SRV bypasses the state tracker);
    // Off just clears ssr so compose shows skybox specular only.
    const bool useRtReflections = rtReflect && pBuildAS != (size_t)-1 && reflectionHistory_.Ready();
    const std::initializer_list<ResourceStateDecl> reflectDecls = {
        { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { D.gb1.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { D.light.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { D.ssr.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } };
    size_t pSSR; // node the blur depends on (reflection chain end)
    if (useRtReflections)
    {
        // RT (S7/S10): write the raw (roughness-jittered) reflection to ssrBlur,
        // then temporally accumulate (S11) into ssr + the current history texture.
        const uint64_t parity = renderer->GetTotalFrameNumber();
        ID3D12Resource* histPrev = reflectionHistory_.Prev(parity);
        ID3D12Resource* histCurr = reflectionHistory_.Curr(parity);
        size_t pRefl = rg.AddPassMT(RenderPass::Main_RTReflections, { pSky }, { pSky, pBuildAS },
            { { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.gb1.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.gb0.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.light.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.ssrBlur.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
            [this, renderer](RenderGraphPassContext ctx) {
                CPU_SCOPE(ProfilerScopes::kPassRTReflections);
                Pass_RTReflections(renderer, ctx, *frame_->camera);
            });
        pSSR = rg.AddPass(RenderPass::Main_RTDenoise, { pRefl },
            { { D.ssrBlur.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.gbVelocity.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { histPrev, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { histCurr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
              { D.ssr.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
            [this, renderer](RenderGraphPassContext ctx) {
                CPU_SCOPE(ProfilerScopes::kPassRTDenoise);
                Pass_RTDenoise(renderer, ctx);
            });
    }
    else if (reflectionsOff)
    {
        pSSR = rg.AddPass(RenderPass::Main_SSR, { pSky },
            { { D.ssr.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
            [this, renderer](RenderGraphPassContext ctx) {
                CPU_SCOPE(ProfilerScopes::kPassSSR);
                Pass_ClearReflections(renderer, ctx);
            });
    }
    else
    {
        pSSR = rg.AddPass(RenderPass::Main_SSR, { pSky }, reflectDecls,
            [this, renderer](RenderGraphPassContext ctx) {
                CPU_SCOPE(ProfilerScopes::kPassSSR);
                Pass_SSR(renderer, ctx, *frame_->camera);
            });
    }

    // First-use states only; the blur ping-pongs SSR<->SSRBlur states between
    // its two dispatches inside the pass body.
    auto pBlur = rg.AddPass(RenderPass::Main_SSRBlur, { pSSR },
        { { D.ssr.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.ssrBlur.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
        [this, renderer](RenderGraphPassContext ctx) { CPU_SCOPE(ProfilerScopes::kPassSSRBlur); Pass_SSR_Blur(renderer, ctx); });

    // First-use states only; Compose transitions scene back to RENDER_TARGET
    // for the transparent pass at the end of its body.
    auto pCompose = rg.AddPass(RenderPass::Main_Compose, { pBlur },
        { { D.gb0.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.gb1.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.gb2.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.light.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.ssr.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.scene.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassCompose);
            Pass_Compose(renderer, ctx, *frame_->camera);
        });
    rg.EndCLGroup();

    // RT debug visualization (S6): runs AFTER the SSR group so it can overwrite
    // the (already-consumed) ssr target with ray-hit data for inspection via
    // TextureDebugViewer -> Ssr, without disturbing the composited scene. Needs
    // the TLAS (mtDep on Main_BuildAS) and ssr free (prereq/mtDep on Compose).
    // The TLAS SRV bypasses the state tracker (staged as a plain descriptor).
    if (rtDebugView && pBuildAS != (size_t)-1)
    {
        rg.AddPassMT(RenderPass::Main_RTDebug, { pCompose }, { pCompose, pBuildAS },
            { { D.ssr.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
              { D.gb1.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } },
            [this, renderer](RenderGraphPassContext ctx) {
                CPU_SCOPE(ProfilerScopes::kPassRTDebug);
                Pass_RTDebug(renderer, ctx, *frame_->camera);
            });
    }

    // No declarations: the driver sequences depth/scene copies (COPY_SOURCE/DEST
    // flips mid-list) before rebinding the targets — inherently ordered work that
    // first-use declarations cannot express.
    auto pTransp = rg.AddPass(RenderPass::Main_Transparent, { pCompose },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassTransparent);
            Pass_Transparent(renderer, ctx, *frame_->camera, *frame_->mainView);
        });

    auto pDebugDraw = rg.AddPass(RenderPass::Main_DebugDraw, { pTransp },
        { { D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
          { D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE } },
        [this, renderer](RenderGraphPassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassDebugDraw);
            Pass_DebugDraw(renderer, ctx, *frame_->camera);
        });

    // Ensure tonemapping runs after the debug draw pass so the resolved backbuffer
    // always includes any debug geometry submitted during rendering.
    // Only the unconditional outputs are declared; the tonemap source (scene or
    // DLSS output) and the backbuffer copy are handled inside the pass body.
    // CL group (step 5): the optional debug-texture draw follows tonemap on the
    // same target with no mtDeps; share one command list (Debug usually early-outs).
    rg.BeginCLGroup();
    auto pTone = rg.AddPass(RenderPass::Main_Tonemap, { pDebugDraw },
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

    // Gather opaque, single-mesh, CPU-placed instances. Instanced clouds (GPU
    // transforms), ocean (displaced) and transparent/glass return false from
    // GetRtInstance and are excluded for now (S13 defines their handling).
    rtInstances_.clear();
    if (frame_->objects)
    {
        uint32_t instanceId = 0;
        for (const auto& obj : *frame_->objects)
        {
            RtInstanceDesc desc{};
            if (obj && obj->GetRtInstance(desc))
            {
                rt::InstanceEntry entry;
                entry.mesh = desc.mesh;
                entry.world = desc.world.m; // Math::mat4 wraps a row-major XMFLOAT4X4
                // TLAS InstanceID = the mesh's bindless geometry index (S9), so a
                // hit can index the geometry/material table directly. Same mesh ->
                // same index (instances share geometry). Falls back to a running
                // index if the bindless table isn't up.
                entry.instanceId = bindless_.Ready()
                    ? bindless_.GetOrRegisterMesh(desc.mesh, desc.albedoSrv, &desc.baseColor.x)
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
        ctx.ApplyDeclaredStates(d.cl);
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

        const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildShadowViewCB(renderer, view.view, view.proj);

        auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        SetCommandListName(t.cl, passName);
        {
            GPU_SCOPE(t.cl, ProfilerScopes::kPassCSM);
            renderer->BindShadowTarget(t.cl, static_cast<int>(cascadeIndex), /*clear=*/false);

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

void SceneRenderer::Pass_GBuffer(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera, const SceneView& mainView)
{
    const auto& D = renderer->GetDeferredForFrame();

    // Shared per-view CB (b1) for every opaque object in this pass.
    const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildGBufferViewCB(renderer, camera);

    RenderGraph<kGBufferRenderGraphPassCount> rgGB(ctx.batchIndex);
    rgGB.AddPass(RenderPass::GBuffer_Driver, {},
        { { D.gb0.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
          { D.gb1.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
          { D.gb2.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
          { D.gbVelocity.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
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
    rgGB.AddPass(RenderPass::GBuffer_OpaqueSimple, {}, [this, renderer, &camera, &mainView, viewCB](RenderGraphPassContext sub) {
        const auto& visibleBuckets = mainView.queue.VisibleBuckets();
        const auto& opaqueSimple = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueSimple)];
        if (!opaqueSimple.empty())
        {
            RenderObjectBatch(renderer, opaqueSimple, sub.batchIndex, camera, /*useBundles=*/true, true, true, 32, viewCB);
        }
        });

    // 1.3 Opaque complex → direct command list, no clears
    rgGB.AddPass(RenderPass::GBuffer_OpaqueComplex, {}, [this, renderer, &camera, &mainView, viewCB](RenderGraphPassContext sub) {
        const auto& visibleBuckets = mainView.queue.VisibleBuckets();
        const auto& opaqueComplex = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::OpaqueComplex)];
        if (!opaqueComplex.empty())
        {
            RenderObjectBatch(renderer, opaqueComplex, sub.batchIndex, camera, /*useBundles=*/false, true, true, 32, viewCB);
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
        ctx.ApplyDeclaredStates(t.cl);

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

        ctx.ApplyDeclaredStates(t.cl);

        // RTVs = Light + Velocity, DSV = GBuffer Depth (read-only), no clears
        renderer->BindLightTargetWithVelocity(t.cl, Renderer::ClearMode::None, true);

        frame_->skybox->Render(renderer, t.cl, camera, 0); // skybox has no b1; viewCB ignored
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void SceneRenderer::Pass_SSR(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera)
{
    //return;
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSSR);
        const auto& D = renderer->GetDeferredForFrame();
        ctx.ApplyDeclaredStates(t.cl);

        auto ssrMaterial = resources_.GetSsrMaterial();
        const UINT cbSize = resources_.GetSsrCBSizeBytes();
        if (!ssrMaterial || cbSize == 0)
        {
            ctx.EndCL(t);
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
    uint32_t ssrUavIndex = 0; uint32_t geomInfoIndex = 0; uint32_t gb0Index = 0; uint32_t frameSeed = 0;
};

// Matches the `Denoise` cbuffer in rt_reflection_denoise_cs.hlsl.
struct RtDenoiseConstants
{
    uint32_t rawIndex = 0;
    uint32_t histPrevIndex = 0;
    uint32_t velocityIndex = 0;
    uint32_t ssrUavIndex = 0;
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
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassRTReflections);
        const auto& D = renderer->GetDeferredForFrame();
        ctx.ApplyDeclaredStates(t.cl); // depth/gb1/light -> NPS, ssr -> UAV

        auto reflectMaterial = resources_.GetRtReflectMaterial();
        const UINT frameIndex = renderer->GetCurrentFrameIndex();
        const D3D12_CPU_DESCRIPTOR_HANDLE tlasSrv = asManager_.TlasSrvCpu(frameIndex);
        if (!reflectMaterial || !bindless_.Ready() || tlasSrv.ptr == 0 ||
            asManager_.TlasInstanceCount(frameIndex) == 0 || !frame_->dirLight)
        {
            // No usable TLAS/bindless/light this frame: leave ssr as is (degenerate scene).
            ctx.EndCL(t);
            return;
        }

        // Per-frame scene descriptors into the bindless heap (geometry VB/IB +
        // geometry-info are persistent, populated in Pass_BuildAS). Scene slots
        // 0-5 are this pass's; the denoise pass uses 6-10 (distinct, so the two
        // passes never alias heap slots within a frame). The raw (jittered)
        // reflection is written to ssrBlur; the denoise pass produces ssr.
        bindless_.WriteSceneDescriptor(frameIndex, 0, tlasSrv);     // TLAS
        bindless_.WriteSceneDescriptor(frameIndex, 1, D.lightSRV);  // lit HDR (fast path)
        bindless_.WriteSceneDescriptor(frameIndex, 2, D.gbSRV[1]);  // GB1 (normal)
        bindless_.WriteSceneDescriptor(frameIndex, 3, D.depthSRV);  // Depth
        bindless_.WriteSceneDescriptor(frameIndex, 4, D.ssrBlurUAV);// raw reflection out (ssrBlur)
        bindless_.WriteSceneDescriptor(frameIndex, 5, D.gbSRV[0]);  // GB0 (rough/metal in .a)

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
        c.outWidth = renderer->GetSsrTextureWidth();
        c.outHeight = renderer->GetSsrTextureHeight();
        c.tlasIndex = bindless_.SceneIndex(frameIndex, 0);
        c.lightIndex = bindless_.SceneIndex(frameIndex, 1);
        c.gb1Index = bindless_.SceneIndex(frameIndex, 2);
        c.depthIndex = bindless_.SceneIndex(frameIndex, 3);
        c.ssrUavIndex = bindless_.SceneIndex(frameIndex, 4); // -> ssrBlur (raw)
        c.gb0Index = bindless_.SceneIndex(frameIndex, 5);
        c.geomInfoIndex = bindless_.GeomInfoIndex();
        c.frameSeed = static_cast<uint32_t>(renderer->GetTotalFrameNumber());

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
        renderer->UAVBarrier(t.cl, D.ssrBlur.Get()); // raw reflection -> consumed by the denoise pass

        // Restore the frame heap: this pass shares its command list with the
        // grouped denoise + blur + compose passes, which bind into the per-frame heap.
        renderer->BindDescriptorHeaps(t.cl);
    }
    ctx.EndCL(t);
}

void SceneRenderer::Pass_RTDenoise(Renderer* renderer, RenderGraphPassContext ctx)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassRTDenoise);
        const auto& D = renderer->GetDeferredForFrame();
        ctx.ApplyDeclaredStates(t.cl); // ssrBlur(raw)/velocity/histPrev -> NPS, ssr/histCurr -> UAV

        auto denoiseMaterial = resources_.GetRtDenoiseMaterial();
        const UINT frameIndex = renderer->GetCurrentFrameIndex();
        if (!denoiseMaterial || !bindless_.Ready() || !reflectionHistory_.Ready())
        {
            ctx.EndCL(t);
            return;
        }

        const uint64_t parity = renderer->GetTotalFrameNumber();
        // Scene slots 6-10 (distinct from the reflection pass's 0-5).
        bindless_.WriteSceneDescriptor(frameIndex, 6, D.ssrBlurSRV);                 // raw reflection (this frame)
        bindless_.WriteSceneDescriptor(frameIndex, 7, reflectionHistory_.PrevSrv(parity)); // accumulated (prev frame)
        bindless_.WriteSceneDescriptor(frameIndex, 8, D.gbSRV[3]);                   // gbVelocity (motion)
        bindless_.WriteSceneDescriptor(frameIndex, 9, D.ssrUAV);                     // denoised out -> blur/compose
        bindless_.WriteSceneDescriptor(frameIndex, 10, reflectionHistory_.CurrUav(parity)); // history (this frame)

        RtDenoiseConstants c{};
        c.rawIndex = bindless_.SceneIndex(frameIndex, 6);
        c.histPrevIndex = bindless_.SceneIndex(frameIndex, 7);
        c.velocityIndex = bindless_.SceneIndex(frameIndex, 8);
        c.ssrUavIndex = bindless_.SceneIndex(frameIndex, 9);
        c.histCurrUavIndex = bindless_.SceneIndex(frameIndex, 10);
        c.outWidth = renderer->GetSsrTextureWidth();
        c.outHeight = renderer->GetSsrTextureHeight();
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
        renderer->UAVBarrier(t.cl, D.ssr.Get());

        // Restore the frame heap for the grouped blur + compose passes.
        renderer->BindDescriptorHeaps(t.cl);
    }
    ctx.EndCL(t);
}

void SceneRenderer::Pass_ClearReflections(Renderer* renderer, RenderGraphPassContext ctx)
{
    // Reflection source = Off: zero the ssr target so the (unchanged) blur +
    // compose produce skybox-specular-only reflections. ssr is per-frame, so it
    // must be cleared every frame, not once.
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSSR);
        const auto& D = renderer->GetDeferredForFrame();
        ctx.ApplyDeclaredStates(t.cl); // ssr -> UNORDERED_ACCESS

        renderer->BindDescriptorHeaps(t.cl);
        const GpuDescHandle uav = renderer->StageSrvUavTable({ D.ssrUAV });
        const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        t.cl->ClearUnorderedAccessViewFloat(uav.gpu, D.ssrUAV, D.ssr.Get(), zero, 0, nullptr);
    }
    ctx.EndCL(t);
}

void SceneRenderer::Pass_SSR_Blur(Renderer* renderer, RenderGraphPassContext ctx)
{
    //return;
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSSRBlur);
        const auto& D = renderer->GetDeferredForFrame();
        const UINT ssrWidth = renderer->GetSsrTextureWidth();
        const UINT ssrHeight = renderer->GetSsrTextureHeight();

        // Horizontal pass (first-use states come from the pass declarations)
        ctx.ApplyDeclaredStates(t.cl);

        auto blurMaterial = resources_.GetBlurMaterial();
        const UINT cbSize = resources_.GetBlurCBSizeBytes();
        if (!blurMaterial || cbSize == 0)
        {
            ctx.EndCL(t);
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
    ctx.EndCL(t);
}

void SceneRenderer::Pass_Compose(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassCompose);
        const auto& D = renderer->GetDeferredForFrame();
        ctx.ApplyDeclaredStates(t.cl);

        const float width = static_cast<float>(renderer->GetRenderWidth());
        const float height = static_cast<float>(renderer->GetRenderHeight());
        if (width <= 0.0f || height <= 0.0f)
        {
            renderer->Transition(t.cl, D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            ctx.EndCL(t);
            return;
        }

        auto composeMaterial = resources_.GetComposeMaterial();
        const UINT cbSize = resources_.GetComposeCBSizeBytes();
        Skybox* skybox = frame_->skybox;
        if (!composeMaterial || cbSize == 0 || !skybox)
        {
            renderer->Transition(t.cl, D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            ctx.EndCL(t);
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
    uint32_t ssrUavIndex = 0;
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
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassRTDebug);
        const auto& D = renderer->GetDeferredForFrame();
        ctx.ApplyDeclaredStates(t.cl); // ssr -> UAV, gb1/depth -> NON_PIXEL_SHADER_RESOURCE

        auto debugMaterial = resources_.GetRtDebugMaterial();
        const UINT frameIndex = renderer->GetCurrentFrameIndex();
        const D3D12_CPU_DESCRIPTOR_HANDLE tlasSrv = asManager_.TlasSrvCpu(frameIndex);
        if (!debugMaterial || !bindless_.Ready() || tlasSrv.ptr == 0 ||
            asManager_.TlasInstanceCount(frameIndex) == 0)
        {
            ctx.EndCL(t);
            return; // no TLAS / bindless table this frame — leave ssr as compose left it
        }

        // Copy this frame's scene descriptors into the persistent bindless heap so
        // the shader can reach them via ResourceDescriptorHeap[]. (Geometry VB/IB +
        // geometry-info live in the heap persistently, populated in Pass_BuildAS.)
        // Scene slots 11-14 (distinct from the reflection 0-5 / denoise 6-10
        // ranges, so the debug pass never aliases their heap slots in a frame).
        bindless_.WriteSceneDescriptor(frameIndex, 11, tlasSrv);    // TLAS SRV
        bindless_.WriteSceneDescriptor(frameIndex, 12, D.gbSRV[1]); // GB1 (normal)
        bindless_.WriteSceneDescriptor(frameIndex, 13, D.depthSRV); // Depth
        bindless_.WriteSceneDescriptor(frameIndex, 14, D.ssrUAV);   // ssr UAV (output)

        RtDebugConstants c{};
        c.invView = camera.GetInvViewMatrix();
        c.invProj = camera.GetInvProjMatrix();
        c.tlasIndex = bindless_.SceneIndex(frameIndex, 11);
        c.gb1Index = bindless_.SceneIndex(frameIndex, 12);
        c.depthIndex = bindless_.SceneIndex(frameIndex, 13);
        c.ssrUavIndex = bindless_.SceneIndex(frameIndex, 14);
        c.geomInfoIndex = bindless_.GeomInfoIndex();
        c.outWidth = renderer->GetSsrTextureWidth();
        c.outHeight = renderer->GetSsrTextureHeight();

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
        renderer->UAVBarrier(t.cl, D.ssr.Get());

        // Leave ssr in the same frame-end state compose does, so the texture
        // inspector reads it exactly as it would the normal SSR result.
        renderer->Transition(t.cl, D.ssr.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    ctx.EndCL(t);
}

void SceneRenderer::Pass_Transparent(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera, const SceneView& mainView)
{
    // Shared per-view/per-frame CB (b1) for every transparent object in this pass.
    const D3D12_GPU_VIRTUAL_ADDRESS viewCB = BuildGlassViewCB(renderer, camera, *frame_);

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

    rgTr.AddPass(RenderPass::Transparent_Simple, {}, [this, renderer, &camera, &mainView, viewCB](RenderGraphPassContext sub) {
        const auto& visibleBuckets = mainView.queue.VisibleBuckets();
        const auto& transparentSimple = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::TransparentSimple)];
        if (!transparentSimple.empty())
        {
            RenderObjectBatch(renderer, transparentSimple, sub.batchIndex, camera, /*useBundles=*/true, false, true, 32, viewCB);
        }
        });

    rgTr.AddPass(RenderPass::Transparent_Complex, {}, [this, renderer, &camera, &mainView, viewCB](RenderGraphPassContext sub) {
        const auto& visibleBuckets = mainView.queue.VisibleBuckets();
        const auto& transparentComplex = visibleBuckets[BucketIndex(SceneRenderQueue::BucketType::TransparentComplex)];
        if (!transparentComplex.empty())
        {
            RenderObjectBatch(renderer, transparentComplex, sub.batchIndex, camera, /*useBundles=*/false, false, true, 32, viewCB);
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
        ctx.ApplyDeclaredStates(t.cl);
        renderer->BindSceneColor(t.cl, Renderer::ClearMode::None, true);

        debugDraw->Render(renderer, t.cl, camera.GetViewMatrix(), camera.GetProjMatrix());
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void SceneRenderer::Pass_Tonemap(Renderer* renderer, RenderGraphPassContext ctx)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
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
            ctx.EndCL(t);
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
            ctx.EndCL(t);
            return;
        }

        debugMaterial->Bind(t.cl, rc);
        t.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        t.cl->DrawInstanced(3, 1, 0, 0);
    }

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
