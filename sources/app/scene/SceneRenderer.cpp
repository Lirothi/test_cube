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
    uint64_t RtMaterialFingerprint(const GBufferRenderable& object)
    {
        // Slot 0 is the only material-parameter slot with a public mutable accessor; slots 1+
        // are immutable after Init and material/mesh hot reloads call InvalidateRaytracing().
        const MaterialParams& p = object.MaterialParamsRef();
        uint64_t h = 1469598103934665603ull;
        const auto mix = [&h](uint64_t value) { h ^= value; h *= 1099511628211ull; };
        const auto mixFloat = [&mix](float value)
        {
            uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            mix(bits);
        };
        mix(reinterpret_cast<uint64_t>(object.GetMaterialData()));
        mix(static_cast<uint64_t>(object.SlotCount()));
        mixFloat(p.baseColor.x); mixFloat(p.baseColor.y);
        mixFloat(p.baseColor.z); mixFloat(p.baseColor.w);
        mixFloat(p.metalRough.x); mixFloat(p.metalRough.y);
        mixFloat(p.mrMultiply);
        mixFloat(p.texFlags.y); // useMR controls whether the MR descriptor participates
        return h;
    }

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
        float4 vsmParams;             // x = useVsm, y = refDist, z = depth-bias floor, w = clip blend width
        float4 clipmapParams;         // Step 24f: x = baseExtent, y = normalBias (texels), z = depthBias (NDC)
        mat4   clipmapViewProj[8];    // Step 24f: directional clipmap level viewProjs
        mat4   clipmapUvNormal;       // P16.16: receiver-plane transform, mirrors lighting_cs
        float4 preExposureParams;     // P16.1: x = pre-exposure, yzw reserved
    };

    // MIRRORS the OceanReflectionCB in shaders/ocean_reflection_cs.hlsl. This one is uploaded by
    // raw memcpy rather than through named field handles, so the offsets have to agree: every
    // block below is laid out to land on the same 16-byte rows HLSL packs it into.
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
        // P13: the ocean plane traces with the same search the deferred SSR pass uses.
        uint32_t technique = 0u;
        uint32_t useHzb = 0u;
        uint32_t hzbMipCount = 1u;
        uint32_t frameIndexMod8 = 0u;
        float2 hzbSize{};
        float2 hzbInvSize{};
        float ueStartMipLevel = 0.0f;
        float ueSlopeCompareToleranceScale = 4.0f;
        uint32_t ueConfirmRetries = 0u;
        uint32_t ueRefineSteps = 0u;
        uint32_t ueNumSteps = 8u;
        uint32_t pad0 = 0u;
        uint32_t pad1 = 0u;
        uint32_t pad2 = 0u;
    };

    // A memcpy'd constant buffer has no field names to bind by, so nothing but this catches a
    // member added on one side only. 368 = 256 matrices + 48 scalars/vectors + 64 of P13 block.
    static_assert(sizeof(OceanReflectionConstants) == 368,
        "OceanReflectionConstants must stay byte-identical to OceanReflectionCB in "
        "shaders/ocean_reflection_cs.hlsl -- it is uploaded by raw memcpy.");

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
            // P4: the ocean and glass paths compute `sunColor.xyz * sunColor.w`, so handing them
            // the effective colour with a retired 1.0 multiplier is the same product they had.
            // The fill reaches only `LitFoamColor` in the ocean shaders and the glass tint -- the
            // water surface itself has no sky fill at all. See the note in DirectionalLight.h.
            vc.sunDirAmbient = float4(dirLight.GetDirection(), dirLight.GetAmbient());
            vc.sunColorExposure = float4(dirLight.GetEffectiveColor(), dirLight.GetExposure());
        }

        float3 camDir = camera.GetDirection();
        if (camDir.Length() > Math::EPS) { camDir = camDir.Normalized(); }
        else { camDir = float3(0.0f, 0.0f, 1.0f); }
        // P5: w carries the prefiltered sky's mip count so glass can use the shared
        // roughness <-> mip mapping. 0 means this sky has no derivatives and glass keeps its
        // old guess. The slot was documented as unused.
        vc.camDirWS = float4(camDir,
            (frame.skybox && frame.skybox->HasIbl()) ? (float)frame.skybox->GetSpecMips() : 0.0f);

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
        // .z = the depth-bias floor, texels -> NDC (same conversion as the lighting CB: a level's
        // depth range is 6x its extent, virtual res 2048, both scale with the extent).
        vc.vsmParams = float4(vsmOn ? 1.0f : 0.0f, vsm::g_refDist,
                              vsm::g_clipmapDepthBiasFloorTexels / (6.0f * (float)vsm::kVirtualRes),
                              vsm::ClipmapBlendWidth());
        // Step 24f: directional clipmap for glass (matches lighting_cs). Same tunables + level viewProjs.
        // .y carries the SAME scaled value the lighting CB gets (UE divide by 1000 on the CPU);
        // .w = the per-level depth-bias decay (see VsmClipmapShadow).
        vc.clipmapParams = float4(vsm::g_clipmapBaseExtent, vsm::g_clipmapNormalBias * 0.001f,
                                  vsm::g_clipmapDepthBias, vsm::g_clipmapDepthBiasDecay);
        if (frame.clipmapViews)
        {
            for (size_t i = 0; i < 8 && i < frame.clipmapViews->size(); ++i)
            {
                const SceneView& cv = (*frame.clipmapViews)[i];
                vc.clipmapViewProj[i] = cv.view * cv.proj;
            }
            // P16.16: built from level 0; the gradient it feeds is a ratio in which the level's
            // extent cancels, so one matrix serves them all. Same construction as UE's
            // CalcTranslatedWorldToShadowUVNormalMatrix.
            vc.clipmapUvNormal = vsm::CalcClipmapUvNormalMatrix(vc.clipmapViewProj[0]);
        }

        // P16.1: glass draws in the transparent pass, after compose, so compose's scaling never
        // reaches it and it applies the factor itself -- and its refraction tap reads the opaque
        // scene copy, which already has the factor on it. Both halves live in glass.hlsl.
        vc.preExposureParams = float4(render::g_preExposure, 0.0f, 0.0f, 0.0f);

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
    rtBindlessObjectCache_.clear();
    ssrTemporalActive_ = false;
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
    asManager_.Reset();
    bindless_.Reset();
    reflectionHistory_.Reset();
    asManagerInited_ = false;
    asScratchRetireFrame_ = 0;
    rtInstances_.clear();
    rtBindlessObjectCache_.clear();
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
    // S13: once an AS/table allocation has failed (low VRAM / descriptor exhaustion), disable RT for
    // the rest of this scene and fall back to SSR — cleanly, never a crash. Sticky
    // until the next level (asManager_.Reset clears it).
    const bool rtFailed = asManager_.BuildFailed() || bindless_.BuildFailed();
    if (rtFailed && !rtFailureLogged_)
    {
        OutputDebugStringA("[RT] Acceleration-structure or bindless-table allocation failed; disabling RT, "
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
    // NOTHING READS THE CLOSEST PYRAMID ANY MORE, so it is not built. The stackless HiZ traversal
    // was its only consumer and it is gone: Unreal's own SSR marches the FURTHEST chain (the one
    // GTAO already builds) at a fixed mip, and a `max`-reduced chain answers a question no pass
    // now asks. The target, its descriptors and the shader's `writeClosest` path all stay --
    // P9's screen-space GI is the next consumer and wants exactly this chain.
    ssrHizActive_ = false;
    // P8 bloom. Gated on everything the body needs, not just the setting: the material and its CB
    // have to exist, the pyramid has to have been created, and a zero intensity is the plan's
    // "schedules no unnecessary active work" -- with it off nothing is dispatched and the tonemap
    // reads a literal 0 for the term.
    {
        const auto& DB = renderer->GetDeferredForFrame();
        // The material EXISTING is not the same as it being usable: Material::CreateCompute keeps
        // the object and leaves the PSO null when the shader fails to build, so a null-check alone
        // dispatches with no pipeline state. That cost a debug session -- the shader was missing its
        // [RootSignature] attribute, which dxc compiles happily and check_shaders therefore passed.
        // P16.1 -- PRE-EXPOSURE, decided here, once, for the same reason `bloomConvolution_` is:
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

        const auto bloomMaterial = resources_.GetBloomMaterial();
        // P8C: which method runs is decided HERE, once, for the same reason the gate itself is --
        // the Prepare declares a different set of resources for each, and a body that disagreed
        // with it would emit a barrier the compile never registered.
        const auto fftMaterial = resources_.GetBloomFftMaterial();
        const auto convMaterial = resources_.GetBloomConvMaterial();
        // P8C-2: the photographed kernel, loaded lazily HERE so the gate below can see the
        // answer. Without it the convolution REFUSES to enable -- UE's own gate
        // (IsFFTBloomPhysicalKernelReady) -- because there is no procedural kernel to fall
        // back to any more.
        // P8C-2r: the kernel is an ASSET CHOICE now, so the gate is the PATH rather than a
        // once-only flag -- pick another image in the inspector and it is reloaded here, kernel
        // spectrum and centre/scatter survey rebuilt from the new pixels on the next frame.
        // Still lazy: nothing is read until the convolution method actually asks for it.
        if (frame.settings.bloom.method == 1u &&
            frame.settings.bloom.convKernel != bloomKernelLoadedPath_)
        {
            const std::string& want = frame.settings.bloom.convKernel;
            bloomKernelLoadedPath_ = want;
            bloomKernelReady_ = false;
            bloomKernelPixels_.clear();
            bloomKernelPixelDim_ = 0u;
            bloomSurveyRatio_ = -1.0f;
            // Every frame slot's kernel spectrum was built from the OLD image; forget the keys or
            // two frames in three would convolve against it until the size happened to change.
            bloomKernelKeys_ = {};
            if (!want.empty())
            {
                renderer->WaitForPreviousFrame();
                UploadBatch up;
                if (up.Begin(renderer))
                {
                    Texture2D::CreateDesc desc;
                    desc.path = std::wstring(want.begin(), want.end());
                    desc.usage = Texture2D::Usage::LinearData;
                    bloomKernelReady_ = bloomKernelTex_.CreateFromFile(
                        renderer, up.CommandList(), desc, up.KeepAlive());
                    // P8C-2o: the same file again, on the CPU, for the centre/scatter survey. A
                    // failure here is not a failure to bloom -- the split falls back to neutral.
                    if (bloomKernelReady_) { Bloom_ReadKernelPixels(desc.path.c_str()); }
                    up.SubmitAndWait(renderer);
                }
            }
        }
        // P8C-2 step 5a / P8C-2d: the ghost bokeh sprite, (re)baked when the blade count moves.
        // The retire half runs unconditionally so a pending upload is always collected, even if
        // the ghosts were switched off in the meantime; only the START is gated, because a bake
        // for a sprite nothing samples is pure cost (it used to run on `method == 1` alone).
        {
            const std::uint64_t nowFrame = renderer->GetTotalFrameNumber();
            if (flareBokehPending_ >= 0 && nowFrame >= flareBokehSafeFrame_)
            {
                flareBokehSlot_ = static_cast<std::uint32_t>(flareBokehPending_);
                flareBokehPending_ = -1;
                flareBokehUpload_.reset(); // intermediates + allocator, now provably consumed
                flareBokehReady_ = true;
                // The slot that just stopped being sampled needs its own ring before the next
                // bake may overwrite it.
                flareBokehSafeFrame_ = nowFrame + render::kFrameCount + 1u;
            }
            // P8C-2m: the flare gates for the whole frame. Zero intensity is a REAL off switch,
            // not a multiply by zero: nothing is dispatched, nothing is drawn, and the Prepare
            // below declares none of the targets, so not even a barrier is emitted.
            const bool ghostsWanted = frame.settings.bloom.convGhosts > 0u &&
                                      frame.settings.bloom.convGhostIntensity > 0.0f;
            // P8C-5: THE GATE MUST ASK WHETHER THE SCATTER CAN ACTUALLY DRAW. It checked only that
            // the target existed, so a lens-flare material with a NULL PIPELINE reported ghosts as
            // ON, the pass issued its clear and its DrawInstanced, every downstream gate agreed --
            // and the flare target came out exactly zero, because `Bind` on a material without a
            // PSO binds nothing and the draw is dropped. "Compiles is not loads": dxc accepting the
            // shader is not the engine building the pipeline. Silent for a whole session.
            auto flareMat = resources_.GetLensFlareMaterial();
            const bool flareReady = flareMat != nullptr &&
                                    flareMat->GetPipelineState() != nullptr &&
                                    resources_.GetLensFlareCBSizeBytes() > 0u;
            if (ghostsWanted && !flareReady)
            {
                static bool warned = false;
                if (!warned)
                {
                    warned = true;
                    FILE* lg = nullptr;
                    if (fopen_s(&lg, diag::LogPath("bloom_kernel.log").c_str(), "a") == 0 && lg)
                    {
                        std::fprintf(lg, "[ghost] DISABLED: lens flare material=%p pso=%p cb=%u\n",
                                     (void*)flareMat.get(),
                                     flareMat ? (void*)flareMat->GetPipelineState() : nullptr,
                                     resources_.GetLensFlareCBSizeBytes());
                        std::fclose(lg);
                    }
                }
            }
            flaresGhosts_ = frame.settings.bloom.enabled && ghostsWanted && flareReady &&
                            frame.settings.bloom.intensity > 0.0f &&
                            DB.lensFlare.Get() != nullptr;
            flaresStreak_ = frame.settings.bloom.enabled &&
                            frame.settings.bloom.convAnamorphicIntensity > 0.0f &&
                            frame.settings.bloom.intensity > 0.0f &&
                            DB.streakA.Get() != nullptr;
            if (frame.settings.bloom.method == 1u && ghostsWanted &&
                flareBokehPending_ < 0 && flareBokehBlades_ != frame.settings.bloom.convBlades &&
                nowFrame >= flareBokehSafeFrame_)
            {
                BakeFlareBokeh(renderer, frame.settings.bloom.convBlades);
            }
        }
        bloomConvolution_ = frame.settings.bloom.method == 1u &&
                            bloomKernelReady_ &&
                            fftMaterial != nullptr && fftMaterial->GetPipelineState() != nullptr &&
                            convMaterial != nullptr && convMaterial->GetPipelineState() != nullptr &&
                            resources_.GetBloomFftCBSizeBytes() > 0u &&
                            resources_.GetBloomConvCBSizeBytes() > 0u &&
                            DB.bloomFftA.Get() != nullptr && DB.bloomFftB.Get() != nullptr &&
                            DB.bloomFftKernel.Get() != nullptr &&
                            DB.lensFlare.Get() != nullptr;
        bloomActive_ = frame.settings.bloom.enabled &&
                       frame.settings.bloom.intensity > 0.0f &&
                       (bloomConvolution_ ||
                        (bloomMaterial != nullptr &&
                         bloomMaterial->GetPipelineState() != nullptr)) &&
                       resources_.GetBloomCBSizeBytes() > 0u &&
                       DB.bloomMips > 0u && DB.bloomDown.Get() != nullptr &&
                       DB.bloomUp.Get() != nullptr;
    }
    // SSR TEMPORAL RESOLVE. Only for the screen-space source: RT traces the TLAS and does not have
    // this instability, and None/SkyOnly dispatch nothing to filter. The history is per-frame-set
    // like the GTAO one, so it is only valid once a previous frame at THIS reflection size has
    // written it -- a resize or a level switch has to seed instead of reading garbage.
    ssrTemporalActive_ = frame.settings.ssrTemporal && !clearReflections && !rtReflect;
    {
        const UINT rw = renderer->GetReflectionTextureWidth();
        const UINT rh = renderer->GetReflectionTextureHeight();
        ssrHistoryValid_ = ssrTemporalActive_ && ssrHistoryFrames_ > 0u &&
                           ssrHistoryWidth_ == rw && ssrHistoryHeight_ == rh;
        ssrHistoryFrames_ = ssrTemporalActive_
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
    const auto& P = renderer->GetDeferredForPrevFrame();
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
        // pass-flow S5: AddPass2, though this pass declares NOTHING — measured: it performs no
        // transitions (the AS build bypasses the barrier compile entirely). The builder is the
        // whole registration, so there is no second lambda that could start declaring behind the
        // body's back.
        pBuildAS = rg.AddPass2(RenderPass::Main_BuildAS, {},
            [this, renderer](RenderGraphPassContext&) -> std::function<void(RenderGraphPassContext)> {
                return [this, renderer](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassBuildAS);
                    Pass_BuildAS(renderer, c);
                };
            });
    }

    // CL group (step 5): the prologue clear and the object-compute dispatches are
    // two tiny back-to-back lists with no mtDeps; share one command list.
    rg.BeginCLGroup();
    // pass-flow S5: declares nothing (measured: the prologue clear performs no transitions), so
    // the builder just hands back the body.
    auto pClear = rg.AddPass2(RenderPass::Main_PrologueClear, {},
        [this, renderer](RenderGraphPassContext&) -> std::function<void(RenderGraphPassContext)> {
            return [this, renderer](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassPrologueClear);
                Pass_PrologueClear(renderer, c);
            };
        });

    // pass-flow S7b: the builder walks the scene ONCE and collects the objects whose compute will
    // actually record; the body runs that list instead of re-walking and re-filtering. The two
    // walks agreeing was previously a matter of the two filters staying identical by hand.
    auto pCompute = rg.AddPass2(RenderPass::Main_ObjectCompute, { pClear },
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            if (!frame_->objects) { return {}; }
            ObjectComputeList list;
            for (const auto& obj : *frame_->objects)
            {
                if (!obj) { continue; }
                if (!obj->PrepareCompute(ctx)) { continue; }
                if (list.size() >= list.capacity())
                {
                    // The cap is a budget, not a guess: it must not silently drop work.
                    RendererInvariantFailure("Main_ObjectCompute: compute-object list overflow "
                                             "(raise kMaxComputeObjects)");
                    break;
                }
                list.push_back(obj.get());
            }
            if (list.empty()) { return {}; }
            return [this, renderer, list](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassObjectCompute);
                Pass_ObjectCompute(renderer, c, list);
            };
        });
    // surf sim injection (pass-flow S3 pilot): the surf sim as its OWN pass, authored with
    // AddPass2 — the builder makes the frame's decisions, declares from them and returns the
    // record lambda; there is no separate Prepare to mirror. Third member of the compute CL
    // group, so it records into the same command list right after the FFT dispatches.
    const size_t pSurfSim = rg.AddPass2(RenderPass::Main_SurfSim, { pCompute },
        [this](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            if (!frame_->ocean) { return {}; }
            return frame_->ocean->BuildSurfSimPass(ctx);
        });
    const size_t pWetness = rg.AddPass2(RenderPass::Main_ShoreWetness, { pSurfSim },
        [this](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            if (!frame_->ocean) { return {}; }
            return frame_->ocean->BuildWetnessPass(ctx);
        });
    rg.EndCLGroup();

    // pass-flow S6: the two OceanSimulation flags were read by the Prepare and read AGAIN by the
    // body; now the builder decides both once, validates every target AND its DSV (the body's
    // renderCascade used to skip on a null DSV, after the pass had declared for it), and commits
    // the cross-frame `shoreSdfDirty_` clear that used to happen mid-record.
    //
    // ORDERING NOTE for that clear: `OceanRenderable::BuildSurfSimPass` reads the same two flags
    // to keep the surf sim off a frame that rebuilds the shore maps. Its pass (Main_SurfSim) sits
    // earlier in the schedule, so its builder runs BEFORE this one — the flag it sees is the same
    // one it saw when the clear lived in the record body.
    const size_t pShoreDepth = rg.AddPass2(RenderPass::Main_TerrainDepth, { pCompute },
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            OceanSimulation* oceanSim = Systems::GetOceanSimulation();
            if (!oceanSim) { return {}; }
            ShoreDepthPoints pts{};
            ID3D12Resource* const shoreDepth = oceanSim->GetShoreDepthResource();
            pts.drawDepth = oceanSim->ShouldRenderShoreDepth() && shoreDepth != nullptr &&
                            oceanSim->GetShoreDepthDsv().ptr != 0;
            ID3D12Resource* const sdfSource = oceanSim->GetShoreSdfSourceResource();
            ID3D12Resource* const scratch = oceanSim->GetShoreSdfScratchResource();
            ID3D12Resource* const sdf = oceanSim->GetShoreSdfResource();
            pts.buildSdf = oceanSim->ShouldBuildShoreSdf() && sdfSource != nullptr &&
                           scratch != nullptr && sdf != nullptr &&
                           oceanSim->GetShoreSdfSourceDsv().ptr != 0 &&
                           oceanSim->CanBuildShoreSdf();
            if (!pts.drawDepth && !pts.buildSdf) { return {}; }

            // The map is static now, so the depth window is re-rendered once per level rather
            // than every time the camera crosses a snap step.
            bool opened = false;
            if (pts.drawDepth)
            {
                pts.depthWrite = ctx.usePoint ? *ctx.usePoint : 0u;
                ctx.Use(shoreDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                ctx.NextPoint();
                pts.depthRead = ctx.usePoint ? *ctx.usePoint : 0u;
                ctx.Use(shoreDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                opened = true;
            }
            if (pts.buildSdf)
            {
                // Its own point rather than riding the depth window's read point: the two
                // cascades draw between them, and a point is emitted wholesale at its first
                // match.
                if (opened) { ctx.NextPoint(); }
                pts.sdfWrite = ctx.usePoint ? *ctx.usePoint : 0u;
                ctx.Use(sdfSource, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                ctx.NextPoint();
                pts.sdfRead = ctx.usePoint ? *ctx.usePoint : 0u;
                ctx.Use(sdfSource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                // The flood's ping-pong pair, taken at the same point the source becomes
                // readable — which is where BuildShoreSdf's own two UAV requests land.
                ctx.Use(scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                ctx.Use(sdf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                // ...and the flood's closing transition, which it still performs by name.
                ctx.NextPoint();
                ctx.Use(sdf, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                // Cross-frame state, committed in the builder: the record body used to clear it
                // AFTER BuildShoreSdf, including on the runs where the flood bailed out.
                oceanSim->MarkShoreSdfBuilt();
            }
            const SceneView* const shoreView = &oceanSim->GetShoreDepthView();
            return [this, renderer, shoreView, pts](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassShoreDepth);
                Pass_ShoreDepth(renderer, c, shoreView, pts);
            };
        });

    // Rung 0 / Step 4: GPU cull -> indirect shadow args, before the shadow passes (its output
    // is not consumed yet). Manages its own UAV states (declares none). Placed in the chain so
    // Step 6's ExecuteIndirect can consume it.
    // pass-flow S7a: PrepareCullPass RETURNS this frame's decisions (and the points it declared
    // them under); RecordCull takes them as a parameter. The two `Will*` predicates that existed
    // only to keep a Prepare and a Record from drifting are deleted.
    auto pShadowCull = rg.AddPass2(RenderPass::Main_ShadowCull, { pShoreDepth },
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            if (!frame_->shadowGpu) { return {}; }
            const ShadowGpuData::CullDecisions dec = frame_->shadowGpu->PrepareCullPass(ctx);
            if (!dec.active) { return {}; }
            return [this, renderer, dec](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassShadowCull);
                Pass_ShadowCull(renderer, c, dec);
            };
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
        // pass-flow S7c: the builder decides `indirect` ONCE and both the registration walk and
        // the per-cascade draw workers get that same value. It selects which objects draw at all
        // (GPU-instanced casters go through the cull when GI folding is active), so the two sides
        // disagreeing means a caster transitioning its instance buffer with nothing declared.
        pShadow = rg.AddPass2(RenderPass::Main_CSM, { pShadowCull }, /*mtDeps=*/{},
            { { D.shadow.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE } },
            [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                if (!frame_->cascadeViews) { return {}; }
                ctx.UseDeclared(); // the CSM atlas -> DEPTH_WRITE
                const std::uint32_t atlasPoint = ctx.usePoint ? *ctx.usePoint : 0u;
                const bool indirect = IndirectShadowDrawsActive();
                ctx.NextPoint();
                PrepareOpaqueDrawStates(ctx, frame_->cascadeViews->data(),
                                        frame_->cascadeViews->size(), indirect);
                return [this, renderer, atlasPoint, indirect](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassCSM);
                    Pass_CSM(renderer, c, *frame_->cascadeViews, atlasPoint, indirect);
                };
            });
    }

    // No declarations: the per-light command lists are recorded in parallel with
    // no deterministic submit order inside the batch, so each list must register
    // the atlas state itself (first-use in whichever list lands first).
    // pass-flow S6: the VSM skip and the "only the ACTIVE views" clamp were computed here AND
    // recomputed by the body; now the builder decides `n` once and it rides into the record as a
    // capture. The clamp itself still matters for the same reason it always did: the view arrays
    // are fixed-size and their tail entries keep queues from earlier frames, whose object
    // pointers a level switch has already freed — reading past it crashed the stress harness
    // inside the very first SwitchLevel.
    auto pSpotShadow = rg.AddPass2(RenderPass::Main_SpotShadows, { pShadow },
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            if (render::VsmActive()) { return {}; } // the spot atlas is a 1x1 placeholder there
            if (!frame_->spotShadowViews) { return {}; }
            const size_t n = std::min(frame_->spotShadowViews->size(),
                                      frame_->lightManager->GetShadowedSpotCount());
            if (n == 0) { return {}; }
            // One registration of the atlas state covers every per-light list: they all ask for
            // the same state, and only the first light's list actually transitions it.
            const std::uint32_t atlasPoint = ctx.usePoint ? *ctx.usePoint : 0u;
            const bool indirect = IndirectShadowDrawsActive(); // S7c: one decision for both sides
            ctx.Use(ctx.renderer->GetDeferredForFrame().spotShadow.Get(),
                    D3D12_RESOURCE_STATE_DEPTH_WRITE);
            ctx.NextPoint();
            PrepareOpaqueDrawStates(ctx, frame_->spotShadowViews->data(), n, indirect);
            return [this, renderer, n, atlasPoint, indirect](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassSpotShadow);
                Pass_SpotShadows(renderer, c, *frame_->spotShadowViews, n, atlasPoint, indirect);
            };
        });

    // B2b: point cube shadows. Same per-CL atlas-state registration story as spot
    // shadows (parallel per-face lists, no declared states). Runs before Pass_PointLights
    // (which samples the cube atlas in B3).
    auto pPointShadow = rg.AddPass2(RenderPass::Main_PointShadows, { pSpotShadow },
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            if (render::VsmActive()) { return {}; } // same skip as the spot pass
            if (!frame_->pointShadowViews) { return {}; }
            const size_t n = std::min(frame_->pointShadowViews->size(),
                                      frame_->lightManager->GetShadowedPointCount() * 6);
            if (n == 0) { return {}; }
            const std::uint32_t atlasPoint = ctx.usePoint ? *ctx.usePoint : 0u;
            const bool indirect = IndirectShadowDrawsActive(); // S7c: one decision for both sides
            ctx.Use(ctx.renderer->GetDeferredForFrame().pointShadow.Get(),
                    D3D12_RESOURCE_STATE_DEPTH_WRITE);
            ctx.NextPoint();
            PrepareOpaqueDrawStates(ctx, frame_->pointShadowViews->data(), n, indirect);
            return [this, renderer, n, atlasPoint, indirect](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassPointShadow);
                Pass_PointShadows(renderer, c, *frame_->pointShadowViews, n, atlasPoint, indirect);
            };
        });

    // pass-flow S7d: the G-buffer's target states used to be written TWICE — here, and again as
    // the inner graph's driver `declares` — with a comment asking the two lists to stay in step.
    // Now there is ONE list, and the inner driver emits the point this builder declared instead of
    // applying a declaration of its own.
    auto pGbuf = rg.AddPass2(RenderPass::Main_GBuffer, { pPointShadow },
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
        RenderGraphPassContext& p = ctx;
        const std::uint32_t bindPoint = p.usePoint ? *p.usePoint : 0u;
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
        // The G-buffer never draws through the indirect shadow path, so its walk registers every
        // visible opaque object.
        if (frame_->mainView) { PrepareOpaqueDrawStates(p, frame_->mainView, 1, /*indirect=*/false); }
        return [this, renderer, bindPoint](RenderGraphPassContext c) {
            CPU_SCOPE(ProfilerScopes::kPassGBuffer);
            Pass_GBuffer(renderer, c, *frame_->camera, *frame_->mainView, bindPoint);
        };
    });

    // Rung 2 / Step 19: VSM page-request pass — reads the camera depth (after GBuffer), marks the
    // virtual pages the frame needs. Independent consumer of depth (its output is unused for now),
    // so it doesn't gate lighting. Manages the request-buffer UAV state itself.
    // pass-flow S3c: authored with AddPass2 — one gate decides declarations and record, and the
    // barrier-point indices travel as a by-value capture. The depth read is registered by the
    // builder itself (NOT via the declare list): this pass runs right after the G-buffer, which
    // leaves depth in DEPTH_WRITE, and reading it without the graph transitioning it was GBV
    // id=1358 on all three Deferred[N].Depth.
    auto pVsmPageRequest = rg.AddPass2(RenderPass::Main_VsmPageRequest, { pGbuf },
        [this, renderer](RenderGraphPassContext& ctx)
            -> std::function<void(RenderGraphPassContext)> {
            // `vsmSkipUpdate_` is decided before the graph is built and does not change during
            // the frame, so this gate is exact.
            if (!render::VsmActive() || vsmSkipUpdate_) { return {}; }
            if (!frame_->vsm || !frame_->vsm->IsAllocated()) { return {}; }
            ctx.Use(ctx.renderer->GetDeferredForFrame().depth.Get(), kSrvAll);
            // VSM owns the buffers its Record* functions barrier, so it declares them itself.
            const VirtualShadowMap::PageRequestPoints pts =
                frame_->vsm->PrepareRequestPass(ctx);
            return [this, renderer, pts](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassVsmPageRequest);
                Pass_VsmPageRequest(renderer, c, pts);
            };
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
        // pass-flow S3: authored with AddPass2 — ONE gate decides both the declarations and the
        // record, and the PageRenderDecisions travel as a by-value lambda capture instead of a
        // class-member bridge Prepare and Record could disagree over.
        pVsmPageRender = rg.AddPass2(RenderPass::Main_VsmPageRender, { pVsmPageRequest },
            [this, renderer](RenderGraphPassContext& ctx)
                -> std::function<void(RenderGraphPassContext)> {
                if (vsmSkipUpdate_ || !frame_->shadowGpu) { return {}; }
                const VirtualShadowMap::PageRenderDecisions dec =
                    frame_->vsm->PrepareRenderPass(ctx, frame_->shadowGpu, frame_->wind);
                return [this, renderer, dec](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassVsmPageRender);
                    Pass_VsmPageRender(renderer, c, dec);
                };
            });
    }
    (void)pVsmPageRender;

    // P6B: AO reads the G-buffer normal and depth and writes its own half-res target, so it only
    // has to order after the G-buffer. Lighting and compose consume it, so they order after this.
    // Skipped entirely when disabled -- an unregistered pass costs nothing, whereas a registered
    // one still pays its barriers.
    // AddPass2: the builder makes the decision ONCE and declares from it, so a disabled frame
    // declares nothing and the body is empty -- no separate Prepare to keep in sync, which is the
    // whole point of the form.
    // P6C: the depth pyramid. Ordered after the G-buffer (it reduces the depth buffer) and before
    // anything that would consume it -- GTAO's horizon search (step 5) and SSR's HiZ march (step 6).
    const size_t pHzb = rg.AddPass2(RenderPass::Main_Hzb, { pGbuf },
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            const auto& D = renderer->GetDeferredForFrame();
            if (!resources_.GetHzbMaterial() || resources_.GetHzbCBSizeBytes() == 0u ||
                D.depthSRV.ptr == 0 || D.hzb.Get() == nullptr || D.hzbClosest.Get() == nullptr ||
                D.hzbMips == 0)
            {
                return {};
            }
            ctx.NextPoint();
            const uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(D.depth.Get(), kSrvAll);
            // The whole chain in ONE state for the whole build; the mips are separated by UAV
            // barriers, not transitions. See the shader header for why.
            ctx.Use(D.hzb.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            // The closest chain is declared UNCONDITIONALLY, even on frames the shader does not
            // write it. Its descriptors sit in the same VOLATILE table either way, and a barrier
            // set that changes with a UI setting is exactly the kind of thing that compiles fine
            // and then breaks the one configuration nobody screenshots.
            ctx.Use(D.hzbClosest.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            // ...and back to their resting state, which is what GTAO, SSR and the inspector read.
            ctx.NextPoint();
            ctx.Use(D.hzb.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ctx.Use(D.hzbClosest.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            return [this, renderer, point](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassHzb);
                Pass_Hzb(renderer, c, point);
            };
        });

    // P6C: the horizon search reads the pyramid, so GTAO orders after the build.
    const size_t pGtao = rg.AddPass2(RenderPass::Main_Gtao, { pGbuf, pHzb },
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            const GtaoSettings& s = frame_->settings.gtao;
            const auto& D = renderer->GetDeferredForFrame();
            const auto& P = renderer->GetDeferredForPrevFrame();
            // Every material and every handle the chain needs, checked HERE rather than in the
            // body: a body that early-outs after the declarations have been made loses the pass's
            // barriers silently. Deciding once is the whole reason this is an AddPass2.
            const bool ready = s.enabled && resources_.GetGtaoMaterial() &&
                resources_.GetGtaoFilterMaterial() && resources_.GetGtaoTemporalMaterial() &&
                resources_.GetGtaoUpsampleMaterial() && resources_.GetGtaoCBSizeBytes() != 0u &&
                resources_.GetGtaoFilterCBSizeBytes() != 0u &&
                resources_.GetGtaoTemporalCBSizeBytes() != 0u &&
                resources_.GetGtaoUpsampleCBSizeBytes() != 0u &&
                D.depthSRV.ptr != 0 && D.gbSRV[1].ptr != 0 && D.gbSRV[3].ptr != 0 &&
                D.gtaoUAV.ptr != 0 && D.gtaoFilteredUAV.ptr != 0 &&
                D.gtaoHistoryUAV.ptr != 0 && D.gtaoUpsampledUAV.ptr != 0 &&
                P.gtaoHistorySRV.ptr != 0;
            if (!ready)
            {
                // Nothing declared, nothing recorded — and the history counter resets, so the
                // temporal stage re-seeds instead of reading a frame that was never written.
                gtaoHistoryFrames_ = 0u;
                return {};
            }

            constexpr D3D12_RESOURCE_STATES kAoRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            GtaoChain chain{};
            chain.denoise = s.denoise;
            chain.temporal = s.temporal;
            chain.frameIndex = gtaoFrameCounter_++;

            // Cross-frame state is committed HERE, in the serial Prepare, not in the body.
            const uint32_t aoW = std::max(1u, (renderer->GetRenderWidth() + 1u) / 2u);
            const uint32_t aoH = std::max(1u, (renderer->GetRenderHeight() + 1u) / 2u);
            if (aoW != gtaoHistoryWidth_ || aoH != gtaoHistoryHeight_)
            {
                gtaoHistoryWidth_ = aoW;
                gtaoHistoryHeight_ = aoH;
                gtaoHistoryFrames_ = 0u; // the whole Deferred set was recreated: no valid history
            }
            chain.historyValid = chain.temporal && gtaoHistoryFrames_ > 0u;
            gtaoHistoryFrames_ = chain.temporal ? std::min(gtaoHistoryFrames_ + 1u, 4u) : 0u;

            // --- declarations, from the same decision ---
            // The chain's own targets are declared NON_PIXEL only: every reader of them is a
            // compute shader, here and in the consumers P6B item 7 will add. Declaring kSrvAll
            // instead would leave them one state away from their canonical at frame end for no
            // reader's benefit — which --canonical-check duly reported. The G-buffer inputs keep
            // kSrvAll because the later forward passes really do sample them from a pixel shader.
            ctx.NextPoint();
            chain.pointRaw = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(D.gb1.Get(), kSrvAll);
            ctx.Use(D.depth.Get(), kSrvAll);
            // The pyramid rests in this state, so this normally compiles to no barrier -- declaring
            // it is what makes that a fact the compile knows rather than an assumption.
            ctx.Use(D.hzb.Get(), kAoRead);
            ctx.Use(D.gtao.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            // Each stage reads what the previous one wrote, so every stage boundary is a
            // UAV -> SRV flip on one target and a fresh UAV on the next. The source chain below
            // has to match the record body's exactly; both read it off `chain`.
            if (chain.denoise)
            {
                ctx.NextPoint();
                chain.pointDenoise = ctx.usePoint ? *ctx.usePoint : 0u;
                ctx.Use(D.gtao.Get(), kAoRead);
                ctx.Use(D.gtaoFiltered.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            if (chain.temporal)
            {
                ctx.NextPoint();
                chain.pointTemporal = ctx.usePoint ? *ctx.usePoint : 0u;
                ctx.Use(chain.denoise ? D.gtaoFiltered.Get() : D.gtao.Get(), kAoRead);
                // The previous frame's accumulation. It rests shader-readable (the upsample read
                // it last frame), so this usually compiles to no barrier at all — but declaring it
                // is what makes that a fact the compile knows rather than an assumption.
                ctx.Use(P.gtaoHistory.Get(), kAoRead);
                ctx.Use(D.gbVelocity.Get(), kSrvAll);
                ctx.Use(D.gtaoHistory.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            ctx.NextPoint();
            chain.pointUpsample = ctx.usePoint ? *ctx.usePoint : 0u;
            ID3D12Resource* const upsampleSrc = chain.temporal ? D.gtaoHistory.Get()
                : (chain.denoise ? D.gtaoFiltered.Get() : D.gtao.Get());
            ctx.Use(upsampleSrc, kAoRead);
            ctx.Use(D.gtaoUpsampled.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            // ...and back to its resting state. Every other target in this chain ends the frame
            // shader-readable because the next stage reads it; the last one has no next stage, so
            // it says so explicitly instead of resting as a UAV.
            ctx.NextPoint();
            chain.pointRestore = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(D.gtaoUpsampled.Get(), kAoRead);

            return [this, renderer, chain](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassGtao);
                Pass_Gtao(renderer, c, *frame_->camera, chain);
            };
        });

    // pass-flow S5: ONE builder shared by both variants below (VSM vs Legacy declare different
    // first-use sets, but the readiness decision and the record are identical). It owns the
    // gate Pass_Lighting used to repeat AFTER declaring — a missing material, a zero CB size or a
    // null staged SRV made the body return without emitting a single one of the eleven barriers
    // it had already declared, which under compiled barriers leaves every later reader of the
    // G-buffer with a wrong before-state.
    auto lightBuilder = [this, renderer](RenderGraphPassContext& ctx)
        -> std::function<void(RenderGraphPassContext)> {
        if (!resources_.GetLightingMaterial() || resources_.GetLightingCBSizeBytes() == 0)
        {
            return {};
        }
        const auto& DL = renderer->GetDeferredForFrame();
        if (DL.gbSRV[0].ptr == 0 || DL.gbSRV[1].ptr == 0 || DL.gbSRV[2].ptr == 0 ||
            DL.gbSRV[3].ptr == 0 || DL.gbAuxSRV.ptr == 0 || DL.depthSRV.ptr == 0 ||
            DL.shadowSRV.ptr == 0)
        {
            return {};
        }
        ctx.UseDeclared();
        const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
        return [this, renderer, point](RenderGraphPassContext c) {
            CPU_SCOPE(ProfilerScopes::kPassLighting);
            Pass_Lighting(renderer, c, *frame_->camera, point);
        };
    };
    size_t pLight;
    // Step 24f: in VSM mode the directional shader samples the clipmap (VSM page pool + table), so it
    // must order AFTER the page render and declare those SRV-readable. Legacy = the CSM-only decls.
    // P6B item 7: lighting now SAMPLES the AO target, so it must order after the chain that writes
    // it. Until this step the AO pass was a leaf nobody depended on, and the two were free to run
    // concurrently -- correct only while nothing read the result.
    if (vsmActive && pVsmPageRender != static_cast<size_t>(-1))
    {
        ID3D12Resource* vpool = frame_->vsm->PagePool();
        ID3D12Resource* vpt = frame_->vsm->PageTable();
        pLight = rg.AddPass2(RenderPass::Main_Lighting, { pGbuf, pVsmPageRender, pGtao }, { pShadow },
            { { D.gb0.Get(), kSrvAll }, { D.gb1.Get(), kSrvAll }, { D.gb2.Get(), kSrvAll },
              { D.gbVelocity.Get(), kSrvAll }, { D.gbAux.Get(), kSrvAll }, { D.depth.Get(), kSrvAll },
              { D.shadow.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { vpool, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { vpt, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.gtaoUpsampled.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
            lightBuilder);
    }
    else
    {
        pLight = rg.AddPass2(RenderPass::Main_Lighting, { pGbuf, pGtao }, { pShadow },
            { { D.gb0.Get(), kSrvAll },
              { D.gb1.Get(), kSrvAll },
              { D.gb2.Get(), kSrvAll },
              { D.gbVelocity.Get(), kSrvAll },
              { D.gbAux.Get(), kSrvAll },
              { D.depth.Get(), kSrvAll },
              { D.shadow.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.gtaoUpsampled.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
            lightBuilder);
    }

    // Step 21: the spot lighting shader always binds the VSM page-table (t7) + pool (t8) SRVs, so
    // when the VSM is allocated they must be in a readable state on entry (declared here). When VSM
    // sampling is active, also order after the page render (fresh page content this frame).
    // pass-flow S6: ONE builder for every variant. The old Prepare gated on the light COUNT only
    // ("body early-outs"), but the body had four more early-outs after it — the light buffer, its
    // CPU pointer and SRV, the staged G-buffer handles, and VSM readiness in VSM mode — each of
    // which returned with ten states already declared. All of them are frame state, knowable
    // here, so they decide once and the pass declares nothing on a frame it will not record.
    auto spotBuilder = [this, renderer](RenderGraphPassContext& ctx)
        -> std::function<void(RenderGraphPassContext)> {
        LightManager& lm = *frame_->lightManager;
        const size_t spotCount = lm.GetSpotLightCount();
        if (spotCount == 0 || !lm.HasSpotLightBuffer(spotCount)) { return {}; }
        const UINT frameIdx = renderer->GetCurrentFrameIndex();
        if (!lm.GetSpotLightBufferCPU(frameIdx) || lm.GetSpotLightSrv(frameIdx).ptr == 0)
        {
            return {};
        }
        const auto& DS = renderer->GetDeferredForFrame();
        if (DS.gbSRV[0].ptr == 0 || DS.gbSRV[1].ptr == 0 || DS.gbSRV[2].ptr == 0 ||
            DS.gbSRV[3].ptr == 0 || DS.gbAuxSRV.ptr == 0 || DS.depthSRV.ptr == 0 ||
            DS.spotShadowSRV.ptr == 0)
        {
            return {};
        }
        // Only when VSM SAMPLING is requested but the pool is not ready (startup / OOM) — never
        // in Legacy mode, which must still light through the atlas.
        const bool vsmReady = frame_->vsm && frame_->vsm->IsAllocated() &&
                              frame_->vsm->PageTableSrv().ptr != 0 &&
                              frame_->vsm->PagePoolSrv().ptr != 0;
        if (render::VsmActive() && !vsmReady) { return {}; }
        ctx.UseDeclared();
        const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
        return [this, renderer, point](RenderGraphPassContext c) {
            CPU_SCOPE(ProfilerScopes::kPassSpotLights);
            Pass_SpotLights(renderer, c, *frame_->camera, point);
        };
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
            pSpotLights = rg.AddPass2(RenderPass::Main_SpotLights, { pLight, pVsmPageRender }, { pSpotShadow }, spotDecls, spotBuilder);
        }
        else
        {
            pSpotLights = rg.AddPass2(RenderPass::Main_SpotLights, { pLight }, { pSpotShadow }, spotDecls, spotBuilder);
        }
    }
    else
    {
        pSpotLights = rg.AddPass2(RenderPass::Main_SpotLights, { pLight }, { pSpotShadow },
            { { D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
              { D.gb0.Get(), kSrvAll }, { D.gb1.Get(), kSrvAll }, { D.gb2.Get(), kSrvAll },
              { D.gbVelocity.Get(), kSrvAll }, { D.gbAux.Get(), kSrvAll }, { D.depth.Get(), kSrvAll },
              { D.spotShadow.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } },
            spotBuilder);
    }

    // Depends on pPointShadow too: the cube must be rendered + transitioned to a
    // shader-readable state before this pass samples it (B3). kSrvAll keeps it readable
    // by both this compute pass and the later transparent (glass) pixel pass. Step 21: the point
    // shader also binds the VSM page-table (t7) + pool (t8) SRVs; ordering after the page render is
    // transitive (this pass depends on pSpotLights, which depends on pVsmPageRender when active).
    auto pointBuilder = [this, renderer](RenderGraphPassContext& ctx)
        -> std::function<void(RenderGraphPassContext)> {
        LightManager& lm = *frame_->lightManager;
        const size_t pointCount = lm.PointLights().size();
        if (pointCount == 0 || !lm.HasPointLightBuffer(pointCount)) { return {}; }
        const UINT frameIdx = renderer->GetCurrentFrameIndex();
        if (!lm.GetPointLightBufferCPU(frameIdx) || lm.GetPointLightSrv(frameIdx).ptr == 0)
        {
            return {};
        }
        const auto& DP = renderer->GetDeferredForFrame();
        if (DP.gbSRV[0].ptr == 0 || DP.gbSRV[1].ptr == 0 || DP.gbSRV[2].ptr == 0 ||
            DP.gbSRV[3].ptr == 0 || DP.gbAuxSRV.ptr == 0 || DP.depthSRV.ptr == 0)
        {
            return {};
        }
        const bool vsmReady = frame_->vsm && frame_->vsm->IsAllocated() &&
                              frame_->vsm->PageTableSrv().ptr != 0 &&
                              frame_->vsm->PagePoolSrv().ptr != 0;
        if (render::VsmActive() && !vsmReady) { return {}; }
        ctx.UseDeclared();
        const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
        return [this, renderer, point](RenderGraphPassContext c) {
            CPU_SCOPE(ProfilerScopes::kPassPointLights);
            Pass_PointLights(renderer, c, *frame_->camera, point);
        };
    };
    size_t pPointLights;
    if (vsmAlloc)
    {
        pPointLights = rg.AddPass2(RenderPass::Main_PointLights, { pSpotLights, pPointShadow }, /*mtDeps=*/{},
            { { D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
              { D.gb0.Get(), kSrvAll }, { D.gb1.Get(), kSrvAll }, { D.gb2.Get(), kSrvAll },
              { D.gbVelocity.Get(), kSrvAll }, { D.gbAux.Get(), kSrvAll }, { D.depth.Get(), kSrvAll },
              { D.pointShadow.Get(), kSrvAll },
              { frame_->vsm->PagePool(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { frame_->vsm->PageTable(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } },
            pointBuilder);
    }
    else
    {
        pPointLights = rg.AddPass2(RenderPass::Main_PointLights, { pSpotLights, pPointShadow }, /*mtDeps=*/{},
            { { D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
              { D.gb0.Get(), kSrvAll }, { D.gb1.Get(), kSrvAll }, { D.gb2.Get(), kSrvAll },
              { D.gbVelocity.Get(), kSrvAll }, { D.gbAux.Get(), kSrvAll }, { D.depth.Get(), kSrvAll },
              { D.pointShadow.Get(), kSrvAll } },
            pointBuilder);
    }

    // pass-flow S5/S6: all three lighting passes have several variants (VSM vs Legacy, with or
    // without spot/point shadows), but each variant declares its own first-use set, so ONE builder
    // per pass covers all of them — and every variant always creates a real pass, so no builder
    // can land on a pass index that aliases somebody else's.

    // pass-flow S5: the builder owns the `frame_->skybox` gate the body used to repeat after the
    // declarations were already made.
    auto pSky = rg.AddPass2(RenderPass::Main_Skybox, { pPointLights }, /*mtDeps=*/{},
        { { D.light.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
          { D.gbVelocity.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
          { D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_READ } },
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            if (!frame_->skybox) { return {}; }
            ctx.UseDeclared();
            const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
            return [this, renderer, point](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassSkybox);
                Pass_Skybox(renderer, c, *frame_->camera, point);
            };
        });

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
    // Compose samples the shore-wetness texture that Main_ShoreWetness writes, but Compose is a
    // NON-FIRST member of this CL group and BeginCLGroup's contract allows an outside prereq only
    // on the FIRST member — a grouped list records as one unit, so nothing can wait in its middle.
    // The dependency therefore rides the group's first member, which orders the whole list after
    // the wetness update and keeps Compose's guarantee. Reflection waiting too costs nothing:
    // wetness is the tail of the early compute group and has long since finished.
    const bool useRtReflections = rtReflect && pBuildAS != (size_t)-1;
    const std::initializer_list<ResourceStateDecl> reflectDecls = {
        { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { D.gb0.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { D.gb1.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { D.gbVelocity.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { D.light.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { P.scene.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        // P6C step 6: the HiZ tracer's pyramid. Already its resting state, so this compiles to no
        // barrier -- declaring it is what makes that a fact the compile knows.
        { D.hzb.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
        { D.reflection.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } };
    size_t pReflectionSource; // node the blur depends on (reflection chain end)
    if (useRtReflections)
    {
        // RT (S7/S10): trace one sharp reflection ray per surface and shade the hit;
        // write the premultiplied reflection straight into the main reflection target
        // (S12: the old temporal-denoise pass was an inert pass-through once glossy was
        // parked, so it was removed -- blur + compose consume `reflection` directly).
        pReflectionSource = rg.AddPass2(RenderPass::Main_RTReflections, { pSky, pWetness }, { pSky, pBuildAS },
            { { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.gb1.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.light.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.reflection.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
            [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                ctx.UseDeclared();
                const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
                return [this, renderer, point](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassRTReflections);
                    Pass_RTReflections(renderer, c, *frame_->camera, point);
                };
            });
    }
    else if (clearReflections)
    {
        pReflectionSource = rg.AddPass2(RenderPass::Main_ReflectionSource, { pSky, pWetness }, /*mtDeps=*/{},
            { { D.reflection.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
            [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                ctx.UseDeclared();
                const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
                return [this, renderer, point](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassReflectionSource);
                    Pass_ClearReflections(renderer, c, point);
                };
            });
    }
    else
    {
        // P6C step 6: the HiZ technique marches the depth pyramid, so this orders after the build.
        // The GLASS SSR pass needs the same guarantee and gets it transitively -- it hangs off
        // Compose, which hangs off the blur, which hangs off this node.
        pReflectionSource = rg.AddPass2(RenderPass::Main_ReflectionSource, { pSky, pWetness, pHzb }, /*mtDeps=*/{},
            reflectDecls,
            [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                ctx.UseDeclared();
                const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
                return [this, renderer, point](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassReflectionSource);
                    Pass_ScreenSpaceReflections(renderer, c, *frame_->camera, point);
                };
            });
    }
    // pass-flow S5: each of the three variants (RT / clear / SSR) is authored with its own
    // AddPass2 — same builder shape, different pass name and first-use set. Which one exists is a
    // GRAPH-SHAPE decision and stays out here; the builder only owns what happens inside the pass.

    // SSR temporal resolve, between the trace and the glossy blur. Skipped entirely when it is not
    // active -- an unregistered pass costs nothing, a registered one still pays its barriers.
    size_t pReflectionFiltered = pReflectionSource;
    if (ssrTemporalActive_)
    {
        pReflectionFiltered = rg.AddPass2(RenderPass::Main_ReflectionTemporal, { pReflectionSource },
            /*mtDeps=*/{},
            { { D.reflection.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.gbVelocity.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.reflectionHistory.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
            [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                ctx.UseDeclared();
                const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
                return [this, renderer, point](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassReflectionTemporal);
                    Pass_SsrTemporal(renderer, c, point);
                };
            });
    }

    // First-use states only; the blur ping-pongs reflection<->scratch states between
    // its two dispatches inside the pass body.
    // Barrier plan step 3 chose this pass as the comparator's first test subject because it
    // exercises the hard cases in one place — `reflection` and `reflectionScratch` each take TWO
    // states inside the body, and the second pair is behind a predicate. That predicate was
    // evaluated in the Prepare AND in the body, and the note left here said "that duplication is
    // exactly what D1.1 forbids; step 5 hoists it into pass state". Step 5 never did.
    // pass-flow S6 does: the builder decides `blur` once, declares the ping-pong point from it,
    // and the body emits markers for both points and re-decides nothing.
    auto pBlur = rg.AddPass2(RenderPass::Main_ReflectionBlur, { pReflectionFiltered }, /*mtDeps=*/{},
        { { D.reflection.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          // The resolve's output is the blur's first input; declared unconditionally so the
          // compiled barrier set does not change with a UI toggle.
          { D.reflectionHistory.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.reflectionScratch.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
          { D.gb0.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } }, // S16: roughness drives glossy blur
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            const auto& DB = renderer->GetDeferredForFrame();
            BlurPoints pts{};
            ctx.UseDeclared();
            pts.apply = ctx.usePoint ? *ctx.usePoint : 0u;
            pts.blur = resources_.GetBlurMaterial() && resources_.GetBlurCBSizeBytes() != 0;
            if (pts.blur)
            {
                ctx.NextPoint(); // the vertical dispatch ping-pongs the two targets
                pts.pingPong = ctx.usePoint ? *ctx.usePoint : 0u;
                ctx.Use(DB.reflectionScratch.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                ctx.Use(DB.reflection.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            return [this, renderer, pts](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassReflectionBlur);
                Pass_ReflectionBlur(renderer, c, pts);
            };
        });

    // First-use states only; Compose transitions scene back to RENDER_TARGET
    // for the transparent pass at the end of its body.
    // pWetness is deliberately NOT listed here: see the CL-group note above the reflection source.
    // It is carried by the group's first member, which orders this whole list after it.
    auto pCompose = rg.AddPass2(RenderPass::Main_Compose, { pBlur }, /*mtDeps=*/{},
        { { D.gb0.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.gb1.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.gb2.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.gbAux.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.light.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.reflection.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          // P6B item 7: compose samples the AO for specular occlusion. Ordering to the AO pass is
          // transitive through lighting, which now depends on it directly.
          { D.gtaoUpsampled.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
          { D.scene.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
        // pass-flow S6: the wetness read is gated, the hand-back point is NOT — Compose gives
        // `scene` back to the transparent pass as a render target on EVERY path, both early-outs
        // and the success tail, so the POINT is unconditional and only its content is decided.
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            const auto& DC = renderer->GetDeferredForFrame();
            ctx.UseDeclared();
            const std::uint32_t apply = ctx.usePoint ? *ctx.usePoint : 0u;
            if (frame_->ocean && frame_->ocean->IsWetnessReady())
            {
                ctx.Use(frame_->ocean->GetWetnessResource(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            ctx.NextPoint();
            const std::uint32_t handBack = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(DC.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            return [this, renderer, apply, handBack](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassCompose);
                Pass_Compose(renderer, c, *frame_->camera, apply, handBack);
            };
        });
    rg.EndCLGroup();

    // RT debug visualization (S6): runs AFTER the reflection group so it can overwrite
    // the already-consumed reflection target with ray-hit data for inspection via
    // TextureDebugViewer -> Reflection, without disturbing the composited scene. Needs
    // the TLAS (mtDep on Main_BuildAS) and reflection free (prereq/mtDep on Compose).
    // The TLAS SRV bypasses the state tracker (staged as a plain descriptor).
    if (rtDebugView && pBuildAS != (size_t)-1)
    {
        // pass-flow S5: the TLAS SRV is staged as a plain descriptor and never transitioned, so
        // the declared trio is the whole ENTRY list — but the body also hands `reflection` back to
        // its resting read state on the frames it traces, and that transition was never registered
        // at all (silently dropped: a request may only match the current point, and there was no
        // second point). The builder decides `trace` once and declares the restore point with it.
        rg.AddPass2(RenderPass::Main_RTDebug, { pCompose }, { pCompose, pBuildAS },
            { { D.reflection.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
              { D.gb1.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } },
            [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                RtDebugPoints pts{};
                ctx.UseDeclared();
                pts.apply = ctx.usePoint ? *ctx.usePoint : 0u;
                const UINT frameIndex = renderer->GetCurrentFrameIndex();
                pts.trace = resources_.GetRtDebugMaterial() && bindless_.FrameReady(frameIndex) &&
                            asManager_.TlasSrvCpu(frameIndex).ptr != 0 &&
                            asManager_.TlasInstanceCount(frameIndex) != 0;
                if (pts.trace)
                {
                    ctx.NextPoint();
                    pts.restore = ctx.usePoint ? *ctx.usePoint : 0u;
                    ctx.Use(renderer->GetDeferredForFrame().reflection.Get(),
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                }
                return [this, renderer, pts](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassRTDebug);
                    Pass_RTDebug(renderer, c, *frame_->camera, pts);
                };
            });
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
        // pass-flow S5: the prepass-material readiness is the builder's — the body used to break
        // out of the pass AFTER declaring both targets, emitting neither barrier.
        size_t pGlassGbuf = rg.AddPass2(RenderPass::Main_GlassReflGbuffer, { pCompose }, /*mtDeps=*/{},
            { { D.glassReflNormal.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
              { D.glassReflDepth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE } },
            [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                if (!resources_.GetGlassReflPrepassMaterial()) { return {}; }
                ctx.UseDeclared();
                const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
                return [this, renderer, point](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassGlassReflGbuffer);
                    Pass_GlassReflGbuffer(renderer, c, *frame_->camera, *frame_->mainView, point);
                };
            });
        const std::initializer_list<ResourceStateDecl> glassReflDecls = {
            { D.glassReflNormal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
            { D.glassReflDepth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
            { D.light.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
            { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
            // P6C step 6: only the SSR variant reads it, but the RT variant declaring a resource
            // already in that state costs nothing and keeps ONE decl list for both branches.
            { D.hzb.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
            { D.glassReflection.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } };
        if (useRtReflections && pBuildAS != (size_t)-1)
        {
            // RT mode: dispatch rt_reflections_cs (needs the TLAS, so mt-dep on pBuildAS).
            pGlassReflect = rg.AddPass2(RenderPass::Main_GlassReflections, { pGlassGbuf }, { pGlassGbuf, pBuildAS },
                glassReflDecls,
                [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                    ctx.UseDeclared();
                    const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
                    return [this, renderer, point](RenderGraphPassContext c) {
                        CPU_SCOPE(ProfilerScopes::kPassGlassReflections);
                        Pass_GlassReflections(renderer, c, *frame_->camera, point);
                    };
                });
        }
        else
        {
            // SSR mode: dispatch ssr_cs over the glass G-buffer (no TLAS, works on all HW).
            pGlassReflect = rg.AddPass2(RenderPass::Main_GlassReflections, { pGlassGbuf }, /*mtDeps=*/{},
                { { D.glassReflNormal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
                  { D.glassReflDepth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
                  { D.light.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
                  { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
                  { D.hzb.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
                  { D.gb0.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
                  { D.gbVelocity.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
                  { P.scene.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
                  { D.glassReflection.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
                [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                    ctx.UseDeclared();
                    const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
                    return [this, renderer, point](RenderGraphPassContext c) {
                        CPU_SCOPE(ProfilerScopes::kPassGlassReflections);
                        Pass_GlassReflectionsSSR(renderer, c, *frame_->camera, point);
                    };
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
    auto pTransp = rg.AddPass2(RenderPass::Main_Transparent, transpDeps,
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
        RenderGraphPassContext& p = ctx;
        const auto& DT = p.renderer->GetDeferredForFrame();
        TransparentPoints pts{};
        pts.copyDepth = DT.depthCopy.Get() != nullptr;
        pts.copyScene = DT.sceneOpaque.Get() != nullptr;
        // The ocean reflection compute's FULL readiness, decided once. Both of the early-outs it
        // used to make itself ran after this pass had declared the read point below.
        pts.oceanReflect = pts.copyDepth && pts.copyScene && DT.oceanReflection.Get() != nullptr &&
                           resources_.GetOceanReflectionMaterial() &&
                           resources_.GetOceanReflectionCBSizeBytes() != 0 &&
                           DT.sceneOpaqueSRV.ptr != 0 && DT.depthCopySRV.ptr != 0 &&
                           DT.oceanReflectionUAV.ptr != 0;

        // 1. Snapshot depth + opaque colour for the refraction/reflection reads.
        pts.copy = p.usePoint ? *p.usePoint : 0u;
        if (pts.copyDepth)
        {
            p.Use(DT.depth.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            p.Use(DT.depthCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        }
        if (pts.copyScene)
        {
            p.Use(DT.scene.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            p.Use(DT.sceneOpaque.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        }
        // 2. RecordOceanReflection's compute — declared only on the frames it runs.
        p.NextPoint();
        pts.oceanRead = p.usePoint ? *p.usePoint : 0u;
        if (pts.oceanReflect)
        {
            p.Use(DT.sceneOpaque.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            p.Use(DT.depthCopy.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            // P13: the UE search reads the furthest pyramid here too. Already its resting state,
            // so this declares a fact rather than requesting a barrier -- and it is NOT
            // re-declared at the next point, because nothing downstream reads the pyramid from a
            // pixel shader.
            if (DT.hzb.Get())
            {
                p.Use(DT.hzb.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            p.Use(DT.oceanReflection.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        // 3. All three become PS-readable for the forward draws, on every path.
        p.NextPoint();
        pts.pixel = p.usePoint ? *p.usePoint : 0u;
        p.Use(DT.sceneOpaque.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        p.Use(DT.depthCopy.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        p.Use(DT.oceanReflection.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        // 4. Rebind the forward targets. The fan-out chunks re-apply the velocity/objectID
        // pair per chunk; same states, so one registration covers them.
        p.NextPoint();
        pts.rebind = p.usePoint ? *p.usePoint : 0u;
        p.Use(DT.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        p.Use(DT.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        p.Use(DT.gbVelocity.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
#if WITH_EDITOR
        p.Use(DT.objectID.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
#endif
        p.Use(DT.glassReflection.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        // 5. Per-object reads (ocean displacement, particle sim buffers), on fan-out workers.
        if (frame_->objects)
        {
            p.NextPoint();
            for (const auto& obj : *frame_->objects)
            {
                if (!obj || !obj->IsTransparent()) { continue; }
                obj->PrepareRender(p);
            }
        }
        return [this, renderer, pts](RenderGraphPassContext c) {
            CPU_SCOPE(ProfilerScopes::kPassTransparent);
            Pass_Transparent(renderer, c, *frame_->camera, *frame_->mainView, pts);
        };
    });

#if WITH_EDITOR
    size_t pObjectIdReadback = pTransp;
    if (renderer->HasPendingObjectIdPick())
    {
        // pass-flow S5: with AddPass2 the builder is attached at Add time, so the old trap this
        // `if` guarded against (a Prepare set on an index that ALIASES pTransp when there is no
        // pending pick) cannot happen — there is no second call to misplace.
        pObjectIdReadback = rg.AddPass2(RenderPass::Main_ObjectIdReadback, { pTransp }, /*mtDeps=*/{},
            { { D.objectID.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE } },
            [renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                ctx.UseDeclared();
                const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
                return [renderer, point](RenderGraphPassContext c) {
                    auto t = c.BeginCL();
                    SetCommandListName(t.cl, c.pass);
                    renderer->EmitPoint(t.cl, point);
                    renderer->RecordObjectIdPickReadback(t.cl);
                    c.EndCL(t);
                };
            });
    }
#else
    const size_t pObjectIdReadback = pTransp;
#endif

    auto pDebugDraw = rg.AddPass2(RenderPass::Main_DebugDraw, { pObjectIdReadback }, /*mtDeps=*/{},
        { { D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET },
          { D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE } },
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            DebugDrawSystem* const dd = renderer->GetDebugDrawSystem();
            if (!dd || !dd->HasCommands()) { return {}; } // nothing submitted this frame
            ctx.UseDeclared();
            const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
            return [this, renderer, point](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassDebugDraw);
                Pass_DebugDraw(renderer, c, *frame_->camera, point);
            };
        });

    size_t pSelectionOutline = pDebugDraw;
#if WITH_EDITOR
    if (frame_->selectedEditorObjectCount != 0)
    {
        // pass-flow S5: the material / CB-size / handle checks the body used to make AFTER
        // declaring depth -> NPS and scene -> UAV are the builder's now.
        pSelectionOutline = rg.AddPass2(RenderPass::Main_SelectionOutline, { pDebugDraw }, /*mtDeps=*/{},
            { { D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
              { D.scene.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
            [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                if (!resources_.GetSelectionOutlineMaterial() ||
                    resources_.GetSelectionOutlineCBSizeBytes() == 0)
                {
                    return {};
                }
                const auto& DS = renderer->GetDeferredForFrame();
                if (DS.stencilSRV.ptr == 0 || DS.sceneUAV.ptr == 0) { return {}; }
                ctx.UseDeclared();
                const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
                return [this, renderer, point](RenderGraphPassContext c) {
                    Pass_SelectionOutline(renderer, c, point);
                };
            });
    }
#endif

    // Ensure tonemapping runs after the debug draw pass so the resolved backbuffer
    // always includes any debug geometry submitted during rendering.
    // Only the unconditional outputs are declared; the tonemap source (scene or
    // DLSS output) and the backbuffer copy are handled inside the pass body.
    // CL group (step 5): the optional debug-texture draw follows tonemap on the
    // same target with no mtDeps; share one command list (Debug usually early-outs).
    // P2: metering runs on the finished scene-referred HDR image, before the tone curve consumes
    // it. Deliberately OUTSIDE the tonemap CL group: it is the group's external prereq, and the
    // group contract only allows that on the first member (see the reflection group above).
    // Scheduled unconditionally but early-outs in the body when the camera is dormant, so a
    // disabled camera costs one empty command list rather than a graph-shape change per frame.
    const size_t pExposure = rg.AddPass2(RenderPass::Main_ExposureMetering, { pSelectionOutline },
        /*mtDeps=*/{},
        { { D.scene.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE } },
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            ExposurePoints pts{};
            // UNCONDITIONAL, and it has to be: the body hands `scene` to the metering read BEFORE
            // it checks whether the camera is dormant, so this happens on every frame including
            // the ones with no dispatches. Returning above it left the body performing a
            // transition the compile had never registered -- the comparator's FATAL direction,
            // "MISSING (performed, never registered) res=Deferred[N].Scene". It only reproduced on
            // a level with NO cameraExposure block at all (d_emissive_test).
            ctx.UseDeclared();
            pts.apply = ctx.usePoint ? *ctx.usePoint : 0u;

            ExposureMetering& metering = renderer->Exposure();
            // The body needs all three materials too — checking only `enabled` + IsReady() here
            // left the two remaining points declared on a frame whose body broke out before them.
            pts.meter = frame_->cameraExposure.enabled && metering.IsReady() &&
                        resources_.GetExposureClearMaterial() &&
                        resources_.GetExposureBuildMaterial() &&
                        resources_.GetExposureSolveMaterial();
            if (!pts.meter)
            {
                return [this, renderer, pts](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassExposureMetering);
                    Pass_ExposureMetering(renderer, c, pts);
                };
            }
            // The histogram and exposure buffers rest at UNORDERED_ACCESS and are used at
            // UNORDERED_ACCESS, so declaring them emits no barrier -- but declaring them is what
            // makes them legal to touch at all, since an undeclared resource is an invariant
            // failure.
            ctx.Use(metering.HistogramResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.Use(metering.ExposureResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            pts.baseLum = resources_.GetExposureBaseLumMaterial() != nullptr;
            if (pts.baseLum)
            {
                // P3B: the base layer leaves its resting read state only for this pass...
                ctx.Use(metering.BaseLumResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                ctx.NextPoint();
                pts.baseLumRead = ctx.usePoint ? *ctx.usePoint : 0u;
                // ...and is back in it before the solve runs, which is why this is its OWN point.
                ctx.Use(metering.BaseLumResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            // The dev-UI readback copies AFTER the solve, then straight back to canonical so the
            // tonemap's UAV binding needs no barrier of its own.
            ctx.NextPoint();
            pts.copySrc = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(metering.ExposureResource(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            ctx.Use(metering.HistogramResource(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            ctx.NextPoint();
            pts.restore = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(metering.ExposureResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.Use(metering.HistogramResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            return [this, renderer, pts](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassExposureMetering);
                Pass_ExposureMetering(renderer, c, pts);
            };
        });

    // DLSS-split: the upscale is its own pass, recorded CONCURRENTLY with the tonemap's bloom and
    // tone curve rather than in front of them in one command list. Three things make that legal:
    //   * GPU order comes from SUBMISSION order, and `pDlss` is in the tonemap's PREREQS (batch
    //     order) while its mtDeps stay empty (no record-time wait) — so the two tasks run on
    //     different workers and the lists still reach the queue in the right order.
    //   * The barrier compile walks the schedule, so this pass's points compile before the
    //     tonemap's, exactly as when they shared a body.
    //   * `ranDlss` is PREDICTED (Renderer::WillEvaluateDlss) instead of being discovered inside
    //     the record — the one thing the split really costs. See DlssHandler::WillEvaluate.
    // Its prereq is the selection outline, NOT the exposure metering: the upscale does not read
    // the metered exposure, so it can start recording while metering is still being recorded.
    const bool willDlss = renderer->WillEvaluateDlss();
    size_t pDlss = static_cast<size_t>(-1);
    if (willDlss)
    {
        pDlss = rg.AddPass2(RenderPass::Main_DLSS, { pSelectionOutline },
            [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
                const auto& DD = renderer->GetDeferredForFrame();
                DlssPoints pts{};
                pts.apply = ctx.usePoint ? *ctx.usePoint : 0u;
                // Inside EvaluateDLSS (DlssHandler): the three inputs plus the upscaled output.
                ctx.Use(DD.scene.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                ctx.Use(DD.gbVelocity.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                ctx.Use(DD.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                ctx.Use(DD.dlssOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                ctx.NextPoint();
                pts.output = ctx.usePoint ? *ctx.usePoint : 0u;
                ctx.Use(DD.dlssOutput.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                return [this, renderer, pts](RenderGraphPassContext c) {
                    CPU_SCOPE(ProfilerScopes::kPassDlss);
                    Pass_Dlss(renderer, c, pts);
                };
            });
    }

    rg.BeginCLGroup();
    // The tonemap's prereqs carry BOTH the metering (its exposure input) and the upscale (its
    // colour input); neither is an mtDep, so neither makes this task wait to be recorded.
    const std::initializer_list<size_t> tonemapPrereqs = willDlss
        ? std::initializer_list<size_t>{ pExposure, pDlss }
        : std::initializer_list<size_t>{ pExposure };
    auto pTone = rg.AddPass(RenderPass::Main_Tonemap, tonemapPrereqs,
        { { D.tonemap.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS },
          { D.fxaa.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS } },
        [this, renderer, willDlss](RenderGraphPassContext ctx) { CPU_SCOPE(ProfilerScopes::kPassTonemap); Pass_Tonemap(renderer, ctx, willDlss); });
    // The resolve SOURCE (fxaa vs tonemap) is still decided inside the body, so BOTH alternatives
    // are registered — a state the body skips is one redundant barrier, the one it takes and never
    // registered is a missing barrier. The DLSS half of that union is gone: the upscale is its own
    // pass now and `willDlss` is decided before either of them records.
    rg.SetPassPrepare(pTone, [this, willDlss](RenderGraphPassContext& p) {
        constexpr D3D12_RESOURCE_STATES kNps = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        p.UseDeclared(); // tonemap + fxaa -> UAV
        // P2: the exposure record is bound to the tonemap dispatch on every path. It rests at
        // UNORDERED_ACCESS and is used at UNORDERED_ACCESS, so this declares intent without
        // emitting a barrier — but an undeclared resource would be an invariant failure.
        if (ExposureMetering& metering = p.renderer->Exposure(); metering.IsReady())
        {
            p.Use(metering.ExposureResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        const auto& DTM = p.renderer->GetDeferredForFrame();
        p.NextPoint();
        if (!willDlss)
        {
            // Hand the forward targets back. The transparent pass ends with depth as DEPTH_WRITE
            // and velocity as RENDER_TARGET because that is what it drew into, and with DLSS on
            // the upscale pass returns both to a read state as a side effect of consuming them.
            // With DLSS off nothing did, so the frame ended off-canonical on all three frame sets
            // -- an invariant that must not depend on which upscaler path is selected.
            p.Use(DTM.gbVelocity.Get(), kNps);
            p.Use(DTM.depth.Get(), kNps);
        }
        // The tonemap source. With the upscale split out, this is `scene` only when DLSS is not
        // running; the DLSS pass leaves `dlssOutput` shader-readable for the other case, and
        // declaring it here as well costs nothing and keeps the read legal for the compile.
        p.Use(willDlss ? DTM.dlssOutput.Get() : DTM.scene.Get(), kNps);
        // P8: the bloom pyramid, built between the upscale above and the tone curve below. Both
        // chains go to UNORDERED_ACCESS for the build and come back shader-readable -- the same
        // shape as the HZB pyramid, and for the same reason: a level reads the level above it
        // through its own UAV because this barrier layer transitions whole resources.
        if (bloomActive_)
        {
            p.NextPoint();
            if (bloomConvolution_)
            {
                // P8C: three grids instead of the two pyramid chains. Declared in the SAME order
                // the body transitions them, because a compiled barrier is matched against the
                // current point in body order.
                // P8C-2m: bloomDown is NOT here. The convolution never writes it (stage 0 packs
                // straight from the HDR image) and the flares read the HDR image too, so the
                // UAV->SRV round trip it used to make was pure cost -- and the only body request
                // that answered its point lived inside Bloom_FlaresBuild, which is how switching
                // the flares off stalled the pass's whole barrier program on an unanswered point
                // and left the FFT grids in UNORDERED_ACCESS under the tonemap's read.
                p.Use(DTM.bloomUp.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                p.Use(DTM.bloomFftA.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                p.Use(DTM.bloomFftB.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                p.Use(DTM.bloomFftKernel.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                // P8C-2l/m: the streak pyramid's own targets -- declared only when the flares
                // actually run this frame, so "off" costs not even a barrier.
                if (flaresGhosts_ || flaresStreak_)
                {
                    p.Use(DTM.streakA.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    p.Use(DTM.streakB.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                }
                p.NextPoint();
                // P8C-2/m: the flare accumulation target, declared only when the flares run.
                // THE POINT ITSELF IS NOT GATED. A point is a position in the pass's barrier
                // program, and the body's transitions are matched against it positionally --
                // dropping one shifts every later point, which is how "flares off" managed to
                // leave the bloom chains in UNORDERED_ACCESS under the tonemap's SRV read.
                if (flaresGhosts_ || flaresStreak_)
                {
                    p.Use(DTM.lensFlare.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
                }
                p.NextPoint();
                if (flaresGhosts_ || flaresStreak_)
                {
                    p.Use(DTM.lensFlare.Get(), kNps);
                }
                p.NextPoint();
                p.Use(DTM.bloomUp.Get(), kNps); // the tonemap samples mip 0
                p.Use(DTM.bloomFftA.Get(), kNps);
                p.Use(DTM.bloomFftB.Get(), kNps);
                p.Use(DTM.bloomFftKernel.Get(), kNps);
                if (flaresGhosts_ || flaresStreak_)
                {
                    p.Use(DTM.streakA.Get(), kNps);
                    p.Use(DTM.streakB.Get(), kNps);
                }
            }
            else
            {
                // P8C-2l: the pyramid runs the same flares, so it declares the same targets.
                p.Use(DTM.bloomDown.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                p.Use(DTM.bloomUp.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                if (flaresGhosts_ || flaresStreak_)
                {
                    p.Use(DTM.streakA.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    p.Use(DTM.streakB.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    p.Use(DTM.lensFlare.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
                }
                p.NextPoint();
                if (flaresGhosts_ || flaresStreak_)
                {
                    p.Use(DTM.lensFlare.Get(), kNps);
                }
                p.NextPoint();
                p.Use(DTM.bloomUp.Get(), kNps);   // the tonemap samples mip 0
                p.Use(DTM.bloomDown.Get(), kNps); // back to canonical
                if (flaresGhosts_ || flaresStreak_)
                {
                    p.Use(DTM.streakA.Get(), kNps);
                    p.Use(DTM.streakB.Get(), kNps);
                }
            }
        }
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

    // The inspector's preview. Must complete before the overlay draws ImGui, which is what will
    // sample it. The request was left during UI building, which happens before Scene::Render.
    const size_t pDebugPreview = rg.AddPass2(RenderPass::Main_DebugPreview, { pTone },
        [this, renderer](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            const auto& DD = renderer->GetDeferredForFrame();
            const Renderer::DebugPreviewRequest& req = renderer->DebugPreviewRequestRef();
            if (req.resource == nullptr || !resources_.GetDebugPreviewMaterial() ||
                resources_.GetDebugPreviewCBSizeBytes() == 0u || DD.debugPreviewUAV.ptr == 0)
            {
                return {};
            }
            ctx.NextPoint();
            const uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
            // The SOURCE is whatever the user picked and is deliberately NOT declared: it can be
            // any target, in any of several resting states, and the graph would have to model all
            // of them. It is transitioned explicitly from its canonical and back, the same way the
            // overlay borrows a texture for display.
            ctx.Use(DD.debugPreview.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.NextPoint();
            ctx.Use(DD.debugPreview.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            return [this, renderer, point](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassDebug);
                Pass_DebugPreview(renderer, c, point);
            };
        });
    (void)pDebugPreview;

    const DebugTexPick debugPick = PickDebugTexTarget(D, frame_->settings.debugTexTarget);
    const bool debugTexOn = frame_->settings.debugTexMode && debugPick.resource != nullptr &&
                            debugPick.srv.ptr != 0;
    // The state this target rests in, which is where the blit has to put it back. Read from the
    // registry rather than assumed, because the list spans targets with different resting states.
    const D3D12_RESOURCE_STATES debugCanon =
        debugPick.resource ? renderer->GetCanonicalState(debugPick.resource)
                           : D3D12_RESOURCE_STATE_COMMON;
    // The blit binds the backbuffer RTV/DSV and draws a triangle, so the only state it needs is the
    // one texture it samples — in a PIXEL shader, which is why it must be declared rather than
    // assumed (it used to be assumed, and got away with it because the shadow atlas happened to be
    // readable). It then puts the target BACK: the overlay's texture inspector transitions out of a
    // resource's CANONICAL state without transitioning back, so every graph pass has to leave its
    // resources where the registry says they rest.
    // pass-flow S6: the pick was already decided once above and captured into BOTH lambdas; now
    // the builder is its single home, and the two points ride with it.
    rg.AddPass2(RenderPass::Main_Debug, { pTone },
        [this, renderer, debugTexOn, debugPick, debugCanon](RenderGraphPassContext& ctx)
            -> std::function<void(RenderGraphPassContext)> {
            if (!debugTexOn) { return {}; }
            DebugBlitPoints pts{};
            pts.read = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(debugPick.resource, kSrvAll);
            ctx.NextPoint();
            pts.restore = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(debugPick.resource, debugCanon);
            return [this, renderer, debugPick, pts](RenderGraphPassContext c) {
                CPU_SCOPE(ProfilerScopes::kPassDebug);
                Pass_Debug(renderer, c, debugPick, pts);
            };
        });
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
        const auto& objects = *frame_->objects;
        if (rtBindlessObjectCache_.size() != objects.size())
        {
            rtBindlessObjectCache_.resize(objects.size());
        }
        uint32_t instanceId = 0;
        std::vector<RtInstanceDesc> descs; // reused across objects this frame
        // wind_test has hundreds of identical multi-slot palms. Keep this scratch allocation
        // across objects instead of allocating/freeing a 4-5 element vector for every palm.
        std::vector<rt::BindlessTable::SlotMaterial> slotMats;
        for (size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
        {
            if (bindless_.BuildFailed()) { break; }
            const auto& obj = objects[objectIndex];
            // The editor's Enabled command maps to visibility. In no-editor
            // builds ordinary level objects retain their default visible state,
            // so this only removes explicitly disabled editor objects from RT.
            if (!obj || !obj->IsVisible()) { continue; }
            // Avoid the vector append for the common one-object/one-instance path. Only the GPU
            // instanced caster overrides GetRtInstances today and needs the reusable array.
            RtInstanceDesc singleDesc{};
            const RtInstanceDesc* descData = nullptr;
            size_t descCount = 0;
            if (obj->IsGpuInstancedCaster())
            {
                descs.clear();
                obj->GetRtInstances(descs);
                descData = descs.data();
                descCount = descs.size();
            }
            else if (obj->GetRtInstance(singleDesc))
            {
                descData = &singleDesc;
                descCount = 1;
            }
            // Per-slot RT materials (B3 follow-up): a multi-slot object registers one record per
            // submesh with THAT slot's albedo/MR/params, so palms reflect bark + green fronds
            // instead of slot-0 everywhere. Hit shaders already index per (InstanceID +
            // GeometryIndex). Single-slot objects and GI instance clouds keep the slot-0 path.
            GBufferRenderable* gb = obj->AsGBufferRenderable();
            const bool perSlot = bindless_.Ready() && gb && gb->MultiSlotDraw();
            // Cache unchanged multi-slot objects, but never share mutable material records between
            // owners: editing one palm must not change its neighbours. BindlessTable independently
            // shares the immutable mesh/texture descriptors, so this does not multiply heap usage.
            RtBindlessObjectCache& objectCache = rtBindlessObjectCache_[objectIndex];
            const uint64_t materialFingerprint = perSlot ? RtMaterialFingerprint(*gb) : 0;
            bool reuseObjectCache = perSlot && objectCache.valid &&
                objectCache.object == obj.get() && objectCache.mesh == gb->GetMesh() &&
                objectCache.materialFingerprint == materialFingerprint;
            if (perSlot && !reuseObjectCache)
            {
                // assign() value-initializes reused entries too, clearing descriptor handles left
                // by the preceding object when this one has no texture for a slot.
                slotMats.assign(gb->SlotCount(), {});
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
            for (size_t descIndex = 0; descIndex < descCount; ++descIndex)
            {
                const RtInstanceDesc& desc = descData[descIndex];
                rt::InstanceEntry entry;
                entry.mesh = desc.mesh;
                entry.world = desc.world.m; // Math::mat4 wraps a row-major XMFLOAT4X4
                // TLAS InstanceID = the mesh's bindless geometry index (S9), so a hit
                // can index the geometry/material table directly. Same owner+mesh ->
                // same index (all instances of a cloud share one record run). Falls back to
                // a running index if the bindless table isn't up.
                if (perSlot && desc.mesh == gb->GetMesh())
                {
                    if (reuseObjectCache)
                    {
                        entry.instanceId = objectCache.instanceId;
                    }
                    else
                    {
                        entry.instanceId = bindless_.GetOrUpdateMesh(
                            obj.get(), desc.mesh, slotMats.data(), slotMats.size());
                    }
                    if (!reuseObjectCache)
                    {
                        objectCache.object = obj.get();
                        objectCache.mesh = desc.mesh;
                        objectCache.materialFingerprint = materialFingerprint;
                        objectCache.instanceId = entry.instanceId;
                        objectCache.valid = entry.instanceId != rt::BindlessTable::kInvalidGeometry;
                        reuseObjectCache = objectCache.valid;
                    }
                }
                else
                {
                    entry.instanceId = bindless_.Ready()
                        ? bindless_.GetOrUpdateMesh(obj.get(), desc.mesh, desc.albedoSrv, desc.mrSrv, &desc.baseColor.x,
                                                      /*roughness*/ desc.metalRough.y, /*metalness*/ desc.metalRough.x,
                                                      desc.mrMultiply)
                        : instanceId;
                }
                if (entry.instanceId == rt::BindlessTable::kInvalidGeometry) { break; }
                rtInstances_.push_back(entry);
                ++instanceId;
            }
        }
    }
    if (!bindless_.UploadGeometryInfo(renderer->GetCurrentFrameIndex()))
    {
        // Never trace a partial/old material table. The next frame selects SSR on a sticky failure.
        rtInstances_.clear();
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
        frame_->vsm->RecordPageAllocate(renderer, t.cl, pts);
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

bool SceneRenderer::IndirectShadowDrawsActive() const
{
    return render::g_indirectShadowsEnabled && frame_ && frame_->shadowGpu &&
           frame_->shadowGpu->IndirectDrawReady();
}

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
    const ObjectComputeList& objects)
{
    // pass-flow S7b: `objects` is what the builder declared for — no second walk of the scene and
    // no second copy of the "does this object's compute record anything" test.
    auto compute = ctx.BeginCL();
    SetCommandListName(compute.cl, ctx.pass);
    {
        GPU_SCOPE(compute.cl, ProfilerScopes::kPassObjectCompute);
        for (RenderableObjectBase* obj : objects)
        {
            obj->ExecuteCompute(renderer, compute.cl);
        }
    }

    ctx.EndCL(compute);
}

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
                        obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB, shadowLod, /*chunkCameraLods=*/true);
                    }
                }

                for (auto* obj : opaqueComplex)
                {
                    if (obj)
                    {
                        obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB, shadowLod, /*chunkCameraLods=*/true);
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
                    obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB, shadowLod, /*chunkCameraLods=*/true);
                }
            }

            for (auto* obj : opaqueComplex)
            {
                if (obj)
                {
                    obj->RenderShadow(renderer, t.cl, view.view, view.proj, viewCB, shadowLod, /*chunkCameraLods=*/true);
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
void SceneRenderer::Pass_DebugPreview(Renderer* renderer, RenderGraphPassContext ctx, uint32_t point)
{
    const auto& D = renderer->GetDeferredForFrame();
    const Renderer::DebugPreviewRequest& req = renderer->DebugPreviewRequestRef();

    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassDebug);
        renderer->EmitPoint(t.cl, point);

        // Borrow the source: canonical -> readable, and put it back below. Explicit because the
        // source is user-chosen and the graph does not model it.
        const D3D12_RESOURCE_STATES srcCanonical = renderer->GetCanonicalState(req.resource);
        Renderer::TransitionExplicit(t.cl, req.resource, srcCanonical,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        // A one-off SRV for exactly the view the inspector asked for (mip + channel swizzle).
        const D3D12_CPU_DESCRIPTOR_HANDLE srcSrv =
            renderer->MakeDebugPreviewSourceSrv(req.resource, req.srv);

        DebugPreviewConstants c{};
        c.previewSize = uint2{ kDebugPreviewSize, kDebugPreviewSize };
        c.gain = req.gain;
        c.stretch = req.stretch ? 1u : 0u;
        c.showAlpha = req.showAlpha ? 1u : 0u;

        const auto samplerDescs = std::array{ *SamplerManager::LinearClamp() };
        RecordComputeDispatch(renderer, t.cl, resources_.GetDebugPreviewMaterial().get(),
            resources_.GetDebugPreviewCBSizeBytes(),
            [&](uint8_t* dest) { resources_.WriteDebugPreviewConstants(c, dest); },
            { srcSrv },
            { D.debugPreviewUAV },
            renderer->GetSamplerManager()->GetTable(renderer, samplerDescs),
            kDebugPreviewSize, kDebugPreviewSize,
            D.debugPreview.Get());

        Renderer::TransitionExplicit(t.cl, req.resource,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, srcCanonical);

        // ...and the preview itself back to shader-readable for ImGui.
        renderer->EmitPoint(t.cl, point + 1u);
    }
    ctx.EndCL(t);
}

// P6C. Builds the whole mip chain in one command list: one dispatch per level, each reading the
// previous level through its UAV and separated by a UAV barrier. One dispatch per mip rather than
// UE's four-mips-per-dispatch groupshared reduction -- the levels are tiny and the barriers are
// cheap, and this version can be checked against a CPU reduction line for line. If the build ever
// measures as significant, the batched form is a drop-in replacement for this loop.
void SceneRenderer::Pass_Hzb(Renderer* renderer, RenderGraphPassContext ctx, uint32_t point)
{
    auto material = resources_.GetHzbMaterial();
    const UINT cbSize = resources_.GetHzbCBSizeBytes();
    const auto& D = renderer->GetDeferredForFrame();

    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassHzb);
        renderer->EmitPoint(t.cl, point);

        const auto samplerDescs = std::array{ *SamplerManager::PointClamp() };
        const D3D12_GPU_DESCRIPTOR_HANDLE samplerTable =
            renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);

        UINT srcW = renderer->GetRenderWidth();
        UINT srcH = renderer->GetRenderHeight();
        for (UINT mip = 0; mip < D.hzbMips; ++mip)
        {
            const UINT dstW = std::max(1u, D.hzbWidth >> mip);
            const UINT dstH = std::max(1u, D.hzbHeight >> mip);

            HzbPassConstants c{};
            c.dstSize = uint2{ dstW, dstH };
            c.srcSize = uint2{ srcW, srcH };
            c.fromDepth = (mip == 0) ? 1u : 0u;
            c.writeClosest = ssrHizActive_ ? 1u : 0u;

            // u0/u2 are the source mips. For mip 0 the source is the depth SRV instead, but the
            // table must stay fully populated, so they are bound to mip 0 as inert placeholders.
            const D3D12_CPU_DESCRIPTOR_HANDLE srcUav =
                (mip == 0) ? D.hzbMipUAV[0] : D.hzbMipUAV[mip - 1];
            const D3D12_CPU_DESCRIPTOR_HANDLE srcUavClosest =
                (mip == 0) ? D.hzbClosestMipUAV[0] : D.hzbClosestMipUAV[mip - 1];

            RecordComputeDispatch(renderer, t.cl, material.get(), cbSize,
                [&](uint8_t* dest) { resources_.WriteHzbConstants(c, dest); },
                { D.depthSRV },
                { srcUav, D.hzbMipUAV[mip], srcUavClosest, D.hzbClosestMipUAV[mip] },
                samplerTable,
                dstW, dstH,
                D.hzb.Get()); // UAV barrier: the next level reads what this one just wrote
            if (ssrHizActive_)
            {
                // Same reason, second resource: RecordComputeDispatch only barriers one.
                renderer->UAVBarrier(t.cl, D.hzbClosest.Get());
            }

            srcW = dstW;
            srcH = dstH;
        }

        // Back to shader-readable for this frame's consumers.
        renderer->EmitPoint(t.cl, point + 1u);
    }
    ctx.EndCL(t);
}

// P6B. The whole AO chain records into ONE command list: raw estimate -> bilateral denoise ->
// temporal accumulation -> edge-aware upsample. Four dispatches of ~0.03 ms each do not each
// deserve their own pass and their own command list; the stage boundaries are ordinary barrier
// points, exactly as the reflection blur ping-pongs its two dispatches inside one pass.
//
// NO EARLY RETURN AFTER BeginCL. Every gate was evaluated in the builder, which declared from the
// same decision; a body that stopped half way through would leave declared barrier points
// unemitted, and the pass after this one would read a target the compile believes was already
// transitioned. `chain` is that decision, arrived by value.
void SceneRenderer::Pass_Gtao(Renderer* renderer, RenderGraphPassContext ctx, const Camera& camera,
    const GtaoChain& chain)
{
    auto material = resources_.GetGtaoMaterial();
    const UINT cbSize = resources_.GetGtaoCBSizeBytes();

    const auto& D = renderer->GetDeferredForFrame();
    const auto& P = renderer->GetDeferredForPrevFrame();

    const UINT aoW = std::max(1u, (renderer->GetRenderWidth() + 1u) / 2u);
    const UINT aoH = std::max(1u, (renderer->GetRenderHeight() + 1u) / 2u);

    GtaoPassConstants c{};
    c.view = camera.GetViewMatrix();
    c.invProj = camera.GetInvProjMatrix();
    c.aoSize = float2(static_cast<float>(aoW), static_cast<float>(aoH));
    c.invAoSize = float2(1.0f / static_cast<float>(aoW), 1.0f / static_cast<float>(aoH));
    // The engine's standard linearisation pair, same as the RT passes use.
    const float zNear = camera.GetZNear();
    const float zFar = camera.GetZFar();
    c.depthA = zNear / (zNear - zFar);
    c.depthB = (zNear * zFar) / (zFar - zNear);
    const GtaoSettings& s = frame_->settings.gtao;
    c.worldRadius = s.worldRadius;
    c.thickness = s.thickness;
    c.intensity = s.intensity;
    c.fadeStart = s.fadeStart;
    c.fadeEnd = s.fadeEnd;
    c.numAngles = s.numAngles;
    c.numSteps = s.numSteps;
    // The vertical half-FOV reciprocal turns a world radius into pixels; the shader multiplies it
    // by the AO height, so it must be the VERTICAL one whatever the aspect ratio is. The camera
    // stores the HORIZONTAL fov, so derive it through the aspect ratio.
    const float aspect = static_cast<float>(renderer->GetRenderWidth()) /
                         static_cast<float>(std::max(1u, renderer->GetRenderHeight()));
    const float tanHalfV = std::tan(camera.GetHFov() * 0.5f) / std::max(aspect, 1e-4f);
    c.invTanHalfFovY = 1.0f / std::max(tanHalfV, 1e-4f);
    // Rotates the sampling directions frame to frame, which is what gives the temporal step
    // something to average instead of a fixed pattern. Committed in the builder.
    c.frameIndex = chain.frameIndex;
    c.useGBufferNormal = s.useGBufferNormal ? 1u : 0u;
    // P6C: only walk the pyramid if one was actually built this frame.
    c.useHzb = (s.useHzb && D.hzbMips > 0u) ? 1u : 0u;
    c.hzbMipBias = s.hzbMipBias;
    c.hzbMipCount = std::max(1u, D.hzbMips);
    // P16.4: the medium radius for the sky-fill channel. The kernel treats `skyRadius <=
    // worldRadius` as OFF and copies the contact answer into both channels, so the settings value
    // passes through untouched and one comparison in the shader is the whole gate.
    c.skyRadius = s.skyRadius;
    c.skyMipBias = s.skyMipBias;
    c.skyIntensity = s.skyIntensity; // 0 = the kernel's sky walk is a dead branch (exact no-op)

    // Shared by all three filter kernels; only the sizes and the stage's own field differ.
    GtaoFilterConstants f{};
    f.aoSize = c.aoSize;
    f.invAoSize = c.invAoSize;
    f.outSize = c.aoSize;
    f.invOutSize = c.invAoSize;
    f.depthA = c.depthA;
    f.depthB = c.depthB;
    f.planeTolerance = std::max(s.filterPlaneTolerance, 1e-4f);
    f.blendWeight = std::clamp(s.temporalBlendWeight, 0.0f, 1.0f);
    f.upsampleTolerance = std::max(s.upsampleTolerance, 1e-4f);
    f.historyValid = chain.historyValid ? 1u : 0u;
    f.filterRadius = std::min(s.filterRadius, 4u);
    f.temporalClampRange = std::clamp(s.temporalClampRange, 0.0f, 1.0f);

    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassGtao);
        const auto samplerDescs = std::array{ *SamplerManager::PointClamp(), *SamplerManager::LinearClamp() };
        const D3D12_GPU_DESCRIPTOR_HANDLE samplerTable =
            renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);

        // 1. raw horizon-search AO.
        renderer->EmitPoint(t.cl, chain.pointRaw);
        RecordComputeDispatch(renderer, t.cl, material.get(), cbSize,
            [&](uint8_t* dest) { resources_.WriteGtaoConstants(c, dest); },
            { D.depthSRV, D.gbSRV[1], D.hzbSRV },
            { D.gtaoUAV },
            samplerTable,
            aoW, aoH,
            D.gtao.Get());

        // The stage that produced what comes next. Kept as one variable so the record order and
        // the builder's declaration order cannot disagree about which target is being read.
        D3D12_CPU_DESCRIPTOR_HANDLE srcSRV = D.gtaoSRV;

        // 2. bilateral denoise.
        if (chain.denoise)
        {
            renderer->EmitPoint(t.cl, chain.pointDenoise);
            RecordComputeDispatch(renderer, t.cl, resources_.GetGtaoFilterMaterial().get(),
                resources_.GetGtaoFilterCBSizeBytes(),
                [&](uint8_t* dest) { resources_.WriteGtaoFilterConstants(f, dest); },
                { srcSRV, D.depthSRV, D.gbSRV[1] },
                { D.gtaoFilteredUAV },
                samplerTable,
                aoW, aoH,
                D.gtaoFiltered.Get());
            srcSRV = D.gtaoFilteredSRV;
        }

        // 3. temporal accumulation against the previous frame's result.
        if (chain.temporal)
        {
            renderer->EmitPoint(t.cl, chain.pointTemporal);
            RecordComputeDispatch(renderer, t.cl, resources_.GetGtaoTemporalMaterial().get(),
                resources_.GetGtaoTemporalCBSizeBytes(),
                [&](uint8_t* dest) { resources_.WriteGtaoTemporalConstants(f, dest); },
                { srcSRV, P.gtaoHistorySRV, D.gbSRV[3] },
                { D.gtaoHistoryUAV },
                samplerTable,
                aoW, aoH,
                D.gtaoHistory.Get());
            srcSRV = D.gtaoHistorySRV;
        }

        // 4. edge-aware upsample to the render resolution. Runs unconditionally: whatever the
        // chain produced, the consumers (P6B item 7) sample one target at one resolution.
        renderer->EmitPoint(t.cl, chain.pointUpsample);
        f.outSize = float2(static_cast<float>(renderer->GetRenderWidth()),
                           static_cast<float>(renderer->GetRenderHeight()));
        f.invOutSize = float2(1.0f / std::max(f.outSize.x, 1.0f), 1.0f / std::max(f.outSize.y, 1.0f));
        RecordComputeDispatch(renderer, t.cl, resources_.GetGtaoUpsampleMaterial().get(),
            resources_.GetGtaoUpsampleCBSizeBytes(),
            [&](uint8_t* dest) { resources_.WriteGtaoUpsampleConstants(f, dest); },
            { srcSRV, D.depthSRV },
            { D.gtaoUpsampledUAV },
            samplerTable,
            renderer->GetRenderWidth(), renderer->GetRenderHeight(),
            D.gtaoUpsampled.Get());

        // Back to the resting state the rest of the engine assumes for it.
        renderer->EmitPoint(t.cl, chain.pointRestore);
    }
    ctx.EndCL(t);
}

void SceneRenderer::Pass_Lighting(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera, std::uint32_t point)
{
    // pass-flow S5: no gates here. The AddPass2 builder ran the material / CB-size / SRV
    // readiness check ONCE and declared only on the frames this body records — the early-outs
    // that used to sit here fired AFTER the pass had declared, which under compiled barriers
    // leaves the compile a transition ahead of the GPU for every later reader of these targets.
    auto lighting = resources_.GetLightingMaterial();
    const UINT cbSize = resources_.GetLightingCBSizeBytes();

    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassLighting);
        const auto& D = renderer->GetDeferredForFrame();
        renderer->EmitPoint(t.cl, point);

        const DirectionalLight& dirLight = *frame_->dirLight;
        LightingPassConstants constants{};
        const mat4& view = camera.GetViewMatrix();
        const mat4& invView = camera.GetInvViewMatrix();
        const mat4& invProj = camera.GetInvProjMatrix();
        const float3 camDir = camera.GetDirection();
        constants.sunDir = dirLight.GetDirection();
        constants.ambient = dirLight.GetAmbient();
        // P4: the sun intensity rides in the colour. lighting_cs multiplies BOTH its fill term
        // (`ambient * lightRgb`) and its direct term by this, which is exactly how the legacy
        // trailing `* exposure` behaved -- hence the migration leaves `ambient` untouched.
        constants.lightRgb = dirLight.GetEffectiveColor();
        constants.ambientRgb = dirLight.GetEffectiveAmbientColor();
        // F8: a real sky fill whenever this level's sky brought prefiltered derivatives with it.
        const Skybox* iblSky = frame_->skybox;
        constants.skyIrradianceEnabled = (iblSky && iblSky->HasIbl()) ? 1u : 0u;
        // Sky intensity (how bright this sky is) times the fill strength (how much of it the
        // diffuse response takes). Two different questions, so two fields.
        // P6B items 6-7. The pass writes gtaoUpsampled only when it runs, so `enabled` gates the
        // read rather than relying on the target holding 1.
        constants.gtaoEnabled = frame_->settings.gtao.enabled ? 1u : 0u;
        constants.gtaoStrength = std::clamp(frame_->settings.gtao.strength, 0.0f, 1.0f);
        constants.skyIrradianceScale =
            (iblSky ? iblSky->GetExposure() : 1.0f) * dirLight.GetSkyFillIntensity();
        // P16.12: the other half of the fill -- what comes back UP off the ground. The shader
        // treats a zero here as "term off", so a level that wants none writes zero rather than
        // needing a second boolean.
        constants.groundAlbedoRgb = dirLight.GetGroundAlbedo();
        // The sky's indirect SPECULAR, moved out of compose so the screen-space reflection pass
        // (which samples this target) sees a metal with its environment on it. These three MUST
        // match compose's own values exactly -- one pass adds the term, the other subtracts the
        // part a reflection replaced, and they only cancel while both agree.
        constants.enableSkySpecular =
            frame_->settings.reflectionSource != ReflectionSource::None ? 1u : 0u;
        constants.skySpecMipCount = (iblSky && iblSky->HasIbl()) ? iblSky->GetSpecMips() : 0u;
        constants.skyboxIntensity = iblSky ? iblSky->GetExposure() : 1.0f;
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
        // Per-level depth-bias shaping (see VsmClipmapShadow). The floor is authored in TEXELS of
        // the landed level; NDC per texel = 1 / (6 * 2048): a level's depth range is 6x its extent
        // (Scene::UpdateClipmap, depthUp 5E + depthDown 1E) and its virtual res is 2048, and both
        // scale with the extent, so one conversion serves every level.
        constants.clipmapDepthBiasDecay = vsm::g_clipmapDepthBiasDecay;
        constants.clipmapDepthBiasFloorNdc =
            vsm::g_clipmapDepthBiasFloorTexels / (6.0f * (float)vsm::kVirtualRes);
        constants.clipmapBlendWidth = vsm::ClipmapBlendWidth();
        constants.clipmapBaseExtent = vsm::g_clipmapBaseExtent;
        // P16.16: UE divide their CVar by 1000 before it reaches the shader
        // (GetNormalBiasForShader, VirtualShadowMapArray.cpp:561). Same here, so the authored
        // number stays directly comparable to `r.Shadow.Virtual.NormalBias`.
        constants.clipmapNormalBias = vsm::g_clipmapNormalBias * 0.001f;
        if (frame_->clipmapViews)
        {
            for (size_t i = 0; i < constants.clipmapViewProj.size() && i < frame_->clipmapViews->size(); ++i)
            {
                const SceneView& cv = (*frame_->clipmapViews)[i];
                constants.clipmapViewProj[i] = cv.view * cv.proj;
            }
            // P16.16: the receiver-plane transform, built from level 0. The gradient the shader
            // takes from it is a ratio in which the level's extent cancels, so one matrix serves
            // every level -- same construction as UE's CalcTranslatedWorldToShadowUVNormalMatrix.
            constants.clipmapUvNormal = vsm::CalcClipmapUvNormalMatrix(constants.clipmapViewProj[0]);
        }

        const auto samplerDescs = std::array{ *SamplerManager::PointClamp(),
                                              *SamplerManager::ComparisonLinearClamp(),
                                              *SamplerManager::LinearWrap(),
                                              // s3: the BRDF LUT and the sky cubes, read exactly
                                              // as compose reads them -- clamped, not wrapped.
                                              *SamplerManager::LinearClamp() };
        RecordComputeDispatch(renderer, t.cl, lighting.get(), cbSize,
            [&](uint8_t* dest) { resources_.WriteLightingConstants(constants, dest); },
            { D.gbSRV[0], D.gbSRV[1], D.gbSRV[2], D.gbSRV[3], D.depthSRV, D.shadowSRV,
              vsmDir ? frame_->vsm->PageTableSrv() : renderer->VsmDummyBufferSrv(),  // t6 (inert in Legacy)
              vsmDir ? frame_->vsm->PagePoolSrv()  : renderer->VsmDummyTexSrv(),     // t7 (inert in Legacy)
              D.gbAuxSRV,                                                             // t8
              causticsSrv.ptr != 0 ? causticsSrv : renderer->VsmDummyTexSrv(),        // t9 (inert without water)
              // t10: F8 sky irradiance. The dummy keeps the VOLATILE table fully populated on a
              // level whose sky has no derivatives; `skyIrradianceEnabled` is 0 there, so it is
              // never sampled.
              (frame_->skybox && frame_->skybox->HasIbl())
                  ? frame_->skybox->GetIrradianceTex()->GetSRVCPU()
                  : renderer->VsmDummyTexSrv(),
              // t11: P6B dynamic AO at render resolution. Bound unconditionally to keep the
              // VOLATILE range fully populated; `gtaoEnabled` is 0 when the pass did not run, and
              // the shader does not sample it then -- which matters, because the target holds
              // whatever was last left in it rather than 1.
              D.gtaoUpsampledSRV,
              // t12/t13/t14: the sky specular set, mirroring compose. All three are bound
              // unconditionally to keep the VOLATILE range populated; `enableSkySpecular` and
              // `skySpecMipCount` decide whether any of them is sampled, exactly as in compose.
              (frame_->skybox && frame_->skybox->HasIbl())
                  ? frame_->skybox->GetSpecTex()->GetSRVCPU()
                  : renderer->VsmDummyTexSrv(),
              (frame_->skybox && frame_->skybox->HasIbl())
                  ? frame_->skybox->GetBrdfLut()->GetSRVCPU()
                  : renderer->VsmDummyTexSrv(),
              frame_->skybox ? frame_->skybox->GetTex()->GetSRVCPU() : renderer->VsmDummyTexSrv() },
            { D.lightUAV },
            renderer->GetSamplerManager()->GetTable(renderer, samplerDescs),
            renderer->GetRenderWidth(), renderer->GetRenderHeight(),
            D.light.Get());
    }
    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void SceneRenderer::Pass_SpotLights(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera, std::uint32_t point)
{
    // pass-flow S6: no gates here. The builder checked the light count, the light buffer, the
    // staged handles and VSM readiness ONCE and declared only on the frames this body records —
    // every one of those early `return`s used to fire AFTER the pass had declared its ten states,
    // which under compiled barriers leaves the compile a transition ahead of the GPU.
    LightManager& lightManager = *frame_->lightManager;
    const size_t spotLightCount = lightManager.GetSpotLightCount();
    const UINT frameIdx = renderer->GetCurrentFrameIndex();
    auto* spotLightBufferCPU = lightManager.GetSpotLightBufferCPU(frameIdx);
    const D3D12_CPU_DESCRIPTOR_HANDLE spotLightSrvHandle = lightManager.GetSpotLightSrv(frameIdx);

    // Rung 2 / Step 21+24b: the shader's root sig always binds t7 (VSM page table) + t8 (VSM pool).
    // In VSM mode they are resident (the builder refuses the frame otherwise); in Legacy mode the
    // pool is freed, so bind inert dummy SRVs — the shader's useVsm=0 branch never samples them.
    const bool vsmSample = render::VsmActive();
    const bool vsmReady = frame_->vsm && frame_->vsm->IsAllocated() &&
                          frame_->vsm->PageTableSrv().ptr != 0 && frame_->vsm->PagePoolSrv().ptr != 0;

    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSpotLights);
        const auto& D = renderer->GetDeferredForFrame();
        renderer->EmitPoint(t.cl, point);

        const auto& spotLights = lightManager.SpotLights();
        for (size_t i = 0; i < spotLightCount; ++i)
        {
            const auto& light = spotLights[i];
            const auto& desc = light.GetDesc();
            const mat4 viewProj = light.GetViewProjMatrix();
            const float3 dir = light.GetDirection();

            spotLightBufferCPU[i].positionRange = float4(desc.position, desc.range);
            spotLightBufferCPU[i].directionCosOuter = float4(dir, light.GetCosOuter());
            spotLightBufferCPU[i].colorIntensity =
                float4(desc.color, render::CandelaFromLumens(desc.luminousFluxLm)); // P16.5
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
    const Camera& camera, std::uint32_t point)
{
    // pass-flow S6: same as Pass_SpotLights — every readiness check now lives in the builder,
    // which declares only on the frames this body records.
    LightManager& lightManager = *frame_->lightManager;
    auto& pointLights = lightManager.PointLights();
    const UINT frameIdx = renderer->GetCurrentFrameIndex();
    auto* pointLightBufferCPU = lightManager.GetPointLightBufferCPU(frameIdx);
    const D3D12_CPU_DESCRIPTOR_HANDLE pointLightSrvHandle = lightManager.GetPointLightSrv(frameIdx);

    // Rung 2 / Step 21+24b: the shader always binds t7 (VSM page table) + t8 (VSM pool). Bind inert
    // dummy SRVs in Legacy mode (freed pool) — useVsm=0 never samples them.
    const bool vsmSample = render::VsmActive();
    const bool vsmReady = frame_->vsm && frame_->vsm->IsAllocated() &&
                          frame_->vsm->PageTableSrv().ptr != 0 && frame_->vsm->PagePoolSrv().ptr != 0;

    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassPointLights);

        const auto& D = renderer->GetDeferredForFrame();
        renderer->EmitPoint(t.cl, point);

        for (size_t i = 0; i < pointLights.size(); ++i)
        {
            const auto& desc = pointLights[i].GetDesc();
            pointLightBufferCPU[i].position = desc.position;
            pointLightBufferCPU[i].radius = desc.radius;
            pointLightBufferCPU[i].color = desc.color;
            pointLightBufferCPU[i].intensity = render::CandelaFromLumens(desc.luminousFluxLm); // P16.5
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
    const Camera& camera, std::uint32_t point)
{
    // pass-flow S5: the `frame_->skybox` gate moved into the builder — it used to early-out AFTER
    // the pass had declared light/velocity as RENDER_TARGET and depth as DEPTH_READ.
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSkybox);

        renderer->EmitPoint(t.cl, point);

        // RTVs = Light + Velocity, DSV = GBuffer Depth (read-only), no clears
        renderer->BindLightTargetWithVelocity(t.cl, Renderer::ClearMode::None, true);

        frame_->skybox->Render(renderer, t.cl, camera, 0); // skybox has no b1; viewCB ignored
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

// P6C step 6. The HiZ tracer's half of the SSR constants, in one place because the opaque and the
// glass dispatch both need it and a second copy is a second thing to forget.
//
// `useHzb` is the UE-technique selector. The tracer reads the furthest-depth chain that GTAO already
// builds every frame; the closest chain is not part of this path.
//
// KNOWN APPROXIMATION: the pyramid's mip 0 is ceil(renderWidth/2), so at an ODD render width it
// covers half a texel more than the depth buffer and the screen->pyramid UV mapping is off by
// 1/renderWidth at the far edge. UE carry an explicit factor for this because their HZB covers an
// arbitrary viewport rect; ours always covers the whole target, the error is sub-texel, and it
// moves where a ray samples rather than whether it hits.
void SceneRenderer::FillSsrHzbConstants(Renderer* renderer, SsrPassConstants& c) const
{
    const auto& D = renderer->GetDeferredForFrame();
    // The FURTHEST chain, which is built on every frame -- so this is really just "does the pyramid
    // exist", and the fallback to the log march only ever fires before the first build.
    const bool ready = D.hzb.Get() != nullptr && D.hzbSRV.ptr != 0 && D.hzbMips > 0u;
    c.useHzb = ready ? 1u : 0u;
    c.hzbMipCount = std::max(1u, D.hzbMips);
    c.hzbSize = float2(static_cast<float>(D.hzbWidth), static_cast<float>(D.hzbHeight));
    c.hzbInvSize = float2(D.hzbWidth > 0u ? 1.0f / static_cast<float>(D.hzbWidth) : 0.0f,
                          D.hzbHeight > 0u ? 1.0f / static_cast<float>(D.hzbHeight) : 0.0f);
}

void SceneRenderer::FillSsrUeConstants(SsrPassConstants& c, bool useRoughnessTexture) const
{
    const UeSsrSettings& s = frame_->settings.ssrUe;
    const ResolvedUeSsrSettings r = ResolveUeSsrSettings(s);
    c.ueNumSteps = r.numSteps;
    c.ueNumRays = r.numRays;
    c.ueGlossyRays = r.glossyRays;
    c.ueStartMipLevel = r.startMipLevel;
    c.ueSlopeCompareToleranceScale = r.slopeCompareToleranceScale;
    c.ueConfirmRetries = r.confirmRetries;
    c.ueRefineSteps = r.refineSteps;
    c.ueUseRoughnessTexture = useRoughnessTexture && s.useSurfaceRoughness ? 1u : 0u;
    c.ueRoughnessOverride = std::clamp(s.roughnessOverride, 0.0f, 1.0f);
}

void SceneRenderer::FillSsrReprojectionConstants(const Camera& camera, SsrPassConstants& c) const
{
    // Row-vector convention, matching UE's View.ClipToPrevClip transform:
    // current clip -> current view -> world -> previous view -> previous clip.
    c.clipToPrevClip = camera.GetInvProjMatrix() * camera.GetInvViewMatrix() *
        camera.GetPrevViewMatrix() * camera.GetPrevProjMatrix();
    c.sceneColorHistoryValid = ssrSceneColorHistoryValid_ ? 1u : 0u;
    // P16.1: the history was written with LAST frame's factor, not this one's. They differ only
    // while the exposure is moving, which is exactly when a reflection would otherwise flicker.
    c.invPrevPreExposure = 1.0f / std::max(prevPreExposure_, 1.0e-8f);
    c.preExposure = preExposure_; // P16.8: the multi-ray compression runs in pre-exposed space

    // Whether the history is being read AT ALL, in the log. Only the UE march resolves its hits
    // from the history; the default LogMarch takes its colour from the light target and never
    // touches the line above. Without this an A/B that exercised neither would look like a clean
    // result, which is exactly the shape of a verification that proves nothing.
    {
        const bool ueMarch = frame_ && frame_->settings.ssrTechnique == SsrTechnique::UeHzb;
        char msg[192];
        std::snprintf(msg, sizeof(msg),
                      "[p16] ssr scene-colour history %s   prevPreExposure ~2^%.0f\n",
                      (ueMarch && c.sceneColorHistoryValid != 0u)
                          ? "READ (hits are pre-exposed, undone in-shader)"
                          : "not read (hits come from the light target)",
                      std::floor(std::log2(std::max(prevPreExposure_, 1.0e-8f))));
        Renderer::DiagLogOnce(msg);
    }
}

void SceneRenderer::Pass_ScreenSpaceReflections(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera, std::uint32_t point)
{
    //return;
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassReflectionSource);
        const auto& D = renderer->GetDeferredForFrame();
        renderer->EmitPoint(t.cl, point);

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
        constants.frameIndexMod8 = static_cast<uint32_t>(renderer->GetTotalFrameNumber() & 7ull);
        FillSsrHzbConstants(renderer, constants);
        FillSsrUeConstants(constants, true);
        FillSsrReprojectionConstants(camera, constants);

        const auto& P = renderer->GetDeferredForPrevFrame();
        const auto samplerDescs = std::array{ *SamplerManager::LinearClamp(), *SamplerManager::PointClamp() };
        RecordComputeDispatch(renderer, t.cl, ssrMaterial.get(), cbSize,
            [&](uint8_t* dest) { resources_.WriteSsrConstants(constants, dest); },
            // t0 Light, t1 GB1, t2 march depth, t3 origin depth (==t2), t4 HZB,
            // t5 previous full-HDR SceneColor, t6 current motion vectors, t7 GB0 roughness.
            { D.lightSRV, D.gbSRV[1], D.depthSRV, D.depthSRV, D.hzbSRV,
              P.sceneSRV, D.gbSRV[3], D.gbSRV[0] },
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
    uint32_t skyIrradianceIndex = 0; float skyIrradianceScale = 1.0f; uint32_t rtPad0 = 0, rtPad1 = 0; // P16.9
    // P16.12: mirrors lighting_cs. Off-screen hits are re-shaded in the RT pass, so the bounce has
    // to reach both or a reflection is about a stop darker than the surface it reflects.
    Math::float3 groundAlbedoRgb{ 0.25f, 0.25f, 0.25f }; float rtPadGround = 0.0f;
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
    const Camera& camera, std::uint32_t point)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassRTReflections);
        const auto& D = renderer->GetDeferredForFrame();
        renderer->EmitPoint(t.cl, point); // depth/gb1/light -> NPS, reflection scratch -> UAV

        auto reflectMaterial = resources_.GetRtReflectMaterial();
        const UINT frameIndex = renderer->GetCurrentFrameIndex();
        const D3D12_CPU_DESCRIPTOR_HANDLE tlasSrv = asManager_.TlasSrvCpu(frameIndex);
        Skybox* skybox = frame_->skybox;
        if (!reflectMaterial || !bindless_.FrameReady(frameIndex) || tlasSrv.ptr == 0 ||
            asManager_.TlasInstanceCount(frameIndex) == 0 || !frame_->dirLight || !skybox)
        {
            // No usable TLAS/bindless/light/skybox this frame: leave reflection as is.
            break;
        }

        // Per-frame scene descriptors into the bindless heap (VB/IB descriptors are immutable;
        // geometry-info has a separate buffer/SRV per frame, populated in Pass_BuildAS). Scene slots
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
        // P16.9: the cosine-convolved irradiance, so an OFF-SCREEN re-shade gets the same sky
        // fill the main pass uses instead of the legacy `ambient * sunColour` fraction.
        const bool haveSkyIrradiance = skybox->HasIbl();
        if (haveSkyIrradiance)
        {
            bindless_.WriteSceneDescriptor(frameIndex, 8, skybox->GetIrradianceTex()->GetSRVCPU());
        }

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
        c.lightRgb = dl.GetEffectiveColor(); // P4: sun intensity folded in, see lighting_cs
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
        c.skyIrradianceIndex = haveSkyIrradiance ? bindless_.SceneIndex(frameIndex, 8) : 0u; // P16.9
        c.skyIrradianceScale = skybox->GetExposure() * dl.GetSkyFillIntensity();
        c.groundAlbedoRgb = dl.GetGroundAlbedo(); // P16.12, same value lighting_cs gets
        c.geomInfoIndex = bindless_.GeomInfoIndex(frameIndex);
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
    const Camera& camera, const SceneView& mainView, std::uint32_t point)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassGlassReflGbuffer);
        const auto& D = renderer->GetDeferredForFrame();
        // pass-flow S5: the prepass-material gate lives in the builder now — breaking out HERE
        // meant the pass had declared glassReflNormal -> RTV and glassReflDepth -> DEPTH_WRITE
        // and then emitted neither.
        auto prepassMat = resources_.GetGlassReflPrepassMaterial();
        renderer->EmitPoint(t.cl, point); // glassReflNormal -> RTV, glassReflDepth -> DEPTH_WRITE

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
void SceneRenderer::Pass_GlassReflections(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera, std::uint32_t point)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassGlassReflections);
        const auto& D = renderer->GetDeferredForFrame();
        renderer->EmitPoint(t.cl, point); // glassReflNormal/Depth/light/depth -> NPS, glassReflection -> UAV

        auto reflectMaterial = resources_.GetRtReflectMaterial();
        const UINT frameIndex = renderer->GetCurrentFrameIndex();
        const D3D12_CPU_DESCRIPTOR_HANDLE tlasSrv = asManager_.TlasSrvCpu(frameIndex);
        Skybox* skybox = frame_->skybox;
        if (!reflectMaterial || !bindless_.FrameReady(frameIndex) || tlasSrv.ptr == 0 ||
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
        c.lightRgb = dl.GetEffectiveColor(); // P4: sun intensity folded in, see lighting_cs
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
        c.geomInfoIndex = bindless_.GeomInfoIndex(frameIndex);
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
void SceneRenderer::Pass_GlassReflectionsSSR(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera, std::uint32_t point)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassGlassReflections);
        const auto& D = renderer->GetDeferredForFrame();
        renderer->EmitPoint(t.cl, point);

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
        constants.frameIndexMod8 = static_cast<uint32_t>(renderer->GetTotalFrameNumber() & 7ull);
        FillSsrHzbConstants(renderer, constants);
        // The glass prepass stores only its normal+depth. Keep the historical mirror assumption
        // until its material roughness is added to that compact G-buffer.
        FillSsrUeConstants(constants, false);
        FillSsrReprojectionConstants(camera, constants);

        const auto& P = renderer->GetDeferredForPrevFrame();
        const auto samplerDescs = std::array{ *SamplerManager::LinearClamp(), *SamplerManager::PointClamp() };
        RecordComputeDispatch(renderer, t.cl, ssrMaterial.get(), cbSize,
            [&](uint8_t* dest) { resources_.WriteSsrConstants(constants, dest); },
            // t0 lit, t1 glass normal, t2 opaque(march), t3 glass(origin), t4 HZB,
            // t5 previous full-HDR SceneColor, t6 current opaque velocity, t7 unused GB0.
            { D.lightSRV, D.glassReflNormalSRV, D.depthSRV, D.glassReflDepthSRV, D.hzbSRV,
              P.sceneSRV, D.gbSRV[3], D.gbSRV[0] },
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

void SceneRenderer::Pass_ClearReflections(Renderer* renderer, RenderGraphPassContext ctx,
    std::uint32_t point)
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
        renderer->EmitPoint(t.cl, point); // reflection -> UNORDERED_ACCESS

        renderer->BindDescriptorHeaps(t.cl);
        const GpuDescHandle uav = renderer->StageSrvUavTable({ D.reflectionUAV });
        const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        t.cl->ClearUnorderedAccessViewFloat(uav.gpu, D.reflectionUAV, D.reflection.Get(), zero, 0, nullptr);
    }
    ctx.EndCL(t);
}

// SSR temporal resolve. Reads this frame's raw reflection and the PREVIOUS frame's accumulation,
// writes the accumulation for this frame -- which is both what the blur consumes and what the next
// frame reads back. Unreal run their SSR through TAA as ETAAPassConfig::ScreenSpaceReflections for
// the same reason; see the shader for the configuration that comes from.
void SceneRenderer::Pass_SsrTemporal(Renderer* renderer, RenderGraphPassContext ctx,
    std::uint32_t point)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassReflectionTemporal);
        const auto& D = renderer->GetDeferredForFrame();
        const auto& P = renderer->GetDeferredForPrevFrame();
        renderer->EmitPoint(t.cl, point);

        auto material = resources_.GetSsrTemporalMaterial();
        const UINT cbSize = resources_.GetSsrTemporalCBSizeBytes();
        if (!material || cbSize == 0)
        {
            break;
        }

        const UINT w = renderer->GetReflectionTextureWidth();
        const UINT h = renderer->GetReflectionTextureHeight();

        SsrTemporalConstants c{};
        c.texSize = float2(static_cast<float>(w), static_cast<float>(h));
        c.invTexSize = float2(w > 0 ? 1.0f / static_cast<float>(w) : 0.0f,
                              h > 0 ? 1.0f / static_cast<float>(h) : 0.0f);
        c.blendWeight = std::clamp(frame_->settings.ssrTemporalBlendWeight, 0.01f, 1.0f);
        c.clampExpand = std::max(0.0f, frame_->settings.ssrTemporalClampExpand);
        // A history that does not exist yet is worse than none: seeding from this frame is what
        // makes the first frame after a resize or a level switch noisy-but-correct instead of
        // whatever the texture happened to hold.
        c.historyValid = ssrHistoryValid_ ? 1u : 0u;

        const auto samplerDescs = std::array{ *SamplerManager::PointClamp(),
                                              *SamplerManager::LinearClamp() };
        RecordComputeDispatch(renderer, t.cl, material.get(), cbSize,
            [&](uint8_t* dest) { resources_.WriteSsrTemporalConstants(c, dest); },
            { D.reflectionSRV, P.reflectionHistorySRV, D.gbSRV[3] }, // t0 raw, t1 history, t2 velocity
            { D.reflectionHistoryUAV },
            renderer->GetSamplerManager()->GetTable(renderer, samplerDescs),
            w, h,
            D.reflectionHistory.Get());
    } while (false);
    ctx.EndCL(t);
}

void SceneRenderer::Pass_ReflectionBlur(Renderer* renderer, RenderGraphPassContext ctx,
    const BlurPoints& pts)
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
        renderer->EmitPoint(t.cl, pts.apply);

        // pass-flow S6: `pts.blur` IS the material/CB check — made once in the builder, which
        // declared the ping-pong point from the same value.
        auto blurMaterial = resources_.GetBlurMaterial();
        const UINT cbSize = resources_.GetBlurCBSizeBytes();
        if (!pts.blur)
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
            // The temporal resolve, when it ran, leaves its result in reflectionHistory -- so that
            // is the blur's input. The second (vertical) tap still lands in `reflection`, which is
            // what compose reads, so nothing downstream changes.
            { ssrTemporalActive_ ? D.reflectionHistorySRV : D.reflectionSRV, D.gbSRV[0] },
            { D.reflectionScratchUAV }, samplerTable, // t0 reflection, t1 GB0 (roughness)
            ssrWidth, ssrHeight,
            D.reflectionScratch.Get());

        // Vertical pass
        renderer->EmitPoint(t.cl, pts.pingPong);

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
    const Camera& camera, std::uint32_t apply, std::uint32_t handBack)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassCompose);
        const auto& D = renderer->GetDeferredForFrame();
        renderer->EmitPoint(t.cl, apply);

        const float width = static_cast<float>(renderer->GetRenderWidth());
        const float height = static_cast<float>(renderer->GetRenderHeight());
        if (width <= 0.0f || height <= 0.0f)
        {
            renderer->EmitPoint(t.cl, handBack);
            break;
        }

        auto composeMaterial = resources_.GetComposeMaterial();
        const UINT cbSize = resources_.GetComposeCBSizeBytes();
        Skybox* skybox = frame_->skybox;
        if (!composeMaterial || cbSize == 0 || !skybox || D.gbAuxSRV.ptr == 0)
        {
            renderer->EmitPoint(t.cl, handBack);
            break;
        }

        ComposePassConstants constants{};
        constants.invView = camera.GetInvViewMatrix();
        constants.invProj = camera.GetInvProjMatrix();
        constants.skyboxIntensity = skybox->GetExposure();
        constants.camPos = camera.GetPosition();
        constants.enableSkySpecular =
            frame_->settings.reflectionSource != ReflectionSource::None ? 1u : 0u;
        // F8: non-zero switches compose onto the split-sum path. Zero when this sky has no
        // prefiltered derivatives, which keeps every pre-F7 level rendering exactly as it did.
        constants.skySpecMipCount = skybox->HasIbl() ? skybox->GetSpecMips() : 0u;
        constants.gtaoEnabled = frame_->settings.gtao.enabled ? 1u : 0u;
        constants.gtaoStrength = std::clamp(frame_->settings.gtao.strength, 0.0f, 1.0f);
        constants.screenSize = float2(width, height);
        constants.invScreenSize = float2(1.0f / width, 1.0f / height);

        // P7 aerial perspective. Disabled, or with no directional light to colour the scattering,
        // the density goes to zero and compose skips the block entirely -- the whole feature is
        // gated on that one number, so "off" is genuinely the pre-P7 image and not a near-miss.
        {
            const AtmospherePacked fog =
                PackAtmosphere(frame_->settings.atmosphere, frame_->dirLight != nullptr);
            constants.fogParams0 = fog.params0;
            constants.fogParams1 = fog.params1;
            constants.fogParams2 = fog.params2;
            constants.fogDebugView = g_atmosphereDebugView;
            constants.preExposure = preExposure_;   // P16.1
            if (frame_->dirLight)
            {
                // GetDirection() is the direction the light TRAVELS; the scattering lobe is keyed
                // on the angle between the view ray and the direction TO the sun, so negate here
                // rather than in the shader where the sign would be one more thing to get wrong.
                const float3 toSun = -frame_->dirLight->GetDirection();
                constants.fogSunDir = float4(toSun, 0.0f);
                constants.fogSunColor = float4(frame_->dirLight->GetEffectiveColor(), 0.0f);
            }
        }

        D3D12_CPU_DESCRIPTOR_HANDLE wetnessSrv = D.depthSRV;
        if (frame_->ocean)
        {
            constants.shoreWetnessWindow = frame_->ocean->GetWetnessComposeWindow();
            constants.shoreWetnessAppearance = frame_->ocean->GetWetnessComposeAppearance();
            constants.shoreWetnessFallback = frame_->ocean->GetWetnessComposeFallback();
            constants.shoreWetnessBreakup = frame_->ocean->GetWetnessComposeBreakup();
            if (frame_->ocean->IsWetnessReady())
            {
                wetnessSrv = frame_->ocean->GetWetnessSrv();
            }
        }

        const auto samplerDescs = std::array{ *SamplerManager::LinearClamp(), *SamplerManager::PointClamp() };
        RecordComputeDispatch(renderer, t.cl, composeMaterial.get(), cbSize,
            [&](uint8_t* dest) { resources_.WriteComposeConstants(constants, dest); },
            // t9..t11 are the F8 IBL set. When this sky has none they are filled with the display
            // cube and the depth SRV: the table must be fully populated (a null descriptor in a
            // VOLATILE range is undefined), and the shader never reads them because
            // `skySpecMipCount` is 0.
            { D.lightSRV, D.gbSRV[2], D.gbSRV[0], D.gbSRV[1], D.depthSRV,
              skybox->GetTex()->GetSRVCPU(), D.reflectionSRV, D.gbAuxSRV, wetnessSrv,
              skybox->HasIbl() ? skybox->GetSpecTex()->GetSRVCPU() : skybox->GetTex()->GetSRVCPU(),
              skybox->HasIbl() ? skybox->GetIrradianceTex()->GetSRVCPU() : skybox->GetTex()->GetSRVCPU(),
              skybox->HasIbl() ? skybox->GetBrdfLut()->GetSRVCPU() : D.depthSRV,
              D.gtaoUpsampledSRV }, // t12: P6B dynamic AO, gated by `gtaoEnabled`
            { D.sceneUAV },
            renderer->GetSamplerManager()->GetTable(renderer, samplerDescs),
            renderer->GetRenderWidth(), renderer->GetRenderHeight(),
            D.scene.Get());

        renderer->EmitPoint(t.cl, handBack);
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
    const Camera& camera, const RtDebugPoints& pts)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassRTDebug);
        const auto& D = renderer->GetDeferredForFrame();
        renderer->EmitPoint(t.cl, pts.apply); // reflection -> UAV, gb1/depth -> NPS

        // pass-flow S5: the TLAS / bindless / material readiness is the BUILDER'S decision now
        // (`pts.trace`), for two reasons. It gated the trailing restore transition below, which
        // the old Prepare never registered at all — so with the RT debug view on, that barrier
        // was silently dropped and `reflection` ended the frame in UNORDERED_ACCESS instead of
        // its canonical read state. And re-deciding it here could only disagree with what the
        // pass declared.
        auto debugMaterial = resources_.GetRtDebugMaterial();
        const UINT frameIndex = renderer->GetCurrentFrameIndex();
        const D3D12_CPU_DESCRIPTOR_HANDLE tlasSrv = asManager_.TlasSrvCpu(frameIndex);
        if (!pts.trace)
        {
            break; // no TLAS / bindless table this frame - leave reflection as compose left it
        }

        // Copy this frame's scene descriptors into the persistent bindless heap so
        // the shader can reach them via ResourceDescriptorHeap[]. Geometry VB/IB descriptors are
        // immutable; Pass_BuildAS uploads the geometry-info buffer/SRV for this frame slot only.
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
        c.geomInfoIndex = bindless_.GeomInfoIndex(frameIndex);
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
        // inspector reads it exactly as it would the normal SSR result. Declared by the builder
        // as its own point (only on the frames that trace), emitted here as a marker.
        renderer->EmitPoint(t.cl, pts.restore);
    } while (false);
    ctx.EndCL(t);
}

void SceneRenderer::RecordOceanReflection(Renderer* renderer, ID3D12GraphicsCommandList* cl,
    const Camera& camera, const TransparentPoints& pts)
{
    GPU_SCOPE(cl, ProfilerScopes::kPassOceanReflection);

    const auto& D = renderer->GetDeferredForFrame();
    // pass-flow S7d: `pts.oceanReflect` is the builder's single answer to "will this compute run"
    // — targets present, material compiled, CB sized, all three descriptors staged. The two
    // early-outs that used to ask those questions HERE both ran after the pass had declared the
    // read point, and the second one had already performed its transitions.
    if (!pts.oceanReflect)
    {
        renderer->EmitPoint(cl, pts.pixel);
        return;
    }

    // P13: the pyramid is already in its resting state, so its entry compiles to no barrier --
    // declaring it is what keeps the pass's declaration and its command list telling one story.
    renderer->EmitPoint(cl, pts.oceanRead);

    auto material = resources_.GetOceanReflectionMaterial();
    const UINT cbSize = resources_.GetOceanReflectionCBSizeBytes();

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

    // P13. Same technique switch, same pyramid, same phase sequence as Pass_ScreenSpaceReflections
    // -- so `ssr.technique` moves the ocean with everything else instead of leaving one surface on
    // the old search. `useHzb` is "does a pyramid exist", which is what makes the log march the
    // automatic fallback on the first frame.
    const bool hzbReady = D.hzb.Get() != nullptr && D.hzbSRV.ptr != 0 && D.hzbMips > 0u;
    const ResolvedUeSsrSettings ue = ResolveUeSsrSettings(frame_->settings.ssrUe);
    constants.technique = static_cast<uint32_t>(frame_->settings.ssrTechnique);
    constants.useHzb = hzbReady ? 1u : 0u;
    constants.hzbMipCount = std::max(1u, D.hzbMips);
    constants.frameIndexMod8 = static_cast<uint32_t>(renderer->GetTotalFrameNumber() & 7ull);
    constants.hzbSize = float2(static_cast<float>(D.hzbWidth), static_cast<float>(D.hzbHeight));
    constants.hzbInvSize = float2(D.hzbWidth > 0u ? 1.0f / static_cast<float>(D.hzbWidth) : 0.0f,
        D.hzbHeight > 0u ? 1.0f / static_cast<float>(D.hzbHeight) : 0.0f);
    constants.ueStartMipLevel = ue.startMipLevel;
    constants.ueSlopeCompareToleranceScale = ue.slopeCompareToleranceScale;
    constants.ueConfirmRetries = ue.confirmRetries;
    constants.ueRefineSteps = ue.refineSteps;
    constants.ueNumSteps = UeSsrMirrorRaySteps(ue);

    const auto samplerDescs = std::array{ *SamplerManager::LinearClamp(), *SamplerManager::PointClamp() };
    RecordComputeDispatch(renderer, cl, material.get(), cbSize,
        [&](uint8_t* dest) { std::memcpy(dest, &constants, sizeof(constants)); },
        // t0 opaque scene colour, t1 opaque depth copy, t2 the FURTHEST pyramid (P13).
        { D.sceneOpaqueSRV, D.depthCopySRV, hzbReady ? D.hzbSRV : D.depthCopySRV },
        { D.oceanReflectionUAV },
        renderer->GetSamplerManager()->GetTable(renderer, samplerDescs),
        renderer->GetOceanReflectionTextureWidth(), renderer->GetOceanReflectionTextureHeight(),
        D.oceanReflection.Get());

    // All three become PS-readable for the forward draws — on every path out of this function.
    renderer->EmitPoint(cl, pts.pixel);
}

void SceneRenderer::Pass_Transparent(Renderer* renderer, RenderGraphPassContext ctx,
    const Camera& camera, const SceneView& mainView, const TransparentPoints& pts)
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

void SceneRenderer::Pass_ExposureMetering(Renderer* renderer, RenderGraphPassContext ctx,
    const ExposurePoints& pts)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassExposureMetering);
        // UNCONDITIONAL, and it has to be: this hands `scene` to the metering read on every
        // frame, including the ones where the camera is dormant and nothing else happens here.
        renderer->EmitPoint(t.cl, pts.apply);

        // Dormant camera (or metering not ready): no dispatches at all. The pass still exists in
        // the graph so its shape (and the barrier compile's cache key) does not change frame to
        // frame; what it costs then is one empty command list, not GPU work. The decision itself
        // is the builder's — see ExposurePoints.
        if (!pts.meter)
        {
            break;
        }

        ExposureMetering& metering = renderer->Exposure();
        auto clearMat = resources_.GetExposureClearMaterial();
        auto buildMat = resources_.GetExposureBuildMaterial();
        auto solveMat = resources_.GetExposureSolveMaterial();

        // P16.1 INSTRUMENT. The peak luminance actually written to scene colour, read from the
        // histogram the metering already builds -- nothing extra is computed. This is the number
        // pre-exposure has to move: raw radiance today, near 1 afterwards, and the FP16 target it
        // is written to tops out at 65504 either way.
        //
        // Printed to the nearest STOP so a settled scene emits one line: DiagLogOnce dedupes by the
        // exact string, and an unrounded value changes every frame.
        {
            static float bins[ExposureMetering::kHistogramBins] = {};
            UINT total = 0;
            if (metering.LatestHistogram(bins, ExposureMetering::kHistogramBins, &total) && total > 0)
            {
                int top = -1;
                for (int i = static_cast<int>(ExposureMetering::kHistogramBins) - 1; i >= 0; --i)
                {
                    if (bins[i] > 0.0f) { top = i; break; }
                }
                if (top >= 0)
                {
                    const float minLog = ExposureMeteringConstants::kMinLogLum;
                    const float maxLog = ExposureMeteringConstants::kMaxLogLum;
                    const float f = (static_cast<float>(top) + 0.5f) /
                                    static_cast<float>(ExposureMetering::kHistogramBins - 1u);
                    const float logLum = minLog + f * (maxLog - minLog);
                    // The histogram is built from scene colour with the pre-exposure DIVIDED BACK
                    // OUT (it has to be, or the metering feeds itself), so what it reports is the
                    // scene's own radiance either way -- it cannot see the gate. The number the
                    // gate actually moves is the one STORED in the FP16 target, which is that peak
                    // times the factor, so both are printed along with the factor itself. Without
                    // the factor on the line there is no evidence in the log that the gate was
                    // even live, and a transparent A/B would be indistinguishable from a vacuous
                    // one.
                    const float preExpLog = std::log2(std::max(preExposure_, 1.0e-8f));
                    // P16.2: the METERED EV100 next to it, because that is the number the unit
                    // claim is falsifiable against -- physical light units mean a sunlit scene must
                    // meter near a real photographic EV (sunny-16 territory, 14-15), not the 4 this
                    // engine has been sitting at. Quantised to half a stop so a settled frame emits
                    // one line through DiagLogOnce instead of one per adaptation step.
                    const float ev = metering.LatestReadback().adaptedEv100;
                    char msg[260];
                    std::snprintf(msg, sizeof(msg),
                                  "[p16] scene peak luminance ~2^%.0f = %.0f raw"
                                  "   metered EV100 %.1f   preExposure ~2^%.0f   stored ~2^%.0f"
                                  "   (FP16 ceiling 65504)\n",
                                  std::floor(logLum), std::exp2(std::floor(logLum)),
                                  std::isfinite(ev) ? std::round(ev * 2.0f) * 0.5f : 0.0f,
                                  std::floor(preExpLog), std::floor(logLum + preExpLog));
                    Renderer::DiagLogOnce(msg);
                }
            }
        }

        const auto& D = renderer->GetDeferredForFrame();
        renderer->BindDescriptorHeaps(t.cl);
        const auto samplers = std::array{ *SamplerManager::LinearClamp() };
        const D3D12_GPU_DESCRIPTOR_HANDLE samplerTable =
            renderer->GetSamplerManager()->GetTable(renderer, samplers);

        // 1) Zero the bins. A dedicated dispatch rather than ClearUnorderedAccessViewUint, which
        // needs a shader-visible descriptor for the UAV and rejects some buffer layouts outright.
        // One 8x8 group covering 256 bins is not worth optimising further.
        RecordComputeDispatch(renderer, t.cl, clearMat.get(),
            { }, { metering.HistogramUav() }, samplerTable,
            kComputeDispatchGroupSize, kComputeDispatchGroupSize,
            metering.HistogramResource());

        // The histogram build and the solve read one shared block, so it is filled before either
        // dispatch -- the metering mask belongs to the build, the percentiles to the solve.
        ExposureMeteringConstants constants{};
        const render::CameraExposureSettings& settings = frame_->cameraExposure;
        constants.maskStrength = settings.meterMaskStrength;
        constants.maskInnerRadius = settings.meterMaskInnerRadius;
        constants.maskOuterRadius = settings.meterMaskOuterRadius;
        constants.maskSkyBias = settings.meterMaskSkyBias;

        // 2) Accumulate. The source is the scene-referred HDR image BEFORE the tone curve and
        // before any exposure has been applied to it.
        const UINT histogramCb = resources_.GetExposureHistogramCBSizeBytes();
        RecordComputeDispatch(renderer, t.cl, buildMat.get(), histogramCb,
            [this, &constants](uint8_t* dest) {
                resources_.WriteExposureHistogramConstants(constants, dest);
            },
            { D.sceneSRV }, { metering.HistogramUav() }, samplerTable,
            ExposureMeteringConstants::kSampleGridX, ExposureMeteringConstants::kSampleGridY,
            metering.HistogramResource());

        // 2b) P3B base log-luminance for local exposure. Written here rather than in a pass of
        // its own because it reads exactly the source the histogram just read, so it costs no
        // extra command list, barrier or scheduling node.
        if (auto baseLumMat = pts.baseLum ? resources_.GetExposureBaseLumMaterial() : nullptr)
        {
            // Its UAV state was declared at `apply` and emitted with it, so there is nothing to
            // ask for here.
            RecordComputeDispatch(renderer, t.cl, baseLumMat.get(),
                resources_.GetExposureBaseLumCBSizeBytes(),
                [this](uint8_t* dest) { resources_.WriteExposureBaseLumConstants(dest); },
                { D.sceneSRV }, { metering.BaseLumUav() }, samplerTable,
                ExposureMetering::kBaseLumWidth, ExposureMetering::kBaseLumHeight,
                metering.BaseLumResource());
            // Straight back to its resting READ state, so the tonemap samples it with no barrier.
            renderer->EmitPoint(t.cl, pts.baseLumRead);
        }

        // 3) Solve + adapt.
        constants.compensationEv = settings.compensationEv;
        constants.manualCompensationEv = settings.manualCompensationEv; // P16.13
        constants.minEv100 = settings.minEv100;
        constants.maxEv100 = settings.maxEv100;
        constants.lowPercentile = settings.lowPercentile;
        constants.highPercentile = settings.highPercentile;
        constants.speedUp = settings.speedUp;
        constants.speedDown = settings.speedDown;
        // P16.6: derived from the camera, never authored. The shader is unchanged -- it still
        // receives one EV -- so the three settings agree with it by construction.
        constants.manualEv100 = render::Ev100FromCamera(
            settings.apertureFStop, settings.shutterSpeedSec, settings.isoSensitivity);
        constants.autoExposure = settings.autoExposure ? 1u : 0u;
        constants.blackBucketInfluence = settings.blackBucketInfluence;
        constants.startDistance = settings.adaptationStartDistance;
        // Slope-match factors for the hybrid adaptation, derived exactly as UE derives them: make
        // the exponential's slope equal the linear's at the switch point so the two halves join
        // without a visible change of rate. Evaluated at a small fixed dt rather than taking the
        // limit, which is what UE does and is accurate enough at any real frame rate.
        {
            constexpr float kFrameTimeEps = 1.0f / 60.0f;
            const auto slopeMatch = [](float speed, float startDistance)
            {
                const float safeSpeed = std::max(speed, 0.001f);
                const float startTime = startDistance / safeSpeed;
                const float denom = (1.0f - std::exp2(-kFrameTimeEps * safeSpeed)) * startTime;
                return (denom > 1e-8f) ? (kFrameTimeEps / denom) : 1.0f;
            };
            constants.exponentialUpM = slopeMatch(settings.speedUp, constants.startDistance);
            constants.exponentialDownM = slopeMatch(settings.speedDown, constants.startDistance);
        }

        // Plan section 6.2: cap the adaptation delta so a debugger pause or a long level load does
        // not resolve into one instantaneous jump the moment rendering resumes.
        constexpr double kMaxAdaptationDeltaSeconds = 0.1;
        const double now = GetTimeSeconds();
        const double rawDelta = lastExposureTimeSeconds_ >= 0.0 ? (now - lastExposureTimeSeconds_) : 0.0;
        lastExposureTimeSeconds_ = now;
        constants.deltaTime = static_cast<float>(
            std::clamp(rawDelta, 0.0, kMaxAdaptationDeltaSeconds));

        // Consume rather than peek: the reset is a one-shot edge (level load, resize, teleport,
        // first frame), and leaving it latched would pin the camera to the metered target forever.
        constants.resetHistory = metering.ConsumeResetRequest() ? 1u : 0u;

        const UINT solveCb = resources_.GetExposureSolveCBSizeBytes();
        RecordComputeDispatch(renderer, t.cl, solveMat.get(), solveCb,
            [this, &constants](uint8_t* dest) { resources_.WriteExposureSolveConstants(constants, dest); },
            { metering.HistogramSrv() }, { metering.ExposureUav() }, samplerTable,
            kComputeDispatchGroupSize, kComputeDispatchGroupSize,
            metering.ExposureResource());

        // 4) Round-trip the exposure record and the histogram bins to a readback ring, so the dev
        // window can show the adapted EV and PLOT the histogram. Without it the metering knobs are
        // being tuned blind -- the percentile sliders in particular are meaningless if you cannot
        // see the distribution they are clipping.
        // Its own point, AFTER the solve — see ExposurePoints for what sharing one with the
        // base-luminance restore actually did.
        renderer->EmitPoint(t.cl, pts.copySrc);
        metering.RecordReadbackCopy(t.cl);
        renderer->EmitPoint(t.cl, pts.restore);
    } while (false);
    ctx.EndCL(t);
}

// P8 -- the bloom pyramid, recorded into the tonemap pass's own command list.
//
// Three stages, one shader, one PSO (bloom_cs.hlsl selects on `stage`):
//   setup      the exposed HDR image, thresholded, into down[0]
//   downsample down[i-1] -> down[i], the 13-tap filter, Karis-averaged on the first level only
//   upsample   up[i+1] tented + down[i] -> up[i], walking back to mip 0
//
// The levels talk to each other through their own UAVs and are separated by UAV BARRIERS, not
// transitions -- identical to Pass_Hzb, and for the identical reason. The two chain transitions
// (in and out of UNORDERED_ACCESS) are the pair declared in the tonemap Prepare.
//
// NO EARLY RETURN: every gate was evaluated into `bloomActive_` before the graph was built, and the
// Prepare declared from that same flag. Stopping half way here would leave a declared barrier point
// unemitted.
// The thresholded DOWN chain, shared by both bloom methods.
//
// It lived inside Bloom_Build until the convolution's ghosts needed it. Those are gathered copies
// of the frame, and while they read the frame DIRECTLY they came out sharp -- a copy of a sharp
// image is sharp, and it reads as a picture pasted over the scene rather than as an optical
// artefact. A real ghost is defocused by the aperture, and a mip chain IS that defocus,
// prefiltered. UE source their flares from this same chain, for the same reason.
void SceneRenderer::Bloom_Downsample(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                     D3D12_CPU_DESCRIPTOR_HANDLE hdrSource, float threshold,
                                     UINT mipCount)
{
    const auto& D = renderer->GetDeferredForFrame();
    auto material = resources_.GetBloomMaterial();
    const UINT cbSize = resources_.GetBloomCBSizeBytes();
    const BloomSettings& settings = frame_->settings.bloom;

    const auto samplerDescs = std::array{ *SamplerManager::LinearClamp() };
    const D3D12_GPU_DESCRIPTOR_HANDLE samplerTable =
        renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);

    ExposureMetering& metering = renderer->Exposure();
    // The SAME predicate the tonemap uses for its own exposure. If these two disagreed, the
    // threshold would be measured against one exposure and the image against another.
    const bool applyExposure = frame_->cameraExposure.enabled && metering.IsReady();

    const auto mipSize = [&](UINT mip) {
        return uint2{ std::max(1u, D.bloomWidth >> mip), std::max(1u, D.bloomHeight >> mip) };
    };

    auto dispatch = [&](const BloomPassConstants& c, UINT dstW, UINT dstH,
                        D3D12_CPU_DESCRIPTOR_HANDLE src,
                        D3D12_CPU_DESCRIPTOR_HANDLE dst,
                        D3D12_CPU_DESCRIPTOR_HANDLE add,
                        ID3D12Resource* barrierRes) {
        RecordComputeDispatch(renderer, cl, material.get(), cbSize,
            [&](uint8_t* dest) { resources_.WriteBloomConstants(c, dest); },
            { hdrSource },
            { src, dst, add, metering.ExposureUav() },
            samplerTable, dstW, dstH, barrierRes);
    };

    // ---- stage 0: threshold into down[0] ----
    {
        const uint2 dst = mipSize(0);
        BloomPassConstants c{};
        c.stage = 0u;
        // P16.1: a pre-exposed source is ALREADY in the units the threshold is authored in, so the
        // helper must not scale it a second time. `exposureEnabled = 0` makes ExposureMultiplier()
        // return 1, which is exactly that.
        c.exposureEnabled = (applyExposure && !render::g_preExposureEnabled) ? 1u : 0u;
        c.dstSize = dst;
        c.srcSize = uint2{ renderer->GetWidth(), renderer->GetHeight() };
        c.threshold = threshold;
        c.softKnee = std::max(settings.softKnee, 1.0e-4f);
        c.radius = settings.radius;
        c.fireflyClamp = settings.fireflyClamp ? 1u : 0u;
        // u0 is unused by this stage but the table may not have a hole, so it is aimed at the
        // destination -- a descriptor that is valid and inert.
        dispatch(c, dst.x, dst.y, D.bloomDownMipUAV[0], D.bloomDownMipUAV[0],
                 D.bloomDownMipUAV[0], D.bloomDown.Get());
    }

    // ---- stage 1: down the chain ----
    const UINT lastMip = (mipCount == 0u) ? D.bloomMips : std::min(mipCount, D.bloomMips);
    for (UINT mip = 1; mip < lastMip; ++mip)
    {
        const uint2 dst = mipSize(mip);
        BloomPassConstants c{};
        c.stage = 1u;
        c.exposureEnabled = 0u;
        c.dstSize = dst;
        c.srcSize = mipSize(mip - 1);
        c.threshold = threshold;
        c.softKnee = settings.softKnee;
        c.radius = settings.radius;
        // Karis on the FIRST reduction only: it is there to stop one blown-out texel from
        // dominating, and deeper in the chain it would just eat energy the tent needs.
        c.fireflyClamp = (mip == 1u && settings.fireflyClamp) ? 1u : 0u;
        dispatch(c, dst.x, dst.y, D.bloomDownMipUAV[mip - 1], D.bloomDownMipUAV[mip],
                 D.bloomDownMipUAV[mip], D.bloomDown.Get());
    }
}

void SceneRenderer::Bloom_Build(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                D3D12_CPU_DESCRIPTOR_HANDLE hdrSource)
{
    const auto& D = renderer->GetDeferredForFrame();
    auto material = resources_.GetBloomMaterial();
    const UINT cbSize = resources_.GetBloomCBSizeBytes();
    const BloomSettings& settings = frame_->settings.bloom;

    GPU_SCOPE(cl, ProfilerScopes::kPassBloom);

    renderer->Transition(cl, D.bloomDown.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->Transition(cl, D.bloomUp.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // P8C-2l: the streak and the ghosts are not the convolution's property. They read the HDR
    // image and add into mip 0, which this method writes too, so the pyramid gets them on the
    // same terms -- build before the chain, composite after it.
    const bool flares = flaresGhosts_ || flaresStreak_;
    const BloomConvConstants flareConv = flares ? Bloom_FlareConstants(renderer)
                                                : BloomConvConstants{};
    if (flares)
    {
        renderer->Transition(cl, D.streakA.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        renderer->Transition(cl, D.streakB.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Bloom_FlaresBuild(renderer, cl, hdrSource, flareConv);
    }

    // P8C-4: the same ABSOLUTE scaling the convolution now uses, so switching method is not also a
    // threshold change. See the note in bloom_conv_cs.hlsl stage 0.
    const float absThreshold = (settings.threshold < 0.0f)
        ? 0.0f
        : settings.threshold * (preExposure_ / render::ExposureMultiplierFromEv100(14.0f));
    Bloom_Downsample(renderer, cl, hdrSource, absThreshold, 0u);

    const auto samplerDescs = std::array{ *SamplerManager::LinearClamp() };
    const D3D12_GPU_DESCRIPTOR_HANDLE samplerTable =
        renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);

    ExposureMetering& metering = renderer->Exposure();

    const auto mipSize = [&](UINT mip) {
        return uint2{ std::max(1u, D.bloomWidth >> mip), std::max(1u, D.bloomHeight >> mip) };
    };

    auto dispatch = [&](const BloomPassConstants& c, UINT dstW, UINT dstH,
                        D3D12_CPU_DESCRIPTOR_HANDLE src,
                        D3D12_CPU_DESCRIPTOR_HANDLE dst,
                        D3D12_CPU_DESCRIPTOR_HANDLE add,
                        ID3D12Resource* barrierRes) {
        RecordComputeDispatch(renderer, cl, material.get(), cbSize,
            [&](uint8_t* dest) { resources_.WriteBloomConstants(c, dest); },
            { hdrSource },
            { src, dst, add, metering.ExposureUav() },
            samplerTable, dstW, dstH, barrierRes);
    };

    // ---- stage 2: back up, accumulating ----
    //
    // The coarsest level of the UP chain is seeded from the DOWN chain by an upsample whose source
    // IS its own destination level: with `radius` taps on a level that is a few texels across the
    // tent is a no-op in all but name, and seeding this way avoids a second shader stage that would
    // exist only to copy one tiny mip.
    for (int mip = static_cast<int>(D.bloomMips) - 2; mip >= 0; --mip)
    {
        const UINT m = static_cast<UINT>(mip);
        const uint2 dst = mipSize(m);
        BloomPassConstants c{};
        c.stage = 2u;
        c.exposureEnabled = 0u;
        c.dstSize = dst;
        c.srcSize = mipSize(m + 1u);
        c.threshold = absThreshold;
        c.softKnee = settings.softKnee;
        c.radius = std::max(settings.radius, 0.0f);
        c.fireflyClamp = 0u;
        // The source is the coarser level of the UP chain -- except at the top, where the UP chain
        // holds nothing yet and the DOWN chain's coarsest level is the seed.
        const bool seeding = (m + 1u == D.bloomMips - 1u);
        const D3D12_CPU_DESCRIPTOR_HANDLE src =
            seeding ? D.bloomDownMipUAV[m + 1u] : D.bloomUpMipUAV[m + 1u];
        dispatch(c, dst.x, dst.y, src, D.bloomUpMipUAV[m], D.bloomDownMipUAV[m], D.bloomUp.Get());
    }

    if (flares)
    {
        Bloom_FlaresComposite(renderer, cl, hdrSource, flareConv);
        renderer->Transition(cl, D.streakA.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(cl, D.streakB.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    renderer->Transition(cl, D.bloomUp.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    renderer->Transition(cl, D.bloomDown.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}




// P8C-2h/i: how deep the anamorphic pyramid may go. Each level of width-halving doubles the
// reach -- 25 display pixels at level 0, so eight levels reach ~3200, past the width of the
// screen. Six levels capped the band at ~870 px measured, which is what made the Length slider's
// upper half inert; the packing budget still fits (320+160+80+40+20+10 = 630 of 640).
static constexpr int kStreakMaxLevels = 8;
// A level narrower than this carries no usable detail.
static constexpr uint32_t kStreakMinLevelWidth = 10u;

// The flare fields both halves need, filled in ONE place so the two bloom methods cannot drift.
// `preExposure_` is what makes the thresholds absolute -- see the comment on them.
// P8C-2o: what the TONEMAP multiplies the bloom target by. The flares composite into that same
// target after the resolve, so both of their intensity knobs have to divide this back out to mean
// "fraction of the source's own brightness" -- and under the centre/scatter split it is no longer
// `bloom.intensity`, it is the surveyed scatter fraction.
float SceneRenderer::Bloom_TonemapBloomScale() const
{
    const BloomApplyConstants a = Bloom_ApplyConstants();
    return std::max(a.scatterApply[0], std::max(a.scatterApply[1], a.scatterApply[2]));
}

BloomConvConstants SceneRenderer::Bloom_FlareConstants(Renderer* renderer) const
{
    const auto& D = renderer->GetDeferredForFrame();
    const BloomSettings& settings = frame_->settings.bloom;
    const float displayW = static_cast<float>(std::max(renderer->GetWidth(), 1u));
    BloomConvConstants c{};
    c.exposureEnabled = 0u;
    // INTENSITY MEANS "FRACTION OF THE SOURCE'S OWN BRIGHTNESS": the pyramid's up-chain is a
    // convex combination, so the band arrives at roughly the source's amplitude, and the
    // composite adds it after the bloom target is written -- only the tonemap's bloom.intensity
    // is still ahead of it, so compensating that alone makes the knob mean what it says.
    c.anamorphicIntensity = std::max(0.0f, settings.convAnamorphicIntensity) /
                            std::max(Bloom_TonemapBloomScale(), 0.01f);
    c.anamorphicLength = std::max(0.0005f, settings.convAnamorphicLength);
    c.anamorphicSigma = std::max(0.5f, settings.convAnamorphicWidth) *
                        (static_cast<float>(D.bloomWidth) / displayW);
    // ABSOLUTE units: "is this a light source" is a property of the SCENE, not the camera, but
    // the buffers hold PRE-EXPOSED values. The authored number means stored brightness at
    // EV100 = 14, rescaled by the frame's own pre-exposure.
    const float thresholdScale = preExposure_ / render::ExposureMultiplierFromEv100(14.0f);
    c.anamorphicThreshold = std::max(0.05f, settings.convAnamorphicThreshold) * thresholdScale;
    c.anamorphicChroma = std::clamp(settings.convAnamorphicChroma, 0.0f, 1.0f);
    c.anamorphicTint[0] = std::max(0.0f, settings.convAnamorphicTint[0]);
    c.anamorphicTint[1] = std::max(0.0f, settings.convAnamorphicTint[1]);
    c.anamorphicTint[2] = std::max(0.0f, settings.convAnamorphicTint[2]);
    // ABSOLUTE, for the same two reasons the streak's intensity is. (1) The composite used to be
    // folded INSIDE the convolution's resolve, so the ghosts were multiplied by
    // kConvolutionGain (8) on their way into the target; lifting it into its own stage dropped
    // exactly that factor, which is the "ghost intensity fell sharply" this fixes. (2) The
    // tonemap still scales the target by bloom.intensity, and that number means very different
    // things to the two bloom methods -- the pyramid's output is about the level count brighter
    // per unit -- so a ghost knob that rides it cannot mean one thing on both.
    c.ghostIntensity = std::max(0.0f, settings.convGhostIntensity) /
                       std::max(Bloom_TonemapBloomScale(), 0.01f);
    return c;
}

// P8C-2l -- THE LENS FLARES BELONG TO NEITHER BLOOM METHOD.
//
// The streak and the ghost chain used to live inside Bloom_Convolve, so the cheap pyramid method
// could not have them at all. Nothing about either is convolution-specific: both read the HDR
// image and both add into mip 0 of the bloom target, which is what the pyramid writes too. Two
// things had to be untied first, and each was a real bug: they read the bloom chain's THRESHOLDED
// mip 0, so `bloom.threshold` cascaded with each consumer's own; and the streak pyramid squatted
// on mip 1 of the bloom chains, which is free only while the convolution runs.
//
// Split in two because the composites must land AFTER whichever method wrote mip 0.
// P8C-2o -- READING THE KERNEL'S PIXELS BACK ON THE CPU.
//
// Deliberately narrow: it accepts the one format this asset ships in (DX10 header,
// R16G16B16A16_FLOAT, square, one mip) and refuses anything else rather than growing into a
// second, half-tested DDS loader beside the real one. If the kernel is ever re-authored in
// another format this returns false, the survey never runs, and the split falls back to the
// neutral constants -- a wrong picture is worse than no picture.
bool SceneRenderer::Bloom_ReadKernelPixels(const wchar_t* path)
{
    bloomKernelPixels_.clear();
    bloomKernelPixelDim_ = 0u;
    bloomSurveyRatio_ = -1.0f;

    std::ifstream f(path, std::ios::binary);
    if (!f) { return false; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    constexpr size_t kHeaderBytes = 148u; // 4 magic + 124 DDS_HEADER + 20 DDS_HEADER_DXT10
    if (bytes.size() < kHeaderBytes) { return false; }

    const auto u32 = [&bytes](size_t off) {
        uint32_t v = 0u;
        std::memcpy(&v, bytes.data() + off, sizeof(v));
        return v;
    };
    if (u32(0) != 0x20534444u) { return false; }            // 'DDS '
    const uint32_t height = u32(12);
    const uint32_t width  = u32(16);
    const uint32_t mips   = u32(28);
    if (std::memcmp(bytes.data() + 84, "DX10", 4) != 0) { return false; }
    if (u32(128) != static_cast<uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT)) { return false; }
    if (width == 0u || width != height || mips > 1u) { return false; }

    const size_t texels = static_cast<size_t>(width) * height;
    if (bytes.size() < kHeaderBytes + texels * 8u) { return false; }

    bloomKernelPixels_.resize(texels * 3u);
    const uint8_t* src = bytes.data() + kHeaderBytes;
    for (size_t i = 0; i < texels; ++i)
    {
        uint16_t h[4];
        std::memcpy(h, src + i * 8u, sizeof(h));
        for (int c = 0; c < 3; ++c)
        {
            bloomKernelPixels_[i * 3u + static_cast<size_t>(c)] =
                DirectX::PackedVector::XMConvertHalfToFloat(h[c]);
        }
    }
    bloomKernelPixelDim_ = width;
    return true;
}

// P8C-2o -- THE SURVEY, transcribed from BloomSurveyKernelCenterEnergy.usf and its siblings.
//
// Three quantities, and the definitions are theirs, not mine:
//
//   * MaxScatterDispersion -- the level the kernel is CLAMPED to, measured on the ring just
//     outside the centre zone. Stage 3 computes the same thing from the same eight ring taps.
//   * CenterEnergy -- the sum, over the centre zone, of `max(kernel - MaxScatterDispersion, 0)`.
//     Not the kernel's value there: the EXCESS above the clamp. That excess IS the delta spike,
//     and the delta is precisely the light that never scattered.
//   * ScatterDispersionEnergy -- the total of the clamped kernel, i.e. everything else.
//
// The centre zone is a SQUARE of Chebyshev radius `ViewTexelRadiusInKernelTexels` -- the kernel
// texels one output pixel covers, which is `ratio`. A square, not a disc, because that is the
// footprint of a pixel.
void SceneRenderer::Bloom_SurveyKernel(float ratio)
{
    if (bloomKernelPixelDim_ == 0u) { return; }
    // THE EARLY-OUT IS THE WHOLE PERFORMANCE STORY. `ratio` moves only when convSize, the bloom
    // resolution or the kernel image changes, so the loop below runs a handful of times per
    // session and never inside a steady frame. Timed into the log rather than asserted.
    if (bloomSurveyRatio_ == ratio) { return; }
    const auto surveyStart = std::chrono::steady_clock::now();

    const int dim = static_cast<int>(bloomKernelPixelDim_);
    const auto at = [this, dim](int x, int y, int c) {
        const int cx = std::clamp(x, 0, dim - 1);
        const int cy = std::clamp(y, 0, dim - 1);
        return bloomKernelPixels_[(static_cast<size_t>(cy) * dim + cx) * 3u +
                                  static_cast<size_t>(c)];
    };
    // The kernel's centre is the texture's centre: this asset is authored centred, and stage 3
    // sets kernelCenterUV to (0.5, 0.5) rather than searching for it the way UE's
    // FindKernelCenterCS must for an arbitrary import.
    const float centre = 0.5f * static_cast<float>(dim) - 0.5f;
    // P8C-2v: the same FIXED ring the shader uses -- these two must agree or the split would
    // weigh a different kernel than the one being convolved.
    const float ringTexels = 2.0f;

    std::array<float, 3> clampLevel{ 0.0f, 0.0f, 0.0f };
    for (int r = 0; r < 8; ++r)
    {
        const float ang = 6.28318530718f * (static_cast<float>(r) / 8.0f);
        const int sx = static_cast<int>(std::lround(centre + std::cos(ang) * ringTexels));
        const int sy = static_cast<int>(std::lround(centre + std::sin(ang) * ringTexels));
        for (int c = 0; c < 3; ++c) { clampLevel[c] = std::max(clampLevel[c], at(sx, sy, c)); }
    }

    std::array<float, 3> centerEnergy{ 0.0f, 0.0f, 0.0f };
    std::array<float, 3> scatterEnergy{ 0.0f, 0.0f, 0.0f };
    const int centreRadius = static_cast<int>(std::floor(std::max(ratio, 1.0f) + 0.5f));
    const int ci = static_cast<int>(std::lround(centre));
    for (int y = 0; y < dim; ++y)
    {
        const bool nearY = std::abs(y - ci) <= centreRadius;
        for (int x = 0; x < dim; ++x)
        {
            const bool inCentre = nearY && std::abs(x - ci) <= centreRadius;
            for (int c = 0; c < 3; ++c)
            {
                const float v = at(x, y, c);
                scatterEnergy[c] += std::min(v, clampLevel[c]);
                if (inCentre) { centerEnergy[c] += std::max(v - clampLevel[c], 0.0f); }
            }
        }
    }

    bloomKernelCenterEnergy_ = centerEnergy;
    bloomKernelScatterEnergy_ = scatterEnergy;
    bloomSurveyRatio_ = ratio;
    const float total = centerEnergy[1] + scatterEnergy[1];
    FILE* log = nullptr;
    if (fopen_s(&log, diag::LogPath("bloom_kernel.log").c_str(), "a") == 0 && log)
    {
        std::fprintf(log,
                     "[p8c-2o] survey ratio %.3f  clamp %.4g  centre %.4g (%.2f%% of total)  "
                     "scatter %.4g  took %.2f ms\n",
                     ratio, clampLevel[1], centerEnergy[1],
                     (total > 0.0f) ? (centerEnergy[1] / total * 100.0f) : 0.0f, scatterEnergy[1],
                     std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - surveyStart).count());
        std::fclose(log);
    }
}

// P8C-2o -- BloomFinalizeApplyConstants.usf, line for line.
//
// The pyramid method has no kernel to survey, so it keeps the old additive meaning exactly: the
// scene passes through at 1 and the flare is added at `intensity`. Only the convolution partitions.
BloomApplyConstants SceneRenderer::Bloom_ApplyConstants() const
{
    const BloomSettings& settings = frame_->settings.bloom;
    const float s = std::max(0.0f, settings.intensity);

    BloomApplyConstants out{};
    // P8C-2o/p -- A THRESHOLD AND A PARTITION ARE MUTUALLY EXCLUSIVE, and getting this wrong is
    // what made the first cut of the split darken the frame for nothing.
    //
    // The split is an identity over the WHOLE image: every photon the scene loses to `sceneApply`
    // comes back through the convolution, because the convolution's input IS the scene. Threshold
    // that input and the identity breaks -- the scene still dims by the full centre fraction while
    // only the above-threshold light returns, and the difference is simply destroyed. So a
    // thresholded bloom is additive BY CONSTRUCTION: it is an extraction, not a partition, and it
    // leaves the scene alone. UE never meet this because their FFT bloom does not threshold.
    const bool partition = bloomConvolution_ && frame_->settings.bloom.threshold < 0.0f;
    if (!partition || bloomSurveyRatio_ < 0.0f)
    {
        // UNIT MATCHING, and it lives HERE now rather than in the resolve. The convolution
        // conserves energy -- its kernel is normalised through the DC divide, so its output carries
        // the thresholded image's total light, redistributed. The pyramid does not: its tent
        // upsample ADDS its levels, so it comes out roughly the level count brighter. kBloomMaxMips
        // is the structural reason for the measured factor and is used instead of the measurement
        // because it is the thing that would change if the pyramid gained or lost a level.
        //
        // The reason it moved out of the shader is that it only means something on THIS path. On
        // the partition below the scale is a fraction of the kernel's own energy, and an arbitrary
        // 8 on top of that would be the one number in the chain meaning nothing -- but deleting it
        // outright, as the first cut of the split did, left the additive path 8x dimmer than the
        // pyramid at the same `intensity`, which is a unit break dressed up as a tuning problem.
        const float match = bloomConvolution_ ? static_cast<float>(kBloomMaxMips) : 1.0f;
        // P8C-2v: there is NO clamp-compensation factor here any more. One was added while the
        // clamp ring still moved with convSize, and pinning that ring made it identically 1.000
        // at every ratio -- an inert multiply is worse than none, because the next reader has to
        // prove it does nothing. `intensity` therefore means exactly what it always did.
        const float a = s * match;
        out.sceneApply = { 1.0f, 1.0f, 1.0f };
        out.scatterApply = { a, a, a };
        return out;
    }

    std::array<float, 3> total{};
    for (int c = 0; c < 3; ++c)
    {
        total[c] = bloomKernelCenterEnergy_[c] + bloomKernelScatterEnergy_[c];
    }
    const float maxTotal = std::max(total[0], std::max(total[1], total[2]));
    if (maxTotal <= 0.0f)
    {
        out.sceneApply = { 1.0f, 1.0f, 1.0f };
        out.scatterApply = { s, s, s };
        return out;
    }

    for (int c = 0; c < 3; ++c)
    {
        // Tint is the kernel's own colour balance, normalised by its largest channel -- their
        // `saturate(RefTotalEnergy / MaxEnergy)`.
        const float tint = std::clamp(total[c] / maxTotal, 0.0f, 1.0f);
        const float finalScatter = bloomKernelScatterEnergy_[c] * s;
        const float finalCentre = total[c] - finalScatter;
        out.sceneApply[c] = tint * std::clamp(finalCentre / total[c], 0.0f, 1.0f);
        out.scatterApply[c] = tint * std::clamp(finalScatter / total[c], 0.0f, 1.0f);
    }
    return out;
}

void SceneRenderer::Bloom_FlaresBuild(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                      D3D12_CPU_DESCRIPTOR_HANDLE hdrSource,
                                      const BloomConvConstants& conv)
{
    const bool ghosts = flaresGhosts_ && flareBokehReady_;
    const bool streak = flaresStreak_;
    const auto& D = renderer->GetDeferredForFrame();
    const BloomSettings& settings = frame_->settings.bloom;
    auto convMaterial = resources_.GetBloomConvMaterial();
    const UINT convCb = resources_.GetBloomConvCBSizeBytes();
    const uint2 streakGrid{ std::max(D.streakWidth, 1u), std::max(D.streakHeight, 1u) };
    auto flareMaterial = resources_.GetLensFlareMaterial();
    const auto samplerDescs = std::array{ *SamplerManager::LinearClamp() };
    const D3D12_GPU_DESCRIPTOR_HANDLE samplerTable =
        renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);
    const auto convDispatchTo = [&](const BloomConvConstants& c, D3D12_CPU_DESCRIPTOR_HANDLE gridUav,
                                    D3D12_CPU_DESCRIPTOR_HANDLE dstUav,
                                    UINT w, UINT h, ID3D12Resource* barrierRes) {
        RecordComputeDispatch(renderer, cl, convMaterial.get(), convCb,
            [&](uint8_t* dest) { resources_.WriteBloomConvConstants(c, dest); },
            { hdrSource, D.lensFlareSRV, bloomKernelTex_.GetSRVCPU() },
            { gridUav, dstUav, renderer->Exposure().ExposureUav() },
            samplerTable, w, h, barrierRes);
    };
    const float streakExtentTexels =
        std::max(conv.anamorphicLength * static_cast<float>(streakGrid.x), 4.0f);
    // ---- P8C-2h: the anamorphic streak, an ANISOTROPIC PYRAMID (KinoStreak's structure) ----
    //
    // Levels halve in WIDTH and keep their height, so the band's vertical resolution never
    // degrades, and every tap is ~1 texel of its own level -- reach comes from resolution, not
    // from a growing step. That is what retires the fixed-resolution cascade this replaces: a
    // step coarser than the falloff replicated the source instead of extending it.
    //
    // Levels live PACKED SIDE BY SIDE in bloomDown mip1 (widths halve, so 1/2 + 1/4 + ... always
    // fits inside the full width), with level 0 in bloomUp mip1. No new allocation: everything
    // below mip 0 of those two chains is unused while the convolution method runs.
    if (streak)
    {
        std::array<uint32_t, kStreakMaxLevels> levelWidth{};
        std::array<uint32_t, kStreakMaxLevels> levelOffset{};
        int levels = 1;
        levelWidth[0] = streakGrid.x;
        for (int k = 1; k < kStreakMaxLevels; ++k)
        {
            const uint32_t w = levelWidth[k - 1] / 2u;
            if (w < kStreakMinLevelWidth) { break; }
            levelWidth[k] = w;
            // Level 0 has its own texture, so the packing of 1..N-1 starts at zero.
            levelOffset[k] = (k == 1) ? 0u : levelOffset[k - 1] + levelWidth[k - 1];
            ++levels;
        }

        // WHICH LEVEL CARRIES THE BAND. Level k reaches 5 * 1.25 * 2^k of its own texels, so the
        // level matching the authored extent is the log2 of that ratio, and a tent across the two
        // neighbouring levels realises a fractional one. `kTentCalibration` is measured, not
        // guessed: the tent's own support overshoots its nominal reach, and dividing by it is
        // what puts the delivered extent within 0.72-1.03x of the number on the slider (numpy,
        // against this exact tap pattern, before the shader was written).
        constexpr float kLevel0Reach = 5.0f * 1.25f;
        constexpr float kTentCalibration = 1.55f;
        const float tCentre = std::clamp(
            std::log2(std::max(streakExtentTexels / kTentCalibration, kLevel0Reach) / kLevel0Reach),
            0.0f, static_cast<float>(levels - 1));

        // CHROMA IS A PER-CHANNEL SHIFT OF THAT TENT, which costs nothing: the weights were
        // already per level, they are simply per channel now. One level is a factor of two in
        // reach, so blue shifted a third of a level up and red a fifth down makes blue run ~40%
        // farther -- the cylindrical-coating look, without a second blur.
        const float chroma = std::clamp(settings.convAnamorphicChroma, 0.0f, 1.0f);
        const float tChannel[3] = { tCentre - 0.20f * chroma,
                                    tCentre,
                                    tCentre + 0.33f * chroma };

        float weight[kStreakMaxLevels][3]{};
        float weightSum[3]{};
        for (int k = 0; k < levels; ++k)
        {
            for (int c = 0; c < 3; ++c)
            {
                const float w =
                    std::max(0.0f, 1.0f - std::abs(static_cast<float>(k) - tChannel[c]));
                weight[k][c] = w;
                weightSum[c] += w;
            }
        }
        for (int c = 0; c < 3; ++c)
        {
            if (weightSum[c] < 1.0e-6f)
            {
                const int k =
                    std::clamp(static_cast<int>(std::lround(tChannel[c])), 0, levels - 1);
                weight[k][c] = 1.0f;
                weightSum[c] = 1.0f;
            }
        }
        const auto share = [&](int level, int c) {
            return weight[level][c] / std::max(weightSum[c], 1.0e-6f);
        };

        BloomConvConstants a = conv;

        // ---- prefilter: bloomDown mip0 -> level 0 ----
        a.convStage = 4u;
        a.sourceSize = uint2{ levelWidth[0], streakGrid.y };
        a.imageSize = uint2{ D.bloomWidth, D.bloomHeight };
        a.streakOffsets = uint2{ 0u, 0u };
        convDispatchTo(a, D.streakBUAV, D.streakAUAV,
                       levelWidth[0], streakGrid.y, D.streakA.Get());

        // ---- downsample chain ----
        a.convStage = 5u;
        for (int k = 1; k < levels; ++k)
        {
            a.sourceSize = uint2{ levelWidth[k], streakGrid.y };
            a.imageSize = uint2{ levelWidth[k - 1], streakGrid.y };
            a.streakOffsets = uint2{ levelOffset[k], (k == 1) ? 0u : levelOffset[k - 1] };
            convDispatchTo(a,
                           (k == 1) ? D.streakAUAV : D.streakBUAV,
                           D.streakBUAV,
                           levelWidth[k], streakGrid.y, D.streakB.Get());
        }

        // ---- up-chain: acc(k) = upsample(acc(k+1)) * srcWeight + level_k * ownWeight ----
        a.convStage = 6u;
        for (int k = levels - 2; k >= 0; --k)
        {
            const bool firstUp = (k == levels - 2);
            const bool intoLevel0 = (k == 0);
            for (int c = 0; c < 3; ++c)
            {
                a.streakWeight[c] = share(k, c);
                // Only the first pass weights its source: that "accumulator" is the coarsest
                // level's raw content. Every later pass receives an already-weighted sum.
                a.streakSrcWeight[c] = firstUp ? share(levels - 1, c) : 1.0f;
            }
            a.sourceSize = uint2{ levelWidth[k], streakGrid.y };
            a.imageSize = uint2{ levelWidth[k + 1], streakGrid.y };
            a.streakOffsets = uint2{ intoLevel0 ? 0u : levelOffset[k], levelOffset[k + 1] };
            convDispatchTo(a,
                           D.streakBUAV,
                           intoLevel0 ? D.streakAUAV : D.streakBUAV,
                           levelWidth[k], streakGrid.y,
                           intoLevel0 ? D.streakA.Get() : D.streakB.Get());
        }
    }

    // ---- P8C-2 step 5a: the bokeh scatter (UE's LensFlareBlur) ----
    //
    // A GRAPHICS pass in a chain of compute: one instanced quad per tile of the thresholded
    // half-res scene, collapsed to zero size where the tile is dark, textured with the baked iris
    // sprite, additively rasterized into the quarter-res flare target. The output is the actual
    // defocused image of the actual bright sources -- which is why no sun position is plumbed
    // anywhere and two suns give two ghost chains for free.
    renderer->Transition(cl, D.lensFlare.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (ghosts)
    {
        const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        cl->ClearRenderTargetView(D.lensFlareRTV, clearColor, 0, nullptr);
        cl->OMSetRenderTargets(1, &D.lensFlareRTV, FALSE, nullptr);
        D3D12_VIEWPORT vp{ 0.0f, 0.0f,
                           static_cast<float>(D.lensFlareWidth),
                           static_cast<float>(D.lensFlareHeight), 0.0f, 1.0f };
        D3D12_RECT sc{ 0, 0, static_cast<LONG>(D.lensFlareWidth),
                       static_cast<LONG>(D.lensFlareHeight) };
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);

        auto h = renderer->GetRenderContextPool()->Acquire();
        auto& rc = h.ref();

        LensFlareConstants fc{};
        // The tile grid runs at the flare target's own (quarter-display) resolution; the source
        // is the half-display bloomDown, so one tile covers 2x2 source texels. UE run 1px tiles
        // on their half-res input; a quarter-res start is the plan's own cost advice.
        fc.tileCount = uint2{ D.lensFlareWidth, D.lensFlareHeight };
        fc.flareRTSize[0] = static_cast<float>(D.lensFlareWidth);
        fc.flareRTSize[1] = static_cast<float>(D.lensFlareHeight);
        fc.srcInvSize[0] = 1.0f / static_cast<float>(std::max(renderer->GetWidth(), 1u));
        fc.srcInvSize[1] = 1.0f / static_cast<float>(std::max(renderer->GetHeight(), 1u));
        // One flare tile covers this many SOURCE texels now that the source is the full-resolution
        // image rather than the half-resolution bloom chain.
        fc.tileSizeTexels = static_cast<float>(std::max(renderer->GetWidth(), 1u)) /
                            static_cast<float>(std::max(D.lensFlareWidth, 1u));
        // UE's LensFlareBokehSize: a percent of the flare view's width.
        fc.kernelSizePx = std::max(settings.convGhostBokeh, 0.05f) * 0.01f *
                          static_cast<float>(D.lensFlareWidth);
        // UE's LensFlareThreshold, verbatim mechanism: the VS collapses every tile below it. The
        // bloom extraction's own threshold is NOT enough -- at bloom.threshold -1 the source is
        // unthresholded and sunlit foliage splats bokehs, which the composite then mirrors into
        // the sky as upside-down palms. This is also the pass's whole cost model: quads that
        // collapse rasterize nothing.
        // Absolute units, same EV14 reference as the streak threshold -- see that comment.
        fc.threshold = std::max(settings.convGhostThreshold, 1.0e-4f) *
                       (preExposure_ / render::ExposureMultiplierFromEv100(14.0f));
        fc.kernelAreaInverse = 1.0f / std::max(1.0f, fc.kernelSizePx * fc.kernelSizePx);

        auto cbAlloc = renderer->GetFrameResource()->AllocDynamic(
            resources_.GetLensFlareCBSizeBytes(), render::kConstantBufferAlignment);
        resources_.WriteLensFlareConstants(fc, static_cast<uint8_t*>(cbAlloc.cpu));
        rc.cbv[0] = cbAlloc.gpu;
        rc.srvTable[0] = renderer->StageSrvUavTable(
            { hdrSource, flareBokeh_[flareBokehSlot_].GetSRVCPU() }).gpu;
        const auto flareSamplers = std::array{ *SamplerManager::LinearClamp() };
        rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, flareSamplers);

        flareMaterial->Bind(cl, rc);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        // P8C-3: four quads an instance, UE's GLensFlareQuadsPerInstance. Mirrored in
        // lens_flare.hlsl's kFlareQuadsPerInstance -- the vertex shader decodes the quad out of
        // the high bits of SV_VertexID and collapses the partial last instance to zero size.
        constexpr UINT kFlareQuadsPerInstance = 4u;
        const UINT tiles = D.lensFlareWidth * D.lensFlareHeight;
        const UINT instances = (tiles + kFlareQuadsPerInstance - 1u) / kFlareQuadsPerInstance;
        cl->DrawInstanced(6u * kFlareQuadsPerInstance, instances, 0, 0);
    }
    renderer->Transition(cl, D.lensFlare.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

}

void SceneRenderer::Bloom_FlaresComposite(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                          D3D12_CPU_DESCRIPTOR_HANDLE hdrSource,
                                          const BloomConvConstants& conv)
{
    const bool ghosts = flaresGhosts_ && flareBokehReady_;
    const bool streak = flaresStreak_;
    const auto& D = renderer->GetDeferredForFrame();
    const BloomSettings& settings = frame_->settings.bloom;
    auto convMaterial = resources_.GetBloomConvMaterial();
    const UINT convCb = resources_.GetBloomConvCBSizeBytes();
    const uint2 streakGrid{ std::max(D.streakWidth, 1u), std::max(D.streakHeight, 1u) };
    const auto samplerDescs = std::array{ *SamplerManager::LinearClamp() };
    const D3D12_GPU_DESCRIPTOR_HANDLE samplerTable =
        renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);
    const auto convDispatchTo = [&](const BloomConvConstants& c, D3D12_CPU_DESCRIPTOR_HANDLE gridUav,
                                    D3D12_CPU_DESCRIPTOR_HANDLE dstUav,
                                    UINT w, UINT h, ID3D12Resource* barrierRes) {
        RecordComputeDispatch(renderer, cl, convMaterial.get(), convCb,
            [&](uint8_t* dest) { resources_.WriteBloomConvConstants(c, dest); },
            { hdrSource, D.lensFlareSRV, bloomKernelTex_.GetSRVCPU() },
            { gridUav, dstUav, renderer->Exposure().ExposureUav() },
            samplerTable, w, h, barrierRes);
    };
    if (streak)
    {
        BloomConvConstants a = conv;
        a.convStage = 7u;
        a.sourceSize = uint2{ D.bloomWidth, D.bloomHeight };
        a.imageSize = uint2{ streakGrid.x, streakGrid.y };  // level 0
        a.streakOffsets = uint2{ 0u, 0u };
        convDispatchTo(a, D.streakAUAV, D.bloomUpMipUAV[0],
                       D.bloomWidth, D.bloomHeight, D.bloomUp.Get());
    }
    if (ghosts)
    {
        // P8C-2l: lifted out of the resolve into its own stage so the pyramid method can run the
        // same one -- it has no resolve to fold a composite into.
        BloomConvConstants a = conv;
        a.convStage = 8u;
        a.sourceSize = uint2{ D.bloomWidth, D.bloomHeight };
        a.ghostCount = std::min(settings.convGhosts, 8u);
        convDispatchTo(a, D.streakAUAV, D.bloomUpMipUAV[0],
                       D.bloomWidth, D.bloomHeight, D.bloomUp.Get());
    }
}

// P8C / P8C-2 -- convolution bloom. Same slot in the frame as the pyramid, same output texture,
// and the tonemap cannot tell which one ran: `intensity` scales either.
//
//   kernel (only when its parameters changed)  resample EXR -> FFT -> [+ streak spectrum] -> cached
//   frame                                      pack -> FFT rows -> FFT cols
//                                                   -> HERMITIAN MULTIPLY (its own pass)
//                                                   -> IFFT cols -> IFFT rows -> resolve
//
// P8C-2 moved the multiply OUT of the column transform: a coloured kernel needs the spectrum at
// (-k), which belongs to a different column that the folded dispatch had not necessarily written.
// One extra full-grid pass per frame is the price of the rainbow kernel.
//
// NO EARLY RETURN once recording starts, for the reason Pass_Gtao documents: the gate was decided
// before the graph was built and the Prepare declared from the same flag.

void SceneRenderer::Bloom_Convolve(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                   D3D12_CPU_DESCRIPTOR_HANDLE hdrSource)
{
    const auto& D = renderer->GetDeferredForFrame();
    auto fftMaterial = resources_.GetBloomFftMaterial();
    auto convMaterial = resources_.GetBloomConvMaterial();
    const UINT fftCb = resources_.GetBloomFftCBSizeBytes();
    const UINT convCb = resources_.GetBloomConvCBSizeBytes();
    const BloomSettings& settings = frame_->settings.bloom;

    GPU_SCOPE(cl, ProfilerScopes::kPassBloomConv);

    auto flareMaterial = resources_.GetLensFlareMaterial();
    // P8C-2m: the same frame gates the graph was declared from -- a body that re-derived them
    // could disagree with the Prepare, which is a barrier the compile never registered.
    const bool ghosts = flaresGhosts_ && flareBokehReady_;
    const bool streak = flaresStreak_;

    const uint2 streakGrid{ std::max(D.streakWidth, 1u), std::max(D.streakHeight, 1u) };

    renderer->Transition(cl, D.bloomUp.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // P8C-2l: no source downsample any more -- the scatter and the streak prefilter both read
    // the HDR image directly, each with its own absolute threshold. Sharing one pre-thresholded
    // texture made `bloom.threshold` cascade with theirs, and it tied both to whichever bloom
    // method had built the chain.
    if (ghosts || streak)
    {
        renderer->Transition(cl, D.streakA.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        renderer->Transition(cl, D.streakB.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    renderer->Transition(cl, D.bloomFftA.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->Transition(cl, D.bloomFftB.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderer->Transition(cl, D.bloomFftKernel.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);


    const auto samplerDescs = std::array{ *SamplerManager::LinearClamp() };
    const D3D12_GPU_DESCRIPTOR_HANDLE samplerTable =
        renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);

    ExposureMetering& metering = renderer->Exposure();
    const bool applyExposure = frame_->cameraExposure.enabled && metering.IsReady();

    // ---- the ACTIVE grid, from bloom.convPercent (P8C-2 step 4) ----
    //
    // The textures are allocated for percent 50 (the ceiling); a lower percent runs the whole
    // chain on a sub-grid of the same textures, so the transform cost actually shrinks -- the
    // FFT's cost is the GRID's, not the image's. UE's r.Bloom.ScreenPercentage, same clamp shape.
    const auto nextPow2 = [](UINT v) {
        UINT p = 16u;
        while (p < v && p < 2048u) { p <<= 1u; }
        return p;
    };
    const float pct = std::clamp(settings.convPercent, 10.0f, 100.0f) * 0.01f;
    UINT imageW = std::max(16u,
        static_cast<UINT>(std::lround(static_cast<float>(renderer->GetWidth()) * pct)));
    UINT imageH = std::max(16u,
        static_cast<UINT>(std::lround(static_cast<float>(renderer->GetHeight()) * pct)));
    // P8C-2w -- THE GRID MUST HOLD THE KERNEL, NOT JUST THE IMAGE.
    //
    // It used to be sized from the image alone (5/4 for the zero pad). The kernel is laid out
    // around the DC-folded origin and reaches +-span/2, so once `span` passed the grid's SHORTER
    // axis the shader's `uv in [0,1]` guard started cropping its tails -- at convPercent 25 the
    // grid is 1024x512 and the image 640 wide, which puts that cliff at exactly convSize 0.8:
    // 0.8 * 640 = 512 = the grid's height. Above it the glow lost its outer falloff and looked
    // eaten, which is precisely what the 0.8-vs-1.0 pair showed.
    const float wantSpan = std::clamp(settings.convSize, 0.02f, 1.0f) *
                           static_cast<float>(std::max(imageW, imageH));
    const UINT spanTexels = static_cast<UINT>(std::ceil(std::max(wantSpan, 1.0f)));
    UINT gridW = nextPow2(std::max((imageW * 5u) / 4u, spanTexels));
    UINT gridH = nextPow2(std::max((imageH * 5u) / 4u, spanTexels));
    if (gridW > D.bloomFftWidth) { gridW = D.bloomFftWidth; imageW = std::min(imageW, (gridW * 4u) / 5u); }
    if (gridH > D.bloomFftHeight) { gridH = D.bloomFftHeight; imageH = std::min(imageH, (gridH * 4u) / 5u); }
    const uint2 grid{ gridW, gridH };
    const uint2 image{ imageW, imageH };

    BloomConvConstants conv{};
    conv.exposureEnabled = (applyExposure && !render::g_preExposureEnabled) ? 1u : 0u;  // P16.1
    conv.transformSize = grid;
    conv.imageSize = image;
    // P8C-4: ABSOLUTE, the same EV14 reference the streak and ghost thresholds already use. A
    // threshold in viewer units answers "is this pixel bright ON SCREEN", which moves with the
    // auto-exposure; this one answers "is this a light source", which does not.
    const float absScale = preExposure_ / render::ExposureMultiplierFromEv100(14.0f);
    conv.threshold = (settings.threshold < 0.0f) ? -1.0f : settings.threshold * absScale;
    // P8C-5: UE's prefilter, in the SAME absolute units -- Min and Max are luminances, so they get
    // the same scaling every other threshold in the bloom gets; Mult is a slope and is unitless.
    // A non-zero Mult turns the threshold cut above OFF inside the shader.
    conv.preFilterMin = std::max(0.0f, settings.convPreFilterMin) * absScale;
    conv.preFilterMax = std::max(0.0f, settings.convPreFilterMax) * absScale;
    conv.preFilterMult = std::max(0.0f, settings.convPreFilterMult);
    conv.softKnee = std::max(settings.softKnee, 1.0e-4f);
    // Kernel placement: the photograph's full width spans convSize x the image's major axis in
    // grid texels (UE's KernelSupportScale rule), and the mip whose texel density matches that
    // span stands in for UE's downsample-chain prefilter.
    const float convSizeFrac = std::clamp(settings.convSize, 0.02f, 1.0f);
    // Capped at what the grid can actually hold: if the allocation ceiling stopped the grid from
    // growing (convPercent already at the top), cropping the kernel would eat its tails silently.
    // Clamping the SPAN instead keeps the whole image, just no larger than it fits.
    conv.kernelSpanTexels =
        std::min(convSizeFrac * static_cast<float>(std::max(image.x, image.y)),
                 static_cast<float>(std::min(grid.x, grid.y)));
    // P8C-2h: minification is box-filtered in the shader on demand, so the asset ships mip 0
    // only -- like UE's, which is why they build a downsample chain at runtime instead. `taps` is
    // how many kernel texels fall in one grid texel; 1 means the kernel is being MAGNIFIED, the
    // common case (at convSize 1.0 it is upscaled) and then a single bilinear tap is exact.
    {
        const float kernelTexels = static_cast<float>(std::max(bloomKernelTex_.GetWidth(), 1u));
        const float ratio = kernelTexels / std::max(conv.kernelSpanTexels, 1.0f);
        // P8C-2v -- THE TAP COUNT IS FIXED, THE FOOTPRINT IS WHAT MOVES.
        //
        // It used to be `ceil(ratio)` taps, which is an INTEGER function of a continuous quantity:
        // at ratio = 1 (convSize 0.8 on a half-res 640-wide image) it steps 2 -> 1 and the filter
        // changes shape underneath the kernel. Measured against a bloom-off baseline, UE's
        // photograph lost 45% of its glare across that one step -- 151 -> 84 at r 40-100, 41 -> 19
        // at r 100-200 -- and did not come back at 0.9. That is the "at 0.8 there is almost
        // nothing left" this fixes.
        //
        // A fixed count with a footprint of `max(ratio, 1)` kernel texels is continuous in ratio
        // everywhere: above 1 it is a proper box over the texels one grid texel covers, at or
        // below 1 it collapses to the one-texel footprint a single bilinear tap already has, so
        // magnification is unchanged. Four taps per axis is UE's own downsample-chain accuracy
        // without the chain, and this stage runs once per kernel rebuild, not per frame.
        constexpr uint32_t kKernelBoxTaps = 4u;
        const float footprint = std::max(ratio, 1.0f);
        conv.kernelBoxTaps = kKernelBoxTaps;
        conv.kernelBoxStep = (footprint / static_cast<float>(kKernelBoxTaps)) / kernelTexels;
        // P8C-2v -- THE CLAMP RING IS FIXED, AND THAT IS A DELIBERATE DEPARTURE FROM UE.
        //
        // Theirs is `ViewTexelRadiusInKernelTexels + 1`, i.e. it grows with the kernel texels one
        // output pixel covers. That is safe for them because the energy the clamp removes is not
        // discarded -- it becomes their CENTRE term and comes back as scene colour, so moving the
        // clamp moves light between two places and the total is untouched.
        //
        // Here the clamped-off energy is simply gone, and the convolution is normalised by what is
        // LEFT. So a moving ring changes the divisor under the whole image, and on a kernel that is
        // 98% one texel that is violent: over convSize 0.7 -> 0.8 the ring shrinks 2.14 -> 2.00
        // texels, the clamp rises, and the glare lost 45% of its brightness at every radius
        // (measured against a bloom-off baseline: 151 -> 84 at r 40-100). A fixed ring makes the
        // clamp level a property of the IMAGE alone, which is what the normalisation already
        // assumes, and convSize goes back to meaning size.
        conv.kernelCoreRingUV = 2.0f / kernelTexels;
        // P8C-2o: the survey shares that same `ratio` -- the centre zone the clamp is measured
        // against and the centre zone whose energy is weighed MUST be the same zone, or the split
        // would hand the tonemap a fraction of a different kernel than the one being convolved.
        Bloom_SurveyKernel(ratio);
    }
    conv.kernelTint[0] = std::max(0.0f, settings.convKernelTint[0]);
    conv.kernelTint[1] = std::max(0.0f, settings.convKernelTint[1]);
    conv.kernelTint[2] = std::max(0.0f, settings.convKernelTint[2]);
    conv.kernelCenterUV[0] = 0.5f;
    conv.kernelCenterUV[1] = 0.5f;
    // P8C-2l: the flare fields come from the shared builder -- assembling them in two places is
    // exactly how the two bloom methods would drift apart.
    {
        const BloomConvConstants f = Bloom_FlareConstants(renderer);
        conv.anamorphicIntensity = f.anamorphicIntensity;
        conv.anamorphicLength = f.anamorphicLength;
        conv.anamorphicSigma = f.anamorphicSigma;
        conv.anamorphicThreshold = f.anamorphicThreshold;
        conv.anamorphicChroma = f.anamorphicChroma;
        conv.anamorphicTint[0] = f.anamorphicTint[0];
        conv.anamorphicTint[1] = f.anamorphicTint[1];
        conv.anamorphicTint[2] = f.anamorphicTint[2];
        conv.ghostIntensity = f.ghostIntensity;
    }
    conv.ghostCount = ghosts ? std::min(settings.convGhosts, 8u) : 0u;

    // The convolution shader writes the grid (u0), the bloom target (u1) and reads the exposure
    // record (u2). The table is positional, so all three are bound on every stage even when a
    // stage touches only one of them. `dstUav` overrides u1 -- the streak stage aims it at the
    // KERNEL SPECTRUM to read the DC energy, everything else leaves it at the bloom target.
    const auto convDispatchTo = [&](const BloomConvConstants& c, D3D12_CPU_DESCRIPTOR_HANDLE gridUav,
                                    D3D12_CPU_DESCRIPTOR_HANDLE dstUav,
                                    UINT w, UINT h, ID3D12Resource* barrierRes) {
        RecordComputeDispatch(renderer, cl, convMaterial.get(), convCb,
            [&](uint8_t* dest) { resources_.WriteBloomConvConstants(c, dest); },
            // t1 is the flare-blur image (only the resolve's ghost composite reads it) and t2 the
            // kernel photograph (only the resample stage reads it) -- but a descriptor table is
            // POSITIONAL, so both are bound on every stage rather than left as holes.
            { hdrSource, D.lensFlareSRV, bloomKernelTex_.GetSRVCPU() },
            { gridUav, dstUav, metering.ExposureUav() },
            samplerTable, w, h, barrierRes);
    };
    const auto convDispatch = [&](const BloomConvConstants& c, D3D12_CPU_DESCRIPTOR_HANDLE gridUav,
                                  UINT w, UINT h, ID3D12Resource* barrierRes) {
        convDispatchTo(c, gridUav, D.bloomUpMipUAV[0], w, h, barrierRes);
    };

    // One thread per element PAIR, one group per scan line -- the shape bloom_fft_cs declares.
    // The multiply and accumulate modes reuse the row shape: one group per ROW (isVertical = 0).
    const auto fftDispatch = [&](const BloomFftConstants& c,
                                 D3D12_CPU_DESCRIPTOR_HANDLE srcUav,
                                 D3D12_CPU_DESCRIPTOR_HANDLE kernelUav,
                                 D3D12_CPU_DESCRIPTOR_HANDLE dstUav,
                                 ID3D12Resource* barrierRes) {
        const UINT lines = (c.isVertical != 0u) ? grid.x : grid.y;
        RecordComputeDispatch(renderer, cl, fftMaterial.get(), fftCb,
            [&](uint8_t* dest) { resources_.WriteBloomFftConstants(c, dest); },
            {},
            { srcUav, kernelUav, dstUav },
            samplerTable,
            // RecordComputeDispatch always divides the extent it is given by 8 (it exists for the
            // 8x8 shaders everything else uses), but this one is a 1D group of kBloomFftThreads and
            // needs exactly ONE GROUP PER SCAN LINE. Multiplying by that same 8 is how a caller asks
            // this helper for `lines` groups; passing the real thread count instead asked for 128x
            // too many, which is what made the first convolution produce nothing.
            lines * kComputeDispatchGroupSize, 1u,
            barrierRes);
    };

    if (flaresGhosts_ || flaresStreak_)
    {
        CPU_SCOPE(ProfilerScopes::kBloomRecFlares);
        Bloom_FlaresBuild(renderer, cl, hdrSource, conv);
    }

    // ---- kernel: resample the photograph, transform, add the streak's spectrum. Rebuilt only
    // when its parameters move -- EVERY placement parameter is in the key, because the spectrum
    // is cached and one left out is a control that appears to do nothing until something else
    // forces a rebuild. ----
    const BloomKernelKey key{ grid.x, grid.y, image.x, image.y, convSizeFrac,
                              { conv.kernelTint[0], conv.kernelTint[1], conv.kernelTint[2] } };
    BloomKernelKey& slotKey = bloomKernelKeys_[renderer->GetCurrentFrameIndex() % render::kFrameCount];
    if (!(key == slotKey))
    {
        CPU_SCOPE(ProfilerScopes::kBloomRecKernel);
        BloomConvConstants k = conv;
        BloomFftConstants f{};
        f.transformSize = grid;

        // The photograph into grid A, centre folded to the DC corner...
        k.convStage = 3u;
        convDispatch(k, D.bloomFftAUAV, grid.x, grid.y, D.bloomFftA.Get());

        // ...and its spectrum into the kernel texture. The DC texel now holds the per-channel
        // sums, which both the streak's amplitude and the multiply's normalisation read.
        f.isVertical = 0u;
        fftDispatch(f, D.bloomFftAUAV, D.bloomFftKernelUAV, D.bloomFftBUAV, D.bloomFftB.Get());
        f.isVertical = 1u;
        fftDispatch(f, D.bloomFftBUAV, D.bloomFftKernelUAV, D.bloomFftKernelUAV,
                    D.bloomFftKernel.Get());

        slotKey = key;
    }

    // ---- frame: pack -> forward -> multiply -> inverse -> resolve ----
    CPU_SCOPE(ProfilerScopes::kBloomRecFft);
    {
        BloomConvConstants s = conv;
        s.convStage = 0u;
        s.sourceSize = uint2{ renderer->GetWidth(), renderer->GetHeight() };
        convDispatch(s, D.bloomFftAUAV, grid.x, grid.y, D.bloomFftA.Get());
    }

    BloomFftConstants f{};
    f.transformSize = grid;
    f.isVertical = 0u;
    fftDispatch(f, D.bloomFftAUAV, D.bloomFftKernelUAV, D.bloomFftBUAV, D.bloomFftB.Get());
    f.isVertical = 1u;
    fftDispatch(f, D.bloomFftBUAV, D.bloomFftKernelUAV, D.bloomFftAUAV, D.bloomFftA.Get());
    // P8C-2: the Hermitian spectral multiply, on the COMPLETE forward spectrum.
    f.mode = 1u;
    f.isVertical = 0u;
    fftDispatch(f, D.bloomFftAUAV, D.bloomFftKernelUAV, D.bloomFftBUAV, D.bloomFftB.Get());
    f.mode = 0u;
    f.isInverse = 1u;
    f.isVertical = 1u;
    fftDispatch(f, D.bloomFftBUAV, D.bloomFftKernelUAV, D.bloomFftAUAV, D.bloomFftA.Get());
    f.isVertical = 0u;
    fftDispatch(f, D.bloomFftAUAV, D.bloomFftKernelUAV, D.bloomFftBUAV, D.bloomFftB.Get());

    {
        BloomConvConstants r = conv;
        r.convStage = 2u;
        r.sourceSize = uint2{ D.bloomWidth, D.bloomHeight };
        convDispatch(r, D.bloomFftBUAV, D.bloomWidth, D.bloomHeight, D.bloomUp.Get());
    }

    // ---- P8C-2h/l: the flare composites, additive onto the freshly written mip 0 ----
    if (flaresGhosts_ || flaresStreak_)
    {
        CPU_SCOPE(ProfilerScopes::kBloomRecFlares);
        Bloom_FlaresComposite(renderer, cl, hdrSource, conv);
    }
    renderer->Transition(cl, D.bloomUp.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    renderer->Transition(cl, D.bloomFftA.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    renderer->Transition(cl, D.bloomFftB.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    renderer->Transition(cl, D.bloomFftKernel.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (ghosts || streak)
    {
        renderer->Transition(cl, D.streakA.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(cl, D.streakB.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
}

// P8C-2 step 5a: the ghost bokeh sprite -- the iris polygon with its bright rim, P8D's
// ApertureMask moved to the CPU. UE author theirs as a texture asset; keeping it procedural is
// what lets `blades` still mean something after the aperture kernel's retirement.
//
// P8C-2d: no GPU wait anywhere in here. The pixels are built on the CPU, the copy is enqueued
// with UploadBatch::Submit (close + execute, no fence wait) into the slot the scatter is NOT
// sampling, and the batch is held until the frame gate retires it -- which is what keeps the
// intermediates and the command allocator alive for exactly as long as the GPU needs them.
void SceneRenderer::BakeFlareBokeh(Renderer* renderer, uint32_t blades)
{
    // 256, not 64: at Ghost Size 3% a sprite is drawn 23-177 px, so a 64 px bake was MAGNIFIED
    // up to 3x and its polygon edges arrived as mush -- half the reason the blade count could not
    // be seen. Minification preserves a shape; magnification invents one.
    constexpr int kSize = 256;
    std::vector<uint8_t> pixels(static_cast<size_t>(kSize) * kSize * 4u);
    const float step = 6.28318530718f / static_cast<float>(std::max(blades, 1u));
    const float apothem =
        (blades >= 3u) ? std::cos(3.14159265f / static_cast<float>(blades)) : 1.0f;
    const auto smoothstepf = [](float e0, float e1, float v) {
        const float t = std::clamp((v - e0) / (e1 - e0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    for (int y = 0; y < kSize; ++y)
    {
        for (int x = 0; x < kSize; ++x)
        {
            const float ox = (static_cast<float>(x) + 0.5f) / (kSize * 0.5f) - 1.0f;
            const float oy = (static_cast<float>(y) + 0.5f) / (kSize * 0.5f) - 1.0f;
            const float round = std::sqrt(ox * ox + oy * oy);
            float d = round;
            if (blades >= 3u)
            {
                // The convex polygon by its half-planes -- the worst edge distance.
                float worst = -1.0e30f;
                for (uint32_t i = 0; i < blades; ++i)
                {
                    const float a = step * static_cast<float>(i);
                    worst = std::max(worst, (ox * std::cos(a) + oy * std::sin(a)) / apothem);
                }
                d = worst;
            }
            // A real ghost has a bright rim: the reflection is brightest where the cone of light
            // grazes the blade edge. Same profile the drawn ghosts used.
            // Tighter than the 64 px version's 0.85..1.0: that band was 18% of the radius, which
            // rounded an octagon into a circle before it ever reached the screen. At 256 px this
            // is still ~10 texels of antialiasing.
            const float body = 1.0f - smoothstepf(0.94f, 1.0f, d);
            const float rim = smoothstepf(0.80f, 0.96f, d) * (1.0f - smoothstepf(0.96f, 1.0f, d));
            const float v = std::clamp(body * 0.75f + rim * 0.6f, 0.0f, 1.0f);
            const uint8_t b = static_cast<uint8_t>(std::lround(v * 255.0f));
            uint8_t* px = &pixels[(static_cast<size_t>(y) * kSize + x) * 4u];
            px[0] = b; px[1] = b; px[2] = b; px[3] = 255u;
        }
    }

    const std::uint32_t slot = 1u - flareBokehSlot_;
    auto batch = std::make_unique<UploadBatch>();
    if (!batch->Begin(renderer))
    {
        return;
    }
    flareBokeh_[slot].CreateFromRGBA8(renderer, batch->CommandList(), pixels.data(), kSize, kSize,
                                      batch->KeepAlive(), L"FlareBokeh");
    if (!batch->Submit(renderer))
    {
        return;
    }
    flareBokehUpload_ = std::move(batch);
    flareBokehPending_ = static_cast<std::int32_t>(slot);
    flareBokehSafeFrame_ = renderer->GetTotalFrameNumber() + render::kFrameCount + 1u;
    flareBokehBlades_ = blades;
}

// DLSS-split: the upscale, on its own command list so its Streamline recording overlaps the
// tonemap's. It performs no decision of its own — `Renderer::WillEvaluateDlss` already said this
// frame runs it, and both points are emitted whatever `Evaluate` returns, because a declared point
// that goes unemitted leaves the compile a transition ahead of the GPU.
void SceneRenderer::Pass_Dlss(Renderer* renderer, RenderGraphPassContext ctx, const DlssPoints& pts)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassDlss);
        // The three inputs to their read states and the output to UAV — the states DlssHandler
        // then hands to Streamline through its COMMON bracket.
        renderer->EmitPoint(t.cl, pts.apply);
        renderer->EvaluateDLSS(t.cl);
        // ...and the upscaled image shader-readable for the tonemap, which is being recorded on
        // another thread right now and was promised exactly this.
        renderer->EmitPoint(t.cl, pts.output);
    }
    ctx.EndCL(t);
}

void SceneRenderer::Pass_Tonemap(Renderer* renderer, RenderGraphPassContext ctx, bool ranDlss)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassTonemap);
        const auto& D = renderer->GetDeferredForFrame();
        ctx.ApplyDeclaredStates(t.cl);
        // DLSS-split: `ranDlss` is the PREDICTION Main_DLSS was built from, not a result read back
        // from the evaluate — the two passes record concurrently, so there is nothing to read.
        if (!ranDlss)
        {
            // With DLSS on, the upscale pass performs these three as a side effect of consuming
            // depth + velocity and reading scene colour. With it off nothing else would, and the
            // frame would end with the forward targets still bound.
            renderer->Transition(t.cl, D.gbVelocity.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            renderer->Transition(t.cl, D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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
        // P8: build the bloom pyramid off whatever the tonemap is about to read — the upscaled
        // image when DLSS runs, which the submission order guarantees is finished on the GPU
        // before this list executes.
        if (bloomActive_)
        {
            // The CPU cost of RECORDING the bloom, which is the wide unnamed block after
            // DLSS::Evaluate on the CPU timeline -- the GPU side already has its own scope.
            CPU_SCOPE(ProfilerScopes::kTonemapBloomRecord);
            if (frame_->settings.bloom.method == 1u)
            {
                Bloom_Convolve(renderer, t.cl, tonemapSrc);
            }
            else
            {
                Bloom_Build(renderer, t.cl, tonemapSrc);
            }
        }
        const D3D12_GPU_DESCRIPTOR_HANDLE samplerTable = renderer->GetSamplerManager()->GetTable(renderer, tonemapSamplers);
        // P2: exposure is applied here — after the DLSS resolve above, before the tone curve.
        // The buffer is bound even while dormant so the descriptor table shape is constant; the
        // shader multiplies by a literal 1.0 when the flag is 0.
        ExposureMetering& metering = renderer->Exposure();
        const bool applyExposure = frame_->cameraExposure.enabled && metering.IsReady();
        const UINT tonemapCb = resources_.GetTonemapCBSizeBytes();
        {
        CPU_SCOPE(ProfilerScopes::kTonemapCurveRecord);
        GPU_SCOPE(t.cl, ProfilerScopes::kTonemapCurve);
        RecordComputeDispatch(renderer, t.cl, tonemapMaterial.get(), tonemapCb,
            [this, applyExposure](uint8_t* dest) {
                // Taken from the GATE, not from the setting: if the chain did not run this frame
                // the target holds the previous frame's image (or nothing at all), and a non-zero
                // scatter here would composite it.
                const BloomApplyConstants apply =
                    bloomActive_ ? Bloom_ApplyConstants() : BloomApplyConstants{};
                resources_.WriteTonemapConstants(applyExposure, frame_->colorPipeline,
                                                 frame_->cameraExposure, apply, dest);
            },
            { tonemapSrc, metering.BaseLumSrv(), D.bloomUpSRV },
            { D.tonemapUAV, metering.ExposureUav() }, samplerTable,
            renderer->GetWidth(), renderer->GetHeight(),
            D.tonemap.Get());
        }

        CPU_SCOPE(ProfilerScopes::kTonemapTailRecord);
        bool ranFxaa = false;
        auto fxaaMaterial = resources_.GetFxaaMaterial();
        const UINT fxaaCbSize = resources_.GetFxaaCBSizeBytes();
        if (fxaaMaterial && fxaaCbSize > 0 && renderer->GetWidth() > 0 && renderer->GetHeight() > 0 && frame_->settings.doFxaa)
        {
            GPU_SCOPE(t.cl, ProfilerScopes::kTonemapFxaa);
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
            GPU_SCOPE(t.cl, ProfilerScopes::kTonemapResolve);
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

// Which resource + SRV the fullscreen debug blit shows, from SceneRenderSettings::debugTexTarget.
// One function so the pass body and its declaration list cannot disagree about the answer.
SceneRenderer::DebugTexPick SceneRenderer::PickDebugTexTarget(
    const RenderTargetManager::DeferredTargets& D, int index)
{
    switch (index)
    {
    case 1: return { D.gtao.Get(), D.gtaoSRV };
    case 2: return { D.gtaoFiltered.Get(), D.gtaoFilteredSRV };
    case 3: return { D.gtaoHistory.Get(), D.gtaoHistorySRV };
    case 4: return { D.gtaoUpsampled.Get(), D.gtaoUpsampledSRV };
    case 5: return { D.hzb.Get(), D.hzbSRV };     // P6C, mip chosen by debugTexMip
    case 6: return { D.depth.Get(), D.depthSRV }; // the pyramid's source, for checking it against
    // CLOSEST is retained as a debug/P9 resource; the current UE SSR path reads FURTHEST instead.
    case 7: return { D.hzbClosest.Get(), D.hzbClosestSRV };
    // The reflection buffer itself, shown as ALPHA = the ray's visibility. This is the only view
    // that answers "did the ray find anything" WITHOUT the answer being filtered through shading,
    // the glossy blur and compose -- which is what a tracer A/B actually needs to compare.
    case 8: return { D.reflection.Get(), D.reflectionSRV };
    default: return { D.shadow.Get(), D.shadowSRV };
    }
}

// `on`, `pick` and `canonical` are decided in Render(), where the declarations are made from the
// same values — the body must not re-derive them, or the two could disagree and the pass would emit
// barriers for a resource it never touches.
void SceneRenderer::Pass_Debug(Renderer* renderer, RenderGraphPassContext ctx,
    const DebugTexPick& pick, const DebugBlitPoints& pts)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassDebug);
        // The blit reads in a PIXEL shader while these targets rest NON_PIXEL readable, so the
        // state has to be declared. It did not used to be: the pass read the shadow atlas with no
        // declaration at all and got away with it.
        renderer->EmitPoint(t.cl, pts.read);
        renderer->RecordBindDefaultsNoClear(t.cl);

        auto h = renderer->GetRenderContextPool()->Acquire();
        auto& rc = h.ref();

        // Mip selection + the depth stretch. Reversed-Z device depth would otherwise blit as a
        // black rectangle, and an unreadable debug view is not a debug view.
        struct DebugTexCB
        {
            std::uint32_t mipLevel;
            std::uint32_t depthStretch;
            std::uint32_t showAlpha;
            std::uint32_t pad1;
        } cb{};
        const int target = frame_->settings.debugTexTarget;
        // The depth-like targets: both pyramids and the depth buffer they come from. Keep this ONE
        // predicate -- when the closest pyramid was added as target 7 and only this list was
        // missed, its capture came out with a value range of 0..2/255 and read as a broken
        // reduction rather than as an unstretched one.
        const bool depthLike = (target == 5 || target == 6 || target == 7);
        cb.mipLevel = static_cast<std::uint32_t>(std::max(0, frame_->settings.debugTexMip));
        cb.depthStretch = depthLike ? 1u : 0u;
        cb.showAlpha = (target == 8) ? 1u : 0u; // the reflection buffer's hit mask
        {
            auto cbAlloc = renderer->GetFrameResource()->AllocDynamic(
                sizeof(DebugTexCB), render::kConstantBufferAlignment);
            std::memcpy(cbAlloc.cpu, &cb, sizeof(cb));
            rc.cbv[0] = cbAlloc.gpu;
        }

        rc.srvTable[0] = renderer->StageSrvUavTable({ pick.srv }).gpu; // t0
        // POINT for the depth-like targets: they are inspected texel by texel (and verified against
        // a CPU reduction), and a linear stretch would blend neighbouring texels into every sample,
        // which is exactly what makes such a check impossible. Linear stays for the smooth targets.
        const auto debugSamplers = depthLike
            ? std::array{ *SamplerManager::PointClamp() }
            : std::array{ *SamplerManager::LinearClamp() };
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

    // OUTSIDE the do-block on purpose: the `break` above (no debug material) must not skip the
    // restore. Once the declarations are made, every declared point has to be emitted, or the next
    // frame's barriers are compiled against a state the resource never reached.
    renderer->EmitPoint(t.cl, pts.restore);

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

        // Engine text UNDER ImGui. ImGui is the interactive layer -- a dev window you dragged over
        // the HUD has to occlude it, not be written through by the FPS line or the LOD debug
        // labels. This was the other way round, which is why the LOD debug view painted over
        // whichever panel you had open while reading it.
        if (auto* tm = renderer->GetTextManager())
        {
            tm->Draw(renderer, t.cl);
        }

        renderer->RenderImGui(t.cl);
        renderer->RestoreGraphicsStateAfterExternalDraw(t.cl);
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}
