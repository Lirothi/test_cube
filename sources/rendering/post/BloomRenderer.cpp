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

#include "core/diagnostics/DiagPaths.h" // P8C-2o kernel survey verdict
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "rendering/core/ComputeDispatch.h"
#include "rendering/core/PhotographicSettings.h" // P16.1 pre-exposure
#include "rendering/core/RenderConstants.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h" // the ghost sprite sheet is uploaded once, lazily

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
                if (bloomKernelReady_) { ReadKernelPixels(desc.path.c_str()); }
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
                FILE* lg = nullptr;
                if (fopen_s(&lg, diag::LogPath("bloom_kernel.log").c_str(), "a") == 0 && lg)
                {
                    std::fprintf(lg, "[ghost] DISABLED: lens flare material=%p pso=%p cb=%u\n",
                                 (void*)flareMat.get(),
                                 flareMat ? (void*)flareMat->GetPipelineState() : nullptr,
                                 resources_->GetLensFlareCBSizeBytes());
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
                        DB.bloomFftA.Get() != nullptr && DB.bloomFftB.Get() != nullptr &&
                        DB.bloomFftKernel.Get() != nullptr &&
                        DB.lensFlare.Get() != nullptr;
    bloomActive_ = frame.settings.bloom.enabled &&
                   frame.settings.bloom.intensity > 0.0f &&
                   (bloomConvolution_ ||
                    (bloomMaterial != nullptr &&
                     bloomMaterial->GetPipelineState() != nullptr)) &&
                   resources_->GetBloomCBSizeBytes() > 0u &&
                   DB.bloomMips > 0u && DB.bloomDown.Get() != nullptr &&
                   DB.bloomUp.Get() != nullptr;
}

void BloomRenderer::Declare(RenderGraphPassContext& ctx, Points& out) const
{
    constexpr D3D12_RESOURCE_STATES kNps = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto& D = ctx.renderer->GetDeferredForFrame();

    ctx.NextPoint();
    out.write = ctx.usePoint ? *ctx.usePoint : 0u;
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
        ctx.Use(D.bloomFftA.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(D.bloomFftB.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Use(D.bloomFftKernel.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
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
        ctx.Use(D.bloomFftA.Get(), kNps);
        ctx.Use(D.bloomFftB.Get(), kNps);
        ctx.Use(D.bloomFftKernel.Get(), kNps);
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
    // The METHOD is the decision Declare() built its points from — asking the settings again here
    // is what let a frame with an unready convolution record one method against the other's
    // declarations.
    if (bloomConvolution_) { Convolve(renderer, cl, hdrSource, pts); }
    else { Build(renderer, cl, hdrSource, pts); }
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

    // P8C-4: the same ABSOLUTE scaling the convolution now uses, so switching method is not also a
    // threshold change. See the note in bloom_conv_cs.hlsl stage 0.
    const float absThreshold = (settings.threshold < 0.0f)
        ? 0.0f
        : settings.threshold * (render::g_preExposure / render::ExposureMultiplierFromEv100(14.0f));
    Downsample(renderer, cl, hdrSource, absThreshold, 0u);

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
        FlaresComposite(renderer, cl, hdrSource, flareConv);
    }
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
// `render::g_preExposure` is what makes the thresholds absolute -- see the comment on them.
// P8C-2o: what the TONEMAP multiplies the bloom target by. The flares composite into that same
// target after the resolve, so both of their intensity knobs have to divide this back out to mean
// "fraction of the source's own brightness" -- and under the centre/scatter split it is no longer
// `bloom.intensity`, it is the surveyed scatter fraction.
float BloomRenderer::TonemapBloomScale() const
{
    const BloomApplyConstants a = ApplyConstants();
    return std::max(a.scatterApply[0], std::max(a.scatterApply[1], a.scatterApply[2]));
}

BloomConvConstants BloomRenderer::FlareConstants(Renderer* renderer) const
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
                            std::max(TonemapBloomScale(), 0.01f);
    c.anamorphicLength = std::max(0.0005f, settings.convAnamorphicLength);
    c.anamorphicSigma = std::max(0.5f, settings.convAnamorphicWidth) *
                        (static_cast<float>(D.bloomWidth) / displayW);
    // ABSOLUTE units: "is this a light source" is a property of the SCENE, not the camera, but
    // the buffers hold PRE-EXPOSED values. The authored number means stored brightness at
    // EV100 = 14, rescaled by the frame's own pre-exposure.
    const float thresholdScale = render::g_preExposure / render::ExposureMultiplierFromEv100(14.0f);
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
                       std::max(TonemapBloomScale(), 0.01f);
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
void BloomRenderer::SurveyKernel(float ratio)
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
BloomApplyConstants BloomRenderer::ApplyConstants() const
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
    const auto convDispatchTo = [&](const BloomConvConstants& c, D3D12_CPU_DESCRIPTOR_HANDLE gridUav,
                                    D3D12_CPU_DESCRIPTOR_HANDLE dstUav,
                                    UINT w, UINT h, ID3D12Resource* barrierRes) {
        RecordComputeDispatch(renderer, cl, convMaterial.get(), convCb,
            [&](uint8_t* dest) { resources_->WriteBloomConvConstants(c, dest); },
            { hdrSource, D.lensFlareSRV, KernelSrv(D.lensFlareSRV) },
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
        // UE's LensFlareThreshold, verbatim mechanism: the VS collapses every tile below it. The
        // bloom extraction's own threshold is NOT enough -- at bloom.threshold -1 the source is
        // unthresholded and sunlit foliage splats bokehs, which the composite then mirrors into
        // the sky as upside-down palms. This is also the pass's whole cost model: quads that
        // collapse rasterize nothing.
        // Absolute units, same EV14 reference as the streak threshold -- see that comment.
        fc.threshold = std::max(settings.convGhostThreshold, 1.0e-4f) *
                       (render::g_preExposure / render::ExposureMultiplierFromEv100(14.0f));
        fc.kernelAreaInverse = 1.0f / std::max(1.0f, fc.kernelSizePx * fc.kernelSizePx);

        auto cbAlloc = renderer->GetFrameResource()->AllocDynamic(
            resources_->GetLensFlareCBSizeBytes(), render::kConstantBufferAlignment);
        resources_->WriteLensFlareConstants(fc, static_cast<uint8_t*>(cbAlloc.cpu));
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
            { hdrSource, D.lensFlareSRV, KernelSrv(D.lensFlareSRV) },
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
    const float absScale = render::g_preExposure / render::ExposureMultiplierFromEv100(14.0f);
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
        SurveyKernel(ratio);
    }
    conv.kernelTint[0] = std::max(0.0f, settings.convKernelTint[0]);
    conv.kernelTint[1] = std::max(0.0f, settings.convKernelTint[1]);
    conv.kernelTint[2] = std::max(0.0f, settings.convKernelTint[2]);
    conv.kernelCenterUV[0] = 0.5f;
    conv.kernelCenterUV[1] = 0.5f;
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
            { hdrSource, D.lensFlareSRV, KernelSrv(D.lensFlareSRV) },
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
        FlaresComposite(renderer, cl, hdrSource, conv);
    }
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
