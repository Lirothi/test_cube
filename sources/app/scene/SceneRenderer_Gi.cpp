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
    uint32_t spotCount = 0;   uint32_t pointLightIndex = 0; uint32_t pointCount = 0;         uint32_t alphaMode = 2;
    uint32_t bounceOn = 0;    float skyRayDistance = 15.0f; uint32_t pad0 = 0;               uint32_t pad1 = 0;
};
static_assert(sizeof(DdgiConstants) == 64 + 9 * 16, "DdgiConstants mirrors ddgi_constants.hlsli");

// Bindless scene slots of the trace (BindlessTable kScenePerFrame = 48): reflections own 0-12, the debug
// view 13-16, the glass dispatch 17-25, the sky visibility 26-31.
constexpr UINT kGiSlotTlas = 32;
constexpr UINT kGiSlotRays = 33;
constexpr UINT kGiSlotIrradiance = 34;
constexpr UINT kGiSlotDepth = 35;
constexpr UINT kGiSlotState = 36;
constexpr UINT kGiSlotSkyIrradiance = 37;
constexpr UINT kGiSlotSpots = 38;
constexpr UINT kGiSlotPoints = 39;

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

void SceneRenderer::FillGiShaderParams(int32_t outBase[4], Math::float4& outParams) const
{
    const int* base = ddgi_.BaseCell();
    outBase[0] = base[0];
    outBase[1] = base[1];
    outBase[2] = base[2];
    outBase[3] = static_cast<int32_t>(ddgi_.AppliedGeneration());
    const SceneRenderSettings& s = frame_->settings;
    outParams = Math::float4(decisions_.giApplied ? ddgi_.Spacing() : 0.0f, std::max(s.giIntensity, 0.0f),
                             static_cast<float>(s.giDebug), 0.0f);
}

void SceneRenderer::BuildGi(Renderer* renderer, GraphBuild& gb)
{
    auto& rg = gb.rg;
    giTraced_.store(false);
    if (!decisions_.gi || !ddgi_.Ready() || gb.pBuildAS == static_cast<size_t>(-1) || !frame_->camera)
    {
        return; // the atlases keep the last generation traced; lighting reads it while giApplied says so
    }
    const SceneRenderSettings& s = frame_->settings;
    GiChain chain{};
    chain.frame = ddgi_.BeginFrame(frame_->camera->GetPosition(), s.giSpacing, s.giUpdateDivisor);

    // The trace: on the DIRECT queue, after Pass_BuildAS -- the TLAS and the bindless table are
    // undeclared reads, and the body decides from FrameReady, which the BuildAS recording sets (the
    // mtDep, as for RTTrace and SkyVis). It needs no G-buffer: it runs as early as the AS allows.
    const size_t pTrace = rg.AddPass2(RenderPass::Main_GiTrace, { gb.pBuildAS }, { gb.pBuildAS }, {},
        [this, renderer, chain](RenderGraphPassContext& ctx) -> std::function<void(RenderGraphPassContext)> {
            GiChain c = chain;
            ctx.NextPoint();
            c.point = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(ddgi_.Rays(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.Use(ddgi_.Irradiance(), DdgiVolume::kRestState); // the infinite bounce reads last frame's
            ctx.Use(ddgi_.Depth(), DdgiVolume::kRestState);
            ctx.Use(ddgi_.State(), DdgiVolume::kRestState);
            ctx.NextPoint();
            c.pointRestore = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(ddgi_.Rays(), DdgiVolume::kRestState); // the update reads them; the transition is the sync
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
            ctx.Use(ddgi_.Rays(), DdgiVolume::kRestState);
            ctx.Use(ddgi_.Irradiance(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.Use(ddgi_.Depth(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.Use(ddgi_.State(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ctx.NextPoint();
            c.pointRestore = ctx.usePoint ? *ctx.usePoint : 0u;
            ctx.Use(ddgi_.Irradiance(), DdgiVolume::kRestState);
            ctx.Use(ddgi_.Depth(), DdgiVolume::kRestState);
            ctx.Use(ddgi_.State(), DdgiVolume::kRestState);
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
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassGiTrace);
        renderer->EmitPoint(t.cl, chain.point); // rays -> UAV

        Material* material = ddgi_.TraceMaterial();
        const UINT frameIndex = renderer->GetCurrentFrameIndex();
        const D3D12_CPU_DESCRIPTOR_HANDLE tlasSrv = rtAs_.Manager().TlasSrvCpu(frameIndex);
        const bool traceable = material && frame_->dirLight && rtAs_.Bindless().FrameReady(frameIndex) &&
                               tlasSrv.ptr != 0 && rtAs_.Manager().TlasInstanceCount(frameIndex) != 0;
        giTraced_.store(traceable);
        if (traceable)
        {
            auto& bindless = rtAs_.Bindless();
            bindless.WriteSceneDescriptor(frameIndex, kGiSlotTlas, tlasSrv);
            bindless.WriteSceneDescriptor(frameIndex, kGiSlotRays, ddgi_.RaysUav());
            bindless.WriteSceneDescriptor(frameIndex, kGiSlotIrradiance, ddgi_.IrradianceSrv());
            bindless.WriteSceneDescriptor(frameIndex, kGiSlotDepth, ddgi_.DepthSrv());
            bindless.WriteSceneDescriptor(frameIndex, kGiSlotState, ddgi_.StateSrv());
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

            const SceneRenderSettings& s = frame_->settings;
            const DirectionalLight& dl = *frame_->dirLight;
            DdgiConstants c = MakeVolumeConstants(chain.frame, s);
            c.sunDirWS = dl.GetDirection();
            c.ambientIntensity = dl.GetAmbient();
            c.lightRgb = dl.GetEffectiveColor(); // lighting_cs's lightRgb: the sun (or the moon) with its intensity
            c.skyIrradianceScale = skybox ? skybox->GetExposure() * dl.GetSkyFillIntensity() : 0.0f;
            c.tlasIndex = bindless.SceneIndex(frameIndex, kGiSlotTlas);
            c.geomInfoIndex = bindless.GeomInfoIndex(frameIndex);
            c.rayUavIndex = bindless.SceneIndex(frameIndex, kGiSlotRays);
            c.irradianceIndex = bindless.SceneIndex(frameIndex, kGiSlotIrradiance);
            c.depthIndex = bindless.SceneIndex(frameIndex, kGiSlotDepth);
            c.stateIndex = bindless.SceneIndex(frameIndex, kGiSlotState);
            c.skyIrradianceIndex = haveSkyIrradiance ? bindless.SceneIndex(frameIndex, kGiSlotSkyIrradiance) : 0u;
            c.spotLightIndex = bindless.SceneIndex(frameIndex, kGiSlotSpots);
            c.spotCount = haveSpots ? spotCount : 0u;
            c.pointLightIndex = bindless.SceneIndex(frameIndex, kGiSlotPoints);
            c.pointCount = havePoints ? pointCount : 0u;
            c.alphaMode = std::min(s.rtAlphaMode, 2u); // the foliage cut-out as every RT ray sees it
            // The volume feeds back into its own hits once a generation exists to read.
            c.bounceOn = (s.giInfiniteBounce && ddgi_.AppliedGeneration() == chain.frame.generation) ? 1u : 0u;
            c.skyRayDistance = std::max(s.skyVisMaxDistance, 0.5f); // plan S's reach for the sky at a hit

            auto cb = renderer->GetFrameResource()->AllocDynamic(sizeof(DdgiConstants), render::kConstantBufferAlignment);
            std::memcpy(cb.cpu, &c, sizeof(c));
            // Bespoke bindless dispatch, as Pass_RTTrace: the persistent heap, then the frame heap back.
            ID3D12DescriptorHeap* heaps[] = { bindless.Heap() };
            t.cl->SetDescriptorHeaps(1, heaps);
            t.cl->SetComputeRootSignature(material->GetRootSignature());
            t.cl->SetPipelineState(material->GetPipelineState());
            t.cl->SetComputeRootConstantBufferView(0, cb.gpu);
            t.cl->Dispatch(chain.frame.probes, 1, 1);
            renderer->BindDescriptorHeaps(t.cl);
        }
        renderer->EmitPoint(t.cl, chain.pointRestore); // rays -> rest
    }
    ctx.EndCL(t);
}

void SceneRenderer::Pass_GiUpdate(Renderer* renderer, RenderGraphPassContext ctx, const GiChain& chain)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassGiUpdate);
        renderer->EmitPoint(t.cl, chain.point); // atlases + state -> UAV
        Material* material = ddgi_.UpdateMaterial();
        if (material && giTraced_.load())
        {
            const DdgiConstants c = MakeVolumeConstants(chain.frame, frame_->settings);
            auto cb = renderer->GetFrameResource()->AllocDynamic(sizeof(DdgiConstants), render::kConstantBufferAlignment);
            std::memcpy(cb.cpu, &c, sizeof(c));
            renderer->BindDescriptorHeaps(t.cl);
            auto h = renderer->GetRenderContextPool()->Acquire();
            RenderContext& rc = h.ref();
            rc.cbv[0] = cb.gpu;
            rc.srvTable[0] = renderer->StageSrvUavTable({ ddgi_.RaysSrv() }).gpu;
            rc.uavTable[0] = renderer->StageSrvUavTable({ ddgi_.IrradianceUav(), ddgi_.DepthUav(), ddgi_.StateUav() }).gpu;
            if (material->Bind(t.cl, rc))
            {
                t.cl->Dispatch(chain.frame.probes, 1, 1); // one group per probe
                ddgi_.MarkTraced(chain.frame.generation);
            }
        }
        renderer->EmitPoint(t.cl, chain.pointRestore); // back to rest for lighting (and next frame's trace)
    }
    ctx.EndCL(t);
}
