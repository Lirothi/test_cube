// R2 (docs/scene_renderer_refactor_plan.md): the lit image: AO inputs, the three light passes, the sky and the composite.
//
// Moved out of SceneRenderer.cpp VERBATIM — same class, same methods, one subject per
// file. The include block is the one the original file carries; trimming it per TU is
// deliberately NOT part of this step, because an unused include is not a defect and a
// trimmed one is a second thing to review.

#include <cmath>
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

// ---- Pass_Hzb + Pass_Gtao ----
void SceneRenderer::Pass_Hzb(Renderer* renderer, RenderGraphPassContext ctx, uint32_t point, bool passA)
{
    auto material = resources_.GetHzbMaterial();
    const UINT cbSize = resources_.GetHzbCBSizeBytes();
    const auto& D = renderer->GetDeferredForFrame();

    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        // Occlusion plan S5: the same build twice a frame -- pass A's depth (Main_HzbA, for the
        // deferred retest) and the final depth (Main_Hzb, for everything else and next frame's
        // main cull) -- under their own scopes, so the profdump shows the second build's cost.
        const Profiler::ScopeNameKey gpuScope = passA ? ProfilerScopes::kPassHzbA : ProfilerScopes::kPassHzb;
        GPU_SCOPE(t.cl, gpuScope);
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
            c.writeClosest = decisions_.ssrHiz ? 1u : 0u;

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
            if (decisions_.ssrHiz)
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

// ---- Pass_Lighting + Pass_SpotLights + Pass_PointLights + Pass_Skybox ----
// The lighting pass's per-frame constants, shared with the volumetric fog's scatter pass (its b0
// is the same cbuffer, lighting_cb.hlsli): filled here once per consumer so the sun's shadow is
// sampled from identical values in both. Moved out of Pass_Lighting verbatim.
void SceneRenderer::FillLightingConstants(Renderer* renderer, const Camera& camera, LightingPassConstants& constants,
                                          D3D12_CPU_DESCRIPTOR_HANDLE& causticsSrv, bool& vsmDir) const
{
    const DirectionalLight& dirLight = *frame_->dirLight;
    constants = LightingPassConstants{};
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
    constants.cascadeTexelWS = float4(cascades.cascadeTexelWS[0], cascades.cascadeTexelWS[1], cascades.cascadeTexelWS[2], cascades.cascadeTexelWS[3]);
    const float width = static_cast<float>(std::max(renderer->GetRenderWidth(), 1u));
    const float height = static_cast<float>(std::max(renderer->GetRenderHeight(), 1u));
    constants.screenSize = float2(width, height);
    constants.invScreenSize = float2(width > 0.f ? (1.0f / width) : 0.0f, height > 0.f ? (1.0f / height) : 0.0f);
    constants.sunMetalSpec = frame_->settings.sunMetalSpecInfluence;
    constants.sunAngularSize = frame_->settings.sunAngularSize;

    // Underwater caustics. Everything comes from the ocean: no water in the level means the
    // whole block stays zeroed and the shader skips it (causticsTint.w == 0).
    causticsSrv = {};
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
    vsmDir = render::VsmActive() && frame_->vsm && frame_->vsm->IsAllocated() &&
                        frame_->vsm->PageTableSrv().ptr != 0 && frame_->vsm->PagePoolSrv().ptr != 0;
    constants.useVsm = vsmDir ? 1u : 0u;
    // S0.3: cascade-tint debug. Forced off whenever the clipmap is the shadow source — the
    // tint visualizes CSM cascades, which that path does not sample.
    constants.csmDebugMode = vsmDir ? 0u : static_cast<uint32_t>(render::g_csmDebugMode);
    {
        // UE maps the artist's 0..1 Shadow Filter Sharpen to shader units as x*7+1 (see
        // ShadowRendering.h:1299). Done on the CPU, once, exactly as they do it.
        const CascadeShadowConfig* cfg = frame_->cascadeConfig;
        constants.csmFilterMode = cfg ? cfg->filterMode : 1u; // 0 box, 1 = 4x4 tent, 2 = 6x6 tent
        // S10: splitsVS[4] cannot ride cascadeSplits (that float4 is splits 0..3), so the last
        // cascade's far plane travels here -- without it c3 has no slice length to fade over.
        constants.csmFadeParams = float4(cascades.splitsVS[kCascades],
                                         cfg ? cfg->blendFraction : 0.1f,
                                         cfg ? cfg->distanceFadeFraction : 0.1f, 0.0f);
        constants.csmFilterParams = cfg
            ? float4(cfg->csmReceiverBias, cfg->shadowFilterSharpen * 7.0f + 1.0f,
                     cfg->pcfOverBlurCorrection ? 1.0f : 0.0f, cfg->normalBiasInTexels)
            : float4(0.9f, 1.0f, 1.0f, 1.0f);
    }
    // Both of these are NDC values that mean a WORLD distance, so both must be divided by the
    // level's depth range -- see vsm::ClipmapRangeMultiple(). `vsmDepthBias` is authored against
    // the historical range (6x the extent), so it is rescaled to keep that meaning when
    // ZRangeScale moves; the floor is authored in TEXELS and converted with the same multiple.
    // Leaving a hardcoded 6 here is what made ZRangeScale 10 -> 50 detach shadows from casters.
    const float clipRangeMul = vsm::ClipmapRangeMultiple();
    constants.vsmDepthBias =
        vsm::g_clipmapDepthBias * (vsm::kClipmapRangeMultipleRef / clipRangeMul);
    constants.clipmapDepthBiasDecay = vsm::g_clipmapDepthBiasDecay;
    constants.clipmapDepthBiasFloorNdc =
        vsm::g_clipmapDepthBiasFloorTexels / (clipRangeMul * (float)vsm::kVirtualRes);
    constants.clipmapBlendWidth = vsm::ClipmapBlendWidth();
    constants.clipmapBaseExtent = vsm::g_clipmapBaseExtent;
    // P16.16: UE divide their CVar by 1000 before it reaches the shader
    // (GetNormalBiasForShader, VirtualShadowMapArray.cpp:561). Same here, so the authored
    // number stays directly comparable to `r.Shadow.Virtual.NormalBias`.
    constants.clipmapNormalBias = vsm::g_clipmapNormalBias * 0.001f;
    // SMRT (docs/vsm_smrt_plan.md). rayCount 0 = the single-tap path, bit-for-bit unchanged.
    constants.smrtRayCount = vsm::g_smrtRayCount;
    constants.smrtSamplesPerRay = vsm::g_smrtSamplesPerRay;
    constants.smrtRayLengthScale = vsm::g_smrtRayLengthScale;
    constants.smrtExtrapolateMaxSlope = vsm::g_smrtExtrapolateMaxSlope;
    // Degrees of full ANGLE -> sin of the angular RADIUS, which is what the shader jitters by.
    constants.smrtSourceRadius =
        std::sin(0.5f * vsm::g_smrtSourceAngleDeg * 3.14159265f / 180.0f);
    constants.smrtTexelDitherScale = vsm::g_smrtTexelDitherScale;
    constants.smrtLevelMargin = vsm::g_smrtLevelMargin;
    // 0 is the reserved "no temporal rotation" value, so the phase runs 1..64.
    constants.smrtFrameIndex = frame_->smrtFrameIndex;
    constants.smrtAdaptiveRayCount = vsm::g_smrtAdaptiveRayCount;
    constants.smrtScreenRayLength = vsm::g_smrtScreenRayLength;
    constants.smrtScreenRaySamples = vsm::g_smrtScreenRaySamples;
    // World -> clip for the screen-space ray. The CB carries only the inverses today, and
    // inverting them back in the shader would cost a matrix inverse per pixel.
    constants.viewProj = camera.GetViewMatrix() * camera.GetProjMatrix();
    constants.projMatrix = camera.GetProjMatrix();
    // The master switch folds into the length: 0 means the shader takes no samples at all,
    // so "off" costs literally nothing rather than costing a branch per pixel.
    constants.contactShadowLength = render::contact::g_enabled ? render::contact::g_length : 0.0f;
    constants.contactShadowLengthInWS = render::contact::g_lengthInWorldSpace ? 1u : 0u;
    constants.contactShadowNormalOffset = render::contact::g_normalOffsetFrac;
    constants.contactShadowGrazingFade = render::contact::g_grazingFadeNdotL;
    constants.contactShadowMinDist = render::contact::g_minDistanceM;
    constants.contactShadowMaxDist = render::contact::g_maxDistanceM;
    constants.contactShadowFadeBand = render::contact::g_fadeBandM;
    constants.contactShadowThickness = render::contact::g_maxThicknessFrac;
    // UE's StateFrameIndexMod8, +1 so that 0 stays the "static dither" sentinel.
    constants.contactShadowFrameId = render::contact::g_temporalDither
        ? static_cast<std::uint32_t>((renderer->GetTotalFrameNumber() & 7ull) + 1ull)
        : 0u;
    constants.contactShadowIntensity = render::contact::g_intensity;
    constants.contactShadowSteps = render::contact::g_steps;
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
}

// ---- Volumetric fog (docs/volumetric_fog_sky_clouds_ssgi_plan.md, part A) ----
namespace
{
// FogGridZParams / FogVolumeNear live in SceneRenderInternal.h (plan A5: every consumer shares them).

// UE's Halton (VolumetricFog.cpp:196-207 uses it with bases 2, 3, 5 over FrameNumber & 1023).
float FogHalton(uint32_t index, uint32_t base)
{
    float result = 0.0f;
    float f = 1.0f;
    while (index > 0u)
    {
        f /= static_cast<float>(base);
        result += f * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}
} // namespace

// Two dispatches under three declared points (see the Main_VolumetricFog builder): the per-cell
// lighting (sun through its shadow map, the sky's irradiance, HG phase; blended with the previous
// slot's volume when the history is valid) into `fogScatter`, then the front-to-back integration
// into `fogIntegrated`, which compose samples by view depth.
//
// NO EARLY RETURN AFTER BeginCL: every gate ran in DecideFrame, which is what registered the pass.
void SceneRenderer::Pass_VolumetricFog(Renderer* renderer, RenderGraphPassContext ctx, const Camera& camera,
                                       const FogPoints& pts)
{
    auto scatterMaterial = resources_.GetFogScatterMaterial();
    auto integrateMaterial = resources_.GetFogIntegrateMaterial();
    const UINT lightingCbSize = resources_.GetLightingCBSizeBytes();
    const UINT scatterCbSize = resources_.GetFogScatterCBSizeBytes();
    const UINT integrateCbSize = resources_.GetFogIntegrateCBSizeBytes();
    const auto& D = renderer->GetDeferredForFrame();
    const auto& P = renderer->GetDeferredForPrevFrame();
    const AtmosphereSettings& a = frame_->settings.atmosphere;

    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassVolumetricFog);
        renderer->EmitPoint(t.cl, pts.scatter);

        // b0: the lighting cbuffer, from the SAME fill the lighting pass uses, so the sun's shadow
        // is sampled from identical constants (clipmap matrices, biases, cascade splits).
        LightingPassConstants lighting{};
        D3D12_CPU_DESCRIPTOR_HANDLE causticsSrv{};
        bool vsmDir = false;
        FillLightingConstants(renderer, camera, lighting, causticsSrv, vsmDir);

        // b1: the fog's own constants.
        FogPassConstants fc{};
        fc.gridZParams = float3(decisions_.fogVolumeZParams.x, decisions_.fogVolumeZParams.y, decisions_.fogVolumeZParams.z);
        // UE: 1 / max(VolumetricFogNearFadeInDistance, tiny); the component's default distance is 0,
        // so the fade is over before the first slice ends. Kept as their default (a hard start at
        // the volume's near, which is what the analytic model's start distance is too).
        fc.nearFadeInInv = 1.0f / 0.00001f;
        fc.gridSize[0] = D.fogGridWidth;
        fc.gridSize[1] = D.fogGridHeight;
        fc.gridSize[2] = D.fogGridDepth;
        fc.flags = (decisions_.fogHistoryValid ? 1u : 0u) | (decisions_.fogConservativeDepth ? 4u : 0u) | (a.temporal ? 8u : 0u);
        fc.misc[0] = decisions_.fogHzbMip; // furthest HZB mip whose texel is one cell (base = half res)
        fc.misc[1] = 4u; // UE HistoryMissSupersampleCount
        fc.misc[2] = static_cast<uint32_t>(renderer->GetTotalFrameNumber() & 1023ull);
        fc.misc[3] = static_cast<uint32_t>(std::clamp(a.samplesPerCell, 1, 4)); // every-frame samples per cell
        // A4: the local lights, from the light passes' own buffers (filled in EnsureFrameResources).
        const bool localVsm = render::VsmActive() && frame_->vsm && frame_->vsm->IsAllocated() &&
                              frame_->vsm->PageTableSrv().ptr != 0 && frame_->vsm->PagePoolSrv().ptr != 0;
        const UINT frameIdx = renderer->GetCurrentFrameIndex();
        D3D12_CPU_DESCRIPTOR_HANDLE spotBufferSrv = renderer->VsmDummyBufferSrv();
        D3D12_CPU_DESCRIPTOR_HANDLE pointBufferSrv = renderer->VsmDummyBufferSrv();
        if (decisions_.fogLocalLights && frame_->lightManager)
        {
            LightManager& lm = *frame_->lightManager;
            const size_t spots = lm.GetSpotLightCount();
            const size_t points = lm.PointLights().size();
            const D3D12_CPU_DESCRIPTOR_HANDLE sSrv = spots > 0 ? lm.GetSpotLightSrv(frameIdx) : D3D12_CPU_DESCRIPTOR_HANDLE{};
            const D3D12_CPU_DESCRIPTOR_HANDLE pSrv = points > 0 ? lm.GetPointLightSrv(frameIdx) : D3D12_CPU_DESCRIPTOR_HANDLE{};
            fc.local[0] = (spots > 0 && sSrv.ptr != 0) ? static_cast<uint32_t>(spots) : 0u;
            fc.local[1] = (points > 0 && pSrv.ptr != 0) ? static_cast<uint32_t>(points) : 0u;
            fc.local[2] = localVsm ? 1u : 0u;
            if (fc.local[0]) { spotBufferSrv = sSrv; }
            if (fc.local[1]) { pointBufferSrv = pSrv; }
        }
        fc.localParams = float4(std::max(a.localLightScatter, 0.0f),
                                1.0f / static_cast<float>(std::max(D.spotShadowRes, 1u)),
                                1.0f / static_cast<float>(std::max(D.pointShadowRes, 1u)), vsm::g_refDist);
        fc.localParams2 = float4(std::max(a.localSoftFading, 0.0f), std::max(a.localDistanceBias, 0.0f), 0.0f, 0.0f);
        // Row-vector convention (mul(v, M)): clip.z = z * _33 + _43, clip.w = z * _34 + _44.
        const mat4& projNoJitter = camera.GetProjMatrixNoJitter();
        fc.projZ = float4(projNoJitter.m._33, projNoJitter.m._43, projNoJitter.m._34, projNoJitter.m._44);
        fc.invViewProjNoJitter = camera.GetInvProjMatrixNoJitter() * camera.GetInvViewMatrix();
        fc.prevViewProjNoJitter = camera.GetPrevViewProjMatrixNoJitter();
        // UE VolumetricFogTemporalRandom: the cell centre unless jitter AND reprojection are on --
        // a jittered sample with nothing to accumulate into is just noise.
        if (a.temporal && decisions_.fogHistoryValid)
        {
            const uint32_t f = static_cast<uint32_t>(renderer->GetTotalFrameNumber() & 1023ull);
            // The jitter knob only removes the offset; the history weight stays (UE Jitter cvar).
            fc.jitter = a.jitter
                ? float4(FogHalton(f, 2u), FogHalton(f, 3u), FogHalton(f, 5u), std::clamp(a.historyWeight, 0.0f, 0.99f))
                : float4(0.5f, 0.5f, 0.5f, std::clamp(a.historyWeight, 0.0f, 0.99f));
            if (a.jitter) { fc.flags |= 2u; }
        }
        else
        {
            fc.jitter = float4(0.5f, 0.5f, 0.5f, 0.0f);
        }
        const AtmospherePacked packed = PackAtmosphere(a, frame_->dirLight != nullptr);
        fc.medium0 = packed.params0; // density, height falloff, reference height, start distance
        fc.medium1 = float4(std::clamp(a.albedo, 0.0f, 1.0f), std::max(a.extinctionScale, 0.0f),
                            std::clamp(a.phaseG, -0.99f, 0.99f), std::max(a.sunScatter, 0.0f));
        fc.medium2 = float4(std::max(a.skyScatter, 0.0f), preExposure_,
                            prevPreExposure_ > 0.0f ? 1.0f / prevPreExposure_ : 1.0f, a.volumetricDistance);

        {
            GPU_SCOPE(t.cl, ProfilerScopes::kFogScatter);
            // Manual bind + dispatch: the scatter runs 4x4x4 groups over a 3D grid, a shape
            // RecordComputeDispatch (8x8x1) cannot express.
            auto h = renderer->GetRenderContextPool()->Acquire();
            RenderContext& rc = h.ref();
            {
                auto cb = renderer->GetFrameResource()->AllocDynamic(lightingCbSize, render::kConstantBufferAlignment);
                resources_.WriteLightingConstants(lighting, static_cast<uint8_t*>(cb.cpu));
                rc.cbv[0] = cb.gpu;
            }
            {
                auto cb = renderer->GetFrameResource()->AllocDynamic(scatterCbSize, render::kConstantBufferAlignment);
                resources_.WriteFogScatterConstants(fc, static_cast<uint8_t*>(cb.cpu));
                rc.cbv[1] = cb.gpu;
            }
            // t0 atlas, t1/t2 VSM table + pool (dummies in Legacy: useVsm = 0 never samples them),
            // t3 sky irradiance (dummy without IBL: skyIrradianceEnabled = 0), t4 the history.
            rc.srvTable[0] = renderer->StageSrvUavTable({
                D.shadowSRV,
                vsmDir ? frame_->vsm->PageTableSrv() : renderer->VsmDummyBufferSrv(),
                vsmDir ? frame_->vsm->PagePoolSrv() : renderer->VsmDummyTexSrv(),
                (frame_->skybox && frame_->skybox->HasIbl()) ? frame_->skybox->GetIrradianceTex()->GetSRVCPU()
                                                             : renderer->VsmDummyTexSrv(),
                P.fogScatterSRV,
                // t5/t6: the furthest pyramids (A3 conservative depth); dummies when the flag is off.
                decisions_.fogConservativeDepth ? D.hzbSRV : renderer->VsmDummyTexSrv(),
                decisions_.fogConservativeDepth ? P.hzbSRV : renderer->VsmDummyTexSrv(),
                // t7..t10 (A4): the light buffers and the Legacy atlases; dummies keep the VOLATILE
                // range populated when there are none / in VSM mode (never sampled then).
                spotBufferSrv,
                pointBufferSrv,
                (fc.local[0] && !localVsm && D.spotShadowSRV.ptr != 0) ? D.spotShadowSRV : renderer->VsmDummyTexSrv(),
                (fc.local[1] && !localVsm && D.pointShadowSRV.ptr != 0) ? D.pointShadowSRV : renderer->VsmDummyTexSrv() }).gpu;
            rc.uavTable[0] = renderer->StageSrvUavTable({ D.fogScatterUAV }).gpu;
            const auto samplerDescs = std::array{ *SamplerManager::PointClamp(),
                                                  *SamplerManager::ComparisonLinearClamp(),
                                                  *SamplerManager::LinearWrap(),
                                                  *SamplerManager::LinearClamp() };
            rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);
            scatterMaterial->Bind(t.cl, rc);
            const UINT gx = (D.fogGridWidth + 3u) / 4u;
            const UINT gy = (D.fogGridHeight + 3u) / 4u;
            const UINT gz = (D.fogGridDepth + 3u) / 4u;
            if (gx > 0u && gy > 0u && gz > 0u)
            {
                t.cl->Dispatch(gx, gy, gz);
            }
        }

        renderer->EmitPoint(t.cl, pts.integrate); // scatter UAV -> NPS, integrated -> UAV
        {
            GPU_SCOPE(t.cl, ProfilerScopes::kFogIntegrate);
            const auto samplerDescs = std::array{ *SamplerManager::PointClamp() };
            RecordComputeDispatch(renderer, t.cl, integrateMaterial.get(), integrateCbSize,
                [&](uint8_t* dest) { resources_.WriteFogIntegrateConstants(fc, dest); },
                { D.fogScatterSRV },
                { D.fogIntegratedUAV },
                renderer->GetSamplerManager()->GetTable(renderer, samplerDescs),
                D.fogGridWidth, D.fogGridHeight);
        }
        renderer->EmitPoint(t.cl, pts.restore); // integrated -> NPS for compose
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

        LightingPassConstants constants{};
        D3D12_CPU_DESCRIPTOR_HANDLE causticsSrv{};
        bool vsmDir = false;
        FillLightingConstants(renderer, camera, constants, causticsSrv, vsmDir);

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

        // The spot buffer's CPU fill moved to EnsureFrameResources (async prep): Pass_RTTrace
        // reads it too and must not depend on this pass's record.

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
        render::contact::FillConstants(constants, camera.GetViewMatrix() * camera.GetProjMatrix(),
                                       camera.GetProjMatrix(), renderer->GetTotalFrameNumber());
        constants.contactShadowLocalMode = render::contact::EffectiveLocalMode();

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

        // The point buffer's CPU fill moved to EnsureFrameResources (async prep), next to the
        // spot fill -- Pass_RTTrace reads it too and must not depend on this pass's record.

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
        render::contact::FillConstants(constants, camera.GetViewMatrix() * camera.GetProjMatrix(),
                                       camera.GetProjMatrix(), renderer->GetTotalFrameNumber());
        constants.contactShadowLocalMode = render::contact::EffectiveLocalMode();

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

// ---- Pass_Compose ----
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
            // Volumetric fog (plan part A): the froxel volume inside `volumetricDistance`, the
            // analytic model beyond it. x = 0 is the pre-plan compose exactly (the volume is a
            // placeholder then). The grid's (B, O, S) are the SAME numbers the fog pass wrote
            // with, computed by the same function -- a slice looked up by other parameters would
            // read the wrong depth silently.
            constants.fogVolumeParams = decisions_.fogVolumeParams;
            constants.fogVolumeZParams = decisions_.fogVolumeZParams;
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
              D.gtaoUpsampledSRV, // t12: P6B dynamic AO, gated by `gtaoEnabled`
              // t13: the integrated fog volume, gated by `fogVolumeParams.x`; the dummy keeps
              // the VOLATILE range populated on frames without the pass.
              decisions_.volumetricFog ? D.fogIntegratedSRV : renderer->VsmDummyTexSrv() },
            { D.sceneUAV },
            renderer->GetSamplerManager()->GetTable(renderer, samplerDescs),
            renderer->GetRenderWidth(), renderer->GetRenderHeight(),
            D.scene.Get());

        renderer->EmitPoint(t.cl, handBack);
    } while (false);

    ctx.EndCL(t);
}

// Matches the `Probe` cbuffer in rt_debug_cs.hlsl (row-major; 2x mat4 then indices).
