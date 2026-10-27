// Interior lighting plan D: the GI probe volume (DDGI over inline RayQuery) -- the two graph passes and
// what lighting reads. The volume's resources and per-frame placement are rendering/lighting/DdgiVolume;
// the shaders ddgi_trace_cs.hlsl, ddgi_update_cs.hlsl and ddgi_common.hlsli.
#include "app/scene/SceneRenderer.h"

#include <algorithm>
#include <cstring>

#include "core/logging/Log.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "materials/Material.h"
#include "rendering/core/RenderContext.h"
#include "rendering/core/RenderContextPool.h"
#include "rendering/core/RenderGraph.h"
#include "rendering/core/Renderer.h"
#include "rendering/lighting/DirectionalLight.h"
#include "rendering/lighting/LightManager.h"
#include "rendering/lighting/Skybox.h"
#include "app/camera/Camera.h"
#include "app/scene/SceneRenderInternal.h"

namespace {
// Mirrors `cbuffer DdgiConstants` in shaders/ddgi_constants.hlsli (row-major; a matrix, then rows of 4).
struct DdgiConstants
{
    Math::mat4 rayRotation;
    int32_t volumeBase[4] = { 0, 0, 0, 0 };
    float volumeParams[4] = { 0.5f, 0.9f, 30.0f, 1.0f };
    uint32_t schedule[4] = { 1, 0, 0, 0 };
    Math::float3 sunDirWS;    float ambientIntensity = 0.0f;
    Math::float3 lightRgb;    float skyIrradianceScale = 1.0f;
    uint32_t tlasIndex = 0;   uint32_t geomInfoIndex = 0;   uint32_t rayUavIndex = 0;        uint32_t irradianceIndex = 0;
    uint32_t depthIndex = 0;  uint32_t stateIndex = 0;      uint32_t skyIrradianceIndex = 0; uint32_t spotLightIndex = 0;
    uint32_t spotCount = 0;   uint32_t pointLightIndex = 0; uint32_t pointCount = 0;         uint32_t twoSidedSheets = 1;
    uint32_t bounceOn = 0;    float skyRayDistance = 15.0f; uint32_t meshOpacity = 1;        uint32_t pad1 = 0;
};
static_assert(sizeof(DdgiConstants) == 64 + 9 * 16, "DdgiConstants mirrors ddgi_constants.hlsli");

// Bindless scene slots of the trace (BindlessTable kScenePerFrame = 52): reflections own 0-12 and 46-49, the debug
// view 13-16, the glass dispatch 17-25, the sky visibility 26-31 and 40-41. Per cascade: rays,
// irradiance, depth, state -- the near one 33-36, the far one 42-45; the rest is shared.
constexpr UINT kGiSlotTlas = 32;
constexpr UINT kGiSlotRays[2] = { 33, 42 };
constexpr UINT kGiSlotIrradiance[2] = { 34, 43 };
constexpr UINT kGiSlotDepth[2] = { 35, 44 };
constexpr UINT kGiSlotState[2] = { 36, 45 };
constexpr UINT kGiSlotSkyIrradiance = 37;
constexpr UINT kGiSlotSpots = 38;
constexpr UINT kGiSlotPoints = 39;
// The near cascade fades into the far one over its outer metres (DdgiIrradiance's edge fade, in cells):
// one cell alone (0.5 m) is a seam a walk across makes visible.
constexpr float kGiNearFadeMetres = 2.0f;

// The part both passes share: where the volume stands, which probes, the rays' rotation.
DdgiConstants MakeVolumeConstants(const DdgiVolume::Frame& f, const SceneRenderSettings& s)
{
    DdgiConstants c{};
    c.rayRotation = f.rayRotation;
    c.volumeBase[0] = f.baseCell[0];
    c.volumeBase[1] = f.baseCell[1];
    c.volumeBase[2] = f.baseCell[2];
    c.volumeBase[3] = static_cast<int32_t>(f.generation);
    c.volumeParams[0] = f.spacing;
    c.volumeParams[1] = std::clamp(s.giHysteresis, 0.0f, 0.99f);
    c.volumeParams[2] = std::max(s.giRayDistance, 1.0f);
    // The depth moments only need to say "an occluder nearer than any point the probe lights":
    // those are at most a cell diagonal away, so twice the spacing keeps FP16 precise.
    c.volumeParams[3] = 2.0f * f.spacing;
    c.schedule[0] = f.divisor;
    c.schedule[1] = f.phase;
    c.schedule[2] = f.probes;
    c.schedule[3] = f.seed;
    return c;
}
} // namespace

void SceneRenderer::FillGiShaderParams(uint32_t cascade, int32_t outBase[4], Math::float4& outParams) const
{
    const DdgiVolume& volume = ddgi_[std::min(cascade, kGiCascades - 1u)];
    const int* base = volume.BaseCell();
    outBase[0] = base[0];
    outBase[1] = base[1];
    outBase[2] = base[2];
    outBase[3] = static_cast<int32_t>(volume.AppliedGeneration());
    const SceneRenderSettings& s = frame_->settings;
    const bool applied = cascade == 0u ? decisions_.giApplied : decisions_.giFarApplied;
    const float spacing = std::max(volume.Spacing(), 1.0e-3f);
    // The near cascade fades over kGiNearFadeMetres when the far one takes over beyond it, else over
    // its last cell (the volume's edge, as before the far cascade existed).
    const float fadeCells = (cascade == 0u && decisions_.giFarApplied) ? std::max(kGiNearFadeMetres / spacing, 1.0f) : 1.0f;
    outParams = Math::float4(applied ? volume.Spacing() : 0.0f, std::max(s.giIntensity, 0.0f),
                             static_cast<float>(s.giDebug), fadeCells);
}

void SceneRenderer::BuildGi(Renderer* renderer, GraphBuild& gb)
{
    auto& rg = gb.rg;
    giTraced_.store(false);
    if (!decisions_.gi || !ddgi_[0].Ready() || gb.pBuildAS == static_cast<size_t>(-1) || !frame_->camera)
    {
        return; // the atlases keep the last generation traced; lighting reads it while giApplied says so
    }
    const SceneRenderSettings& s = frame_->settings;
    GiChain chain{};
    chain.cascades = decisions_.giFar ? 2u : 1u;
    const Math::float3 cameraPos = frame_->camera->GetPosition();
    chain.frame[0] = ddgi_[0].BeginFrame(cameraPos, s.giSpacing, s.giUpdateDivisor);
    if (chain.cascades > 1u)
    {
        // At least twice the near spacing, or the far cascade is the near one again.
        chain.frame[1] = ddgi_[1].BeginFrame(cameraPos, std::max(s.giFarSpacing, 2.0f * s.giSpacing), s.giFarDivisor);
    }

    // The trace: on the DIRECT queue, after Pass_BuildAS -- the TLAS and the bindless table are
    // undeclared reads, and the body decides from FrameReady, which the BuildAS recording sets (the
    // mtDep, as for RTTrace and SkyVis). It needs no G-buffer, but it WAITS for one: BuildAS is on the
    // async queue, so a graphics pass with nothing else before it is scheduled right after the prologue
    // and parks the whole graphics queue on that cross-queue wait -- 518 us idle every frame on
    // wind_test (the compute queue starts BuildAS ~350 us after the prologue's signal, the graphics
    // queue resumes ~160 us after BuildAS's). Behind the G-buffer the wait is long satisfied: GPU.Frame
    // 4.019 -> 3.521 ms, CPU.Frame 4.242 -> 3.628 (interleaved x3, every run apart). The rule of
    // Main_OceanReadback's hole (docs/async_compute_plan.md, "the GI trace parked the graphics queue").
    // Both cascades in one pass each: two dispatches in one list, their rays in their own buffers.
    RenderGraph<kMainRenderGraphPassCount>::DependencyList tracePrereqs{};
    RenderGraph<kMainRenderGraphPassCount>::PushDependency(tracePrereqs, gb.pBuildAS);
    RenderGraph<kMainRenderGraphPassCount>::PushDependency(tracePrereqs, gb.pGbufDone);
    const size_t pTrace = rg.AddPass2(RenderPass::Main_GiTrace, tracePrereqs, { gb.pBuildAS }, {},
        [this, renderer, chain](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            GiChain c = chain;
            ctx.NextPoint();
            c.point = ctx.usePoint ? *ctx.usePoint : 0u;
            for (uint32_t k = 0u; k < c.cascades; ++k)
            {
                ctx.Use(ddgi_[k].Rays(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                ctx.Use(ddgi_[k].Irradiance(), DdgiVolume::kRestState); // the infinite bounce reads last frame's
                ctx.Use(ddgi_[k].Depth(), DdgiVolume::kRestState);
                ctx.Use(ddgi_[k].State(), DdgiVolume::kRestState);
            }
            ctx.NextPoint();
            c.pointRestore = ctx.usePoint ? *ctx.usePoint : 0u;
            for (uint32_t k = 0u; k < c.cascades; ++k)
            {
                ctx.Use(ddgi_[k].Rays(), DdgiVolume::kRestState); // the update reads them; the transition is the sync
            }
            return [this, renderer, c](RenderGraphPassContext pc) {
                CPU_SCOPE(ProfilerScopes::kPassGiTrace);
                Pass_GiTrace(renderer, pc, c);
            };
        });
    // The update blends what the trace wrote; its mtDep on the trace is what lets it read giTraced_.
    gb.pGiUpdate = rg.AddPass2(RenderPass::Main_GiUpdate, { pTrace }, { pTrace }, {},
        [this, renderer, chain](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            GiChain c = chain;
            ctx.NextPoint();
            c.point = ctx.usePoint ? *ctx.usePoint : 0u;
            for (uint32_t k = 0u; k < c.cascades; ++k)
            {
                ctx.Use(ddgi_[k].Rays(), DdgiVolume::kRestState);
                ctx.Use(ddgi_[k].Irradiance(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                ctx.Use(ddgi_[k].Depth(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                ctx.Use(ddgi_[k].State(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            ctx.NextPoint();
            c.pointRestore = ctx.usePoint ? *ctx.usePoint : 0u;
            for (uint32_t k = 0u; k < c.cascades; ++k)
            {
                ctx.Use(ddgi_[k].Irradiance(), DdgiVolume::kRestState);
                ctx.Use(ddgi_[k].Depth(), DdgiVolume::kRestState);
                ctx.Use(ddgi_[k].State(), DdgiVolume::kRestState);
            }
            return [this, renderer, c](RenderGraphPassContext pc) {
                CPU_SCOPE(ProfilerScopes::kPassGiUpdate);
                Pass_GiUpdate(renderer, pc, c);
            };
        });
}

void SceneRenderer::Pass_GiTrace(Renderer* renderer, RenderGraphPassContext ctx, const GiChain& chain)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    renderer->EmitPoint(t.cl, chain.point); // rays -> UAV

    Material* material = ddgi_[0].TraceMaterial();
    const UINT frameIndex = renderer->GetCurrentFrameIndex();
    const D3D12_CPU_DESCRIPTOR_HANDLE tlasSrv = rtAs_.Manager().TlasSrvCpu(frameIndex);
    const bool traceable = material && frame_->dirLight && rtAs_.Bindless().FrameReady(frameIndex) &&
                           tlasSrv.ptr != 0 && rtAs_.Manager().TlasInstanceCount(frameIndex) != 0;
    giTraced_.store(traceable);
    if (traceable)
    {
        auto& bindless = rtAs_.Bindless();
        bindless.WriteSceneDescriptor(frameIndex, kGiSlotTlas, tlasSrv);
        for (uint32_t k = 0u; k < chain.cascades; ++k)
        {
            bindless.WriteSceneDescriptor(frameIndex, kGiSlotRays[k], ddgi_[k].RaysUav());
            bindless.WriteSceneDescriptor(frameIndex, kGiSlotIrradiance[k], ddgi_[k].IrradianceSrv());
            bindless.WriteSceneDescriptor(frameIndex, kGiSlotDepth[k], ddgi_[k].DepthSrv());
            bindless.WriteSceneDescriptor(frameIndex, kGiSlotState[k], ddgi_[k].StateSrv());
        }
        Skybox* skybox = frame_->skybox;
        const bool haveSkyIrradiance = skybox && skybox->HasIbl();
        if (haveSkyIrradiance) { bindless.WriteSceneDescriptor(frameIndex, kGiSlotSkyIrradiance, skybox->IrradianceSrv()); }
        // The spot/point buffers, CPU-filled in EnsureFrameResources (as Pass_RTTrace reads them).
        LightManager* lm = frame_->lightManager;
        const UINT spotCount = lm ? static_cast<UINT>(lm->GetSpotLightCount()) : 0u;
        const UINT pointCount = lm ? static_cast<UINT>(lm->PointLights().size()) : 0u;
        const D3D12_CPU_DESCRIPTOR_HANDLE spotSrv = lm ? lm->GetSpotLightSrv(frameIndex) : D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
        const D3D12_CPU_DESCRIPTOR_HANDLE pointSrv = lm ? lm->GetPointLightSrv(frameIndex) : D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
        const bool haveSpots = spotCount > 0u && spotSrv.ptr != 0;
        const bool havePoints = pointCount > 0u && pointSrv.ptr != 0;
        if (haveSpots) { bindless.WriteSceneDescriptor(frameIndex, kGiSlotSpots, spotSrv); }
        if (havePoints) { bindless.WriteSceneDescriptor(frameIndex, kGiSlotPoints, pointSrv); }

        // Bespoke bindless dispatch, as Pass_RTTrace: the persistent heap, then the frame heap back.
        ID3D12DescriptorHeap* heaps[] = { bindless.Heap() };
        t.cl->SetDescriptorHeaps(1, heaps);
        t.cl->SetComputeRootSignature(material->GetRootSignature());
        t.cl->SetPipelineState(material->GetPipelineState());
        const SceneRenderSettings& s = frame_->settings;
        const DirectionalLight& dl = *frame_->dirLight;
        for (uint32_t k = 0u; k < chain.cascades; ++k)
        {
            GPU_SCOPE(t.cl, k == 0u ? ProfilerScopes::kPassGiTrace : ProfilerScopes::kPassGiTraceFar);
            DdgiConstants c = MakeVolumeConstants(chain.frame[k], s);
            c.sunDirWS = dl.GetDirection();
            c.ambientIntensity = dl.GetAmbient();
            c.lightRgb = dl.GetEffectiveColor(); // lighting_cs's lightRgb: the sun (or the moon) with its intensity
            c.skyIrradianceScale = skybox ? skybox->GetExposure() * dl.GetSkyFillIntensity() : 0.0f;
            c.tlasIndex = bindless.SceneIndex(frameIndex, kGiSlotTlas);
            c.geomInfoIndex = bindless.GeomInfoIndex(frameIndex);
            c.rayUavIndex = bindless.SceneIndex(frameIndex, kGiSlotRays[k]);
            c.irradianceIndex = bindless.SceneIndex(frameIndex, kGiSlotIrradiance[k]);
            c.depthIndex = bindless.SceneIndex(frameIndex, kGiSlotDepth[k]);
            c.stateIndex = bindless.SceneIndex(frameIndex, kGiSlotState[k]);
            c.skyIrradianceIndex = haveSkyIrradiance ? bindless.SceneIndex(frameIndex, kGiSlotSkyIrradiance) : 0u;
            c.spotLightIndex = bindless.SceneIndex(frameIndex, kGiSlotSpots);
            c.spotCount = haveSpots ? spotCount : 0u;
            c.pointLightIndex = bindless.SceneIndex(frameIndex, kGiSlotPoints);
            c.pointCount = havePoints ? pointCount : 0u;
            // Each cascade feeds back into its own hits once a generation of it exists to read.
            c.bounceOn = (s.giInfiniteBounce && ddgi_[k].AppliedGeneration() == chain.frame[k].generation) ? 1u : 0u;
            c.twoSidedSheets = s.giTwoSidedSheets ? 1u : 0u;
            c.meshOpacity = s.rtMeshOpacity ? 1u : 0u;
            c.skyRayDistance = std::max(s.skyVisMaxDistance, 0.5f); // plan S's reach for the sky at a hit

            auto cb = renderer->GetFrameResource()->AllocDynamic(sizeof(DdgiConstants), render::kConstantBufferAlignment);
            std::memcpy(cb.cpu, &c, sizeof(c));
            t.cl->SetComputeRootConstantBufferView(0, cb.gpu);
            t.cl->Dispatch(chain.frame[k].probes, 1, 1);
        }
        renderer->BindDescriptorHeaps(t.cl);
    }
    renderer->EmitPoint(t.cl, chain.pointRestore); // rays -> rest
    ctx.EndCL(t);
}

void SceneRenderer::Pass_GiUpdate(Renderer* renderer, RenderGraphPassContext ctx, const GiChain& chain)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    renderer->EmitPoint(t.cl, chain.point); // atlases + state -> UAV
    Material* material = ddgi_[0].UpdateMaterial();
    if (material && giTraced_.load())
    {
        renderer->BindDescriptorHeaps(t.cl);
        for (uint32_t k = 0u; k < chain.cascades; ++k)
        {
            GPU_SCOPE(t.cl, k == 0u ? ProfilerScopes::kPassGiUpdate : ProfilerScopes::kPassGiUpdateFar);
            DdgiVolume& volume = ddgi_[k];
            const DdgiConstants c = MakeVolumeConstants(chain.frame[k], frame_->settings);
            auto cb = renderer->GetFrameResource()->AllocDynamic(sizeof(DdgiConstants), render::kConstantBufferAlignment);
            std::memcpy(cb.cpu, &c, sizeof(c));
            auto h = renderer->GetRenderContextPool()->Acquire();
            RenderContext& rc = h.ref();
            rc.cbv[0] = cb.gpu;
            rc.srvTable[0] = renderer->StageSrvUavTable({ volume.RaysSrv() }).gpu;
            rc.uavTable[0] = renderer->StageSrvUavTable({ volume.IrradianceUav(), volume.DepthUav(), volume.StateUav() }).gpu;
            if (material->Bind(t.cl, rc))
            {
                t.cl->Dispatch(chain.frame[k].probes, 1, 1); // one group per probe
                volume.MarkTraced(chain.frame[k].generation);
            }
        }
    }
    renderer->EmitPoint(t.cl, chain.pointRestore); // back to rest for lighting (and next frame's trace)
    ctx.EndCL(t);
}
