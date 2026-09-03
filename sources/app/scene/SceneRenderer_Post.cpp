// R2 (docs/scene_renderer_refactor_plan.md): after the scene is lit: metering, the upscale, the tone curve and the debug blits.
//
// Moved out of SceneRenderer.cpp VERBATIM — same class, same methods, one subject per
// file. The include block is the one the original file carries; trimming it per TU is
// deliberately NOT part of this step, because an unused include is not a defect and a
// trimmed one is a second thing to review.

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

// ---- Pass_DebugPreview ----
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

// ---- Pass_ExposureMetering ----
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
                    // One record per DISTINCT quantised state (logging plan L6; this used to
                    // dedupe through the barrier-diagnostics sink). The peak flips between two
                    // stops on an unsettled scene, so "on change" would print every frame.
                    LOG_INFO_ONCE_PER_MESSAGE(logging::LogCategory::Render, "{}", msg);
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
        // The clear never reads t0, but CSClear shares CSBuild's root signature, which DECLARES the
        // SRV table -- and a declared table left unbound is what Material::Bind reports as
        // "UNBOUND SRV table at root index 1" (and what GBV calls an uninitialised root argument).
        // Binding the same scene SRV the build binds costs one staged descriptor and keeps the two
        // dispatches binding identically, as the shader's own header promises.
        RecordComputeDispatch(renderer, t.cl, clearMat.get(),
            { D.sceneSRV }, { metering.HistogramUav() }, samplerTable,
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

// ---- Pass_Dlss + Pass_Tonemap ----
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

void SceneRenderer::Pass_Tonemap(Renderer* renderer, RenderGraphPassContext ctx,
    const TonemapPoints& pts)
{
    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    do
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassTonemap);
        const auto& D = renderer->GetDeferredForFrame();
        // pass-flow S8: the body names no resource and no state. Every gate below is a value the
        // builder decided and declared from — including `ranDlss`, which since the DLSS split is a
        // PREDICTION Main_DLSS was built from rather than a result read out of the evaluate.
        renderer->EmitPoint(t.cl, pts.apply);
        // With DLSS on, the upscale pass hands depth + velocity back as a side effect of consuming
        // them and this point is empty; with it off nothing else would, and the frame would end
        // with the forward targets still bound. The marker is emitted either way — an empty point
        // is a pure advance.
        renderer->EmitPoint(t.cl, pts.source);

        renderer->BindDescriptorHeaps(t.cl);

        auto tonemapMaterial = resources_.GetTonemapMaterial();
        if (!tonemapMaterial)
        {
            break;
        }

        const D3D12_CPU_DESCRIPTOR_HANDLE tonemapSrc = pts.ranDlss ? D.dlssOutputSRV : D.sceneSRV;
        const auto tonemapSamplers = std::array{ *SamplerManager::LinearClamp() };
        // P8: build the bloom pyramid off whatever the tonemap is about to read — the upscaled
        // image when DLSS runs, which the submission order guarantees is finished on the GPU
        // before this list executes.
        // pass-flow S8: `pts.bloom` and `pts.convolution` ARE the settings, read once by the
        // builder — the body no longer asks `bloomActive_` or the method a second time.
        if (pts.bloom)
        {
            // The CPU cost of RECORDING the bloom, which is the wide unnamed block after
            // DLSS::Evaluate on the CPU timeline -- the GPU side already has its own scope.
            // R3: which method runs is the subsystem's own decision, the one its points were
            // declared from — this body no longer chooses.
            CPU_SCOPE(ProfilerScopes::kTonemapBloomRecord);
            bloom_.Record(renderer, t.cl, tonemapSrc, pts.bloom_);
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
            [this, applyExposure, bloomRan = pts.bloom](uint8_t* dest) {
                // Taken from the GATE, not from the setting: if the chain did not run this frame
                // the target holds the previous frame's image (or nothing at all), and a non-zero
                // scatter here would composite it.
                const BloomApplyConstants apply =
                    bloomRan ? bloom_.ApplyConstants() : BloomApplyConstants{};
                resources_.WriteTonemapConstants(applyExposure, frame_->colorPipeline,
                                                 frame_->cameraExposure, apply, dest);
            },
            { tonemapSrc, metering.BaseLumSrv(), D.bloomUpSRV },
            { D.tonemapUAV, metering.ExposureUav() }, samplerTable,
            renderer->GetWidth(), renderer->GetHeight(),
            D.tonemap.Get());
        }

        CPU_SCOPE(ProfilerScopes::kTonemapTailRecord);
        // pass-flow S8: `pts.fxaa` is the SAME conjunction the builder declared from (material, CB
        // size, a non-zero output size and the setting). It used to be evaluated here and again in
        // the Prepare, and the two agreeing was the only thing keeping the resolve source's
        // barriers matched to the resolve source the body actually picked.
        auto fxaaMaterial = resources_.GetFxaaMaterial();
        const UINT fxaaCbSize = resources_.GetFxaaCBSizeBytes();
        if (pts.fxaa)
        {
            GPU_SCOPE(t.cl, ProfilerScopes::kTonemapFxaa);
            renderer->EmitPoint(t.cl, pts.fxaaRead);

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
        }

        ID3D12Resource* const backbuffer = renderer->GetCurrentBackbuffer();
        ID3D12Resource* const resolveSource = pts.fxaa ? D.fxaa.Get() : D.tonemap.Get();
        if (pts.resolve)
        {
            GPU_SCOPE(t.cl, ProfilerScopes::kTonemapResolve);
            renderer->EmitPoint(t.cl, pts.resolveCopy);
            // The backbuffer's state cycle is owned OUTSIDE the graph and is fully determined:
            // RecordBindAndClear takes it PRESENT -> RENDER_TARGET at the top of the frame and the
            // present epilogue takes it back, both with hand-rolled barriers. So the resolve knows
            // its own before-states and needs no state tracking -- this pair was the LAST client of
            // ResourceStateTracker, and converting it is what let the tracker be deleted.
            Renderer::TransitionExplicit(t.cl, backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                         D3D12_RESOURCE_STATE_COPY_DEST);
            t.cl->CopyResource(backbuffer, resolveSource);
            // The resolve source back to UAV — and `tonemap` with it when FXAA ran, which is the
            // one that used to be asked for by a trailing named Transition with no point behind
            // it: on the FXAA path the pass ended with `tonemap` left shader-readable instead of
            // at its canonical UAV, because the restore point only ever named the resolve source.
            renderer->EmitPoint(t.cl, pts.resolveBack);
            Renderer::TransitionExplicit(t.cl, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                         D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    } while (false);

    ctx.EndCL(t);
}

// Which resource + SRV the fullscreen debug blit shows, from SceneRenderSettings::debugTexTarget.
// One function so the pass body and its declaration list cannot disagree about the answer.

// ---- PickDebugTexTarget + Pass_Debug ----
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
    default: return { D.shadow, D.shadowSRV }; // S3.5: non-owning alias, already a raw pointer
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

// ---- Pass_Overlay ----
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
