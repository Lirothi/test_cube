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
        std::uint32_t write = 0;     // the chains (and the streak pair) -> UAV
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
    // point cursor four times whatever the decisions are — a point is a POSITION in the pass's
    // program, so only its CONTENT is gated.
    void Declare(RenderGraphPassContext& ctx, Points& out) const;

    // The tonemap body, on the tonemap's list, with whatever the tone curve is about to read.
    void Record(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                D3D12_CPU_DESCRIPTOR_HANDLE hdrSource, const Points& pts);

    // Read by the tone curve: how much of the chain to add back, and the scale it is added with.
    BloomApplyConstants ApplyConstants() const;
    float TonemapBloomScale() const;

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
    void BakeFlareBokeh(Renderer* renderer, uint32_t blades);
    bool ReadKernelPixels(const wchar_t* path);
    void SurveyKernel(float ratio);

    SceneResourceBootstrapper* resources_ = nullptr;
    const SceneFrameData* frame_ = nullptr;

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

    // The kernel's spectrum is a pure function of these, so it is only rebuilt when one moves.
    // P8C-2: the shape controls died with the generated aperture; what remains is the kernel
    // IMAGE's placement (size, active grid) and the streak composited into it.
    struct BloomKernelKey
    {
        uint32_t width = 0u, height = 0u;          // the ACTIVE grid
        uint32_t imageWidth = 0u, imageHeight = 0u; // the span depends on the image's major axis
        float convSize = -1.0f;
        // P8C-6: the TINT is part of the key. The spectrum is cached and rebuilt only when the key
        // moves, so a tint left out of it would be a colour picker that does nothing until
        // something else happens to force a rebuild -- which is worse than no picker at all.
        float tint[3] = { -1.0f, -1.0f, -1.0f };
        bool operator==(const BloomKernelKey& o) const
        {
            return width == o.width && height == o.height &&
                   imageWidth == o.imageWidth && imageHeight == o.imageHeight &&
                   convSize == o.convSize &&
                   tint[0] == o.tint[0] && tint[1] == o.tint[1] && tint[2] == o.tint[2];
        }
    };
    // ONE KEY PER FRAME SLOT, not one for the renderer. The kernel spectrum lives in
    // DeferredTargets, which is per-frame -- a single key meant slot 0 built the kernel and slots 1
    // and 2 convolved against an empty texture, i.e. two frames in three had no bloom at all. That
    // is what the first convolution captures actually showed.
    std::array<BloomKernelKey, render::kFrameCount> bloomKernelKeys_{};
    // P8C-2: the photographed convolution kernel (UE's DefaultBloomKernel as an FP16 DDS with
    // mips). Loaded once, lazily, at the frame-gate site -- WITHOUT it the convolution method
    // refuses to enable (UE's own gate: IsFFTBloomEnabled is false with no kernel texture), rather
    // than falling back to a procedural kernel that no longer exists.
    Texture2D bloomKernelTex_;
    bool bloomKernelReady_ = false;
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
    // Sums from the last survey, and the `ratio` they were taken at. Energy, not colour -- the
    // apply constants are formed from these every frame because they also depend on the intensity.
    std::array<float, 3> bloomKernelCenterEnergy_{};
    std::array<float, 3> bloomKernelScatterEnergy_{};
    float bloomSurveyRatio_ = -1.0f;
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
