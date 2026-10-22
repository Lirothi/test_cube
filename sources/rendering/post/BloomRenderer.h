#pragma once
// R3 (docs/scene_renderer_refactor_plan.md): the bloom subsystem.
//
// It records into the TONEMAP's command list — bloom must see the upscaled image and the tone
// curve must see the bloom — so it is not a render-graph pass of its own. What it IS, is the owner
// of nineteen members nothing else ever read, of the decision about which method runs this frame,
// and of the four barrier points that decision declares. SceneRenderer used to hold all three.
//
// Three entry points, in the order a frame uses them:
//   Decide()  — once per frame, before the graph is built: loads the kernel asset if the setting
//               moved, retires/starts the ghost bokeh bake, and settles active/convolution/flares.
//   Declare() — from the tonemap's AddPass2 builder: four points, declared from those decisions.
//   Record()  — from the tonemap's body: the dispatches, emitting the points as markers.
//
// The point LAYOUT is the same for both methods on purpose (write / flare RT / flare read / read).
// P8C-2l and P8C-2m were each a point moving or vanishing under a gate; one shape for both is what
// lets the shared flare build emit its markers without knowing which method called it.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <d3d12.h>

#include "app/scene/SceneFrameData.h"
#include "app/scene/SceneResourceBootstrapper.h"
#include "materials/Texture2D.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/core/RenderGraph.h"
#include "rendering/core/UploadBatch.h"

class Renderer;

class BloomRenderer
{
public:
    // The four points this subsystem declares inside the tonemap pass, in body order. They are
    // indices into the TONEMAP's barrier program — the bloom does not own a program of its own.
    struct Points
    {
        std::uint32_t history = 0;   // the temporal input (stage 12): this slot's history -> UAV
        std::uint32_t write = 0;     // the chains (and the streak pair) -> UAV; the history -> read
        std::uint32_t flareRt = 0;   // lens flare -> RENDER_TARGET
        std::uint32_t flareRead = 0; // lens flare -> shader-readable
        std::uint32_t read = 0;      // the chains back to shader-readable for the tone curve
    };

    // The bootstrapper outlives every frame, so the pointer is taken once.
    void Initialize(SceneResourceBootstrapper* resources) { resources_ = resources; }
    // NOTE: there is deliberately no Reset(). Nothing here is level state — the kernel image and
    // the bokeh sprite are ASSETS, and SceneRenderer::Reset never cleared them either. Decide()
    // re-checks every material and target each frame, which is what a level switch actually needs.

    // Per-frame, before the graph is built. `frame` is remembered for the rest of the frame:
    // Declare and Record both read the same settings this decision was taken from.
    void Decide(Renderer* renderer, const SceneFrameData& frame);

    bool Active() const { return bloomActive_; }
    bool Convolution() const { return bloomConvolution_; }
    bool Flares() const { return flares_; }

    // The tonemap builder calls this between its source point and its FXAA point. It advances the
    // point cursor five times whatever the decisions are — a point is a POSITION in the pass's
    // program, so only its CONTENT is gated.
    void Declare(RenderGraphPassContext& ctx, Points& out) const;

    // The tonemap body, on the tonemap's list, with whatever the tone curve is about to read.
    void Record(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                D3D12_CPU_DESCRIPTOR_HANDLE hdrSource, const Points& pts);
    // Whether local exposure's bilateral grid was built this frame (the metering builder's gate):
    // the convolution's input gets local exposure as UE's does, and an unbuilt grid is never sliced.
    void SetLocalExposureGrid(bool built) { localGridBuilt_ = built; }
    // What the convolution's setup (stage 0) reads: the scene before the upscaler when
    // BloomSettings::convBeforeUpscale asks for it, else the tonemap's source. A null handle = the
    // tonemap's source. Set before Record, every frame.
    void SetConvolutionInput(D3D12_CPU_DESCRIPTOR_HANDLE h) { convInput_ = h; }

    // Stage 10, the SUN PROBE: meters the sun disc once into the metering record (sun_probe.hlsli)
    // for the glare and the corona, holding its last value while the disc is off the frame. On the
    // tonemap's list BEFORE Record() and the tone curve; nothing when neither sun effect is on or
    // the sun is not in view.
    void RecordSunProbe(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                        D3D12_CPU_DESCRIPTOR_HANDLE hdrSource);
    // Stage 11, the PIXEL PROBE (SceneRenderSettings::debugProbe*): a debug read-out of the HDR frame
    // around one screen position into the metering record, read back for the editor MCP's
    // get_readouts. Same list as the sun probe; nothing while its radius is 0. Sources 0/1 are read
    // before the bloom is built (`afterBloom` false), source 2 -- the bloom target -- after it.
    void RecordPixelProbe(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                          D3D12_CPU_DESCRIPTOR_HANDLE hdrSource, bool afterBloom);
    // How far past the frame's edge (degrees) the sun glare and the corona fade out over.
    static constexpr float kSunOffFrameFadeDeg = 8.0f;

    // Read by the tone curve: the kernel centre's share of the scene (UE's SceneColorApply) and the
    // scale the bloom target is added with (1: it already holds scene units).
    BloomApplyConstants ApplyConstants() const;
    // The sun's star for the tone curve pass (BloomSettings::sunRays*); off when the setting is 0
    // or the sun is behind the camera / well off screen.
    SunRaysConstants SunRays() const;
    // The SUN GLARE as the light shafts' seed sees it (owner, 2026-10-14: "blur after the glare is laid"):
    // stage 9's analytic halo in the scene's own pre-exposed units, from a sun-probe colour (the caller's
    // read-back one: the shafts run before this frame's probe). False when the glare is off, the bloom is
    // off (the glare is then not in the picture either) or the sun is out of view.
    struct SunGlareSeed
    {
        Math::float3 color{};   // at the sun's centre, before the core / veil fall-off
        Math::float3 viewDir{}; // to the sun, view space
        float projX = 1.0f, projY = 1.0f;
        float coreRad = 0.0f, veil = 0.0f, veilRad = 0.0f;
    };
    bool SunGlareSeedFor(const float probeRgb[3], SunGlareSeed& out) const;

private:
    void Downsample(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                    D3D12_CPU_DESCRIPTOR_HANDLE hdrSource, float threshold, UINT mipCount);
    void Build(Renderer* renderer, ID3D12GraphicsCommandList* cl,
               D3D12_CPU_DESCRIPTOR_HANDLE hdrSource, const Points& pts);
    void Convolve(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                  D3D12_CPU_DESCRIPTOR_HANDLE hdrSource, const Points& pts);
    BloomConvConstants FlareConstants(Renderer* renderer) const;
    void FlaresBuild(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                     D3D12_CPU_DESCRIPTOR_HANDLE hdrSource, const BloomConvConstants& conv,
                     const Points& pts);
    void FlaresComposite(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                         D3D12_CPU_DESCRIPTOR_HANDLE hdrSource, const BloomConvConstants& conv);
    // Stage 9: the fake sun glare (BloomSettings::sunGlare*), additive onto bloom mip 0 after
    // either method's own output. Records nothing when it is off or the sun is not in view.
    void SunGlareComposite(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                           D3D12_CPU_DESCRIPTOR_HANDLE hdrSource);
    // The glare's strength this frame: sunGlareIntensity eased down on a low sun; 0 = off.
    float SunGlareStrength() const;
    // What the glare's core term is multiplied by, per unit of the probe's disc colour: the strength
    // times the disc's solid angle over the profile's integral, so `sunGlareIntensity` is the share
    // of the disc's light that the glare carries. `discTan` from SunInView.
    float SunGlareScale(float discTan) const;
    // The convolution resolve's per-channel scale: UE's FFTMultiply (Tint * the scatter's share of
    // the kernel at this intensity) times the kernel tint. Neutral when the survey has not run.
    std::array<float, 3> ConvolutionScatterScale() const;
    // The local-exposure fields of the convolution's constants (stage 0 and the pixel probe).
    void FillLocalExposure(BloomConvConstants& conv) const;
    // The sun in this frame's view: its direction in view space, the projection's x/y scales and
    // the tangent of the disc's half angle. False when it is behind the camera or further past the
    // frame's edge than kSunOffFrameFadeDeg.
    bool SunInView(Math::float3& viewDir, float& projX, float& projY, float& discTan) const;
    // The direction TO the sun in world space, unit; false without a sun.
    bool SunWorldDirection(Math::float3& toSun) const;
    // A world direction into this frame's view space (rotation only). Needs a camera.
    Math::float3 ToView(const Math::float3& world) const;
    void BakeFlareBokeh(Renderer* renderer, uint32_t blades);
    bool ReadKernelPixels(const wchar_t* path);
    // UE's survey at a kernel drawn `spanTexels` grid texels wide (see the definition).
    void SurveyKernel(float spanTexels);

    SceneResourceBootstrapper* resources_ = nullptr;
    const SceneFrameData* frame_ = nullptr;
    double probeLastSeconds_ = -1.0; // the sun probe's temporal blend: when it last ran
    bool localGridBuilt_ = false;    // SetLocalExposureGrid

    // ---- the decisions, taken once per frame in Decide() ----
    bool bloomActive_ = false;
    // P8C: which bloom method this frame runs. Read by the tonemap builder AND its body, so the
    // declared resources and the emitted barriers cannot disagree.
    bool bloomConvolution_ = false;
    // P8C-2m: the flare gates for the whole frame. Zero intensity is a REAL off switch, not a
    // multiply by zero: nothing is dispatched, nothing is drawn, and none of the targets are
    // declared, so not even a barrier is emitted.
    bool flaresGhosts_ = false;
    bool flaresStreak_ = false;
    bool flares_ = false; // ghosts || streak, the form the declarations and the bodies use
    // The temporal input (stage 12, BloomSettings::convTemporalWeight): on while the bloom reads the
    // scene BEFORE an upscaler that runs (the jittered image), and its history's bookkeeping -- valid
    // only after a frame that wrote it at this size and camera history (the light shafts' rule), with
    // the pre-exposure it was written under so a moving exposure rescales it.
    bool temporal_ = false;
    bool historyValid_ = false;
    std::uint32_t historyFrames_ = 0u;
    std::uint32_t historyWidth_ = 0u, historyHeight_ = 0u;
    std::uint64_t historyRevision_ = 0u;
    float historyPreExposure_ = 1.0f;
    void RecordTemporalInput(Renderer* renderer, ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE scene);

    // The kernel's spectrum is a pure function of these, so it is only rebuilt when one moves.
    // P8C-2: the shape controls died with the generated aperture; what remains is the kernel
    // IMAGE's placement (size, active grid) and the streak composited into it.
    struct BloomKernelKey
    {
        uint32_t width = 0u, height = 0u;          // the ACTIVE grid
        uint32_t imageWidth = 0u, imageHeight = 0u; // the span depends on the image's major axis
        float convSize = -1.0f;
        // The survey's clamp level and the kernel's centre texel shape the resampled kernel too.
        // (The kernel TINT is not in the key any more: it is applied by the resolve, after the
        // per-channel normalisation that would otherwise cancel it.)
        float clampLevel = -1.0f;
        float centerU = -1.0f, centerV = -1.0f;
        // THE TARGETS' GENERATION IS PART OF THE KEY, and leaving it out is the bug behind
        // "toggling DLSS breaks bloom". Every other field describes the kernel's CONTENT, and all
        // of them are derived from the DISPLAY resolution, which a DLSS toggle does not move. But
        // the toggle changes the RENDER resolution, `Renderer::RecreateDeferredTargets` destroys
        // and rebuilds every DeferredTargets resource, and `bloomFftKernel` comes back EMPTY while
        // the key beside it still describes the spectrum that used to be in the old texture. The
        // rebuild is then skipped and the convolution multiplies by zeros -- bloom disappears
        // completely, and only after a toggle, which is why a cold boot in either mode looks right.
        //
        // The first fix keyed on the texture's ADDRESS, and that is an ABA bug: the runtime frees
        // three kernel objects and allocates three new ones of the same size, so a new texture
        // lands on the address the old one had -- measured on wind_test, 17 of 40 recreations put
        // one slot's new kernel exactly where its old one was (slot 2 nearly every time). The key
        // then matches an EMPTY texture, that slot skips its rebuild, and bloom blinks at the
        // frame-ring frequency: one frame in three with no kernel and no streak, which is what
        // "toggle DLSS a few times and the bloom flickers" looks like. A generation counter cannot
        // be recycled: `Renderer::CreateDeferredTargets` bumps it every time the targets are built,
        // so the key mismatches after every recreation.
        uint64_t generation = 0u;
        bool operator==(const BloomKernelKey& o) const
        {
            return width == o.width && height == o.height &&
                   imageWidth == o.imageWidth && imageHeight == o.imageHeight &&
                   convSize == o.convSize && generation == o.generation &&
                   clampLevel == o.clampLevel && centerU == o.centerU && centerV == o.centerV;
        }
    };
    // ONE key, because there is one kernel spectrum: RenderTargetManager keeps a single set of
    // convolution grids for every frame in flight (each DeferredTargets slot aliases it). It used to
    // be a key per frame slot when every slot had its own texture -- a single key then meant slot 0
    // built the kernel and slots 1 and 2 convolved against an empty one. A per-frame grid set
    // reappearing needs the per-slot keys back with it.
    BloomKernelKey bloomKernelKey_{};
    // P8C-2: the photographed convolution kernel (UE's DefaultBloomKernel as an FP16 DDS with
    // mips). Loaded once, lazily, at the frame-gate site -- WITHOUT it the convolution method
    // refuses to enable (UE's own gate: IsFFTBloomEnabled is false with no kernel texture), rather
    // than falling back to a procedural kernel that no longer exists.
    Texture2D bloomKernelTex_;
    bool bloomKernelReady_ = false;
    D3D12_CPU_DESCRIPTOR_HANDLE convInput_{};   // SetConvolutionInput
    // The conv shader's t2 slot for EVERY conv dispatch site -- the table is positional, so the
    // slot is always staged, but the kernel photograph is resident only once the CONVOLUTION
    // method has loaded it. The streak/ghost stages never sample t2, yet CopyDescriptors
    // dereferences every source handle: staging the empty texture's null handle is a driver
    // access violation, not an unused hole (hit by pyramid bloom + anamorphic streak, where the
    // kernel never loads). All three conv-table assembly sites must come through here.
    D3D12_CPU_DESCRIPTOR_HANDLE KernelSrv(D3D12_CPU_DESCRIPTOR_HANDLE fallback) const
    {
        return bloomKernelReady_ ? bloomKernelTex_.GetSRVCPU() : fallback;
    }
    // P8C-2r: which image is currently resident. Empty means none; comparing it against the
    // setting is the whole reload gate, which is why the old once-only `bloomKernelTried_`
    // flag is gone rather than kept beside it -- two gates would have disagreed.
    std::string bloomKernelLoadedPath_;
public:
    // The sun corona's image for the tone curve's t4 (BloomSettings::sunRaysTexture), or
    // `fallback` while none is resident -- the table is positional, so something is always staged.
    D3D12_CPU_DESCRIPTOR_HANDLE CoronaSrv(D3D12_CPU_DESCRIPTOR_HANDLE fallback) const
    {
        return coronaReady_ ? coronaTex_.GetSRVCPU() : fallback;
    }
private:
    Texture2D coronaTex_;
    bool coronaReady_ = false;
    std::string coronaLoadedPath_;
    // P8C-2o -- THE SAME PIXELS ON THE CPU, for UE's centre/scatter survey.
    //
    // They survey the kernel on the GPU (FindKernelCenter -> SurveyKernelCenterEnergy ->
    // SumScatterDispersionEnergy), which for us would mean a readback the tonemap cannot wait for.
    // It does not have to: what the survey produces are RATIOS over a static image, and a ratio is
    // invariant to the resampling that stands between the texture and the grid -- box minification
    // scales both sums by the same (span/texels)^2. So the identical numbers come off the texture
    // itself, once per kernel key, with nothing to race.
    std::vector<float> bloomKernelPixels_;      // RGB triples, row-major, square
    uint32_t bloomKernelPixelDim_ = 0u;
    // Sums from the last survey, and the view-texel diameter (in kernel texels) they were taken at.
    // Energy, not colour -- the apply constants are formed from these every frame because they also
    // depend on the intensity.
    std::array<float, 3> bloomKernelCenterEnergy_{};
    std::array<float, 3> bloomKernelScatterEnergy_{};
    float bloomSurveyRatio_ = -1.0f;
    // UE's MaxScatterDispersion (one grey level) and FindKernelCenter (the brightest texel).
    float bloomKernelClampLevel_ = 1.0e30f;
    uint32_t bloomKernelCenterX_ = 0u, bloomKernelCenterY_ = 0u;
    // P8C-2 step 5a: the ghost BOKEH SPRITE -- the iris polygon the scatter splats. Baked on the
    // CPU from the blade count (its new home after the aperture kernel's retirement) and rebaked
    // when it moves.
    //
    // P8C-2d -- DOUBLE BUFFERED, AND THE UPLOAD DOES NOT WAIT. The first version called
    // WaitForPreviousFrame() + SubmitAndWait() from the frame gate, so every change of the blade
    // controls flushed the whole GPU -- once per frame while a slider was being dragged. The wait
    // was there for one reason: CreateFromRGBA8 RELEASES the resource it creates over, and the
    // previous frame may still be sampling it. Two slots remove that reason -- the bake writes the
    // one that is not being sampled -- and `safeFrame` keeps both halves honest: the pending slot
    // is not sampled until the copy has had a full frame ring to land, and the slot just retired
    // from sampling is not overwritten until it has had the same.
    Texture2D flareBokeh_[2];
    std::unique_ptr<UploadBatch> flareBokehUpload_;
    std::uint64_t flareBokehSafeFrame_ = 0;
    std::uint32_t flareBokehSlot_ = 0;    // the slot the scatter samples
    std::int32_t flareBokehPending_ = -1; // the slot being uploaded, -1 = idle
    bool flareBokehReady_ = false;
    std::uint32_t flareBokehBlades_ = 0xffffffffu;
};
