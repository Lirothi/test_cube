// R2 (docs/scene_renderer_refactor_plan.md): reflections in all their variants: SSR, RT, glass, the blur/resolve chain, the ocean plane.
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

// R1 (docs/scene_renderer_refactor_plan.md): the helpers every pass body uses moved to an
// INTERNAL header, verbatim, so the bodies can be split across translation units. The
// using-directive keeps every call site spelled exactly as it was.
#include "app/scene/SceneRenderInternal.h"
using namespace scene_internal;

// ---- the SSR constant fills + Pass_ScreenSpaceReflections ----
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
    c.ueIntensity = r.intensity;
    c.ueRoughnessMaskScale = r.roughnessMaskScale;
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

// ---- RT reflections + the two glass paths ----
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
    // alphaMissKeep/frameSeed: stochastic coverage inflation for the RT foliage alpha test —
    // see RtAlphaCandidatePasses in rt_geometry.hlsli.
    uint32_t screenDepthIndex = 0; float alphaMissKeep = 0.0f; uint32_t frameSeed = 0;
    uint32_t alphaTestOff = 0; // 1 = RAY_FLAG_FORCE_OPAQUE at trace time (hard alpha kill switch)
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
        const D3D12_CPU_DESCRIPTOR_HANDLE tlasSrv = rtAs_.Manager().TlasSrvCpu(frameIndex);
        Skybox* skybox = frame_->skybox;
        if (!reflectMaterial || !rtAs_.Bindless().FrameReady(frameIndex) || tlasSrv.ptr == 0 ||
            rtAs_.Manager().TlasInstanceCount(frameIndex) == 0 || !frame_->dirLight || !skybox)
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
        rtAs_.Bindless().WriteSceneDescriptor(frameIndex, 0, tlasSrv);     // TLAS
        rtAs_.Bindless().WriteSceneDescriptor(frameIndex, 1, D.lightSRV);  // lit HDR (fast path)
        rtAs_.Bindless().WriteSceneDescriptor(frameIndex, 2, D.gbSRV[1]);  // GB1 (normal)
        rtAs_.Bindless().WriteSceneDescriptor(frameIndex, 3, D.depthSRV);  // Depth
        rtAs_.Bindless().WriteSceneDescriptor(frameIndex, 4, D.reflectionUAV); // reflection out -> blur/compose
        rtAs_.Bindless().WriteSceneDescriptor(frameIndex, 5, skybox->GetTex()->GetSRVCPU()); // skybox cube (env reflection)
        // P16.9: the cosine-convolved irradiance, so an OFF-SCREEN re-shade gets the same sky
        // fill the main pass uses instead of the legacy `ambient * sunColour` fraction.
        const bool haveSkyIrradiance = skybox->HasIbl();
        if (haveSkyIrradiance)
        {
            rtAs_.Bindless().WriteSceneDescriptor(frameIndex, 8, skybox->GetIrradianceTex()->GetSRVCPU());
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
        if (haveSpots)  { rtAs_.Bindless().WriteSceneDescriptor(frameIndex, 6, spotSrv); }
        if (havePoints) { rtAs_.Bindless().WriteSceneDescriptor(frameIndex, 7, pointSrv); }

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
        c.tlasIndex = rtAs_.Bindless().SceneIndex(frameIndex, 0);
        c.lightIndex = rtAs_.Bindless().SceneIndex(frameIndex, 1);
        c.gb1Index = rtAs_.Bindless().SceneIndex(frameIndex, 2);
        c.depthIndex = rtAs_.Bindless().SceneIndex(frameIndex, 3);
        c.screenDepthIndex = c.depthIndex; // opaque: primary == on-screen depth (no change)
        c.alphaMissKeep = std::clamp(frame_->settings.rtAlphaMissKeep, 0.0f, 1.0f);
        c.alphaTestOff = frame_->settings.rtAlphaTest ? 0u : 1u;
        // FROZEN dither, by measurement (ssr_bronze_palms, resolved frame-to-frame boil in the
        // mirror band): fill 0 = 0.41; fill 0.15 re-rolled per frame = 0.74; re-rolled once per
        // EMA time constant = 1.25 (coherent drift is the WORST case for the resolve); frozen =
        // 0.43. A static pattern realises the coverage inflation spatially, adds no dance at all,
        // and the EMA has nothing to chase. The seed still reaches the shader so a future
        // animated-dither experiment only touches this line.
        c.frameSeed = 0u;
        c.reflectionUavIndex = rtAs_.Bindless().SceneIndex(frameIndex, 4);
        c.skyboxIndex = rtAs_.Bindless().SceneIndex(frameIndex, 5);
        c.skyboxIntensity = skybox->GetExposure();
        c.skyIrradianceIndex = haveSkyIrradiance ? rtAs_.Bindless().SceneIndex(frameIndex, 8) : 0u; // P16.9
        c.skyIrradianceScale = skybox->GetExposure() * dl.GetSkyFillIntensity();
        c.groundAlbedoRgb = dl.GetGroundAlbedo(); // P16.12, same value lighting_cs gets
        c.geomInfoIndex = rtAs_.Bindless().GeomInfoIndex(frameIndex);
        c.spotLightIndex = rtAs_.Bindless().SceneIndex(frameIndex, 6);
        c.spotCount = haveSpots ? spotCount : 0u;
        c.pointLightIndex = rtAs_.Bindless().SceneIndex(frameIndex, 7);
        c.pointCount = havePoints ? pointCount : 0u;

        auto cb = renderer->GetFrameResource()->AllocDynamic(sizeof(RtReflectConstants), render::kConstantBufferAlignment);
        std::memcpy(cb.cpu, &c, sizeof(c));

        // Bespoke bindless dispatch (binds the persistent heap, not the frame heap).
        ID3D12DescriptorHeap* heaps[] = { rtAs_.Bindless().Heap() };
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
        const D3D12_CPU_DESCRIPTOR_HANDLE tlasSrv = rtAs_.Manager().TlasSrvCpu(frameIndex);
        Skybox* skybox = frame_->skybox;
        if (!reflectMaterial || !rtAs_.Bindless().FrameReady(frameIndex) || tlasSrv.ptr == 0 ||
            rtAs_.Manager().TlasInstanceCount(frameIndex) == 0 || !frame_->dirLight || !skybox)
        {
            break;
        }

        // Glass scene-descriptor range (17-25): distinct from reflections 0-7 / denoise 8-12
        // / debug 13-16 so same-frame passes never alias bindless heap slots.
        constexpr UINT B = 17;
        rtAs_.Bindless().WriteSceneDescriptor(frameIndex, B + 0, tlasSrv);                          // TLAS
        rtAs_.Bindless().WriteSceneDescriptor(frameIndex, B + 1, D.lightSRV);                        // on-screen lit (light buffer)
        rtAs_.Bindless().WriteSceneDescriptor(frameIndex, B + 2, D.glassReflNormalSRV);              // glass normal (gb1)
        rtAs_.Bindless().WriteSceneDescriptor(frameIndex, B + 3, D.glassReflDepthSRV);               // glass depth (primary)
        rtAs_.Bindless().WriteSceneDescriptor(frameIndex, B + 4, D.glassReflectionUAV);              // output
        rtAs_.Bindless().WriteSceneDescriptor(frameIndex, B + 5, skybox->GetTex()->GetSRVCPU());     // skybox
        rtAs_.Bindless().WriteSceneDescriptor(frameIndex, B + 8, D.depthSRV);                        // screen (opaque) depth for the match

        LightManager* lm = frame_->lightManager;
        const UINT spotCount = lm ? static_cast<UINT>(lm->GetSpotLightCount()) : 0u;
        const UINT pointCount = lm ? static_cast<UINT>(lm->PointLights().size()) : 0u;
        const D3D12_CPU_DESCRIPTOR_HANDLE spotSrv = lm ? lm->GetSpotLightSrv(frameIndex) : D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
        const D3D12_CPU_DESCRIPTOR_HANDLE pointSrv = lm ? lm->GetPointLightSrv(frameIndex) : D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
        const bool haveSpots = spotCount > 0u && spotSrv.ptr != 0;
        const bool havePoints = pointCount > 0u && pointSrv.ptr != 0;
        if (haveSpots)  { rtAs_.Bindless().WriteSceneDescriptor(frameIndex, B + 6, spotSrv); }
        if (havePoints) { rtAs_.Bindless().WriteSceneDescriptor(frameIndex, B + 7, pointSrv); }

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
        c.tlasIndex = rtAs_.Bindless().SceneIndex(frameIndex, B + 0);
        c.lightIndex = rtAs_.Bindless().SceneIndex(frameIndex, B + 1);
        c.gb1Index = rtAs_.Bindless().SceneIndex(frameIndex, B + 2);
        c.depthIndex = rtAs_.Bindless().SceneIndex(frameIndex, B + 3);        // primary = glass depth
        c.screenDepthIndex = rtAs_.Bindless().SceneIndex(frameIndex, B + 8);  // visibility match = opaque depth
        c.alphaMissKeep = std::clamp(frame_->settings.rtAlphaMissKeep, 0.0f, 1.0f);
        c.alphaTestOff = frame_->settings.rtAlphaTest ? 0u : 1u;
        // FROZEN dither, by measurement (ssr_bronze_palms, resolved frame-to-frame boil in the
        // mirror band): fill 0 = 0.41; fill 0.15 re-rolled per frame = 0.74; re-rolled once per
        // EMA time constant = 1.25 (coherent drift is the WORST case for the resolve); frozen =
        // 0.43. A static pattern realises the coverage inflation spatially, adds no dance at all,
        // and the EMA has nothing to chase. The seed still reaches the shader so a future
        // animated-dither experiment only touches this line.
        c.frameSeed = 0u;
        c.reflectionUavIndex = rtAs_.Bindless().SceneIndex(frameIndex, B + 4);
        c.skyboxIndex = rtAs_.Bindless().SceneIndex(frameIndex, B + 5);
        c.skyboxIntensity = skybox->GetExposure();
        c.geomInfoIndex = rtAs_.Bindless().GeomInfoIndex(frameIndex);
        c.spotLightIndex = rtAs_.Bindless().SceneIndex(frameIndex, B + 6);
        c.spotCount = haveSpots ? spotCount : 0u;
        c.pointLightIndex = rtAs_.Bindless().SceneIndex(frameIndex, B + 7);
        c.pointCount = havePoints ? pointCount : 0u;

        auto cb = renderer->GetFrameResource()->AllocDynamic(sizeof(RtReflectConstants), render::kConstantBufferAlignment);
        std::memcpy(cb.cpu, &c, sizeof(c));

        ID3D12DescriptorHeap* heaps[] = { rtAs_.Bindless().Heap() };
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

// ---- the clear variant, the temporal resolve, the blur ----
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
            { decisions_.reflectionTemporal ? D.reflectionHistorySRV : D.reflectionSRV, D.gbSRV[0] },
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

// ---- Pass_RTDebug ----
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
    uint32_t alphaTestOff = 0; // mirrors the reflection pass
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
        const D3D12_CPU_DESCRIPTOR_HANDLE tlasSrv = rtAs_.Manager().TlasSrvCpu(frameIndex);
        if (!pts.trace)
        {
            break; // no TLAS / bindless table this frame - leave reflection as compose left it
        }

        // Copy this frame's scene descriptors into the persistent bindless heap so
        // the shader can reach them via ResourceDescriptorHeap[]. Geometry VB/IB descriptors are
        // immutable; Pass_BuildAS uploads the geometry-info buffer/SRV for this frame slot only.
        // Scene slots 13-16 (distinct from the reflection 0-7 / denoise 8-12
        // ranges, so the debug pass never aliases their heap slots in a frame).
        rtAs_.Bindless().WriteSceneDescriptor(frameIndex, 13, tlasSrv);    // TLAS SRV
        rtAs_.Bindless().WriteSceneDescriptor(frameIndex, 14, D.gbSRV[1]); // GB1 (normal)
        rtAs_.Bindless().WriteSceneDescriptor(frameIndex, 15, D.depthSRV); // Depth
        rtAs_.Bindless().WriteSceneDescriptor(frameIndex, 16, D.reflectionUAV);   // reflection UAV (output)

        RtDebugConstants c{};
        c.invView = camera.GetInvViewMatrix();
        c.invProj = camera.GetInvProjMatrix();
        c.tlasIndex = rtAs_.Bindless().SceneIndex(frameIndex, 13);
        c.gb1Index = rtAs_.Bindless().SceneIndex(frameIndex, 14);
        c.depthIndex = rtAs_.Bindless().SceneIndex(frameIndex, 15);
        c.reflectionUavIndex = rtAs_.Bindless().SceneIndex(frameIndex, 16);
        c.geomInfoIndex = rtAs_.Bindless().GeomInfoIndex(frameIndex);
        c.alphaTestOff = frame_->settings.rtAlphaTest ? 0u : 1u;
        c.outWidth = renderer->GetReflectionTextureWidth();
        c.outHeight = renderer->GetReflectionTextureHeight();

        auto cb = renderer->GetFrameResource()->AllocDynamic(sizeof(RtDebugConstants), render::kConstantBufferAlignment);
        std::memcpy(cb.cpu, &c, sizeof(c));

        // Bespoke dispatch: bind the bindless heap (not the per-frame heap) and the
        // shader's root sig (RootFlags CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED + CBV(b0) +
        // static samplers). Root param 0 is the b0 CBV.
        ID3D12DescriptorHeap* heaps[] = { rtAs_.Bindless().Heap() };
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

// ---- RecordOceanReflection ----
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
