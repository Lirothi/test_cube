// R3 (docs/scene_renderer_refactor_plan.md): the bloom subsystem, extracted from SceneRenderer.
//
// The bodies moved VERBATIM. What changed is ownership: the nineteen members that only this code
// ever read live here now, the per-frame decision (`Decide`) and the four barrier points
// (`Declare`) sit next to the recording that answers them, and the tonemap asks for all of it
// through three entry points instead of reaching into flags on the renderer.
//
// The point LAYOUT is deliberately untouched (write / flare RT / flare read / read, the same for
// both methods): P8C-2l and P8C-2m were both a point moving or vanishing under a gate, and this
// step is not the place to re-earn those lessons.

#include "rendering/post/BloomRenderer.h"
#include "rendering/core/RenderStats.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <DirectXPackedVector.h> // P8C-2o: the kernel is FP16 on disk
#include <memory>
#include <utility>
#include <vector>

#include "core/logging/Log.h" // P8C-2o kernel survey verdict, lens-flare readiness
#include "core/Helpers.h" // GetTimeSeconds: the sun probe's temporal blend
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "rendering/core/ComputeDispatch.h"
#include "rendering/core/PhotographicSettings.h" // P16.1 pre-exposure
#include "rendering/core/RenderConstants.h"
#include "app/camera/Camera.h"                  // stage 9: the sun glare's view
#include "rendering/lighting/DirectionalLight.h" // stage 9: the sun's direction and disc
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h" // the ghost sprite sheet is uploaded once, lazily

// UE'S GAUSSIAN BLOOM, as weights on the pyramid's levels. UE sum six blurs of the scene
// (PostProcessBloomSetup.cpp, AddGaussianBloomPasses), each Bloom{n}Size percent of the view width
// times BloomSizeScale (4) and weighted Bloom{n}Tint * Intensity / 6 -- their defaults, Scene.cpp:459-471.
// The blur is exp(-16.7 (x / R)^2) with R = width * size% / 2 (PostProcessWeightedSampleSum.cpp), so
// its sigma is R / sqrt(2 * 16.7). Each of the six lands on the pyramid level whose own blur is
// nearest, split linearly between the two around it in octaves; a level's blur is taken as
// kLevelSigma of its texels (the 13-tap downsample chain's, estimated), and a size past the coarsest
// level is held there -- at 1440p UE's two widest (30 and 64 %) both sit on the last level, narrower
// than theirs. The WEIGHTS are theirs exactly: the pyramid's total is UE's, sum(Tint) * I / 6.
static std::array<float, kBloomMaxMips> GaussianLevelWeights(UINT levels, float bloomTexelPx,
                                                             float viewWidthPx, float intensity)
{
    struct Stage { float sizePercent; float tint; };
    static constexpr Stage kStages[6] = {
        { 0.3f, 0.3465f }, { 1.0f, 0.138f }, { 2.0f, 0.1176f },
        { 10.0f, 0.066f }, { 30.0f, 0.066f }, { 64.0f, 0.061f } };
    constexpr float kSizeScale = 4.0f;      // BloomSizeScale
    constexpr float kLevelSigma = 1.2f;     // a level's blur, in its own texels
    std::array<float, kBloomMaxMips> w{};
    const int top = static_cast<int>(std::clamp<UINT>(levels, 1u, kBloomMaxMips)) - 1;
    const float sigmaPerRadius = 1.0f / std::sqrt(2.0f * 16.7f);
    for (const Stage& s : kStages)
    {
        const float sigmaPx = viewWidthPx * s.sizePercent * kSizeScale * 0.01f * 0.5f * sigmaPerRadius;
        const float t = std::clamp(std::log2(std::max(sigmaPx / (kLevelSigma * bloomTexelPx), 1.0f)),
                                   0.0f, static_cast<float>(top));
        const int lo = static_cast<int>(std::floor(t));
        const int hi = std::min(lo + 1, top);
        const float f = t - static_cast<float>(lo);
        const float weight = s.tint * intensity / 6.0f;
        w[static_cast<size_t>(lo)] += weight * (1.0f - f);
        w[static_cast<size_t>(hi)] += weight * f;
    }
    return w;
}

// ---- the three entry points, moved out of SceneRenderer with the state they read ----

void BloomRenderer::Decide(Renderer* renderer, const SceneFrameData& frame)
{
    // Remembered for the rest of the frame: Declare and Record must read the SAME settings this
    // decision was taken from, which is the whole reason the flags exist rather than being
    // recomputed at each site.
    frame_ = &frame;
    if (!renderer || !resources_) { bloomActive_ = false; return; }

    const auto& DB = renderer->GetDeferredForFrame();
    // The material EXISTING is not the same as it being usable: Material::CreateCompute keeps
    // the object and leaves the PSO null when the shader fails to build, so a null-check alone
    // dispatches with no pipeline state. That cost a debug session -- the shader was missing its
    // [RootSignature] attribute, which dxc compiles happily and check_shaders therefore passed.
    const auto bloomMaterial = resources_->GetBloomMaterial();
    // P8C: which method runs is decided HERE, once, for the same reason the gate itself is --
    // the builder declares a different set of resources for each, and a body that disagreed
    // with it would emit a barrier the compile never registered.
    const auto fftMaterial = resources_->GetBloomFftMaterial();
    const auto convMaterial = resources_->GetBloomConvMaterial();
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
        // The cached kernel spectrum was built from the OLD image; forget its key or the frame
        // would convolve against it until the size happened to change.
        bloomKernelKey_ = {};
        if (!want.empty())
        {
            renderer->WaitForPreviousFrame();
            UploadBatch up;
            if (up.Begin(renderer))
            {
                Texture2D::CreateDesc desc;
                desc.path = std::wstring(want.begin(), want.end());
                desc.usage = Texture2D::Usage::LinearData;
                bloomKernelTex_.SetMemTag(MemTag::Bloom);
                bloomKernelReady_ = bloomKernelTex_.CreateFromFile(
                    renderer, up.CommandList(), desc, up.KeepAlive());
                // P8C-2o: the same file again, on the CPU, for the centre/scatter survey. A
                // failure here is not a failure to bloom -- the split falls back to neutral.
                if (bloomKernelReady_) { ReadKernelPixels(desc.path.c_str()); }
                up.SubmitAndWait(renderer);
            }
        }
    }
    // The sun corona's image, lazily and only while the corona is on and names one; a new path
    // reloads it. WIC loads get a CPU-built mip chain, which the corona's thin rays need: the
    // image is drawn smaller than its own size and would shimmer from mip 0.
    {
        const BloomSettings& b = frame.settings.bloom;
        const std::string want = b.sunRaysIntensity > 0.0f ? b.sunRaysTexture : std::string();
        if (want != coronaLoadedPath_)
        {
            coronaLoadedPath_ = want;
            coronaReady_ = false;
            if (!want.empty())
            {
                renderer->WaitForPreviousFrame();
                UploadBatch up;
                if (up.Begin(renderer))
                {
                    Texture2D::CreateDesc desc;
                    desc.path = std::wstring(want.begin(), want.end());
                    desc.usage = Texture2D::Usage::AlbedoSRGB;
                    coronaTex_.SetMemTag(MemTag::Bloom);
                    coronaReady_ = coronaTex_.CreateFromFile(renderer, up.CommandList(), desc, up.KeepAlive());
                    up.SubmitAndWait(renderer);
                }
                if (!coronaReady_)
                {
                    LOG_WARNING(logging::LogCategory::Render, "sun corona image not loaded ({}): procedural corona", want);
                }
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
        // not a multiply by zero: nothing is dispatched, nothing is drawn, and the builder
        // declares none of the targets, so not even a barrier is emitted.
        const bool ghostsWanted = frame.settings.bloom.convGhosts > 0u &&
                                  frame.settings.bloom.convGhostIntensity > 0.0f;
        // P8C-5: THE GATE MUST ASK WHETHER THE SCATTER CAN ACTUALLY DRAW. It checked only that
        // the target existed, so a lens-flare material with a NULL PIPELINE reported ghosts as
        // ON, the pass issued its clear and its DrawInstanced, every downstream gate agreed --
        // and the flare target came out exactly zero, because `Bind` on a material without a
        // PSO binds nothing and the draw is dropped. "Compiles is not loads": dxc accepting the
        // shader is not the engine building the pipeline. Silent for a whole session.
        auto flareMat = resources_->GetLensFlareMaterial();
        const bool flareReady = flareMat != nullptr &&
                                flareMat->GetPipelineState() != nullptr &&
                                resources_->GetLensFlareCBSizeBytes() > 0u;
        if (ghostsWanted && !flareReady)
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                LOG_WARNING(logging::LogCategory::Render,
                            "lens flare ghosts DISABLED: material={} pso={} cb={}",
                            static_cast<const void*>(flareMat.get()),
                            flareMat ? static_cast<const void*>(flareMat->GetPipelineState()) : nullptr,
                            resources_->GetLensFlareCBSizeBytes());
            }
        }
        flaresGhosts_ = frame.settings.bloom.enabled && ghostsWanted && flareReady &&
                        frame.settings.bloom.intensity > 0.0f &&
                        DB.lensFlare.Get() != nullptr;
        flaresStreak_ = frame.settings.bloom.enabled &&
                        frame.settings.bloom.convAnamorphicIntensity > 0.0f &&
                        frame.settings.bloom.intensity > 0.0f &&
                        DB.streakA.Get() != nullptr;
        flares_ = flaresGhosts_ || flaresStreak_;
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
                        resources_->GetBloomFftCBSizeBytes() > 0u &&
                        resources_->GetBloomConvCBSizeBytes() > 0u &&
                        DB.bloomFftA != nullptr && DB.bloomFftB != nullptr &&
                        DB.bloomFftKernel != nullptr &&
                        DB.lensFlare.Get() != nullptr;
    bloomActive_ = frame.settings.bloom.enabled &&
                   frame.settings.bloom.intensity > 0.0f &&
                   (bloomConvolution_ ||
                    (bloomMaterial != nullptr &&
                     bloomMaterial->GetPipelineState() != nullptr)) &&
                   resources_->GetBloomCBSizeBytes() > 0u &&
                   DB.bloomMips > 0u && DB.bloomDown.Get() != nullptr &&
                   DB.bloomUp.Get() != nullptr;

    // The temporal input (stage 12): only while the bloom reads the JITTERED scene before an upscaler
    // that runs -- without DLSS the source is the tonemap's own, steady image. Its history holds after
    // a frame that wrote it at this size and camera history (a cut resets it), the light shafts' rule.
    const auto& PB = renderer->GetDeferredForPrevFrame();
    temporal_ = bloomActive_ && frame.settings.bloom.convBeforeUpscale &&
                frame.settings.bloom.convTemporalWeight < 0.999f && renderer->WillEvaluateDlss() &&
                convMaterial != nullptr && convMaterial->GetPipelineState() != nullptr &&
                resources_->GetBloomConvCBSizeBytes() > 0u &&
                DB.bloomHistory.Get() != nullptr && PB.bloomHistory.Get() != nullptr;
    const std::uint64_t rev = frame.camera ? frame.camera->GetHistoryRevision() : 0ull;
    const bool sameHistory = historyWidth_ == DB.bloomHistoryWidth && historyHeight_ == DB.bloomHistoryHeight &&
                             historyRevision_ == rev;
    historyValid_ = temporal_ && historyFrames_ > 0u && sameHistory;
    historyFrames_ = temporal_ ? (sameHistory ? historyFrames_ + 1u : 1u) : 0u;
    historyWidth_ = DB.bloomHistoryWidth;
    historyHeight_ = DB.bloomHistoryHeight;
    historyRevision_ = rev;
}

void BloomRenderer::Declare(RenderGraphPassContext& ctx, Points& out) const
{
    constexpr D3D12_RESOURCE_STATES kNps = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto& D = ctx.renderer->GetDeferredForFrame();

    // The temporal input (stage 12): last frame's history (the previous slot's, at rest) and the
    // G-buffer velocity read, this slot's history written -- then read at the write point by the
    // setup, the ghosts and the streak.
    ctx.NextPoint();
    out.history = ctx.usePoint ? *ctx.usePoint : 0u;
    if (temporal_)
    {
        const auto& P = ctx.renderer->GetDeferredForPrevFrame();
        ctx.Use(P.bloomHistory.Get(), kNps);
        ctx.Use(D.gbVelocity.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ctx.Use(D.bloomHistory.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    ctx.NextPoint();
    out.write = ctx.usePoint ? *ctx.usePoint : 0u;
    if (temporal_) { ctx.Use(D.bloomHistory.Get(), kNps); }
    // The scene's depth, for the art split's sky test: the setup (stage 0, t1) and the ghost scatter
    // (t2) bind it on every frame -- the tables are positional -- so it is declared readable whenever
    // the bloom runs. Already at rest there: DLSS hands it back readable, and without DLSS the tonemap
    // pass declared it the same way at its source point.
    ctx.Use(D.depth.Get(), kNps);
    // ONE point layout for both methods: write / flare RT / flare read / read. The pyramid path
    // used to fold the flare's render-target barrier into its write point, which is why the shared
    // FlaresBuild had to know which method called it. Both P8C-2l and P8C-2m were a point moving or
    // vanishing under a gate, so the layout stays fixed and only its CONTENT is gated.
    if (bloomConvolution_)
    {
        // P8C: three grids instead of the two pyramid chains. Declared in the SAME order the body
        // transitions them, because a compiled barrier is matched against the current point in
        // body order.
        // P8C-2m: bloomDown is NOT here. The convolution never writes it (stage 0 packs straight
        // from the HDR image) and the flares read the HDR image too, so the UAV->SRV round trip it
        // used to make was pure cost -- and the only body request that answered its point lived
        // inside FlaresBuild, which is how switching the flares off stalled the pass's whole
        // barrier program on an unanswered point and left the FFT grids in UNORDERED_ACCESS under
        // the tonemap's read.
        ctx.Use(D.bloomUp.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(D.bloomFftA, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(D.bloomFftB, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(D.bloomFftKernel, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    else
    {
        ctx.Use(D.bloomDown.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(D.bloomUp.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    // P8C-2l/m: the streak pyramid's own targets -- declared only when the flares actually run
    // this frame, so "off" costs not even a barrier.
    if (flares_)
    {
        ctx.Use(D.streakA.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(D.streakB.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    ctx.NextPoint();
    out.flareRt = ctx.usePoint ? *ctx.usePoint : 0u;
    // P8C-2/m: the flare accumulation target, declared only when the flares run.
    // THE POINT ITSELF IS NOT GATED. A point is a position in the pass's barrier program, and the
    // body's transitions are matched against it positionally -- dropping one shifts every later
    // point, which is how "flares off" managed to leave the bloom chains in UNORDERED_ACCESS under
    // the tonemap's SRV read.
    if (flares_) { ctx.Use(D.lensFlare.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET); }
    ctx.NextPoint();
    out.flareRead = ctx.usePoint ? *ctx.usePoint : 0u;
    if (flares_) { ctx.Use(D.lensFlare.Get(), kNps); }
    ctx.NextPoint();
    out.read = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(D.bloomUp.Get(), kNps); // the tonemap samples mip 0
    if (bloomConvolution_)
    {
        ctx.Use(D.bloomFftA, kNps);
        ctx.Use(D.bloomFftB, kNps);
        ctx.Use(D.bloomFftKernel, kNps);
    }
    else
    {
        ctx.Use(D.bloomDown.Get(), kNps); // back to canonical
    }
    if (flares_)
    {
        ctx.Use(D.streakA.Get(), kNps);
        ctx.Use(D.streakB.Get(), kNps);
    }
}

void BloomRenderer::Record(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                           D3D12_CPU_DESCRIPTOR_HANDLE hdrSource, const Points& pts)
{
    // The temporal input first: it replaces what the setup, the ghosts and the streak read.
    renderer->EmitPoint(cl, pts.history);
    if (temporal_) { RecordTemporalInput(renderer, cl, convInput_.ptr != 0 ? convInput_ : hdrSource); }
    // The METHOD is the decision Declare() built its points from — asking the settings again here
    // is what let a frame with an unready convolution record one method against the other's
    // declarations.
    if (bloomConvolution_) { Convolve(renderer, cl, hdrSource, pts); }
    else { Build(renderer, cl, hdrSource, pts); }
}

// Stage 12 into this slot's history from last slot's, reprojected by the G-buffer velocity (see the
// stage). Positional tables: t0 the scene, t1 the velocity, t2 last frame's history, t3/t4 the local
// exposure pair as every conv dispatch binds them; u0 = u1 = this frame's history, u2 the exposure
// record. Afterwards the convolution's input IS the history.
void BloomRenderer::RecordTemporalInput(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                        D3D12_CPU_DESCRIPTOR_HANDLE scene)
{
    const auto& D = renderer->GetDeferredForFrame();
    const auto& P = renderer->GetDeferredForPrevFrame();
    auto convMaterial = resources_->GetBloomConvMaterial();
    const UINT convCb = resources_->GetBloomConvCBSizeBytes();
    BloomConvConstants c{};
    c.convStage = 12u;
    c.sourceSize = uint2{ D.bloomHistoryWidth, D.bloomHistoryHeight };
    const float pe = std::max(render::g_preExposure, 1.0e-30f);
    c.historyParams = Math::float4(std::clamp(frame_->settings.bloom.convTemporalWeight, 0.01f, 1.0f),
                                   historyValid_ ? pe / std::max(historyPreExposure_, 1.0e-30f) : 1.0f,
                                   historyValid_ ? 1.0f : 0.0f, 0.0f);
    historyPreExposure_ = pe;
    const auto samplerDescs = std::array{ *SamplerManager::LinearClamp() };
    RecordComputeDispatch(renderer, cl, convMaterial.get(), convCb,
        [&](uint8_t* dest) { resources_->WriteBloomConvConstants(c, dest); },
        { scene, D.gbSRV[3], P.bloomHistorySRV,
          renderer->Exposure().BilateralGridSrv(), renderer->Exposure().BaseLumSrv() },
        { D.bloomHistoryUAV, D.bloomHistoryUAV, renderer->Exposure().ExposureUav() },
        renderer->GetSamplerManager()->GetTable(renderer, samplerDescs),
        D.bloomHistoryWidth, D.bloomHistoryHeight, D.bloomHistory.Get());
    convInput_ = D.bloomHistorySRV;
}

void BloomRenderer::Downsample(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                     D3D12_CPU_DESCRIPTOR_HANDLE hdrSource, float threshold,
                                     UINT mipCount)
{
    const auto& D = renderer->GetDeferredForFrame();
    auto material = resources_->GetBloomMaterial();
    const UINT cbSize = resources_->GetBloomCBSizeBytes();
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
            [&](uint8_t* dest) { resources_->WriteBloomConstants(c, dest); },
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

void BloomRenderer::Build(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                D3D12_CPU_DESCRIPTOR_HANDLE hdrSource, const Points& pts)
{
    const auto& D = renderer->GetDeferredForFrame();
    auto material = resources_->GetBloomMaterial();
    const UINT cbSize = resources_->GetBloomCBSizeBytes();
    const BloomSettings& settings = frame_->settings.bloom;

    GPU_SCOPE(cl, ProfilerScopes::kPassBloom);

    // pass-flow S8: the whole chain (and the streak pair when the flares run) in one marker —
    // the tonemap builder declared them together, in this order.
    renderer->EmitPoint(cl, pts.write);

    // P8C-2l: the streak and the ghosts are not the convolution's property. They read the HDR
    // image and add into mip 0, which this method writes too, so the pyramid gets them on the
    // same terms -- build before the chain, composite after it.
    const bool flares = flares_;
    const BloomConvConstants flareConv = flares ? FlareConstants(renderer)
                                                : BloomConvConstants{};
    if (flares)
    {
        FlaresBuild(renderer, cl, hdrSource, flareConv, pts);
    }

    // UE's BloomThreshold, in the units the viewer sees (bloom_cs.hlsl Setup); negative = none.
    Downsample(renderer, cl, hdrSource, settings.threshold, 0u);
    const std::array<float, kBloomMaxMips> levelWeight = GaussianLevelWeights(
        D.bloomMips,
        static_cast<float>(std::max(renderer->GetWidth(), 1u)) / static_cast<float>(std::max(D.bloomWidth, 1u)),
        static_cast<float>(std::max(renderer->GetWidth(), 1u)),
        std::max(0.0f, settings.intensity));

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
            [&](uint8_t* dest) { resources_->WriteBloomConstants(c, dest); },
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
        c.threshold = settings.threshold;
        c.softKnee = settings.softKnee;
        c.radius = std::max(settings.radius, 0.0f);
        c.fireflyClamp = 0u;
        // The source is the coarser level of the UP chain -- except at the top, where the UP chain
        // holds nothing yet and the DOWN chain's coarsest level is the seed.
        const bool seeding = (m + 1u == D.bloomMips - 1u);
        // Each level enters the sum at its share of UE's weights; the seed carries the coarsest's.
        c.levelWeight = levelWeight[m];
        c.srcWeight = seeding ? levelWeight[m + 1u] : 1.0f;
        const D3D12_CPU_DESCRIPTOR_HANDLE src =
            seeding ? D.bloomDownMipUAV[m + 1u] : D.bloomUpMipUAV[m + 1u];
        dispatch(c, dst.x, dst.y, src, D.bloomUpMipUAV[m], D.bloomDownMipUAV[m], D.bloomUp.Get());
    }

    if (flares)
    {
        FlaresComposite(renderer, cl, hdrSource, flareConv);
    }
    SunGlareComposite(renderer, cl, hdrSource);
    // ...and the whole chain back to shader-readable for the tone curve, in one marker.
    renderer->EmitPoint(cl, pts.read);
}




// P8C-2h/i: how deep the anamorphic pyramid may go. Each level of width-halving doubles the
// reach -- 25 display pixels at level 0, so eight levels reach ~3200, past the width of the
// screen. Six levels capped the band at ~870 px measured, which is what made the Length slider's
// upper half inert; the packing budget still fits (320+160+80+40+20+10 = 630 of 640).
static constexpr int kStreakMaxLevels = 8;
// A level narrower than this carries no usable detail.
static constexpr uint32_t kStreakMinLevelWidth = 10u;

// The flare fields both halves need, filled in ONE place so the two bloom methods cannot drift.
// Every composite lands in the bloom target AFTER the method's own output is final (the
// convolution's resolve applies its scale, the pyramid's levels carry theirs) and the tonemap adds
// that target as it is, so these intensities are in the scene's own units -- as UE's lens flares,
// which are composited into the bloom texture after the FFT's ApplyTint.
BloomConvConstants BloomRenderer::FlareConstants(Renderer* renderer) const
{
    const auto& D = renderer->GetDeferredForFrame();
    const BloomSettings& settings = frame_->settings.bloom;
    const float displayW = static_cast<float>(std::max(renderer->GetWidth(), 1u));
    BloomConvConstants c{};
    c.exposureEnabled = 0u;
    // "Fraction of the source's own brightness": the streak's up-chain is a convex combination, so
    // the band arrives at roughly the source's amplitude.
    c.anamorphicIntensity = std::max(0.0f, settings.convAnamorphicIntensity);
    c.anamorphicLength = std::max(0.0005f, settings.convAnamorphicLength);
    c.anamorphicSigma = std::max(0.5f, settings.convAnamorphicWidth) *
                        (static_cast<float>(D.bloomWidth) / displayW);
    // Pre-exposed units, as the ghosts' threshold and UE's LensFlareThreshold.
    c.anamorphicThreshold = std::max(0.05f, settings.convAnamorphicThreshold);
    c.anamorphicChroma = std::clamp(settings.convAnamorphicChroma, 0.0f, 1.0f);
    c.anamorphicTint[0] = std::max(0.0f, settings.convAnamorphicTint[0]);
    c.anamorphicTint[1] = std::max(0.0f, settings.convAnamorphicTint[1]);
    c.anamorphicTint[2] = std::max(0.0f, settings.convAnamorphicTint[2]);
    // UE's `LensFlareIntensity * BloomIntensity` (PostProcessLensFlares.cpp:441); the composite then
    // divides by the guard band's area and tints per ghost, as their LensFlareHDRColor does.
    c.ghostIntensity = std::max(0.0f, settings.convGhostIntensity) * std::max(0.0f, settings.intensity);
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
bool BloomRenderer::ReadKernelPixels(const wchar_t* path)
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

// P8C-2o -- THE SURVEY, transcribed from UE's (PostProcessFFTBloom.cpp:393-540 and the shaders it
// runs: BloomFindKernelCenter, BloomSurveyMaxScatterDispersion, BloomSurveyKernelCenterEnergy,
// BloomSumScatterDispersionEnergy). The definitions are theirs, not mine:
//
//   * the CENTRE is the kernel's brightest texel (by luminance);
//   * D = ViewTexelDiameterInKernelTexels = max(kernel width / its width in grid texels, 1) -- the
//     kernel texels one output pixel covers -- and R = D / 2;
//   * MaxScatterDispersion -- the level the kernel is CLAMPED to: the max over a square annulus
//     around the centre, Chebyshev distance between 2R + 1 and 2R + 4 (soft-edged, as their
//     saturate()s are), of each texel's LARGEST channel. One grey value, so the clamp cannot tint;
//   * CenterEnergy -- the sum, over the centre square of Chebyshev radius R + 0.5 (soft-edged), of
//     `max(kernel - MaxScatterDispersion, 0)`: the EXCESS above the clamp, which is the delta spike,
//     the light that never scattered;
//   * ScatterDispersionEnergy -- the whole clamped kernel, `clamp(kernel, 0, MaxScatterDispersion)`.
//
// On the CPU from the texture's own pixels rather than on the GPU as theirs is: a readback the
// tonemap cannot wait for, for numbers that move only when the kernel or its size does.
void BloomRenderer::SurveyKernel(float spanTexels)
{
    if (bloomKernelPixelDim_ == 0u) { return; }
    const int dim = static_cast<int>(bloomKernelPixelDim_);
    const float diameter = std::max(static_cast<float>(dim) / std::max(spanTexels, 1.0f), 1.0f);
    // THE EARLY-OUT IS THE WHOLE PERFORMANCE STORY. `diameter` moves only when convSize, the bloom
    // resolution or the kernel image changes, so the loops below run a handful of times per session
    // and never inside a steady frame. Timed into the log rather than asserted.
    if (bloomSurveyRatio_ == diameter) { return; }
    const auto surveyStart = std::chrono::steady_clock::now();
    const float radius = 0.5f * diameter;

    const auto at = [this, dim](int x, int y, int c) {
        // Their survey reads `(centre + offset) % size`: the kernel wraps.
        const int cx = ((x % dim) + dim) % dim;
        const int cy = ((y % dim) + dim) % dim;
        return bloomKernelPixels_[(static_cast<size_t>(cy) * dim + cx) * 3u + static_cast<size_t>(c)];
    };

    // FindKernelCenter: the brightest texel. The first one found on a tie, scanning rows.
    int cx = dim / 2, cy = dim / 2;
    {
        float best = -1.0f;
        for (int y = 0; y < dim; ++y)
        {
            for (int x = 0; x < dim; ++x)
            {
                const float l = 0.2126f * at(x, y, 0) + 0.7152f * at(x, y, 1) + 0.0722f * at(x, y, 2);
                if (l > best) { best = l; cx = x; cy = y; }
            }
        }
    }

    // MaxScatterDispersion over the annulus. Their survey runs a grid of 8x8 groups sized for R + 4
    // (`SurveyGroupGridSize = 2 * DivideAndRoundUp(ceil(R) + 4, 8)`), offsets [-4G, 4G - 1], while the
    // annulus reaches 2R + 4 -- so past R ~ 4 they crop its outer edge. Kept, for the same number.
    float clampLevel = 0.0f;
    {
        const int groups = 2 * ((static_cast<int>(std::ceil(radius)) + 4 + 7) / 8);
        const int lo = -4 * groups, hi = 4 * groups - 1;
        const int reach = static_cast<int>(std::ceil(2.0f * radius + 4.0f));
        for (int oy = std::max(-reach, lo); oy <= std::min(reach, hi); ++oy)
        {
            for (int ox = std::max(-reach, lo); ox <= std::min(reach, hi); ++ox)
            {
                const float cheb = static_cast<float>(std::max(std::abs(ox), std::abs(oy)));
                const float isCentre = std::clamp(2.0f * radius + 1.0f - cheb, 0.0f, 1.0f);
                const float isEdgeOrCentre = std::clamp(2.0f * radius + 4.0f - cheb, 0.0f, 1.0f);
                const float zone = std::clamp(1.0f - isCentre, 0.0f, 1.0f) * isEdgeOrCentre;
                if (zone <= 0.0f) { continue; }
                const float m = std::max(at(cx + ox, cy + oy, 0), std::max(at(cx + ox, cy + oy, 1), at(cx + ox, cy + oy, 2)));
                clampLevel = std::max(clampLevel, m * zone);
            }
        }
    }

    std::array<float, 3> centerEnergy{ 0.0f, 0.0f, 0.0f };
    {
        const int reach = static_cast<int>(std::ceil(radius + 0.5f));
        for (int oy = -reach; oy <= reach; ++oy)
        {
            for (int ox = -reach; ox <= reach; ++ox)
            {
                const float cheb = static_cast<float>(std::max(std::abs(ox), std::abs(oy)));
                const float zone = std::clamp(radius + 0.5f - cheb, 0.0f, 1.0f);
                if (zone <= 0.0f) { continue; }
                for (int c = 0; c < 3; ++c)
                {
                    centerEnergy[c] += std::max(at(cx + ox, cy + oy, c) - clampLevel, 0.0f) * zone;
                }
            }
        }
    }
    std::array<float, 3> scatterEnergy{ 0.0f, 0.0f, 0.0f };
    for (int y = 0; y < dim; ++y)
    {
        for (int x = 0; x < dim; ++x)
        {
            for (int c = 0; c < 3; ++c) { scatterEnergy[c] += std::clamp(at(x, y, c), 0.0f, clampLevel); }
        }
    }

    bloomKernelCenterEnergy_ = centerEnergy;
    bloomKernelScatterEnergy_ = scatterEnergy;
    bloomKernelClampLevel_ = clampLevel;
    bloomKernelCenterX_ = static_cast<uint32_t>(cx);
    bloomKernelCenterY_ = static_cast<uint32_t>(cy);
    bloomSurveyRatio_ = diameter;
    const float total = centerEnergy[1] + scatterEnergy[1];
    LOG_INFO(logging::LogCategory::Render,
             "bloom kernel survey: texel diameter {:.3f}, centre ({}, {}), clamp {:.1f}, centre energy {:.1f} ({:.2f}% of total), scatter {:.1f}, took {:.2f} ms",
             diameter, cx, cy, clampLevel, centerEnergy[1],
             (total > 0.0f) ? (centerEnergy[1] / total * 100.0f) : 0.0f, scatterEnergy[1],
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - surveyStart).count());
}

// P8C-2o -- BloomFinalizeApplyConstants.usf, line for line:
//
//     Tint          = saturate(Total / max3(Total))          Total = Centre + Scatter
//     FinalScatter  = Scatter * ScatterDispersion * BloomIntensity * ConvolutionIntensity
//     SceneApply    = Tint * saturate((Total - FinalScatter) / Total)
//     FFTMultiply   = Tint * saturate(FinalScatter / Total)
//
// ScatterDispersion and ConvolutionIntensity are UE's defaults, 1, and are not separate knobs here:
// `intensity` is BloomIntensity. The two halves PARTITION the light: the convolution's input is the
// whole scene (no threshold -- UE's FFT bloom has none), so what SceneApply takes off the scene comes
// back through FFTMultiply, spread by the kernel. Returned per channel: [0] SceneApply, [1] FFTMultiply.
// The kernel tint is ours (UE have none) and enters HERE, as a per-channel ScatterDispersion: a
// bluish tint scatters more of the blue, and what a channel does not scatter stays in the scene, so
// the partition still sums to Tint in every channel. (Inside the kernel image the per-channel
// normalisation would cancel it; on the scattered light alone it would darken and colour the scene.)
static std::array<std::array<float, 3>, 2> FinalizeApply(const std::array<float, 3>& centre,
                                                         const std::array<float, 3>& scatter,
                                                         float intensity, const float kernelTint[3])
{
    std::array<std::array<float, 3>, 2> out{};
    std::array<float, 3> total{};
    for (int c = 0; c < 3; ++c) { total[c] = centre[c] + scatter[c]; }
    const float maxTotal = std::max(total[0], std::max(total[1], total[2]));
    for (int c = 0; c < 3; ++c)
    {
        const float s = intensity * std::max(0.0f, kernelTint[c]);
        if (!(maxTotal > 0.0f)) { out[0][c] = 1.0f; out[1][c] = s; continue; }
        if (!(total[c] > 0.0f)) { out[0][c] = 0.0f; out[1][c] = 0.0f; continue; }
        const float tint = std::clamp(total[c] / maxTotal, 0.0f, 1.0f);
        const float finalScatter = scatter[c] * s;
        out[0][c] = tint * std::clamp((total[c] - finalScatter) / total[c], 0.0f, 1.0f);
        out[1][c] = tint * std::clamp(finalScatter / total[c], 0.0f, 1.0f);
    }
    return out;
}

BloomApplyConstants BloomRenderer::ApplyConstants() const
{
    BloomApplyConstants out{};
    // The bloom target already holds the scene's units (the resolve applied FFTMultiply, the
    // pyramid's levels carry UE's Gaussian weights), so the tonemap adds it as it is -- UE's
    // `FinalLinearColor += Bloom * exposure`. Only the scene is scaled, by the kernel centre's share.
    out.scatterApply = { 1.0f, 1.0f, 1.0f };
    if (bloomConvolution_ && bloomSurveyRatio_ >= 0.0f)
    {
        const BloomSettings& settings = frame_->settings.bloom;
        out.sceneApply = FinalizeApply(bloomKernelCenterEnergy_, bloomKernelScatterEnergy_,
                                       std::max(0.0f, settings.intensity), settings.convKernelTint)[0];
    }
    return out;
}

std::array<float, 3> BloomRenderer::ConvolutionScatterScale() const
{
    const BloomSettings& settings = frame_->settings.bloom;
    const float s = std::max(0.0f, settings.intensity);
    if (bloomSurveyRatio_ >= 0.0f)
    {
        return FinalizeApply(bloomKernelCenterEnergy_, bloomKernelScatterEnergy_, s, settings.convKernelTint)[1];
    }
    // Without a survey (the kernel's pixels could not be read on the CPU) there is no split to
    // make: the scene passes untouched and the convolution adds at `intensity`, tinted.
    std::array<float, 3> scale{};
    for (int c = 0; c < 3; ++c) { scale[c] = s * std::max(0.0f, settings.convKernelTint[c]); }
    return scale;
}

void BloomRenderer::FlaresBuild(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                      D3D12_CPU_DESCRIPTOR_HANDLE hdrSource,
                                      const BloomConvConstants& conv, const Points& pts)
{
    const bool ghosts = flaresGhosts_ && flareBokehReady_;
    const bool streak = flaresStreak_;
    const auto& D = renderer->GetDeferredForFrame();
    const BloomSettings& settings = frame_->settings.bloom;
    auto convMaterial = resources_->GetBloomConvMaterial();
    const UINT convCb = resources_->GetBloomConvCBSizeBytes();
    const uint2 streakGrid{ std::max(D.streakWidth, 1u), std::max(D.streakHeight, 1u) };
    auto flareMaterial = resources_->GetLensFlareMaterial();
    const auto samplerDescs = std::array{ *SamplerManager::LinearClamp() };
    const D3D12_GPU_DESCRIPTOR_HANDLE samplerTable =
        renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);
    // The ghosts and the streak read the same scene as the convolution (SetConvolutionInput): before
    // the upscaler when convBeforeUpscale. DLSS rings on a disc thousands of times its sky, a little
    // differently every frame; the ghosts sum the disc's tiles, and on a still frame they flickered
    // 3 % frame to frame (spikes of 12-30 %) while the FFT bloom beside them held to four digits.
    // Their tile grid is in UV, so the render-resolution source needs no other change.
    hdrSource = convInput_.ptr != 0 ? convInput_ : hdrSource;
    const auto convDispatchTo = [&](const BloomConvConstants& c, D3D12_CPU_DESCRIPTOR_HANDLE gridUav,
                                    D3D12_CPU_DESCRIPTOR_HANDLE dstUav,
                                    UINT w, UINT h, ID3D12Resource* barrierRes) {
        RecordComputeDispatch(renderer, cl, convMaterial.get(), convCb,
            [&](uint8_t* dest) { resources_->WriteBloomConvConstants(c, dest); },
            { hdrSource, D.lensFlareSRV, KernelSrv(D.lensFlareSRV),
              renderer->Exposure().BilateralGridSrv(), renderer->Exposure().BaseLumSrv() },
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
    // pass-flow S8: its own point in BOTH bloom methods now. The pyramid path used to fold this
    // barrier into its write point, which meant this shared helper had to know which method called
    // it; one layout for both is what lets it just emit the marker.
    renderer->EmitPoint(cl, pts.flareRt);
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
        // UE's LensFlareThreshold, verbatim: the VS collapses every tile whose dot(rgb, 1) is
        // below it, in the pre-exposed units the source holds (default 8). This is also the
        // pass's whole cost model: quads that collapse rasterize nothing.
        fc.threshold = std::max(settings.convGhostThreshold, 1.0e-4f);
        fc.kernelAreaInverse = 1.0f / std::max(1.0f, fc.kernelSizePx * fc.kernelSizePx);
        // The art split (BloomSettings::convSunGhostScale): off at 1 and while the sun is not in view.
        {
            Math::float3 v;
            float sx = 1.0f, sy = 1.0f, discTan = 0.0f;
            const float scale = std::clamp(settings.convSunGhostScale, 0.0f, 10.0f);
            if (scale != 1.0f && SunInView(v, sx, sy, discTan))
            {
                fc.sunDirView = Math::float4(v.x, v.y, v.z, 1.0f);
                const float radius = std::max(settings.convSunSplitRadiusDeg * 3.14159265f / 180.0f,
                                              2.0f * std::atan(discTan));
                fc.sunShare = Math::float4(scale, radius, sx, sy);
            }
        }

        auto cbAlloc = renderer->GetFrameResource()->AllocDynamic(
            resources_->GetLensFlareCBSizeBytes(), render::kConstantBufferAlignment);
        resources_->WriteLensFlareConstants(fc, static_cast<uint8_t*>(cbAlloc.cpu));
        rc.cbv[0] = cbAlloc.gpu;
        rc.srvTable[0] = renderer->StageSrvUavTable(
            { hdrSource, flareBokeh_[flareBokehSlot_].GetSRVCPU(), D.depthSRV }).gpu;
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
        render::g_renderStats.AddDraw(6u * kFlareQuadsPerInstance, instances);
    }
    renderer->EmitPoint(cl, pts.flareRead);
}

void BloomRenderer::FlaresComposite(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                          D3D12_CPU_DESCRIPTOR_HANDLE hdrSource,
                                          const BloomConvConstants& conv)
{
    const bool ghosts = flaresGhosts_ && flareBokehReady_;
    const bool streak = flaresStreak_;
    const auto& D = renderer->GetDeferredForFrame();
    const BloomSettings& settings = frame_->settings.bloom;
    auto convMaterial = resources_->GetBloomConvMaterial();
    const UINT convCb = resources_->GetBloomConvCBSizeBytes();
    const uint2 streakGrid{ std::max(D.streakWidth, 1u), std::max(D.streakHeight, 1u) };
    const auto samplerDescs = std::array{ *SamplerManager::LinearClamp() };
    const D3D12_GPU_DESCRIPTOR_HANDLE samplerTable =
        renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);
    const auto convDispatchTo = [&](const BloomConvConstants& c, D3D12_CPU_DESCRIPTOR_HANDLE gridUav,
                                    D3D12_CPU_DESCRIPTOR_HANDLE dstUav,
                                    UINT w, UINT h, ID3D12Resource* barrierRes) {
        RecordComputeDispatch(renderer, cl, convMaterial.get(), convCb,
            [&](uint8_t* dest) { resources_->WriteBloomConvConstants(c, dest); },
            { hdrSource, D.lensFlareSRV, KernelSrv(D.lensFlareSRV),
              renderer->Exposure().BilateralGridSrv(), renderer->Exposure().BaseLumSrv() },
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

bool BloomRenderer::SunWorldDirection(Math::float3& toSun) const
{
    // The SKY's sun (time-of-day T2): the glare and the corona belong to the drawn disc, which stays
    // the sun's while the moon lights the ground.
    if (!frame_ || !frame_->skyLight) { return false; }
    toSun = -frame_->skyLight->GetDirection();
    const float len = std::sqrt(toSun.x * toSun.x + toSun.y * toSun.y + toSun.z * toSun.z);
    if (!(len > 0.0f)) { return false; }
    toSun = toSun * (1.0f / len);
    return true;
}

Math::float3 BloomRenderer::ToView(const Math::float3& world) const
{
    // Row vectors: v * view, rotation part only.
    const Math::mat4& view = frame_->camera->GetViewMatrix();
    return Math::float3(
        world.x * view.m._11 + world.y * view.m._21 + world.z * view.m._31,
        world.x * view.m._12 + world.y * view.m._22 + world.z * view.m._32,
        world.x * view.m._13 + world.y * view.m._23 + world.z * view.m._33);
}

bool BloomRenderer::SunInView(Math::float3& viewDir, float& projX, float& projY, float& discTan) const
{
    if (!frame_ || !frame_->camera) { return false; }
    Math::float3 toSun;
    if (!SunWorldDirection(toSun)) { return false; }
    viewDir = ToView(toSun);
    if (viewDir.z <= 0.02f) { return false; } // behind the camera: nothing to measure it from
    const Math::mat4& proj = frame_->camera->GetProjMatrixNoJitter();
    projX = proj.m._11;
    projY = proj.m._22;
    // Further past the frame's edge than the probe's fade (bloom_conv_cs.hlsl stage 10 measures it
    // the same way, as angles per axis): the glare and the corona have faded to nothing.
    const float pastX = std::max(std::atan(std::abs(viewDir.x) / viewDir.z) - std::atan(1.0f / projX), 0.0f);
    const float pastY = std::max(std::atan(std::abs(viewDir.y) / viewDir.z) - std::atan(1.0f / projY), 0.0f);
    if (std::sqrt(pastX * pastX + pastY * pastY) > kSunOffFrameFadeDeg * (3.14159265f / 180.0f))
    {
        return false;
    }
    discTan = std::tan(std::max(frame_->skyLight->GetSunHalfApexRadians(), 0.0025f));
    return true;
}

SunRaysConstants BloomRenderer::SunRays() const
{
    SunRaysConstants c{};
    if (!frame_) { return c; }
    const BloomSettings& settings = frame_->settings.bloom;
    Math::float3 v;
    float sx = 1.0f, sy = 1.0f, discTan = 0.0f;
    if (!(settings.sunRaysIntensity > 0.0f) || !SunInView(v, sx, sy, discTan)) { return c; }
    constexpr float kDeg = 3.14159265f / 180.0f;
    // The rays leave from the disc's centre (pinned there 2026-10-14: the probe's centroid of what shows
    // ran about with every leaf over the disc). A sun wholly under the sea throws nothing.
    const float discRadius = std::atan(discTan);
    if (Math::float3 toSun; SunWorldDirection(toSun) && std::asin(std::clamp(toSun.y, -1.0f, 1.0f)) <= -discRadius)
    {
        return c;
    }
    c.dir = Math::float4(v.x, v.y, v.z, std::max(settings.sunRaysHalo, 0.0f));
    // z: how much corona a partly hidden sun keeps (tonemap_cs.hlsl SunRays); w unused since the sun
    // probe took the disc taps.
    c.proj = Math::float4(sx, sy, std::clamp(settings.sunRaysVisibleExponent, 0.0f, 1.0f), 0.0f);
    // THE CORONA FOLLOWS THE DISC'S ENERGY, NOT ITS RADIANCE: glare is the eye's point-spread
    // function, so its light is the disc's ILLUMINANCE (radiance times solid angle) spread over the
    // corona. Since the disc became physical (thousands of times the sky round it, where it had been
    // clamped to a few times) a radiance-scaled corona whited the frame out. The intensity is
    // therefore a share of the disc's light over the corona's nominal area, 2 pi length^2 -- not an
    // exact energy fraction (the needle pattern is not normalised), but a physical scaling.
    const float coronaLen = std::max(settings.sunRaysLengthDeg, 0.1f) * kDeg;
    const float discSolidAngle = 6.28318530718f * (1.0f - std::cos(discRadius));
    const float coronaScale = settings.sunRaysIntensity * discSolidAngle / (6.28318530718f * coronaLen * coronaLen);
    c.shape = Math::float4(coronaScale, static_cast<float>(std::max(settings.sunRaysCount, 16u)),
        coronaLen, std::max(settings.sunRaysSpikes, 0.0f));
    c.look = Math::float4(std::clamp(settings.sunRaysSpikeLengthDeg, 0.1f, 60.0f) * kDeg, settings.sunRaysRotationDeg * kDeg,
        discRadius, static_cast<float>(std::max(settings.sunRaysSpikeCount, 2u)));
    c.extra = Math::float4(static_cast<float>(std::max(settings.sunRaysBundles, 1u)),
        std::max(settings.sunRaysHaloRadiusDeg, 0.1f) * kDeg, 0.0f, 0.0f);
    // The widths are authored as full width at half maximum; the shader's Gaussian takes the 1/e
    // half-width, FWHM / (2 sqrt(ln 2)). A bundle is a raised cosine, whose half-width IS its FWHM,
    // passed in turns (it is an angle around the sun).
    constexpr float kFwhmToE = 1.0f / 1.6651092f;
    c.width = Math::float4(std::max(settings.sunRaysWidthDeg, 0.005f) * kDeg * kFwhmToE,
        std::max(settings.sunRaysSpikeWidthDeg, 0.005f) * kDeg * kFwhmToE,
        std::max(settings.sunRaysBundleWidthDeg, 0.1f) / 360.0f, std::clamp(settings.sunRaysBundleStrength, 0.0f, 1.0f));
    // IMAGE MODE: the image laid about the sun, its half-width where the procedural length was.
    if (coronaReady_ && !settings.sunRaysTexture.empty())
    {
        c.shape.z = std::max(settings.sunRaysTextureSizeDeg, 0.5f) * kDeg;
        c.extra.z = 1.0f;
        c.extra.w = static_cast<float>(std::max(coronaTex_.GetWidth(), 1u));
    }
    return c;
}

void BloomRenderer::RecordSunProbe(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                   D3D12_CPU_DESCRIPTOR_HANDLE sceneSource, bool jittered)
{
    if (!frame_) { return; }
    const BloomSettings& settings = frame_->settings.bloom;
    Math::float3 v;
    float sx = 1.0f, sy = 1.0f, discTan = 0.0f;
    ExposureMetering& metering = renderer->Exposure();
    if (!(settings.sunGlareIntensity > 0.0f || settings.sunRaysIntensity > 0.0f) || !metering.IsReady() ||
        !SunInView(v, sx, sy, discTan))
    {
        return;
    }
    const auto& D = renderer->GetDeferredForFrame();
    auto convMaterial = resources_->GetBloomConvMaterial();
    if (!convMaterial) { return; }
    const UINT convCb = resources_->GetBloomConvCBSizeBytes();
    const auto samplerDescs = std::array{ *SamplerManager::LinearClamp() };
    const D3D12_GPU_DESCRIPTOR_HANDLE samplerTable =
        renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);

    // What the glare and the corona are fed, in the pre-exposed units the knobs act on -- the number
    // to tune them against (a few frames old: the metering record's read-back).
    if (const ExposureMetering::Readback rb = metering.LatestReadback(); rb.valid)
    {
        LOG_INFO_THROTTLED(std::chrono::seconds(5), logging::LogCategory::Render,
            "sun probe: disc ({:.2f}, {:.2f}, {:.2f}) pre-exposed, visible share {:.2f}, EV100 {:.2f}",
            rb.sunProbe[0], rb.sunProbe[1], rb.sunProbe[2], rb.sunProbeShare, rb.eyeEv100);
    }

    constexpr float kDeg = 3.14159265f / 180.0f;
    BloomConvConstants g{};
    g.convStage = 10u;
    g.sourceSize = uint2{ 1u, 1u };
    // w: the temporal blend of the probe's output (stage 10), 1 - exp(-dt / smoothing) on a delta capped
    // like the light shafts' -- a frame the probe did not run in (sun out of view) restarts it softly.
    const double now = GetTimeSeconds();
    const double dt = probeLastSeconds_ >= 0.0 ? std::clamp(now - probeLastSeconds_, 0.0, 0.1) : 0.1;
    probeLastSeconds_ = now;
    const float smoothing = std::max(settings.sunGlareSmoothingSec, 0.0f);
    const float blend = smoothing > 0.0f ? std::max(1.0f - std::exp(-static_cast<float>(dt) / smoothing), 1.0e-3f) : 1.0f;
    g.sunGlareDir = Math::float4(v.x, v.y, v.z, blend);
    g.sunGlareProj = Math::float4(sx, sy, 0.5f * sx * discTan, 0.5f * sy * discTan);
    // yzw: world up in view space, so the probe can tell the taps above the horizon from the sea.
    const Math::float3 upView = ToView(Math::float3(0.0f, 1.0f, 0.0f));
    g.sunGlareParams = Math::float4(kSunOffFrameFadeDeg * kDeg, upView.x, upView.y, upView.z);
    // historyParams.xy (stage 12's, free here): this frame's jitter in NDC, what Camera added to the
    // projection's _31/_32 -- the scene was drawn with it, so the taps follow the disc.
    if (jittered)
    {
        const Math::mat4& drawn = frame_->camera->GetProjMatrix();
        const Math::mat4& plain = frame_->camera->GetProjMatrixNoJitter();
        g.historyParams = Math::float4(drawn.m._31 - plain.m._31, drawn.m._32 - plain.m._32, 0.0f, 0.0f);
    }
    else
    {
        g.historyParams = Math::float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    // One group, one working thread; the UAV barrier makes its record visible to the glare and the
    // tone curve after it. u0/u1 are only there because the table is positional.
    RecordComputeDispatch(renderer, cl, convMaterial.get(), convCb,
        [&](uint8_t* dest) { resources_->WriteBloomConvConstants(g, dest); },
        { sceneSource, D.lensFlareSRV, KernelSrv(D.lensFlareSRV),
              renderer->Exposure().BilateralGridSrv(), renderer->Exposure().BaseLumSrv() },
        { D.streakAUAV, D.bloomUpMipUAV[0], metering.ExposureUav() },
        samplerTable, 1u, 1u, metering.ExposureResource());
}

// UE apply local exposure to the FFT's input (PostProcessFFTBloom.cpp:826-850): the tone curve's own
// parameters and base, under the tonemap's own rules -- a grid not built this frame is never sliced
// (blend 1), and the exposure is the one the stored value already carries. Stage 0 and the pixel
// probe read the same fields.
void BloomRenderer::FillLocalExposure(BloomConvConstants& conv) const
{
    const render::CameraExposureSettings& cam = frame_->cameraExposure;
    conv.leHighlightContrast = cam.localHighlightContrast;
    conv.leShadowContrast = cam.localShadowContrast;
    conv.leDetailStrength = cam.localDetailStrength;
    conv.leBlurredBlend = localGridBuilt_ ? std::clamp(cam.localBlurredBlend, 0.0f, 1.0f) : 1.0f;
    conv.leHighlightThreshold = cam.localHighlightThreshold;
    conv.leShadowThreshold = cam.localShadowThreshold;
    conv.leMinLogLum = ExposureMeteringConstants::kMinLogLum;
    conv.leInvLogLumRange = 1.0f / (ExposureMeteringConstants::kMaxLogLum - ExposureMeteringConstants::kMinLogLum);
    conv.leExposure = render::g_preExposureEnabled ? render::g_preExposure : 0.0f;
}

void BloomRenderer::RecordPixelProbe(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                     D3D12_CPU_DESCRIPTOR_HANDLE hdrSource, bool afterBloom)
{
    if (!frame_ || !(frame_->settings.debugProbeRadiusPx > 0.0f)) { return; }
    // Source 2 reads the bloom target and is called after the bloom's build; 0 and 1 before it.
    if (afterBloom != (frame_->settings.debugProbeSource == 2u)) { return; }
    ExposureMetering& metering = renderer->Exposure();
    if (!metering.IsReady()) { return; }
    const auto& D = renderer->GetDeferredForFrame();
    auto convMaterial = resources_->GetBloomConvMaterial();
    if (!convMaterial) { return; }
    const UINT convCb = resources_->GetBloomConvCBSizeBytes();
    const auto samplerDescs = std::array{ *SamplerManager::LinearClamp() };
    const D3D12_GPU_DESCRIPTOR_HANDLE samplerTable =
        renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);

    BloomConvConstants g{};
    g.convStage = 11u;
    g.sourceSize = uint2{ 1u, 1u };
    // Stage 11 reads its window from sunGlareDir: xy the centre (UV), z the radius (px).
    g.sunGlareDir = Math::float4(frame_->settings.debugProbeU, frame_->settings.debugProbeV,
                                 frame_->settings.debugProbeRadiusPx, 0.0f);
    FillLocalExposure(g);
    // Source 1: the scene before the upscaler (the graph declares it for this read).
    const D3D12_CPU_DESCRIPTOR_HANDLE source =
        frame_->settings.debugProbeSource == 1u ? D.sceneSRV : hdrSource;
    // One group of 64; same table as the sun probe, which runs just before it in this pass.
    RecordComputeDispatch(renderer, cl, convMaterial.get(), convCb,
        [&](uint8_t* dest) { resources_->WriteBloomConvConstants(g, dest); },
        { source, D.lensFlareSRV, KernelSrv(D.lensFlareSRV),
              renderer->Exposure().BilateralGridSrv(), renderer->Exposure().BaseLumSrv() },
        { D.streakAUAV, D.bloomUpMipUAV[0], metering.ExposureUav() },
        samplerTable, 1u, 1u, metering.ExposureResource());
}

float BloomRenderer::SunGlareStrength() const
{
    if (!frame_) { return 0.0f; }
    const BloomSettings& settings = frame_->settings.bloom;
    if (!(settings.sunGlareIntensity > 0.0f)) { return 0.0f; }
    constexpr float kDeg = 3.14159265f / 180.0f;
    // A LOW sun's glare eases down to its floor (BloomSettings::sunGlareLowSun*).
    float lowSun = 1.0f;
    if (Math::float3 toSun; settings.sunGlareLowSunDeg > 0.0f && SunWorldDirection(toSun))
    {
        const float elevationDeg = std::asin(std::clamp(toSun.y, -1.0f, 1.0f)) / kDeg;
        const float t = std::clamp(elevationDeg / settings.sunGlareLowSunDeg, 0.0f, 1.0f);
        const float atHorizon = std::clamp(settings.sunGlareLowSunFloor, 0.0f, 1.0f);
        lowSun = atHorizon + (1.0f - atHorizon) * t * t * (3.0f - 2.0f * t);
    }
    return settings.sunGlareIntensity * lowSun;
}

float BloomRenderer::SunGlareScale(float discTan) const
{
    const float strength = SunGlareStrength();
    if (!(strength > 0.0f)) { return 0.0f; }
    const BloomSettings& settings = frame_->settings.bloom;
    constexpr float kDeg = 3.14159265f / 180.0f;
    constexpr float kPi = 3.14159265f;
    // THE GLARE IS A SHARE OF THE DISC'S LIGHT, as the corona is (SunRays): radiance x solid angle,
    // spread over the profile's own integral on the sky -- the core exp(-a / rc) integrates to
    // 2 pi rc^2, the veil v / (1 + (a / rv)^2) to v pi rv^2 ln(1 + (pi / rv)^2) out to the antipode.
    // So `sunGlareIntensity` is the fraction of the disc's light in the glare,
    // whatever its radii, and the veil keeps its meaning: its weight relative to the core's peak.
    const float rc = std::max(settings.sunGlareRadiusDeg, 0.05f) * kDeg;
    const float rv = std::max(settings.sunGlareVeilRadiusDeg, 0.1f) * kDeg;
    const float veil = std::max(settings.sunGlareVeil, 0.0f);
    const float profile = 2.0f * kPi * rc * rc + veil * kPi * rv * rv * std::log(1.0f + (kPi / rv) * (kPi / rv));
    const float discSolidAngle = 2.0f * kPi * (1.0f - std::cos(std::atan(discTan)));
    return strength * discSolidAngle / std::max(profile, 1.0e-12f);
}

bool BloomRenderer::SunGlareSeedFor(const float probeRgb[3], SunGlareSeed& out) const
{
    if (!frame_) { return false; }
    const BloomSettings& settings = frame_->settings.bloom;
    Math::float3 v;
    float sx = 1.0f, sy = 1.0f, discTan = 0.0f;
    if (!SunInView(v, sx, sy, discTan)) { return false; }
    // Stage 9's glare in the SCENE's units: the bloom target is added by the tone curve as it is
    // (exposureMultiplier is 1 under pre-exposure).
    const float k = SunGlareScale(discTan);
    if (!(k > 0.0f)) { return false; }
    constexpr float kDeg = 3.14159265f / 180.0f;
    out.color = Math::float3(std::max(probeRgb[0], 0.0f) * k, std::max(probeRgb[1], 0.0f) * k,
                             std::max(probeRgb[2], 0.0f) * k);
    out.viewDir = v;
    out.projX = sx;
    out.projY = sy;
    out.coreRad = std::max(settings.sunGlareRadiusDeg, 0.05f) * kDeg;
    out.veil = std::max(settings.sunGlareVeil, 0.0f);
    out.veilRad = std::max(settings.sunGlareVeilRadiusDeg, 0.1f) * kDeg;
    return out.color.x > 0.0f || out.color.y > 0.0f || out.color.z > 0.0f;
}

void BloomRenderer::SunGlareComposite(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                      D3D12_CPU_DESCRIPTOR_HANDLE hdrSource)
{
    const BloomSettings& settings = frame_->settings.bloom;
    Math::float3 v;
    float sx = 1.0f, sy = 1.0f, discTan = 0.0f;
    if (!(settings.sunGlareIntensity > 0.0f) || !SunInView(v, sx, sy, discTan))
    {
        return;
    }
    constexpr float kDeg = 3.14159265f / 180.0f;
    const float intensity = SunGlareScale(discTan);
    if (!(intensity > 0.0f)) { return; }

    const auto& D = renderer->GetDeferredForFrame();
    auto convMaterial = resources_->GetBloomConvMaterial();
    if (!convMaterial) { return; }
    const UINT convCb = resources_->GetBloomConvCBSizeBytes();
    const auto samplerDescs = std::array{ *SamplerManager::LinearClamp() };
    const D3D12_GPU_DESCRIPTOR_HANDLE samplerTable =
        renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);

    BloomConvConstants g{};
    g.convStage = 9u;
    g.sourceSize = uint2{ D.bloomWidth, D.bloomHeight };
    g.sunGlareDir = Math::float4(v.x, v.y, v.z, 0.0f);
    g.sunGlareProj = Math::float4(sx, sy, 0.5f * sx * discTan, 0.5f * sy * discTan);
    g.sunGlareParams = Math::float4(intensity,
        std::max(settings.sunGlareRadiusDeg, 0.05f) * kDeg,
        std::max(settings.sunGlareVeil, 0.0f),
        std::max(settings.sunGlareVeilRadiusDeg, 0.1f) * kDeg);
    RecordComputeDispatch(renderer, cl, convMaterial.get(), convCb,
        [&](uint8_t* dest) { resources_->WriteBloomConvConstants(g, dest); },
        { hdrSource, D.lensFlareSRV, KernelSrv(D.lensFlareSRV),
              renderer->Exposure().BilateralGridSrv(), renderer->Exposure().BaseLumSrv() },
        { D.streakAUAV, D.bloomUpMipUAV[0], renderer->Exposure().ExposureUav() },
        samplerTable, D.bloomWidth, D.bloomHeight, D.bloomUp.Get());
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

void BloomRenderer::Convolve(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                   D3D12_CPU_DESCRIPTOR_HANDLE hdrSource, const Points& pts)
{
    const auto& D = renderer->GetDeferredForFrame();
    auto fftMaterial = resources_->GetBloomFftMaterial();
    auto convMaterial = resources_->GetBloomConvMaterial();
    const UINT fftCb = resources_->GetBloomFftCBSizeBytes();
    const UINT convCb = resources_->GetBloomConvCBSizeBytes();
    const BloomSettings& settings = frame_->settings.bloom;

    GPU_SCOPE(cl, ProfilerScopes::kPassBloomConv);

    auto flareMaterial = resources_->GetLensFlareMaterial();
    // P8C-2m: the same frame gates the graph was declared from -- a body that re-derived them
    // could disagree with the Prepare, which is a barrier the compile never registered.
    const bool ghosts = flaresGhosts_ && flareBokehReady_;
    const bool streak = flaresStreak_;

    const uint2 streakGrid{ std::max(D.streakWidth, 1u), std::max(D.streakHeight, 1u) };

    // pass-flow S8: the up chain, the three grids and (when the flares run) the streak pair in ONE
    // marker — the tonemap builder declared them together, in this order.
    // P8C-2l: no source downsample any more -- the scatter and the streak prefilter both read
    // the HDR image directly, each with its own absolute threshold. Sharing one pre-thresholded
    // texture made `bloom.threshold` cascade with theirs, and it tied both to whichever bloom
    // method had built the chain.
    renderer->EmitPoint(cl, pts.write);


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
    float pct = std::clamp(settings.convPercent, 10.0f, 100.0f) * 0.01f;
    // THE POWER-OF-TWO CLIFF AT 4K. The grid is the next power of two above the image plus its 5/4
    // zero pad, so the cost does not follow the percent smoothly: at 3840x2160 the default 25 % is a
    // 960x540 image whose pad (1200x675) spills out of 1024x512 into a 2048x1024 grid -- four times
    // the cells 1440p transforms, 0.63 ms instead of 0.22 -- to blur a bloom image 1.5x finer than the
    // one the level was tuned on. When the resolution pushes the grid past the 1440p one, the percent
    // steps down to the largest that fits the next-smaller grid, if that keeps at least 3/4 of it:
    // 25 -> 19 % at 4K, measured 0.63 -> 0.33 ms with no visible change. 1440p and below never step.
    {
        const float outW = static_cast<float>(renderer->GetWidth());
        const float outH = static_cast<float>(renderer->GetHeight());
        const float spanFrac = std::clamp(settings.convSize, 0.02f, 1.0f);
        const auto gridCells = [&](float p) -> std::uint64_t
        {
            const UINT w = std::max(16u, static_cast<UINT>(std::lround(outW * p)));
            const UINT h = std::max(16u, static_cast<UINT>(std::lround(outH * p)));
            const UINT span = static_cast<UINT>(std::ceil(std::max(spanFrac * static_cast<float>(std::max(w, h)), 1.0f)));
            return static_cast<std::uint64_t>(nextPow2(std::max((w * 5u) / 4u, span))) *
                   nextPow2(std::max((h * 5u) / 4u, span));
        };
        constexpr std::uint64_t k1440Grid = 1024ull * 512ull;
        const std::uint64_t full = gridCells(pct);
        float lo = 0.75f * pct;
        // The cheapest grid within the 3/4 bound, then the largest percent that still fits it:
        // gridCells never falls as the percent grows, so bisect for that grid's upper edge.
        const std::uint64_t target = gridCells(lo);
        if (full > k1440Grid && target < full)
        {
            float hi = pct;
            for (int i = 0; i < 24; ++i)
            {
                const float mid = 0.5f * (lo + hi);
                (gridCells(mid) <= target ? lo : hi) = mid;
            }
            LOG_INFO_ONCE_PER_MESSAGE(logging::LogCategory::Render,
                "bloom: convPercent {:.1f} -> {:.1f} at {}x{} (FFT grid {} -> {} cells)",
                pct * 100.0f, lo * 100.0f, renderer->GetWidth(), renderer->GetHeight(), full, gridCells(lo));
            pct = lo;
        }
    }
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
    // UE's FilterPixel (BloomConvolutionPreFilterMin/Max/Mult), in the pre-exposed units the
    // scene colour is stored in -- theirs runs on the pre-exposed scene too. No threshold: UE's FFT
    // bloom has none, and the partition below needs the whole scene as its input.
    conv.preFilterMin = std::max(0.0f, settings.convPreFilterMin);
    conv.preFilterMax = std::max(0.0f, settings.convPreFilterMax);
    conv.preFilterMult = std::max(0.0f, settings.convPreFilterMult);
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
        // UE's survey at this size: the clamp level the kernel is cut to before it is resampled,
        // the centre texel it is laid out around, and the centre/scatter energies the tonemap's
        // partition is formed from -- one survey, so the split weighs the kernel being convolved.
        // (The clamp moves with convSize, as theirs does: the light it takes out of the kernel is
        // not lost, it stays in the scene through the centre term.)
        SurveyKernel(conv.kernelSpanTexels);
    }
    if (bloomSurveyRatio_ >= 0.0f && bloomKernelPixelDim_ > 0u)
    {
        conv.kernelClampLevel = bloomKernelClampLevel_;
        conv.kernelCenterUV[0] = (static_cast<float>(bloomKernelCenterX_) + 0.5f) / static_cast<float>(bloomKernelPixelDim_);
        conv.kernelCenterUV[1] = (static_cast<float>(bloomKernelCenterY_) + 0.5f) / static_cast<float>(bloomKernelPixelDim_);
    }
    else
    {
        // No survey: the kernel as it is, centred -- the split falls back to additive too.
        conv.kernelClampLevel = 1.0e30f;
        conv.kernelCenterUV[0] = 0.5f;
        conv.kernelCenterUV[1] = 0.5f;
    }
    {
        const std::array<float, 3> scale = ConvolutionScatterScale();
        conv.resolveScale[0] = scale[0];
        conv.resolveScale[1] = scale[1];
        conv.resolveScale[2] = scale[2];
    }
    FillLocalExposure(conv);
    // P8C-2l: the flare fields come from the shared builder -- assembling them in two places is
    // exactly how the two bloom methods would drift apart.
    {
        const BloomConvConstants f = FlareConstants(renderer);
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
    // The art split for stage 0 (BloomSettings::convSunKernelScale): off at 1 -- an exact no-op -- and
    // while the sun is not in view.
    {
        Math::float3 v;
        float sx = 1.0f, sy = 1.0f, discTan = 0.0f;
        const float scale = std::clamp(settings.convSunKernelScale, 0.0f, 10.0f);
        if (scale != 1.0f && SunInView(v, sx, sy, discTan))
        {
            conv.sunGlareDir = Math::float4(v.x, v.y, v.z, 0.0f);
            conv.sunGlareProj = Math::float4(sx, sy, 0.0f, 0.0f);
            const float radius = std::max(settings.convSunSplitRadiusDeg * 3.14159265f / 180.0f,
                                          2.0f * std::atan(discTan));
            conv.sunShare = Math::float4(scale, radius, 1.0f, 0.0f);
        }
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
            [&](uint8_t* dest) { resources_->WriteBloomConvConstants(c, dest); },
            // t1 is the flare-blur image (only the resolve's ghost composite reads it) and t2 the
            // kernel photograph (only the resample stage reads it) -- but a descriptor table is
            // POSITIONAL, so both are bound on every stage rather than left as holes. The kernel
            // is guaranteed resident here (bloomConvolution_ requires bloomKernelReady_), but the
            // slot still comes through KernelSrv: one assembly helper for the shared table.
            // t0: only the setup (stage 0) reads it -- the scene before the upscaler when asked for.
            // The setup takes the scene's DEPTH in t1 instead of the flare image it never reads: the
            // art split's sky test (SunShareMask).
            { convInput_.ptr != 0 ? convInput_ : hdrSource, c.convStage == 0u ? D.depthSRV : D.lensFlareSRV,
              KernelSrv(D.lensFlareSRV),
              renderer->Exposure().BilateralGridSrv(), renderer->Exposure().BaseLumSrv() },
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
            [&](uint8_t* dest) { resources_->WriteBloomFftConstants(c, dest); },
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
        FlaresBuild(renderer, cl, hdrSource, conv, pts);
    }

    // ---- kernel: resample the photograph, transform, add the streak's spectrum. Rebuilt only
    // when its parameters move -- EVERY placement parameter is in the key, because the spectrum
    // is cached and one left out is a control that appears to do nothing until something else
    // forces a rebuild. ----
    const BloomKernelKey key{ grid.x, grid.y, image.x, image.y, convSizeFrac,
                              conv.kernelClampLevel, conv.kernelCenterUV[0], conv.kernelCenterUV[1],
                              renderer->GetDeferredTargetsGeneration() };
    if (!(key == bloomKernelKey_))
    {
        CPU_SCOPE(ProfilerScopes::kBloomRecKernel);
        // The audit trail for the recreation bug: every "deferred targets created" line in the
        // session log must be followed by exactly one of these. Logged only when the TARGETS
        // moved, never for a control moving -- a slider drag rebuilds every frame.
        if (bloomKernelKey_.generation != key.generation)
        {
            LOG_INFO(logging::LogCategory::Render,
                     "bloom kernel spectrum rebuilt: grid {}x{}, targets generation {}",
                     grid.x, grid.y, key.generation);
        }
        BloomConvConstants k = conv;
        BloomFftConstants f{};
        f.transformSize = grid;

        // The photograph into grid A, centre folded to the DC corner...
        k.convStage = 3u;
        convDispatch(k, D.bloomFftAUAV, grid.x, grid.y, D.bloomFftA);

        // ...and its spectrum into the kernel texture. The DC texel now holds the per-channel
        // sums, which both the streak's amplitude and the multiply's normalisation read.
        f.isVertical = 0u;
        fftDispatch(f, D.bloomFftAUAV, D.bloomFftKernelUAV, D.bloomFftBUAV, D.bloomFftB);
        f.isVertical = 1u;
        fftDispatch(f, D.bloomFftBUAV, D.bloomFftKernelUAV, D.bloomFftKernelUAV,
                    D.bloomFftKernel);

        bloomKernelKey_ = key;
    }

    // ---- frame: pack -> forward -> multiply -> inverse -> resolve ----
    CPU_SCOPE(ProfilerScopes::kBloomRecFft);
    {
        BloomConvConstants s = conv;
        s.convStage = 0u;
        s.sourceSize = uint2{ renderer->GetWidth(), renderer->GetHeight() };
        convDispatch(s, D.bloomFftAUAV, grid.x, grid.y, D.bloomFftA);
    }

    BloomFftConstants f{};
    f.transformSize = grid;
    f.isVertical = 0u;
    fftDispatch(f, D.bloomFftAUAV, D.bloomFftKernelUAV, D.bloomFftBUAV, D.bloomFftB);
    f.isVertical = 1u;
    fftDispatch(f, D.bloomFftBUAV, D.bloomFftKernelUAV, D.bloomFftAUAV, D.bloomFftA);
    // P8C-2: the Hermitian spectral multiply, on the COMPLETE forward spectrum.
    f.mode = 1u;
    f.isVertical = 0u;
    fftDispatch(f, D.bloomFftAUAV, D.bloomFftKernelUAV, D.bloomFftBUAV, D.bloomFftB);
    f.mode = 0u;
    f.isInverse = 1u;
    f.isVertical = 1u;
    fftDispatch(f, D.bloomFftBUAV, D.bloomFftKernelUAV, D.bloomFftAUAV, D.bloomFftA);
    f.isVertical = 0u;
    fftDispatch(f, D.bloomFftAUAV, D.bloomFftKernelUAV, D.bloomFftBUAV, D.bloomFftB);

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
        FlaresComposite(renderer, cl, hdrSource, conv);
    }
    SunGlareComposite(renderer, cl, hdrSource);
    // ...and all of them back to shader-readable for the tone curve, in one marker.
    renderer->EmitPoint(cl, pts.read);
}

// P8C-2 step 5a: the ghost bokeh sprite -- the iris polygon with its bright rim, P8D's
// ApertureMask moved to the CPU. UE author theirs as a texture asset; keeping it procedural is
// what lets `blades` still mean something after the aperture kernel's retirement.
//
// P8C-2d: no GPU wait anywhere in here. The pixels are built on the CPU, the copy is enqueued
// with UploadBatch::Submit (close + execute, no fence wait) into the slot the scatter is NOT
// sampling, and the batch is held until the frame gate retires it -- which is what keeps the
// intermediates and the command allocator alive for exactly as long as the GPU needs them.
void BloomRenderer::BakeFlareBokeh(Renderer* renderer, uint32_t blades)
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
    flareBokeh_[slot].SetMemTag(MemTag::Bloom);
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
