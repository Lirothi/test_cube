#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include <wrl/client.h>

#include "rendering/core/RenderGraph.h"
#include "rendering/core/RenderPass.h"
// pass-flow S7a: Pass_ShadowCull takes ShadowGpuData::CullDecisions by reference, so the type has
// to be complete here (SceneFrameData only forward-declares the class).
#include "rendering/shadows/ShadowGpuData.h"
#include "rendering/rt/AccelerationStructure.h"
#include "rendering/rt/BindlessTable.h"
#include "rendering/rt/ReflectionHistory.h"
#include "core/task/TaskSystem.h"
#include "app/scene/SceneFrameData.h"
#include "materials/Texture2D.h"
#include "rendering/core/UploadBatch.h"
#include "app/scene/SceneRenderQueue.h"
#include "app/scene/SceneResourceBootstrapper.h"

class Renderer;
class Camera;
class Skybox;
class RenderableObjectBase;
struct SceneView;

// Owns the frame render pipeline: render-graph construction and all concrete
// pass bodies, plus the pass materials (SceneResourceBootstrapper). World state
// (objects, lights, camera, views) stays in Scene; per-frame inputs arrive
// through SceneFrameData.
class SceneRenderer
{
public:
    static constexpr size_t kMainRenderGraphPassCount = static_cast<size_t>(RenderPass::Main_Count);
    static constexpr size_t kEpilogueRenderGraphPassCount = static_cast<size_t>(RenderPass::Epilogue_Count)
        - static_cast<size_t>(RenderPass::Epilogue_Overlay);
    static constexpr size_t kGBufferRenderGraphPassCount = static_cast<size_t>(RenderPass::GBuffer_Count)
        - static_cast<size_t>(RenderPass::GBuffer_Driver);
    static constexpr size_t kTransparentRenderGraphPassCount = static_cast<size_t>(RenderPass::Transparent_Count)
        - static_cast<size_t>(RenderPass::Transparent_Driver);

    void InitializeCommonResources(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
    void FinalizeLevelLoad(Renderer* renderer,
        const std::vector<std::unique_ptr<RenderableObjectBase>>& objects,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
        Skybox* skybox);
    void RefreshMaterialHandles(Renderer* renderer,
        const std::vector<std::unique_ptr<RenderableObjectBase>>& objects,
        Skybox* skybox);
    void Reset();

    // I2: drop the cached RT acceleration structures + bindless geometry-info so the next RT frame
    // re-registers every mesh with its CURRENT material SRVs. Needed after a material's textures are
    // rebuilt (the bindless caches albedo/MR SRVs per-mesh; a rebuild frees the old ones, leaving
    // the geom-info table dangling -> DEVICE_HUNG). MUST be called with the GPU idle. No-op-cheap
    // when RT is off (state is empty). Geometry is unchanged, but re-registering is simplest+safe.
    void InvalidateRaytracing();

    // RT reflections for glass (S15): whether RT reflections are active this frame
    // (rtSupported && source==RT && AS not failed), and the current TLAS SRV — read
    // by the transparent pass / glass renderable. {0} when no TLAS is built.
    bool IsRtReflectActive() const { return rtReflectActive_; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetTlasSrvCpu(UINT frameIndex) const { return asManager_.TlasSrvCpu(frameIndex); }

    // Renders one frame (main graph, overlay epilogue, EndFrame).
    // `frame` must stay valid for the duration of the call.
    void Render(Renderer* renderer, const SceneFrameData& frame);

private:
    static constexpr int kCascades = SceneFrameData::kCascades;

    void RenderObjectBatch(Renderer* renderer, const std::vector<RenderableObjectBase*>& objects, size_t batchIndex,
        const Camera& camera, bool useCommandBundle, bool bindGbufOrScene, bool bindVelocity, size_t chunkSize,
        D3D12_GPU_VIRTUAL_ADDRESS viewCB, uint32_t localOrderBase = 0);

    // Barrier plan step 5: register the draw-time resource states of every OPAQUE object.
    // Shared by the G-buffer and the three shadow passes: RecordShadow reads the same
    // buffers in the same states as Render, and if that ever diverges the comparator
    // reports it as MISSING rather than letting it pass silently.
    // Registers the per-object states a draw pass's body will transition, for the objects that
    // body will actually draw — hence the view list, whose visible buckets are exactly what the
    // bodies iterate. `shadowDraw` selects the shadow bodies' extra GPU-driven gate.
    // pass-flow S7c: `indirect` arrives from the pass builder rather than being recomputed here.
    // It used to be derived in FOUR places from the same inputs — this function, Pass_CSM,
    // Pass_SpotShadows and Pass_PointShadows — and it decides WHICH OBJECTS DRAW, so a
    // disagreement between the registration and the draw is a GPU-instanced caster transitioning
    // its instance buffer with no declaration behind it.
    void PrepareOpaqueDrawStates(RenderGraphPassContext& p, const SceneView* views, size_t viewCount,
                                 bool indirect);
    // The one place that decision is made. `shadowDraw` is false for the G-buffer, whose draws
    // never go through the indirect path.
    bool IndirectShadowDrawsActive() const;

    void Pass_BuildAS(Renderer* r, RenderGraphPassContext ctx);
    void Pass_PrologueClear(Renderer* r, RenderGraphPassContext ctx);
    // pass-flow S7b: the objects whose compute records something this frame, collected by the
    // Main_ObjectCompute builder as it declares for them. Fixed capacity because it rides into the
    // record lambda by value — an overflow is an invariant failure, never a silent drop.
    static constexpr size_t kMaxComputeObjects = 64;
    using ObjectComputeList = tc::inl_vector<RenderableObjectBase*, kMaxComputeObjects>;
    void Pass_ObjectCompute(Renderer* r, RenderGraphPassContext ctx,
        const ObjectComputeList& objects);

    void Pass_ShadowCull(Renderer* r, RenderGraphPassContext ctx,
        const ShadowGpuData::CullDecisions& dec);
    void Pass_CSM(Renderer* r, RenderGraphPassContext ctx,
        const std::array<SceneView, kCascades>& cascadeViews,
        std::uint32_t atlasPoint, bool indirect);
    // pass-flow S7d: `bindPoint` is the outer pass's single declared point; the INNER graph's
    // driver emits it instead of carrying a second copy of the same declaration list.
    void Pass_GBuffer(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera, const SceneView& mainView, std::uint32_t bindPoint);
    // pass-flow S3c: `pts` arrives from the AddPass2 builder's PrepareRequestPass call — the
    // point indices the declarations were made under, captured by value into the record lambda.
    void Pass_VsmPageRequest(Renderer* r, RenderGraphPassContext ctx,
        const VirtualShadowMap::PageRequestPoints& pts);
    // pass-flow S3: `dec` arrives from the AddPass2 builder's PrepareRenderPass call — the same
    // values the declarations were made from, captured by value into the record lambda.
    void Pass_VsmPageRender(Renderer* r, RenderGraphPassContext ctx,
        const VirtualShadowMap::PageRenderDecisions& dec);
    // P6B: rotates the AO sampling directions per frame; the temporal step averages them.
    uint32_t gtaoFrameCounter_ = 0u;
    // P6B item 4: how many consecutive frames the temporal stage has run at the current AO size.
    // 0 means the history texture holds nothing this frame may read — the first frame after a
    // resize, a level switch, or the stage being switched on.
    uint32_t gtaoHistoryFrames_ = 0u;
    uint32_t gtaoHistoryWidth_ = 0u;
    uint32_t gtaoHistoryHeight_ = 0u;

    // P6B: the AO chain's decision, made ONCE in the AddPass2 builder (which runs serially, before
    // any recording) and carried into the record body by value. `point*` are the barrier-point
    // indices the declarations were made under; the body emits exactly those.
    struct GtaoChain
    {
        bool denoise = false;
        bool temporal = false;
        bool historyValid = false;
        uint32_t frameIndex = 0u;
        uint32_t pointRaw = 0u;
        uint32_t pointDenoise = 0u;
        uint32_t pointTemporal = 0u;
        uint32_t pointUpsample = 0u;
        // The chain's last write leaves `gtaoUpsampled` a UAV; this point puts it back
        // shader-readable, which is its canonical. See the RenderTargetManager comment: the
        // texture inspector transitions FROM the canonical without transitioning back, so a target
        // that rests in a different barrier layout than SHADER_RESOURCE breaks the next frame.
        uint32_t pointRestore = 0u;
    };

    // P6C: the hierarchical depth pyramid. `mipCount` is decided in the builder so the record body
    // dispatches exactly the levels that were declared.
    void Pass_SsrTemporal(Renderer* r, RenderGraphPassContext ctx, std::uint32_t point);
    void Pass_Hzb(Renderer* r, RenderGraphPassContext ctx, uint32_t point);
    // P8: the bloom pyramid. Not a pass of its own -- it records into the tonemap pass's list,
    // between the DLSS evaluate and the tone curve, because both of those live in that pass.
    void Bloom_Build(Renderer* r, ID3D12GraphicsCommandList* cl,
                     D3D12_CPU_DESCRIPTOR_HANDLE hdrSource);
    // The thresholded DOWN chain on its own. Split out because BOTH methods need it: the pyramid
    // builds on it, and the convolution -- which has no pyramid of its own -- needs it as the SOFT
    // source its ghosts are gathered from. Assumes bloomDown is already UNORDERED_ACCESS.
    // `mipCount` 0 means the whole chain, which is what the pyramid needs; the convolution's ghost
    // source asks for only the levels it samples.
    // P8C-2m: whether the flares run THIS frame, decided before the graph is built so the
    // Prepare declares exactly what the body will touch. Off means off: no dispatch, no draw, and
    // no barrier -- the targets are not declared at all.
    bool flaresGhosts_ = false;
    bool flaresStreak_ = false;
    // P8C-2l: the flare constants, filled once so the two bloom methods cannot drift apart.
    BloomConvConstants Bloom_FlareConstants(Renderer* r) const;
    // P8C-2l: the lens flares, shared by BOTH bloom methods. Build runs before the bloom target
    // is written (scatter + streak pyramid), composite after it (both add into mip 0).
    void Bloom_FlaresBuild(Renderer* r, ID3D12GraphicsCommandList* cl,
                           D3D12_CPU_DESCRIPTOR_HANDLE hdrSource, const BloomConvConstants& conv);
    void Bloom_FlaresComposite(Renderer* r, ID3D12GraphicsCommandList* cl,
                               D3D12_CPU_DESCRIPTOR_HANDLE hdrSource,
                               const BloomConvConstants& conv);
    // P8C-2 step 5a: bake the ghost bokeh sprite from the blade count.
    void BakeFlareBokeh(Renderer* r, uint32_t blades);
    void Bloom_Downsample(Renderer* r, ID3D12GraphicsCommandList* cl,
                          D3D12_CPU_DESCRIPTOR_HANDLE hdrSource, float threshold, UINT mipCount);
    // P8C: the convolution alternative. Same slot, same output texture.
    // P8C-2o: the kernel survey and the constants it feeds the tonemap. Reading the pixels is
    // narrow on purpose -- it accepts exactly the format this one asset ships in.
    bool Bloom_ReadKernelPixels(const wchar_t* path);
    void Bloom_SurveyKernel(float ratio);
    BloomApplyConstants Bloom_ApplyConstants() const;
    float Bloom_TonemapBloomScale() const;
    void Bloom_Convolve(Renderer* r, ID3D12GraphicsCommandList* cl,
                        D3D12_CPU_DESCRIPTOR_HANDLE hdrSource);

    // P6C step 6: fills the HZB tracer's half of the SSR constants. ONE definition, called by the
    // opaque and the glass dispatch, so the two can never disagree about whether the furthest
    // pyramid exists this frame.
    void FillSsrHzbConstants(Renderer* r, SsrPassConstants& c) const;
    void FillSsrUeConstants(SsrPassConstants& c, bool useRoughnessTexture) const;
    // UE SSRT hit-color path: supplies the current->previous clip transform and whether the
    // previous Deferred.scene is safe to sample. Shared by opaque and glass SSR.
    void FillSsrReprojectionConstants(const Camera& camera, SsrPassConstants& c) const;

    // The texture inspector's preview, resampled through our own shader so it can be brightened.
    void Pass_DebugPreview(Renderer* r, RenderGraphPassContext ctx, uint32_t point);

    // P6B: screen-space ambient occlusion, half res, between the G-buffer and lighting.
    void Pass_Gtao(Renderer* r, RenderGraphPassContext ctx, const Camera& camera,
        const GtaoChain& chain);

    // The fullscreen debug blit's subject, resolved from SceneRenderSettings::debugTexTarget.
    // Static + shared by the pass body and its declaration list, so the state that gets declared is
    // always the state that gets sampled.
    struct DebugTexPick
    {
        ID3D12Resource* resource = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE srv{};
    };
    static DebugTexPick PickDebugTexTarget(const RenderTargetManager::DeferredTargets& D, int index);
    // pass-flow S6: the debug blit's two points. The pass exists at all only when the builder
    // decided the target is pickable, so the body carries no `on` flag any more; `restore` is
    // emitted on EVERY path, including the one where the debug material is missing.
    struct DebugBlitPoints
    {
        std::uint32_t read = 0;
        std::uint32_t restore = 0;
    };
    void Pass_Debug(Renderer* r, RenderGraphPassContext ctx, const DebugTexPick& pick,
        const DebugBlitPoints& pts);
    // pass-flow S5: `point` is the pass's single declared barrier point, decided by the AddPass2
    // builder (which also owns the readiness gate this body used to repeat) and emitted here as
    // one marker instead of ApplyDeclaredStates' list of named transitions.
    void Pass_Lighting(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera, std::uint32_t point);
    // pass-flow S6: `viewCount` is the builder's clamp (the shadowed-light count against the
    // fixed-size view array), not a second one made here — the two used to be computed
    // independently in the Prepare and in the body. `atlasPoint` is the point the atlas's
    // DEPTH_WRITE was declared under; the per-object draw states the fan-out workers transition
    // stay named (a legal mixed pass until S7c takes the walk apart).
    void Pass_SpotShadows(Renderer* r, RenderGraphPassContext ctx,
        const std::array<SceneView, LightManager::kMaxShadowedSpotLights>& views,
        size_t viewCount, std::uint32_t atlasPoint, bool indirect);
    void Pass_PointShadows(Renderer* r, RenderGraphPassContext ctx,
        const std::array<SceneView, LightManager::kMaxShadowedPointLights * 6>& views,
        size_t viewCount, std::uint32_t atlasPoint, bool indirect);
    void Pass_SpotLights(Renderer* renderer, RenderGraphPassContext ctx,
        const Camera& camera, std::uint32_t point);
    void Pass_PointLights(Renderer* renderer, RenderGraphPassContext ctx,
        const Camera& camera, std::uint32_t point);
    void Pass_Skybox(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera, std::uint32_t point);
    // pass-flow S6: Main_TerrainDepth's frame decisions and the barrier points they were declared
    // under. `drawDepth` / `buildSdf` used to be read by the Prepare AND recomputed by the body
    // from the same two OceanSimulation flags; the builder decides them once, commits
    // MarkShoreSdfBuilt itself, and the body just walks the points.
    struct ShoreDepthPoints
    {
        bool drawDepth = false;
        bool buildSdf = false;
        std::uint32_t depthWrite = 0;  // shore depth window -> DEPTH_WRITE
        std::uint32_t depthRead = 0;   // ...and back to shader-readable
        std::uint32_t sdfWrite = 0;    // SDF source -> DEPTH_WRITE
        std::uint32_t sdfRead = 0;     // source readable + the flood's two buffers -> UAV
    };
    // Shore depth window, plus (once per level) the SDF source render and its jump flood.
    void Pass_ShoreDepth(Renderer* r, RenderGraphPassContext ctx,
        const SceneView* view, const ShoreDepthPoints& pts);
    void Pass_ScreenSpaceReflections(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera, std::uint32_t point);
    void Pass_RTReflections(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera, std::uint32_t point);
    void Pass_RTDenoise(Renderer* r, RenderGraphPassContext ctx); // S11 temporal accumulate
    // S15b off-screen glass reflections: render a glass front-face normal/depth G-buffer,
    // then dispatch rt_reflections_cs over it into glassReflection (sampled by forward glass).
    void Pass_GlassReflGbuffer(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera, const SceneView& mainView, std::uint32_t point);
    void Pass_GlassReflections(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera, std::uint32_t point); // RT mode
    void Pass_GlassReflectionsSSR(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera, std::uint32_t point); // SSR mode
    void Pass_ClearReflections(Renderer* r, RenderGraphPassContext ctx,
        std::uint32_t point); // S8 None/SkyOnly: zero traced reflection
    // pass-flow S6: the blur's decision + its two points. `blur` (material + CB size) was the
    // ORIGINAL D1.1 duplication this repo documented and never removed — the Prepare evaluated it
    // to decide whether the ping-pong point exists, and the body evaluated it again to decide
    // whether to record the pair.
    struct BlurPoints
    {
        std::uint32_t apply = 0;     // first-use states for the horizontal dispatch
        std::uint32_t pingPong = 0;  // scratch -> readable, reflection -> UAV, for the vertical
        bool blur = false;
    };
    void Pass_ReflectionBlur(Renderer* r, RenderGraphPassContext ctx, const BlurPoints& pts);
    // pass-flow S6: `apply` is the first-use point (plus the shore-wetness read when the ocean has
    // one), `handBack` is the unconditional `scene -> RENDER_TARGET` the transparent pass needs —
    // unconditional because EVERY path through this body takes it, including both early-outs.
    void Pass_Compose(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera, std::uint32_t apply, std::uint32_t handBack);
    // pass-flow S5: the RT debug view's two points. `trace` is the builder's readiness decision
    // (material + bindless table + a non-empty TLAS); the restore point exists ONLY on the frames
    // that trace, because only they move `reflection` out of UNORDERED_ACCESS.
    struct RtDebugPoints
    {
        std::uint32_t apply = 0;
        std::uint32_t restore = 0;
        bool trace = false;
    };
    void Pass_RTDebug(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera, const RtDebugPoints& pts);
    // pass-flow S7d: the transparent pass's four driver points plus the decisions they were
    // declared from. The driver used to perform this sequence by name while the outer Prepare
    // re-stated it as four points — one list now, in the builder.
    struct TransparentPoints
    {
        std::uint32_t copy = 0;    // depth/scene -> COPY_SOURCE, their copies -> COPY_DEST
        std::uint32_t oceanRead = 0; // the copies -> NPS, HZB, ocean reflection -> UAV
        std::uint32_t pixel = 0;   // ...and all three PS-readable for the forward draws
        std::uint32_t rebind = 0;  // scene/depth/velocity (+objectID) back to their draw states
        bool copyDepth = false;
        bool copyScene = false;
        bool oceanReflect = false; // the reflection compute runs (targets + material + descriptors)
    };
    void RecordOceanReflection(Renderer* r, ID3D12GraphicsCommandList* cl,
        const Camera& camera, const TransparentPoints& pts);
    void Pass_Transparent(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera, const SceneView& mainView, const TransparentPoints& pts);
    void Pass_DebugDraw(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera, std::uint32_t point);
#if WITH_EDITOR
    void Pass_SelectionOutline(Renderer* r, RenderGraphPassContext ctx, std::uint32_t point);
#endif
    // pass-flow S6: the metering pass's decision + its four points, in the order the body takes
    // them. `apply` is UNCONDITIONAL — the body hands `scene` to the metering read before it looks
    // at whether the camera is dormant, and that has to keep happening on every frame.
    //
    // The three points after it used to be TWO, and the bundling was a real bug: `baseLum -> read`
    // shared a point with `exposure/histogram -> COPY_SOURCE`, and a point is emitted wholesale at
    // its first match — so the copy-source barriers fired at the END OF THE BASE-LUM DISPATCH,
    // before the solve wrote the exposure record and read the histogram. Confirmed with
    // --barrier-flip-trace: "point 1/3 (3 barriers) asked Exposure.BaseLogLum 0x40". The readback
    // copy that follows the solve was then left with no barrier of its own between the solve's
    // write and its read.
    struct ExposurePoints
    {
        std::uint32_t apply = 0;        // scene -> metering read (+ the pass's buffers to UAV)
        std::uint32_t baseLumRead = 0;  // P3B base layer back to its resting read state
        std::uint32_t copySrc = 0;      // exposure + histogram -> COPY_SOURCE, for the readback
        std::uint32_t restore = 0;      // ...and back to UAV, which is where they rest
        bool meter = false;             // enabled + metering ready + all three materials
        bool baseLum = false;           // the P3B base-luminance dispatch runs this frame
    };
    // P2: clear + build the luminance histogram and solve the adapted exposure. Runs before the
    // tonemap, which consumes the value it writes.
    void Pass_ExposureMetering(Renderer* r, RenderGraphPassContext ctx, const ExposurePoints& pts);
    // DLSS-split: the upscale's own pass. Two points — the tagged inputs/output on the way in, the
    // upscaled image on the way out — both emitted whatever Streamline returns.
    struct DlssPoints
    {
        std::uint32_t apply = 0;
        std::uint32_t output = 0;
    };
    void Pass_Dlss(Renderer* r, RenderGraphPassContext ctx, const DlssPoints& pts);
    // `ranDlss` is the prediction Main_DLSS was built from, captured by value: the two passes are
    // recorded in parallel, so the tonemap cannot be told what the evaluate returned.
    void Pass_Tonemap(Renderer* r, RenderGraphPassContext ctx, bool ranDlss);
    void Pass_Overlay(Renderer* r, RenderGraphPassContext ctx, TaskSystem::TaskHandle& overlayPrepTask);

    // Barrier plan step 4: create/grow everything the pass bodies used to create lazily,
    // once per frame before the graph is built. See the definition for why.
    void EnsureFrameResources(Renderer* renderer);

    SceneResourceBootstrapper resources_{};

    // P2: wall-clock stamp of the previous metering dispatch, for the adaptation rate. Negative =
    // no previous frame. Kept here rather than derived from a frame counter so a stall (breakpoint,
    // level load) shows up as a large delta that the cap below can clamp, per plan section 6.2.
    double lastExposureTimeSeconds_ = -1.0;

    // The frame's main render graph, owned rather than built as a local in Render():
    // it is ~16 KB (MaxPasses x Pass, each holding a std::function), which on the stack
    // left Render at C6262's 16 KB threshold with no headroom. Reset() per frame gives
    // the same freshly-empty graph without the stack cost or a per-frame allocation.
    std::unique_ptr<RenderGraph<kMainRenderGraphPassCount>> mainRenderGraph_;
    // Same reason as the main graph: once it carries a Prepare, building it as a local
    // would heap-allocate the PrepareState every frame.
    std::unique_ptr<RenderGraph<kEpilogueRenderGraphPassCount>> epilogueRenderGraph_;

    // RT acceleration structures (S5). Built by Pass_BuildAS when RT is supported
    // and enabled; no consumer yet. asManager_ owns the per-mesh BLAS cache and
    // the per-frame TLAS. asScratchRetireFrame_ defers releasing one-time BLAS
    // build scratch until its command list's frame has surely completed.
    rt::AccelerationStructureManager asManager_;
    rt::BindlessTable bindless_; // S9: per-mesh VB/IB + geometry-info for RT hit shading
    rt::ReflectionHistory reflectionHistory_; // S11: ping-pong temporal-accumulation textures
    bool asManagerInited_ = false;
    uint64_t asScratchRetireFrame_ = 0;
    bool rtFailureLogged_ = false; // S13: one-time "AS alloc failed -> SSR fallback" log
    bool asVramLogged_ = false;    // S13: one-time AS VRAM accounting log
    bool rtReflectActive_ = false; // S15: RT reflections active this frame (for glass)
    bool glassReflActive_ = false; // S15b: traced glass reflections active (RT or SSR)
    // P6C step 6: does anything trace the CLOSEST depth pyramid this frame? ONE flag, read by the
    // pyramid build (whether to write that chain at all) and by both SSR dispatches (whether the
    // HiZ tracer may run). Two independent evaluations of "is HiZ on" is how a pass ends up
    // tracing a chain nobody filled in.
    bool ssrHizActive_ = false;
    // P8: does the bloom chain run this frame? ONE flag, decided where the graph is built and read
    // by BOTH the tonemap pass's Prepare (which declares the pyramid's barrier points) and its body
    // (which emits them). Two independent evaluations is how a body ends up emitting a barrier the
    // compile never registered -- see the note on Pass_Gtao's `chain`.
    bool bloomActive_ = false;
    // P16.1: the pre-exposure factor for THIS frame. Every writer of scene colour multiplies by it
    // and the tonemap divides it out, so the stored values sit near 1 instead of near the radiance.
    // 1.0 means "not pre-exposed", which is what every path sees until it is wired up.
    float preExposure_ = 1.0f;
    // P16.1: the factor the PREVIOUS frame's scene colour was written with. The SSR history IS that
    // image, so a reader of it has to undo the factor it was STORED with, not this frame's.
    float prevPreExposure_ = 1.0f;
    // P8C: which bloom method this frame runs. Read by the tonemap Prepare AND its body, so the
    // declared resources and the emitted barriers cannot disagree.
    bool bloomConvolution_ = false;
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
    // SSR temporal resolve: whether it ran this frame (the blur's input depends on it) and whether
    // the previous frame left a history worth reading.
    bool ssrTemporalActive_ = false;
    bool ssrHistoryValid_ = false;
    uint32_t ssrHistoryFrames_ = 0u;
    uint32_t ssrHistoryWidth_ = 0u;
    uint32_t ssrHistoryHeight_ = 0u;
    // UE samples the previous temporal SceneColor at the reprojected HIT, independently of the
    // later SSR temporal resolve. Track that history even while the temporal reflection filter is
    // disabled, because Deferred.scene is produced every frame.
    bool ssrSceneColorHistoryValid_ = false;
    uint32_t ssrSceneColorHistoryFrames_ = 0u;
    uint32_t ssrSceneColorHistoryWidth_ = 0u;
    uint32_t ssrSceneColorHistoryHeight_ = 0u;
    uint64_t ssrSceneColorCameraRevision_ = 0u;
    std::vector<rt::InstanceEntry> rtInstances_; // reused scratch (only Pass_BuildAS touches it)
    struct RtBindlessObjectCache
    {
        const RenderableObjectBase* object = nullptr;
        const Mesh* mesh = nullptr;
        uint64_t materialFingerprint = 0;
        uint32_t instanceId = 0;
        bool valid = false;
    };
    // Per-object bindless registration is stable across frames even though TLAS transforms are
    // uploaded/refit every frame. Indexed like SceneFrameData::objects; pointer checks make object
    // replacement safe, while RT invalidation clears the entire cache after material hot reloads.
    std::vector<RtBindlessObjectCache> rtBindlessObjectCache_;

    // VSM (Rung 2) skip-when-still: last camera view matrix + whether the VSM has been rendered
    // since the gate turned on. When the camera view is unchanged the pool + page table persist, so
    // the whole VSM update (request/alloc/render) is skipped that frame.
    mat4 vsmLastView_{};
    bool vsmHasRendered_ = false;
    bool vsmSkipUpdate_ = false;
    std::uint32_t vsmStillFrames_ = 0; // consecutive fully-still frames (settle before skipping)

    // Valid only during Render(); pass bodies (running on task threads) read it.
    const SceneFrameData* frame_ = nullptr;
};
