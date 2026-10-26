#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>
#include <wrl/client.h>

#include "rendering/core/RenderGraph.h"
#include "rendering/core/RenderPass.h"
// pass-flow S7a: Pass_ShadowCull takes ShadowGpuData::CullDecisions by reference, so the type has
// to be complete here (SceneFrameData only forward-declares the class).
#include "rendering/shadows/ShadowGpuData.h"
#include "rendering/shadows/SdsmShadows.h" // S15: the analyze pass's decision type
#include "rendering/rt/RtSceneAs.h"
#include "core/task/TaskSystem.h"
#include "app/scene/SceneFrameData.h"
#include "materials/Texture2D.h"
#include "rendering/core/UploadBatch.h"
#include "app/scene/SceneRenderQueue.h"
#include "app/scene/SceneResourceBootstrapper.h"
#include "rendering/lighting/SkyAtmosphere.h"
#include "rendering/lighting/VolumetricCloud.h"
#include "rendering/lighting/DdgiVolume.h"
#include "rendering/post/BloomRenderer.h" // R3: the bloom subsystem, a member below

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
    static constexpr size_t kTranslucentRenderGraphPassCount = static_cast<size_t>(RenderPass::Translucent_Count)
        - static_cast<size_t>(RenderPass::Translucent_Driver);

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
    bool IsRtReflectActive() const { return decisions_.rtReflect; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetTlasSrvCpu(UINT frameIndex) const { return rtAs_.TlasSrvCpu(frameIndex); }

    // Renders one frame (main graph, overlay epilogue, EndFrame).
    // `frame` must stay valid for the duration of the call.
    void Render(Renderer* renderer, const SceneFrameData& frame);

private:
    static constexpr int kCascades = SceneFrameData::kCascades;

    // R6 (docs/scene_renderer_refactor_plan.md): everything this frame DECIDES, in one place.
    //
    // These were loose members and loose locals scattered through Render(), which made them
    // indistinguishable from the state below that legitimately CROSSES frames (the SSR and VSM
    // histories, the pre-exposure pair). Nothing in here survives Render(): DecideFrame() writes
    // every field before the graph is built, and no pass body or builder may write one — the whole
    // point of the pass-flow plan was that a decision taken twice is a decision that can disagree.
    //
    // It is a MEMBER rather than a local because the pass builders read it: they capture `this` and
    // run later, from ExecuteParallel, long after the registration code that filled this in.
    struct FrameDecisions
    {
        bool rtSupported = false;      // hardware answer, not a setting
        bool rtDebugView = false;      // the RT debug visualisation is up
        bool rtReflect = false;        // reflections trace the TLAS (was rtReflectActive_)
        bool rtBuildAS = false;        // ...so the acceleration structures have to be built
        bool skyVis = false;           // plan S: the RT sky visibility is wanted (setting + RT hardware)
        // ...and its pass was registered with everything it needs: the one flag lighting and compose
        // read (skyVisParams.x). The body writes the target on every path (traced, or cleared to the
        // open sky when the TLAS is not ready), so "registered" is enough for them to read it.
        bool skyVisTraced = false;
        bool gi = false;               // plan D: the GI probe volume is wanted (setting + RT hardware)
        // ...and lighting reads it this frame: its resources exist and some generation was traced
        // (the passes may skip a frame -- the TLAS not ready -- and the atlases still hold the last one).
        bool giApplied = false;
        // ...and the FAR cascade (gi.far): wanted, and read by lighting this frame, the same two rules.
        bool giFar = false;
        bool giFarApplied = false;
        bool clearReflections = false; // source is None/SkyOnly: nothing traces, the target is cleared
        bool glassRefl = false;        // glass gets traced reflections (was glassReflActive_)
        bool reflectionTemporal = false; // the reflection temporal resolve runs (SSR or RT source)
        // P6C step 6: does anything trace the CLOSEST depth pyramid this frame? ONE flag, read by
        // the pyramid build (whether to write that chain at all) and by both SSR dispatches
        // (whether the HiZ tracer may run). Two independent evaluations of "is HiZ on" is how a
        // pass ends up tracing a chain nobody filled in.
        bool ssrHiz = false;           // was ssrHizActive_
        // Directional shadows come from the VSM clipmap, so the CSM pass is omitted entirely.
        // Render() used to compute this predicate twice under two names (`vsmDirectional` and
        // `vsmActive`) from the same three terms.
        bool vsmActive = false;
        // S15: directional shadows come from GPU-fitted partitions, so Main_CSM is omitted and the
        // three SDSM passes take its place -- AFTER the G-buffer, because the fit reduces its depth.
        bool sdsmActive = false;
        // Occlusion S5b inside SDSM: the mode runs the two-pass light-space occlusion cull this
        // frame (the knob, the pyramids, and a previous frame to test against).
        bool sdsmHzb = false;
        // S16: the mode samples EVSM4 moments instead of PCF this frame (the knob, the mode,
        // the converter PSO and the moments atlas all present).
        bool sdsmEvsm = false;
        bool vsmSkipUpdate = false;    // nothing moved: keep last frame's pages (was vsmSkipUpdate_)
        // The sun is under a closed overcast deck (DirectionalLight::GetCloudVisibility() == 0, i.e.
        // coverage 1): no surface receives it, so the SUN's shadow passes (Legacy CSM, SDSM, VSM
        // update) register empty and its three readers (lighting, fog, glass) skip the shadow lookup
        // on lightRgb == 0. Spot and point shadows are untouched; so is Main_ShadowCull, which also
        // culls the camera's indirect G-buffer.
        bool sunShadowsOff = false;
        bool willDlss = false;         // the DLSS evaluate is predicted to run (was a local)
        // Occlusion plan S5: the camera's two-pass HZB occlusion inside the indirect G-buffer --
        // Main_HzbA, Main_CamCullPost and Main_GBufferB exist, and the G-buffer's consumers chain
        // off pass B. The registry's own builder may still decline (empty passes then).
        bool camHzb = false;
        // Volumetric fog (docs/volumetric_fog_sky_clouds_ssgi_plan.md, part A): the froxel pass
        // runs and compose reads the volume; `fogHistoryValid` = the previous slot's scatter
        // volume was written last frame at this grid size under this camera history (temporal).
        bool volumetricFog = false;
        bool fogHistoryValid = false;
        // A3: the furthest HZB (this and the previous slot) exists at the mip whose texel is a cell.
        bool fogConservativeDepth = false;
        std::uint32_t fogHzbMip = 3u; // furthest-HZB mip whose texel is one cell: log2(cell px) - 1 (base = half res)
        // A4: spots / points light the volume this frame (the knob, and there are any).
        bool fogLocalLights = false;
        // Volumetric clouds (plan part C): the level has them, the sky is procedural and the ring
        // targets exist; `cloudHistoryValid` = the previous slot's resolve was written last frame
        // at this size under this camera history. Cleared again in BuildLighting if no pass registered.
        bool volumetricCloud = false;
        bool cloudHistoryValid = false;
        // The volume's lookup parameters, decided ONCE for every consumer (fog pass, compose,
        // ocean, glass, particles): (on, far view depth, 1/preExposure, slice count) and (B, O, S).
        Math::float4 fogVolumeParams{};
        Math::float4 fogVolumeZParams{};
        // Plan A7 light shafts (UE LightShaftBloom): the pass exists this frame (sun in front of the
        // camera, bloom non-zero, materials + targets ready) and where the sun is on screen --
        // TextureSpaceBlurOrigin, aspect-corrected uv (v * H/W), decided once for the five dispatches.
        bool lightShafts = false;
        Math::float2 lightShaftOrigin{};
        // ...and its temporal step: last frame's output is a usable history (the pass ran last frame
        // at this size and camera history), and this frame's weight in the blend.
        bool lightShaftHistoryValid = false;
        float lightShaftBlend = 1.0f;
        float lightShaftFade = 1.0f; // time-of-day N3: by the sun's height, 1 with the disc up, 0 by -6 degrees
        bool lightShaftFromMoon = false;   // N4: the shafts radiate from the moon (its own bloom, SkyMoonFrame)
    };
    void DecideFrame(Renderer* renderer, const SceneFrameData& frame);
    FrameDecisions decisions_{};

    // R5: what the seven phase builders (SceneRenderer_Graph.cpp) hand each other.
    //
    // Render() no longer constructs the graph itself — it opens this, calls the phases in schedule
    // order and executes. The phases are pure REGISTRATION: every AddPass2 builder runs later, from
    // ExecuteParallel, so moving a registration between phases changes nothing except the order
    // passes are added in, which is the schedule and is preserved exactly.
    //
    // The pass INDICES are the interface, so they are named fields rather than a bag of size_t —
    // and only the ones that actually cross a phase boundary are here. An index used solely inside
    // one phase stays a local there, which is what makes this list readable as the dependency
    // graph between phases.
    struct GraphBuild
    {
        RenderGraph<kMainRenderGraphPassCount>& rg;
        // The deferred targets are stable between BeginFrame and Present, so pass declarations
        // capture the frame's resources directly.
        const RenderTargetManager::DeferredTargets& D;
        const RenderTargetManager::DeferredTargets& P; // the PREVIOUS frame's set (temporal history reads)

        static constexpr size_t kNone = static_cast<size_t>(-1);

        size_t pBuildAS = kNone;   // prologue -> reflections, RT debug, glass
        // ASYNC COMPUTE: the ocean sim + particles, on the compute queue. Its ONLY graphics
        // consumer is Main_Transparent (the ocean surface samples the maps, and particles are
        // transparent objects drawn in the same pass), which is why it is carried this far instead
        // of being consumed by the next pass along. That dependency is LOAD-BEARING: the graph
        // derives a cross-queue fence from prereqs and mtDeps, NOT from resource declarations, so
        // without it the transparent pass would read maps the compute queue is still writing.
        size_t pObjectCompute = kNone; // prologue -> transparent (cross-queue)
        size_t pWetness = kNone;   // prologue -> reflection source
        size_t pShoreDepth = kNone;// prologue -> shadow cull
        size_t pShadow = kNone;    // shadows  -> lighting (mtDep)
        size_t pSpotShadow = kNone;// shadows  -> spot lights (mtDep)
        size_t pPointShadow = kNone;
        size_t pOcclusion = kNone; // S3a: gbuffer -> hzb (depth read-only before its SRV consumers)
        size_t pVisTest = kNone;   // S3b: hzb -> (nothing downstream; its readback is next frame's)
        size_t pGbuf = kNone;      // gbuffer  -> lighting, AO
        // Occlusion plan S5: pass A's pyramid, the deferred retest, pass B into the same G-buffer.
        size_t pHzbA = kNone;
        size_t pCamCullPost = kNone;
        size_t pGbufB = kNone;
        // The pass after which the G-buffer is COMPLETE: pass B when it exists, else Main_GBuffer.
        // Every consumer of the G-buffer chains off this, never off pGbuf directly.
        size_t pGbufDone = kNone;
        size_t pVsmPageRender = kNone;
        // S15 (SDSM): the depth reduction, the caster cull against the boxes it wrote, and the
        // depth pass into the CSM atlas. kNone in every other mode. `pSdsmShadow` is what the
        // sun's consumers (lighting, fog, glass) chain off, exactly as they chain off
        // pVsmPageRender in VSM mode -- in SDSM the shadow map is finished AFTER the G-buffer.
        size_t pSdsmAnalyze = kNone;
        size_t pSdsmCull = kNone;
        size_t pSdsmShadow = kNone;
        size_t pHzb = kNone;       // gbuffer  -> SSR
        size_t pGtao = kNone;      // gbuffer  -> lighting
        size_t pSkyLuts = kNone; // B1 -> compose debug (later SkyView)
        size_t pFog = kNone;       // volumetric fog -> compose (plan part A)
        size_t pCloudNoise = kNone;  // plan C1: seed-dirty noise set -> shadow / trace
        size_t pSkyMoon = kNone;     // time-of-day N1: the moon's texture, once -> the skybox
        size_t pSkyNight = kNone;    // time-of-day N2: the Milky Way's map, once -> the capture, the skybox
        size_t pCloudShadow = kNone; // plan C3: cloud shadow map -> lighting, fog
        size_t pCloud = kNone;       // plan C2: half-res trace + resolve -> compose
        size_t pRtTrace = kNone;   // gbuffer/AS -> RT resolve (gather phase of the RT split)
        size_t pSkyVis = kNone;    // plan S: gbuffer/AS -> lighting, compose (the RT sky visibility)
        size_t pGiUpdate = kNone;  // plan D: AS -> GI trace -> GI update -> lighting
        size_t pSky = kNone;       // lighting -> reflection source
        size_t pCompose = kNone;   // reflections -> transparent
        size_t pGlassReflect = kNone;
        size_t pSelectionOutline = kNone; // forward -> metering, DLSS
    };
    void BuildPrologue(Renderer* renderer, GraphBuild& gb);
    void BuildShadows(Renderer* renderer, GraphBuild& gb);
    void BuildGBufferAndAo(Renderer* renderer, GraphBuild& gb);
    void BuildLighting(Renderer* renderer, GraphBuild& gb);
    void BuildReflections(Renderer* renderer, GraphBuild& gb);
    void BuildForwardAndEditor(Renderer* renderer, GraphBuild& gb);
    void BuildPost(Renderer* renderer, GraphBuild& gb);

    // Which targets a fan-out chunk binds before it draws (each chunk records its own list).
    enum class BatchTargets : uint8_t
    {
        GBuffer,       // the opaque G-buffer MRT
        SceneVelocity, // scene + velocity (+ editor objectID): glass, particles
        Water,         // scene + velocity + the water's G-buffer layer (+ objectID): Renderer::BindWaterTargets
    };
    void RenderObjectBatch(Renderer* renderer, const std::vector<RenderableObjectBase*>& objects, size_t batchIndex,
        RenderPass pass, const Camera& camera, bool useCommandBundle, BatchTargets targets, size_t chunkSize,
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
    // Local-light plan S7, the direct read: the shadow cache pool's SRV for the receivers' pool slot (spot / point / fog /
    // glass), or `fallback` -- an atlas, an array of the same kind, never sampled then -- while there is no pool.
    D3D12_CPU_DESCRIPTOR_HANDLE LocalShadowPoolSrv(D3D12_CPU_DESCRIPTOR_HANDLE fallback) const;
    // ...and the pool itself, for the receivers' declarations (null = none).
    ID3D12Resource* LocalShadowPoolResource() const;

    // `point` is the D7 release the builder declared for Main_ObjectCompute's move to the compute
    // queue — the ocean maps handed over in a compute-legal state. Empty on an ocean-less level.
    void Pass_PrologueClear(Renderer* r, RenderGraphPassContext ctx, std::uint32_t point);
    // pass-flow S7b: the objects whose compute records something this frame, collected by the
    // Main_ObjectCompute builder as it declares for them (into graphPass_, which the body reads).
    // Fixed capacity -- an overflow is an invariant failure, never a silent drop.
    static constexpr size_t kMaxComputeObjects = 64;
    using ObjectComputeList = tc::inl_vector<RenderableObjectBase*, kMaxComputeObjects>;
    void Pass_ObjectCompute(Renderer* r, RenderGraphPassContext ctx,
                            const ObjectComputeList& objects, Profiler::ScopeNameKey gpuScope);

    void Pass_ShadowCull(Renderer* r, RenderGraphPassContext ctx,
        const ShadowGpuData::CullDecisions& dec);
    void Pass_CSM(Renderer* r, RenderGraphPassContext ctx,
        const std::array<SceneView, kCascades>& cascadeViews,
        std::uint32_t atlasPoint, bool indirect);
    // Occlusion plan S5b: the cascades' light-space two-pass HZB cull. The tile pyramids from
    // pass A's atlas (point, point + 1), the deferred casters against them, pass B into the tiles.
    void Pass_CsmHzb(Renderer* r, RenderGraphPassContext ctx, std::uint32_t point);
    void Pass_ShadowCullPost(Renderer* r, RenderGraphPassContext ctx,
        const ShadowGpuData::CullPostDecisions& dec);
    void Pass_CSMPost(Renderer* r, RenderGraphPassContext ctx,
        const std::array<SceneView, kCascades>& cascadeViews, std::uint32_t atlasPoint);
    // S15 (SDSM). Analyze reduces this frame's depth into the partition buffer + the cull planes;
    // Cull re-runs the SHARED caster cull over the four directional view slots with those planes;
    // Shadow draws them into the CSM atlas. Same builder/record contract as everything above: the
    // decision travels by value, so the declarations and the record cannot disagree.
    void Pass_SdsmAnalyze(Renderer* r, RenderGraphPassContext ctx,
        const SdsmShadows::AnalyzeDecisions& dec);
    void Pass_SdsmCull(Renderer* r, RenderGraphPassContext ctx,
        const ShadowGpuData::DirectionalCullDecisions& dec);
    // `passB` draws the post cull's survivors (the main cull's bad occlusion guesses) into the
    // same tiles, without the clear -- occlusion S5b's two-pass shape, reused verbatim.
    void Pass_SdsmShadow(Renderer* r, RenderGraphPassContext ctx, std::uint32_t atlasPoint, bool passB);
    void Pass_SdsmCullPost(Renderer* r, RenderGraphPassContext ctx,
        const ShadowGpuData::CullPostDecisions& dec);
    void Pass_SdsmMoments(Renderer* r, RenderGraphPassContext ctx,
        const SdsmShadows::MomentsDecisions& dec);
    // pass-flow S7d: `bindPoint` is the outer pass's single declared point; the INNER graph's
    // driver emits it instead of carrying a second copy of the same declaration list.
    void Pass_GBuffer(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera, const SceneView& mainView, std::uint32_t bindPoint);
    // pass-flow S3c: `pts` arrives from the AddPass2 builder's PrepareRequestPass call — the
    // point indices the declarations were made under, captured by value into the record lambda.
    void Pass_VsmPageRequest(Renderer* r, RenderGraphPassContext ctx,
        const VirtualShadowMap::PageRequestPoints& pts);
    // Plan S10d: the distant lights and their round robin, in the page-render builder before PrepareRenderPass.
    void PlanVsmDistantLights(Renderer* renderer);
    // pass-flow S3: `dec` arrives from the AddPass2 builder's PrepareRenderPass call — the same
    // values the declarations were made from, captured by value into the record lambda.
    void Pass_VsmPageRender(Renderer* r, RenderGraphPassContext ctx,
        const VirtualShadowMap::PageRenderDecisions& dec);
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
    // Occlusion plan S5: `passA` = the SAME build from pass A's depth (Main_HzbA), under its own
    // profiler scope; the final pyramid (Main_Hzb) is the one the next frame's main cull reads.
    void Pass_Hzb(Renderer* r, RenderGraphPassContext ctx, uint32_t point, bool passA = false);
    // Volumetric fog (plan part A): the froxel scatter (sun shadow + sky per cell, history) and
    // the front-to-back integration, two dispatches under three declared points.
    struct FogPoints
    {
        uint32_t scatter = 0;   // shadow source NPS, scatter volume UAV, history volume NPS
        uint32_t integrate = 0; // scatter NPS, integrated UAV
        uint32_t restore = 0;   // integrated NPS (its resting state, what compose samples)
    };
    void Pass_VolumetricFog(Renderer* r, RenderGraphPassContext ctx, const Camera& camera, const FogPoints& pts);
    // Plan part C: what one frame's cloud constants are made of (camera, sun, exposure, the sky's
    // per-frame products, the wind clock, the ring sizes); VolumetricCloud::MakeConstants is pure.
    VolumetricCloud::FrameInputs MakeCloudInputs(Renderer* r) const;
    // Plan A7 light shafts: six dispatches under six declared points (downsample + mask, the temporal
    // blend into the history, three radial blurs ping-ponging the half-res targets, the additive apply
    // into scene colour).
    struct LightShaftPoints
    {
        uint32_t downsample = 0; // scene + depth NPS, A UAV
        uint32_t temporal = 0;   // A + previous history NPS, velocity SRV, history UAV
        uint32_t blur[3] = { 0u, 0u, 0u }; // history->B, B->A, A->B
        uint32_t apply = 0;      // B NPS, scene UAV
    };
    void Pass_LightShafts(Renderer* r, RenderGraphPassContext ctx, const Camera& camera, const LightShaftPoints& pts);
    // The lighting pass's per-frame constants, filled ONCE per consumer: the lighting pass and the
    // fog scatter (its b0 is the same cbuffer, lighting_cb.hlsli) both call this, so the sun's
    // shadow is sampled from identical constants. `causticsSrv` / `vsmDir` are what the lighting
    // pass's descriptor table needs from the same decision.
    void FillLightingConstants(Renderer* renderer, const Camera& camera, LightingPassConstants& constants,
                               D3D12_CPU_DESCRIPTOR_HANDLE& causticsSrv, bool& vsmDir) const;
    // Occlusion plan S3b: the plan's boxes against the pyramid, one dispatch + readback copy.
    void Pass_VisTest(Renderer* r, RenderGraphPassContext ctx, uint32_t point);
    // Occlusion plan S5: the deferred camera candidates against pass A's pyramid, and pass B --
    // the survivors drawn into the same G-buffer, one list, `bindPoint` the builder's target
    // declarations (the same list as Main_GBuffer's).
    void Pass_CamCullPost(Renderer* r, RenderGraphPassContext ctx, const ShadowGpuData::CamCullPostDecisions& dec);
    void Pass_GBufferB(Renderer* r, RenderGraphPassContext ctx, const Camera& camera, std::uint32_t bindPoint);
    // Occlusion plan S3a: the box queries against the G-buffer depth (read-only), right after
    // Main_GBuffer, before anything reads depth as a texture.
    void Pass_OcclusionQueries(Renderer* r, RenderGraphPassContext ctx, uint32_t point);
    // pass-flow S8: the tonemap's decisions and the barrier points they were declared under.
    // Declared HERE, above the bloom helpers, because they take it: the bloom records into the
    // tonemap's command list and therefore emits the tonemap's points.
    //
    // This pass used to be THE counterexample: `ranDlss` was the return of an evaluate that could
    // only be known mid-record, so its Prepare had to declare both alternatives. The DLSS split
    // moved that evaluate into its own pass, and nothing else in here is discovered while
    // recording — bloom method, flares, FXAA readiness, the tonemap material and the backbuffer
    // are all frame state. So the union is gone and this is an ordinary builder-decided pass.
    //
    // The point layout is deliberately the SAME for both bloom methods (write / flare RT / flare
    // read / read) even though the pyramid path used to fold the flare's render-target barrier
    // into its write point. P8C-2l/m were both caused by a point moving or vanishing under a
    // gate; one shape for both methods is what makes the shared `Bloom_FlaresBuild` able to emit
    // its markers without knowing which method called it.
    struct TonemapPoints
    {
        std::uint32_t apply = 0;       // tonemap + fxaa -> UAV, plus the exposure record
        std::uint32_t source = 0;      // the tone curve's input (and the forward targets back)
        // R3: the bloom declares and emits these itself; they are still THIS pass's points.
        BloomRenderer::Points bloom_;
        std::uint32_t fxaaRead = 0;    // tonemap -> shader-readable, the FXAA input
        std::uint32_t resolveCopy = 0; // the resolve source -> COPY_SOURCE
        std::uint32_t resolveBack = 0; // ...and back to UAV (with `tonemap` when FXAA ran)
        bool ranDlss = false;   // the prediction Main_DLSS was built from
        bool bloom = false;
        bool convolution = false;
        bool flares = false;
        bool fxaa = false;
        bool resolve = false;   // the tonemap material and a backbuffer both exist
        // P3B: the metering builder built the bilateral grid this frame, so the tonemap may
        // blend it in; otherwise the host writes a pure-blur blend and the grid is never sliced.
        bool bilateral = false;
    };
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
    // Local-light plan S7: `cached` = this frame's views may copy their statics from the cache pool (their cacheMode);
    // false = every view clears and draws all its casters (the static rows too on a split frame).
    void Pass_SpotShadows(Renderer* r, RenderGraphPassContext ctx,
        const std::vector<SceneView>& views,
        size_t viewCount, std::uint32_t atlasPoint, bool indirect, bool cached);
    void Pass_PointShadows(Renderer* r, RenderGraphPassContext ctx,
        const std::vector<SceneView>& views,
        size_t viewCount, std::uint32_t atlasPoint, bool indirect, bool cached);
    // S7: the cache misses' static casters into the pool (spot views [0, spotCount), point faces [0, faceCount)).
    void Pass_LocalShadowCache(Renderer* r, RenderGraphPassContext ctx, size_t spotCount, size_t faceCount,
        std::uint32_t fillPoint, std::uint32_t readPoint);
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
    // Gather-then-shade split of the opaque RT reflection (async-compute prep). Trace = traversal
    // + full hit shading except the lit-HDR sample (inputs exist before shadows/lighting run --
    // the pass that later moves to the compute queue); Resolve = payload + lightT -> reflection.
    void Pass_RTTrace(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera, std::uint32_t point);
    // Interior lighting plan S: the RT sky visibility (rt_sky_visibility_cs.hlsl). The decision -- and
    // the cross-frame history state -- is made once in the AddPass2 builder; `point` declares the
    // inputs + the target as a UAV, `pointRestore` hands the target back shader-readable.
    struct SkyVisChain
    {
        bool historyValid = false;
        uint32_t frameIndex = 0u;
        uint32_t point = 0u;
        uint32_t pointFilter = 0u; // the accumulation -> NON_PIXEL, the filtered target -> UAV
        uint32_t pointRestore = 0u;
    };
    void Pass_SkyVis(Renderer* r, RenderGraphPassContext ctx, const Camera& camera, const SkyVisChain& chain);
    // lighting_cb.hlsli / compose_cs.hlsl `skyVisParams`: x 1 = applied this frame, y diffuse, z specular strength.
    Math::float4 SkyVisShaderParams() const;
    // Interior lighting plan D: the GI probe volume (SceneRenderer_Gi.cpp). Both passes take the frame
    // the builder placed; `point` declares the writes as UAV, `pointRestore` hands them back at rest.
    // The update blends only what the trace wrote THIS frame (giTraced_, set by the trace's body).
    struct GiChain
    {
        DdgiVolume::Frame frame[2];   // per cascade (kGiCascades)
        uint32_t cascades = 1u;       // 1 = the near one only, 2 = with the far one
        uint32_t point = 0u;
        uint32_t pointRestore = 0u;
    };
    void BuildGi(Renderer* r, GraphBuild& gb);
    void Pass_GiTrace(Renderer* r, RenderGraphPassContext ctx, const GiChain& chain);
    void Pass_GiUpdate(Renderer* r, RenderGraphPassContext ctx, const GiChain& chain);
    // lighting_cb.hlsli `ddgiBase` / `ddgiParams` (cascade 0) and `ddgiFarBase` / `ddgiFarParams` (cascade 1):
    // x spacing, 0 = not applied; y intensity; z debug view; w the cells its edge fades over.
    void FillGiShaderParams(uint32_t cascade, int32_t outBase[4], Math::float4& outParams) const;
    void Pass_RTResolve(Renderer* r, RenderGraphPassContext ctx, std::uint32_t point);
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
        std::uint32_t apply = 0;     // first-use states for the horizontal dispatch (or the pyramid's build)
        std::uint32_t pingPong = 0;  // scratch -> readable, reflection -> UAV, for the vertical
        bool blur = false;
        // The RT reflection's GLOSSY filter instead of the separable blur (reflection_glossy_cs.hlsl): the reflection's
        // mip pyramid, then a gather over each pixel's GGX lobe. `gather` hands the pyramid over readable and the
        // reflection back as the UAV it writes.
        bool lobe = false;
        std::uint32_t gather = 0;
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
        std::uint32_t pixel = 0;   // the copies (and the environment) PS-readable for the water draw
        std::uint32_t rebind = 0;  // scene/depth/velocity (+objectID) back to their draw states
        bool copyDepth = false;
        bool copyScene = false;
        // Plan S1: the water draws this frame, so its G-buffer layer (GB0, GB1, GB2, GBAux) is a
        // render target from `rebind` on. The SAME predicate (WaterDrawsThisFrame) decides
        // Main_WaterReflection's hand-back of those four to their resting state.
        bool water = false;
    };
    bool WaterDrawsThisFrame() const;
    // Ocean G-buffer plan S3/S4: Main_WaterReflection's points and the decisions they were declared
    // from. `entry` is emitted on every path (it returns the water's G-buffer layer to rest).
    struct WaterReflectionPoints
    {
        std::uint32_t entry = 0;     // layer -> NPS; origin/march depth, HZB, colour history NPS; trace target UAV
        std::uint32_t temporal = 0;  // raw trace NPS, this frame's history UAV, last frame's NPS
        std::uint32_t composite = 0; // the reflection to composite NPS, scene UAV
        bool trace = false;          // the SSR dispatch runs (water drawn + material + targets)
        bool temporalRun = false;    // ...and its temporal resolve (waterReflectionTemporal + history ready)
        bool compositeRun = false;   // ...and the composite into scene colour
    };
    void Pass_WaterReflection(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera, const WaterReflectionPoints& pts);
    void Pass_Transparent(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera, const SceneView& mainView, const TransparentPoints& pts);
    void Pass_Translucent(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera, const SceneView& mainView, std::uint32_t rebindPoint);
    void Pass_TransparentFog(Renderer* r, RenderGraphPassContext ctx,
        const Camera& camera, std::uint32_t point);
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
        bool bilateral = false;         // ...and the bilateral grid right after it (same points)
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
    void Pass_Tonemap(Renderer* r, RenderGraphPassContext ctx, const TonemapPoints& pts);
    void Pass_Overlay(Renderer* r, RenderGraphPassContext ctx, TaskSystem::TaskHandle& overlayPrepTask);

    // Barrier plan step 4: create/grow everything the pass bodies used to create lazily,
    // once per frame before the graph is built. See the definition for why.
    void EnsureFrameResources(Renderer* renderer);

    // ================================================================================
    // R7 — the state, in three blocks, sorted by LIFETIME.
    //
    // That is the only classification that has ever mattered here. A member's topic tells you
    // nothing about whether clearing it on a level switch is required, forbidden, or a bug; its
    // lifetime tells you all three. R6 removed the per-FRAME members from this list entirely (they
    // are FrameDecisions now), which is what makes the remaining three groups clean.
    // ================================================================================

    // ---- SUB-OBJECTS: own their own state, their own reset rules and their own API. ----
    // The pass materials, CBs and descriptor handles every body binds. Reset() replaces it wholesale
    // on a level switch, which is why nothing else in this class caches anything it hands out.
    SceneResourceBootstrapper resources_{};
    // R3: the bloom (rendering/post/BloomRenderer.h). It still records into the TONEMAP's list —
    // bloom must see the upscaled image and the tone curve must see the bloom — but it owns its
    // decision, its declarations and the nineteen members only it ever read. Talked to through
    // Decide / Declare / Record. Deliberately has no Reset: its kernel and sprite are assets.
    BloomRenderer bloom_;
    SkyAtmosphere skyAtmosphere_;
    VolumetricCloud volumetricCloud_; // plan part C: noise set, shadow map, the half-res trace passes
    // Plan D: the GI probe volumes -- [0] the near cascade (gi.spacing), [1] the far one around it
    // (gi.farSpacing, gi.far), the same 32x16x32 probes each.
    static constexpr uint32_t kGiCascades = 2;
    std::array<DdgiVolume, kGiCascades> ddgi_{ DdgiVolume(0), DdgiVolume(1) };
    std::atomic<bool> giTraced_{ false }; // plan D: the trace body dispatched this frame (its update blends)
    // R4: the RT acceleration structures (S5), the bindless table (S9) and the two caches that keep
    // their rebuild incremental, together with the build body that was Pass_BuildAS. The RT passes
    // reach them through rtAs_.Manager() / rtAs_.Bindless().
    RtSceneAs rtAs_;
    // The frame's render graphs, owned rather than built as locals in Render(): the main one is
    // ~16 KB (MaxPasses x Pass, each holding a std::function), which on the stack left Render at
    // C6262's 16 KB threshold with no headroom. Reset() per frame gives the same freshly-empty
    // graph without the stack cost or a per-frame allocation; the epilogue graph is here for the
    // same reason once it carries a Prepare.
    std::unique_ptr<RenderGraph<kMainRenderGraphPassCount>> mainRenderGraph_;
    std::unique_ptr<RenderGraph<kEpilogueRenderGraphPassCount>> epilogueRenderGraph_;
    // The pass inputs too big for a closure: std::function keeps 48 B of capture inline, and past that every
    // builder or body that copied them was a heap allocation each frame (memtrace, 2026-10-23). Written by a
    // builder, read by its body during the same frame's graph run, overwritten by the next frame's: no reset.
    struct GraphPassFrame
    {
        std::array<ObjectComputeList, 2> computeObjects; // [feedsShadowCull]: Main_ObjectCompute, Main_GpuInstanceCompute
        SkyViewFrameData skyDrawView{};                  // Main_Skybox
        NightSkyFrameData skyNightView{};
        TonemapPoints tonemap{};                         // Main_Tonemap
        ShadowGpuData::CullDecisions shadowCull{};       // Main_ShadowCull (~750 B)
        ShadowGpuData::DirectionalCullDecisions sdsmCull{}; // Main_SdsmCull
    };
    GraphPassFrame graphPass_;
    // RenderObjectBatch's per-call state. Its fan-out workers read it until the frame's
    // WaitForTrackedAsyncTasks, so it cannot live on the caller's stack, and a job lambda carrying it all
    // (~96 B) was a heap allocation per call -- the last per-frame churn in the trace (two a frame,
    // 2026-10-23). The job captures its slot instead. Claimed atomically (the G-buffer and transparent
    // bodies run concurrently), reset before each frame's graph runs; running out is an invariant failure.
    struct ObjectBatchJob
    {
        Renderer* renderer = nullptr;
        const Camera* camera = nullptr;
        const std::vector<RenderableObjectBase*>* objects = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS viewCB = 0;
        Renderer::TransitionLog* cmpLog = nullptr;
        Renderer::CompiledBarriers* cmpBarriers = nullptr;
        size_t chunkSize = 1;
        size_t batchIndex = 0;
        uint32_t localOrderBase = 0;
        RenderPass pass{};
        BatchTargets targets = BatchTargets::GBuffer;
        bool useBundles = false;
    };
    static constexpr uint32_t kMaxObjectBatchJobs = 16; // five call sites, each once a frame
    std::array<ObjectBatchJob, kMaxObjectBatchJobs> objectBatchJobs_{};
    std::atomic<uint32_t> objectBatchJobCount_{0};

    // ---- CROSS-FRAME STATE: this frame READS what the last one wrote. ----
    // Every member here is a history or the validity of one, and every one of them has the same
    // failure mode: read it after a resize or a level switch and the frame samples garbage. That is
    // why each history carries its own size + frame count rather than trusting a global "first
    // frame" flag — the sizes move independently (render, reflection and AO targets all differ).

    // P16.1: the pre-exposure factor for THIS frame, and the one the PREVIOUS frame's scene colour
    // was written with. Every writer of scene colour multiplies by the first and the tonemap
    // divides it out, so stored values sit near 1 instead of near the radiance; a reader of the SSR
    // history has to undo the factor that image was STORED with, which is the second.
    float preExposure_ = 1.0f;
    float prevPreExposure_ = 1.0f;
    // P2: wall-clock stamp of the previous metering dispatch, for the adaptation rate. Negative =
    // no previous frame. Kept as a stamp rather than derived from a frame counter so a stall
    // (breakpoint, level load) shows up as a large delta that the rate cap can clamp.
    double lastExposureTimeSeconds_ = -1.0;
    // SSR temporal resolve: whether the previous frame left a history worth reading, at what size.
    // (Whether the resolve RUNS is this frame's decision and lives in FrameDecisions::ssrTemporal.)
    bool ssrHistoryValid_ = false;
    // The ocean reflection's own history bookkeeping (its buffer has its own size), same rules.
    // P3B: the metering builder's decision that the bilateral grid is built this frame, read by
    // the tonemap builder later in the same serial phase (see SceneRenderer_Graph.cpp).
    bool exposureBilateralRan_ = false;
    bool oceanHistoryValid_ = false;
    uint32_t oceanHistoryFrames_ = 0u;
    uint32_t oceanHistoryWidth_ = 0u;
    uint32_t oceanHistoryHeight_ = 0u;
    uint32_t ssrHistoryFrames_ = 0u;
    uint32_t ssrHistoryWidth_ = 0u;
    uint32_t ssrHistoryHeight_ = 0u;
    // UE samples the previous temporal SceneColor at the reprojected HIT, independently of the
    // later SSR temporal resolve. Track that history even while the temporal reflection filter is
    // disabled, because Deferred.scene is produced every frame. The camera revision is part of the
    // key: an explicit cut invalidates it even when nothing resized.
    bool ssrSceneColorHistoryValid_ = false;
    uint32_t ssrSceneColorHistoryFrames_ = 0u;
    uint32_t ssrSceneColorHistoryWidth_ = 0u;
    uint32_t ssrSceneColorHistoryHeight_ = 0u;
    uint64_t ssrSceneColorCameraRevision_ = 0u;
    // P6B: the AO temporal history. `gtaoFrameCounter_` rotates the sampling directions per frame
    // (the temporal stage averages them); `gtaoHistoryFrames_` = 0 means the history texture holds
    // nothing this frame may read — the first frame after a resize, a level switch, or the stage
    // being switched on.
    uint32_t gtaoFrameCounter_ = 0u;
    uint32_t gtaoHistoryFrames_ = 0u;
    uint32_t gtaoHistoryWidth_ = 0u;
    uint32_t gtaoHistoryHeight_ = 0u;
    // Plan S: the RT sky visibility's history, the same rules (0 frames = nothing to blend in).
    uint32_t skyVisFrameCounter_ = 0u;
    uint32_t skyVisHistoryFrames_ = 0u;
    uint32_t skyVisHistoryWidth_ = 0u;
    uint32_t skyVisHistoryHeight_ = 0u;
    // VSM (Rung 2) skip-when-still: the last camera view matrix, whether the VSM has been rendered
    // since the gate turned on, and how many consecutive still frames have passed. When nothing
    // moved the pool + page table persist and the whole VSM update is skipped — DecideFrame reads
    // these three and publishes the answer as FrameDecisions::vsmSkipUpdate.
    mat4 vsmLastView_{};
    std::uint32_t vsmLastLightVersion_ = 0; // the sun's transformVersion at vsmLastView_ (time-of-day plan T0)
    // Local-light plan S10e: what the page request depends on besides the view -- the mip select (mode, biases, refDist,
    // the render resolution the pixel footprint is taken at). A change is not "still": the pages would stay the old
    // select's while the samplers read with the new one.
    std::array<float, 7> vsmLastRequestKey_{};
    bool vsmHasRendered_ = false;
    std::uint32_t vsmStillFrames_ = 0;
    // FrameDecisions::sunShadowsOff last frame (the change is logged once), and whether the VSM update
    // was put to sleep for it: the frame after that drops every page (casters moved unrecorded).
    bool sunShadowsWereOff_ = false;
    bool vsmSlept_ = false;
    // Occlusion plan S5: per deferred slot, which frame built its camera pyramid, at what render
    // size, under which camera history -- the Main_Hzb builder (serial) stamps it, DecideFrame
    // reads the PREVIOUS slot's to decide whether last frame's pyramid may be tested against.
    struct CamHzbBuilt
    {
        std::uint64_t frame = ~0ull; // never built
        UINT width = 0, height = 0;
        std::uint64_t revision = 0;
    };
    std::array<CamHzbBuilt, render::kFrameCount> camHzbBuilt_{};
    int camHzbLogged_ = -1;          // last logged (on, prevValid) state, so the session log gets the flips only
    // Volumetric fog temporal history (plan part A): like the GTAO history -- frames written in a
    // row at this grid size under this camera history; 0 means the previous slot holds nothing
    // this frame may blend with.
    unsigned fogDistantRevision_ = 0u;
    uint32_t fogHistoryFrames_ = 0u;
    UINT fogHistoryWidth_ = 0u;
    UINT fogHistoryHeight_ = 0u;
    UINT fogHistoryDepth_ = 0u;
    std::uint64_t fogHistoryRevision_ = 0u;
    int fogLogged_ = -1;             // last logged (on, historyValid) state
    // Volumetric cloud temporal history (plan part C): the same rule as the fog's.
    uint32_t cloudHistoryFrames_ = 0u;
    UINT cloudHistoryWidth_ = 0u;
    UINT cloudHistoryHeight_ = 0u;
    std::uint64_t cloudHistoryRevision_ = 0u;
    int cloudLogged_ = -1;
    int lightShaftsLogged_ = -1;     // plan A7: last logged on/off, so the log gets the flips only
    // Light shaft temporal history: the cloud's rule, plus the clock its blend weight is taken from.
    uint32_t lightShaftHistoryFrames_ = 0u;
    UINT lightShaftHistoryWidth_ = 0u;
    UINT lightShaftHistoryHeight_ = 0u;
    std::uint64_t lightShaftHistoryRevision_ = 0u;
    double lightShaftLastSeconds_ = -1.0;

    // ---- PER-LEVEL / ONE-SHOT: survives frames, cleared by Reset(). ----
    bool rtFailureLogged_ = false; // S13: one "AS alloc failed -> SSR fallback" line per scene

    // ---- VALID ONLY DURING Render(). ----
    // Pass bodies run on task threads and read this; it is null outside the call. `decisions_`
    // (above, with FrameDecisions) has exactly the same lifetime.
    const SceneFrameData* frame_ = nullptr;
};
