#pragma once
#include <atomic>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "core/math/Math.h"
#include "rendering/descriptors/DescriptorAllocator.h"
#include "rendering/core/FrameResource.h"
#include "rendering/core/GraphicsDevice.h"
#include "rendering/core/SwapchainManager.h"
#include "rendering/core/FrameScheduler.h"
#include "rendering/core/ResourceDeclarations.h"
#include "rendering/core/ExposureMetering.h"
#include "rendering/core/RenderTargetManager.h"
#include "rendering/core/SubmitTimeline.h"
#include "rendering/core/RenderConstants.h"
#include "third_party/robin_hood.h"
#include "rendering/descriptors/SamplerManager.h"
#include "materials/Material.h"
#include "rendering/descriptors/InputLayoutManager.h"
#include "rendering/meshes/MeshManager.h"
#include "text/TextManager.h"
#include "text/FontManager.h"
#include "materials/MaterialDataManager.h"
#include "rendering/core/RenderContextPool.h"
#include "rendering/lighting/LightManager.h"
#include "rendering/debug/DebugDraw.h"
#include "ui/ImGuiLayer.h"
#include "streamline/include/sl.h"
#include "streamline/include/sl_core_types.h"
#include "streamline/include/sl_dlss.h"

using Microsoft::WRL::ComPtr;

class Camera;

class DlssHandler;

namespace render {
// Barrier plan step 6: at the end of every frame, diff each resource's tracked state against
// the canonical state it declared, and log the ones that drifted. DEFAULT OFF (`--canonical-check`)
// — it walks the whole declaration table once per frame under the tracker's lock. This is the
// measurement that says whether D2's "every frame ends at canonical" invariant is real, and
// exactly which resources need an epilogue transition before Step 7 makes it load-bearing.
inline bool g_canonicalCheck = false;

// Step 8: recompile the barriers every frame EVEN ON A CACHE HIT and diff the result against what
// the cache would have served. The only honest way to ship a cache on the barrier path — run it
// over the full --scene-stress churn and require silence. `--barrier-cache-verify`.
inline bool g_barrierCacheVerify = false;
// Step 7: per-emission trace of the compiled barriers — which point each Transition request
// matched, and `[flip-miss]` (with the CURRENT point's contents) when it matched none. Loud; for
// chasing a specific resource only. `--barrier-flip-trace`.
inline bool g_barrierFlipTrace = false;

// Async-compute plan step 1: `--compute-lane-probe` runs a ONE-SHOT check that the COMPUTE lane of
// the FrameResource pools actually works, and logs the verdict to logs/device_caps.log.
//
// It exists because that lane has never executed: `FrameResource` has pooled per-type allocators
// and lists since long before this plan, but nothing in the engine had ever asked for a COMPUTE
// one, so `CreateCommandList(COMPUTE)`, its `Reset` and the `SetDescriptorHeaps` that follows were
// all untested code paths. Proving them in isolation, at boot, on one throwaway list, is far
// cheaper than first running them concurrently with a real pass in step 8.
//
// The probe SUBMITS NOTHING — it opens a list, checks it, closes it and drops it. Default off.
inline bool g_computeLaneProbe = false;

// Async-compute plan step 2: `--async-empty-submit` submits ONE empty COMPUTE command list to the
// async queue every frame, so the two-queue frame fence is exercised by a real submission instead
// of by a queue that never receives work.
//
// It is the step's proof device, not a feature: without it the compute fence is signalled on an
// idle queue and completes instantly, so every "wait for both queues" added in step 2 would be
// vacuously true and the machinery would ship untested. Step 8 replaces it with the first real
// pass. Default off.
inline bool g_asyncEmptySubmit = false;

// Async-compute plan step 3: `--async-order-probe` makes the empty compute submission SIGNAL the
// cross-queue fence and the next frame's graphics submission WAIT on it, producing a pair whose
// order is known by construction. The two-track trace must then show every `Async.EmptySubmit`
// ending before the following `GPU.Frame` starts — which is how the shared timebase (two queues,
// two calibrations) gets checked rather than assumed.
//
// It deliberately SERIALISES the queues, so it is separate from --async-empty-submit and is never
// on by default. It is also the first exercise of step 2's dormant SignalCrossQueue/WaitCrossQueue.
inline bool g_asyncOrderProbe = false;

// Async-compute plan step 6: `--dump-submit-order` writes the frame's SUBMITTED command-list arrays
// — by debug name, in submission order, per queue — to logs/submit_order.log, once, then stops.
//
// It exists to make step 6's acceptance checkable: per-queue submission must leave the GRAPHICS
// array byte-identical to what a single-queue submit produced. Names rather than pointers, because
// pointers differ between runs and say nothing; the names are the pass identities, which is what
// "the array is unchanged" actually means.
inline bool g_dumpSubmitOrder = false;

// Async-compute plan step 8 (design D4): `--no-async-compute` forces EVERY pass back onto the
// graphics queue, whatever it was registered as.
//
// PERMANENT, exactly as `--legacy-barriers` is for the barrier model. A suspected async regression
// must be one flag away from being bisected, not a rebuild away — and on hardware or in a
// configuration where the second queue misbehaves, this is the switch that makes the renderer
// whole again without touching code.
//
// Applied at the ONE place the queue is decided (RenderGraph::AddPass2Internal), so it cannot be
// half-honoured: a pass forced to Graphics also declares, compiles and submits as Graphics.
//
// ALSO FLIPPABLE AT RUNTIME (developer window, Render tab), which is sound for three reasons and
// would be a landmine without any one of them:
//   - the graph is rebuilt from scratch every frame (SceneRenderer::Render -> RenderGraph::Reset),
//     so the read above happens per frame, on the main thread, before ExecuteParallel;
//   - the barrier compile cache carries the pass QUEUE in its key (CompileInputsUnchanged), so a
//     flip misses the slot instead of serving barriers compiled for the other queue;
//   - SubmitTimeline::BeginBatch sets the batch queue explicitly, so a pooled slot cannot keep the
//     queue it had last frame.
// The ON->OFF direction is already exercised mid-session by `--rt-force-as-fail` (step 11): both
// async passes vanish from the graph while the two-queue machinery stays live.
inline bool g_noAsyncCompute = false;

// What the switch above actually PRODUCED on the GPU last frame: command lists submitted to the
// compute queue, and cross-queue waits the graph asked for. Written by the submit loop, read by the
// developer window. A checkbox for a scheduling decision is worth nothing without the number that
// says the decision reached the queue — with every pass on Graphics both are 0 by construction.
inline std::uint32_t g_asyncComputeLists = 0;
inline std::uint32_t g_crossQueueWaits = 0;

// Poll GetDeviceRemovedReason() once per BeginFrame, so a device removal leaves a file behind no
// matter where it surfaces — a failed Present, a fence wait that never returns, a TDR between
// frames. `--no-dr-check` turns the POLL off; the report from Present's own catch block stays
// either way, because that path costs nothing until something has already gone wrong.
//
// MEASURED, so the switch does not pretend to be a perf lever: GetDeviceRemovedReason costs
// **0.207 us** (20000 calls, three Release runs: 0.2060 / 0.2069 / 0.2109 us). Once per frame
// against a ~3270 us CPU frame is 0.006 % — some fifty times under the 0.6 % run-to-run spread,
// i.e. unmeasurable. The flag exists to rule the call out, not to buy frame time.
inline bool g_deviceRemovalCheck = true;
} // namespace render

class Renderer {
public:
    struct ThreadCL {
        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12GraphicsCommandList* cl = nullptr;
        D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    };
    enum class ClearMode { None, Color, ColorDepth };
    using DeferredTargets = RenderTargetManager::DeferredTargets;

    Renderer();
    ~Renderer();
    void Shutdown();
    void ReportLiveObjects();

    // Initialize device/queue/swap/RTV/DSV plus frame resources and the fence
    void InitD3D12(HWND window, UINT width, UINT height);
    void InitFence(); // kept for compatibility; does nothing if already initialized

    // Frame cycle
    void BeginFrame();                 // waits for its frame, resets allocator and command list
    void EndFrame();                   // barrier RT->Present, Execute, Present, signal fence
    void InitImGui();
    void BeginImGuiFrame();
    void RenderImGui(ID3D12GraphicsCommandList* commandList);
    // ImGui and other external renderers replace descriptor heaps, root state,
    // viewport, and scissor. Call before recording engine-native draws afterward.
    void RestoreGraphicsStateAfterExternalDraw(ID3D12GraphicsCommandList* commandList);
    ImTextureID CreateImGuiTextureId(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc);
#if WITH_EDITOR
    // Drop the cached ImGui preview descriptors for a resource (editor thumbnail
    // cache eviction). The caller idles the GPU before freeing the resource.
    void ReleaseImGuiTextureDescriptors(ID3D12Resource* resource);
    // Register an editor-owned ImGui texture as pixel-shader-readable so
    // RenderImGui does not emit a bogus COMMON->PSR barrier. Call each frame the
    // resource is displayed, before RenderImGui.
    void MarkImGuiTextureShaderReadable(ID3D12Resource* resource);
#endif
    void ShutdownImGui();
    bool HandleImGuiWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    bool ImGuiWantsMouse() const;
    bool ImGuiWantsKeyboard() const;

    void Tick(float dt);
    bool ConsumeMaterialHotReloadFlag();

    void CreateDeferredTargets(UINT width, UINT height);
    void DestroyDeferredTargets();

    void BindGBuffer(ID3D12GraphicsCommandList* cl, ClearMode mode);
    void BindLightTarget(ID3D12GraphicsCommandList* cl, ClearMode mode, bool withDepth);
    void BindSceneColor(ID3D12GraphicsCommandList* cl, ClearMode mode, bool withDepth);
    void BindLightTargetWithVelocity(ID3D12GraphicsCommandList* cl, ClearMode mode, bool withDepth);
    void BindSceneColorWithVelocity(ID3D12GraphicsCommandList* cl, ClearMode mode, bool withDepth);
    void BindShadowTarget(ID3D12GraphicsCommandList* cl, int cascadeIndex, bool clearDepth);
    void BindSpotShadowTarget(ID3D12GraphicsCommandList* cl, UINT lightIndex, bool clearDepth);
    // Bind one cube face (cubeSlot in [0,kMaxShadowedPointLights), face in [0,6)) of the
    // point shadow atlas as the color render target, with the shared scratch depth. When
    // clear=true, clears the face to "far" (1.0) and resets the shared depth.
    void BindPointShadowTarget(ID3D12GraphicsCommandList* cl, UINT cubeSlot, UINT face, bool clear);
    // Prebuilt SRV tables (in the frame's shader-visible heap)
    D3D12_GPU_DESCRIPTOR_HANDLE StageGBufferSrvTable(); // t0..t3 : GB0,GB1,GB2,Depth
    D3D12_GPU_DESCRIPTOR_HANDLE StageComposeSrvTable(); // t0..t1 : Light,GB2
    D3D12_GPU_DESCRIPTOR_HANDLE StageTonemapSrvTable(); // t0     : Scene or DLSS output

    // Format accessors — definitions live in RenderConstants.h (render:: namespace).
    DXGI_FORMAT GetLightTargetFormat() const { return render::kLightTargetFormat; }
    DXGI_FORMAT GetSceneColorFormat() const { return render::kSceneColorFormat; }
    DXGI_FORMAT GetBackbufferFormat() const { return render::kBackbufferFormat; }
    DXGI_FORMAT GetBackbufferResourceFormat() const { return render::kBackbufferResourceFormat; }
    DXGI_FORMAT GetGBuffer0Format() const { return render::kGBuffer0Format; }
    DXGI_FORMAT GetGBuffer1Format() const { return render::kGBuffer1Format; }
    DXGI_FORMAT GetGBuffer2Format() const { return render::kGBuffer2Format; }
    DXGI_FORMAT GetGBufferAuxFormat() const { return render::kGBufferAuxFormat; }
    DXGI_FORMAT GetDeferredDepthFormat() const { return render::kDeferredDepthFormat; }
    DXGI_FORMAT GetDsvFormat() const { return render::kDeferredDepthFormat; }
    DXGI_FORMAT GetDepthSrvFormat() const { return render::kDeferredDepthSrvFormat; }
    DXGI_FORMAT GetGBufferVelocityFormat() const { return render::kGBufferVelocityFormat; }
#if WITH_EDITOR
    DXGI_FORMAT GetObjectIdFormat() const { return render::kObjectIdFormat; }
#endif
    DXGI_FORMAT GetReflectionFormat() const { return render::kReflectionFormat; }
    DXGI_FORMAT GetReflectionScratchFormat() const { return render::kReflectionScratchFormat; }

    const DeferredTargets& GetDeferredForFrame() const { return rtManager_.Deferred(currentFrameIndex_); }

    // P6B: the set the PREVIOUS frame rendered into. The frame index cycles 0..kFrameCount-1 in
    // order, so this is genuinely frame N-1 and not "some earlier frame" — which is what a temporal
    // history has to be. Only the GTAO history is read this way; everything else in a Deferred set
    // is written and consumed inside one frame.
    const DeferredTargets& GetDeferredForPrevFrame() const {
        return rtManager_.Deferred((currentFrameIndex_ + render::kFrameCount - 1u) % render::kFrameCount);
    }

    // Step 24c: full-res (Legacy) vs 1x1 (VSM) legacy spot/point shadow atlases — the shadow-mode
    // reconcile calls this at GPU idle so only the active mode's shadow memory is resident.
    void SetLocalShadowResidency(bool full) { if (GetDevice()) { rtManager_.SetLocalShadowResidency(GetDevice(), Declarations(), full); } }
    bool IsLocalShadowFull() const { return rtManager_.IsLocalShadowFull(); }

    // Step 21: transient VSM page-table + pool SRVs for the transparent (glass) pass, which lacks
    // frame/VSM access. Set by SceneRenderer::Pass_Transparent each frame; read by the glass draws.
    // Step 24b: when VSM is off/freed (Legacy mode) a null handle is passed — substitute an inert
    // DUMMY SRV so the glass draw still binds valid descriptors (glass.hlsl reads them only when
    // vsmParams.x != 0) instead of skipping. The light passes use VsmDummy*Srv() the same way.
    void SetVsmShadowSrvs(D3D12_CPU_DESCRIPTOR_HANDLE pageTable, D3D12_CPU_DESCRIPTOR_HANDLE pool)
    {
        EnsureVsmDummySrvs();
        vsmPageTableSrv_ = pageTable.ptr ? pageTable : vsmDummyBufferSrv_;
        vsmPoolSrv_      = pool.ptr      ? pool      : vsmDummyTexSrv_;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE GetVsmPageTableSrv() const { return vsmPageTableSrv_; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetVsmPoolSrv() const { return vsmPoolSrv_; }
    // Inert stand-in SRVs (null StructuredBuffer / null Texture2D) for the VSM t7/t8 slots when VSM
    // isn't resident (Legacy mode) — valid to bind, never sampled (useVsm=0). See the spot/point passes.
    D3D12_CPU_DESCRIPTOR_HANDLE VsmDummyBufferSrv() { EnsureVsmDummySrvs(); return vsmDummyBufferSrv_; }
    D3D12_CPU_DESCRIPTOR_HANDLE VsmDummyTexSrv()    { EnsureVsmDummySrvs(); return vsmDummyTexSrv_; }

#if WITH_EDITOR
    bool RequestObjectIdPick(float displayX, float displayY);
    bool HasPendingObjectIdPick() const { return objectIdPickRequested_; }
    void RecordObjectIdPickReadback(ID3D12GraphicsCommandList* cl);
    void ResolveObjectIdPickReadback();
    bool ConsumeObjectIdPick(uint32_t& outObjectId);
#endif

    // Utility functions
    void WaitForPreviousFrame();       // full synchronization (used during resize/destruction)
    void OnResize(UINT width, UINT height);

    ThreadCL BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE type, ID3D12PipelineState* initialPSO = nullptr);
    // localOrder positions this list deterministically within its batch's
    // direct namespace, assigned BEFORE dispatch (chunk/cascade/light index;
    // a lone direct in its batch uses 0). See SubmitTimeline.
    void EndThreadCommandList(ThreadCL& t, size_t batchIndex, uint32_t localOrder = 0);
    ThreadCL BeginThreadCommandBundle(ID3D12PipelineState* initialPSO = nullptr);
    // localOrder positions this bundle within its batch's bundle namespace.
    void EndThreadCommandBundle(ThreadCL& b, size_t batchIndex, uint32_t localOrder = 0);

    void BeginSubmitTimeline();
    size_t BeginSubmitBatch(RenderQueue queue = RenderQueue::Graphics);
    // Step 6: record a cross-queue dependency for a batch (see SubmitTimeline).
    void SetSubmitBatchCrossQueueWait(size_t batchIndex, size_t waitForBatch);
    void ExecuteTimelineAndPresent();
    void RecordBindAndClear(ID3D12GraphicsCommandList* cl);
    void RecordBindDefaultsNoClear(ID3D12GraphicsCommandList* cl);
    void RegisterPassDriver(ID3D12GraphicsCommandList* cl, size_t batchIndex);

    // Getters
    ID3D12Device* GetDevice() const { return graphicsDevice_.Device(); }
    ID3D12CommandQueue* GetCommandQueue() const { return graphicsDevice_.Queue(); }
    // Async-compute step 1: the second queue, created and idle. Null when the device refused it,
    // which every later step must treat as "async compute is unavailable", not as an error.
    ID3D12CommandQueue* GetComputeQueue() const { return graphicsDevice_.ComputeQueue(); }

    // Async-compute: apply a pending change to `render::g_noAsyncCompute`, draining both queues so
    // the switch never takes effect while frames under the old queue topology are in flight. Call
    // immediately before building the frame's graph — that is where the new value first matters.
    void SyncAsyncQueueMode();
    // Write logs/device_removed.log if the device is gone. Once per process; cheap enough per frame.
    void ReportDeviceRemovalOnce();
    // Append the D3D12 debug layer's queued messages to logs/invariant_failure.log. No-op without
    // the debug layer; with it, this is what makes the layer's diagnosis readable headless.
    void DumpDebugLayerMessages(const char* context);
    bool HasComputeQueue() const { return graphicsDevice_.ComputeQueue() != nullptr; }

    // DXR (S1). On non-RT hardware GetDevice5() is null and
    // IsRaytracingSupported() is false; all ray-tracing paths gate on the latter.
    bool IsRaytracingSupported() const { return graphicsDevice_.IsRaytracingSupported(); }
    ID3D12Device5* GetDevice5() const { return graphicsDevice_.Device5(); }
    // Step 12: the EFFECTIVE enhanced switch (supported AND opted in).
    bool UseEnhancedBarriers() const { return graphicsDevice_.UseEnhancedBarriers(); }
    // QI a recorded command list to CommandList4 (cheap). The returned pointer
    // borrows the passed list's lifetime — use it within the same scope, do not
    // store it. Null if the list is null or the interface is unavailable.
    ID3D12GraphicsCommandList4* AsCmdList4(ID3D12GraphicsCommandList* cl) const;
    // Step 9: enhanced-barrier view of a command list; null on older runtimes.
    ID3D12GraphicsCommandList7* AsCmdList7(ID3D12GraphicsCommandList* cl) const;
    HWND GetHWND() const { return hWnd_; }
    UINT GetWidth() const { return width_; }
    UINT GetHeight() const { return height_; }
    UINT GetRenderWidth() const { return renderWidth_; }
    UINT GetRenderHeight() const { return renderHeight_; }
    float GetRenderResolutionScale() const { return renderResolutionScale_; }
    Math::float2 GetCameraJitter() const;


    // NVIDIA-recommended texture mip bias when rendering below display resolution for DLSS:
    // log2(renderWidth / displayWidth), negative — the upscaler reconstructs display-res detail,
    // so sampling one mip sharper keeps textures from going soft. Quantized to 0.25 steps
    // (sampler-cache friendly; the render scale changes with DLSS mode) and clamped to [-2, 0].
    float GetDlssMipBias() const
    {
        const float rw = static_cast<float>(renderWidth_ ? renderWidth_ : 1u);
        const float dw = static_cast<float>(width_ ? width_ : 1u);
        const float bias = std::log2(rw / dw);
        const float q = std::floor(bias * 4.0f + 0.5f) / 4.0f;
        return std::min(0.0f, std::max(-2.0f, q));
    }

    ID3D12Resource* GetCurrentBackbuffer() const { return currentFrameIndex_ < render::kFrameCount ? swapchain_.Backbuffer(currentFrameIndex_) : nullptr; }
    // The backbuffer that was most recently Present()ed (the one on screen) — for screenshots.
    ID3D12Resource* GetLastPresentedBackbuffer() const { return lastPresentedIndex_ < render::kFrameCount ? swapchain_.Backbuffer(lastPresentedIndex_) : nullptr; }

    // Access the global descriptor allocator and current frame
    DescriptorAllocator& GetDescAlloc() { return frameScheduler_.GetFrameResource(currentFrameIndex_)->GetDescAlloc(); }
    DescriptorAllocator& GetSamplerAlloc() { return frameScheduler_.GetFrameResource(currentFrameIndex_)->GetSamplerAlloc(); }
    FrameResource* GetFrameResource() { return frameScheduler_.GetFrameResource(currentFrameIndex_); }

    UINT GetCurrentFrameIndex() const { return currentFrameIndex_; }
    uint64_t GetTotalFrameNumber() const { return totalFrameNumber_; }

    void InitTextSystem(ID3D12GraphicsCommandList* uploadCl, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive, const std::wstring& folder);

    SamplerManager* GetSamplerManager() { return &samplerManager_; }
    MaterialManager* GetMaterialManager() { return &materialManager_; }
    InputLayoutManager* GetInputLayoutManager() { return &inputLayoutManager_; }
    MeshManager* GetMeshManager() { return &meshManager_; }
    TextManager* GetTextManager() { return &textManager_; }
    FontManager* GetFontManager() { return &fontManager_; }
    MaterialDataManager* GetMaterialDataManager() { return &materialDataManager_; }
    RenderContextPool* GetRenderContextPool() { return &ctxPool_; }
    DebugDrawSystem* GetDebugDrawSystem() { return &debugDrawSystem_; }
    const DebugDrawSystem* GetDebugDrawSystem() const { return &debugDrawSystem_; }

	float GetFPS() const { return fps_; }
    void SetWireframeMode(bool w) { wireframeMode_ = w; }
    bool GetWireframeMode() const { return wireframeMode_; }

    // --- Barrier plan, step 6: canonical (resting) states ---
    //
    // D2: every graph resource declares the state it RESTS in, once, at creation — which is
    // exactly what these ~45 call sites already pass (R2: SetResourceState was never tracking,
    // it was always a creation-time seed). The invariant Step 7 needs is that every frame
    // begins AND ends with each resource at its canonical state, so the compiled barriers can
    // seed from this static table instead of a live cross-frame map. Written at create/destroy
    // only, read-only during a frame — it is the tracker's `knownStates_` that dies, not this.
    //
    // So `SetResourceState` IS the declaration, and it must carry a RESTING state: a mid-frame
    // poke would redefine canonical every frame and make the frame-end check vacuous.
    void SetResourceState(ID3D12Resource* res, D3D12_RESOURCE_STATES state);
    // Step 6b: creation state and resting state were separate facts while the tracker needed
    // seeding with where the resource actually IS. Step 7 aligned them (resources are created
    // directly in their resting state) and deleted the tracker; the overload survives for the
    // upload-initialised resources whose sites still state both.
    void SetResourceState(ID3D12Resource* res, D3D12_RESOURCE_STATES creationState,
                          D3D12_RESOURCE_STATES canonicalState);
    void ClearResourceState(ID3D12Resource* res);
    D3D12_RESOURCE_STATES GetCanonicalState(ID3D12Resource* res) const; // COMMON if undeclared
    // Step 7: exclude a resource from the barrier compile — its state is written by code
    // outside the render graph, so tracking it is not the graph's business.
    void SetResourceUnmanaged(ID3D12Resource* res) { canonicalStates_.SetUnmanaged(res); }
    bool IsResourceUnmanaged(ID3D12Resource* res) const { return canonicalStates_.IsUnmanaged(res); }
    // The compile and Transition MUST agree on which resources are modelled: compiling a barrier
    // that Transition will not emit advances the running state past a barrier nobody emits, and
    // the next user of that resource gets a wrong before-state.
    bool IsResourceCompileManaged(ID3D12Resource* res) const { return canonicalStates_.IsCompileManaged(res); }
    // Step 10: buffer vs texture, recorded at declaration (enhanced barriers need the split).
    bool IsResourceBuffer(ID3D12Resource* res) const { return canonicalStates_.IsBuffer(res); }
    // Where the last barrier compile left this resource; the seed for the next one.
    D3D12_RESOURCE_STATES GetPredictedState(ID3D12Resource* res) const { return canonicalStates_.GetPredicted(res); }
    // Step 7: which QUEUE left it there. State alone no longer decides whether the next consumer
    // may use it — a direct-queue-legal state can be illegal on the compute queue.
    RenderQueue GetPredictedOwner(ID3D12Resource* res) const { return canonicalStates_.GetPredictedOwner(res); }
    // Bumped by every declare/forget/unmanaged/clear — the barrier compile's cache key.
    std::uint64_t DeclarationsGeneration() const { return canonicalStates_.Generation(); }
    void SetPredictedState(ID3D12Resource* res, D3D12_RESOURCE_STATES s,
                           RenderQueue owner = RenderQueue::Graphics) { canonicalStates_.SetPredicted(res, s, owner); }
    // For subsystems that create resources without a Renderer& (RenderTargetManager).
    ResourceDeclarations Declarations() { return ResourceDeclarations{ &canonicalStates_, canonicalStates_.Liveness() }; }

    // P1: persistent eye-adaptation state. Dormant -- created and lifecycle-managed, never
    // dispatched, until P2. See ExposureMetering.h.
    ExposureMetering& Exposure() { return exposureMetering_; }
    const ExposureMetering& Exposure() const { return exposureMetering_; }
    // Step 6 diagnostic: log every declared resource that did not END the frame at canonical.
    // Logging, not enforcing — each hit is either a mis-declaration or a resource that genuinely
    // needs an epilogue transition, and this is the step that finds them all.
    void ReportOffCanonicalStates();

    void Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES after);
    // Step 7: a transition whose BEFORE state the caller knows by construction. This is what
    // out-of-graph paths need instead of a tracker: an upload just created the resource in
    // COPY_DEST, the editor pick readback runs after the frame with objectID still a render
    // target. Emitting directly is both correct and the reason the tracker can go away —
    // guessing the before state was its only job.
    static void TransitionExplicit(ID3D12GraphicsCommandList* cl, ID3D12Resource* res,
                                   D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

    // --- Barrier plan, step 3: transition observation (diagnostic) ---
    //
    // While a log is installed on this thread, Transition() also appends what it was
    // asked to do. The render graph installs one around a converted pass's body so it
    // can diff what the pass DID against what its Prepare REGISTERED — the check that
    // makes the conversion in step 5 safe (a pass may declare X and transition to Y,
    // and nothing else would notice).
    //
    // Thread-local because pass bodies record on worker threads. A pass that fans work
    // out to OTHER threads is not observed there — those transitions are invisible to
    // this, which is why the comparator reports fan-out passes as unverified rather
    // than as clean.
    struct ObservedTransition {
        ID3D12Resource*       resource = nullptr;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    };
    struct TransitionLog {
        ObservedTransition*         entries = nullptr;
        std::atomic<std::uint32_t>* count = nullptr;  // atomic: fan-out workers append concurrently
        std::uint32_t               capacity = 0;
        std::atomic<bool>           overflowed{ false };
    };
    static void SetThreadTransitionLog(TransitionLog* log);
    static TransitionLog* CurrentThreadTransitionLog();

    // Step 7: the compiled barriers belonging to the pass currently recording on this thread.
    //
    // `Transition` claims the entry matching (resource, stateAfter) and emits it. Claiming is by
    // MATCH rather than by a cursor because fan-out passes record from several threads at once and
    // their order is genuinely nondeterministic — a cursor would be a race by construction. Each
    // entry is claimed at most once (`claimed` is atomic), so a body that transitions the same
    // resource to the same state twice gets the second one for free, which is exactly the
    // redundant-transition case the tracker used to swallow.
    struct CompiledBarriers {
        // Grouped by POINT, because a point is the unit that must be emitted atomically: the
        // compile's running state advances past a whole point, so emitting only the entries a
        // body happens to name leaves the model and reality disagreeing — which surfaces as
        // "before state does not match" (D3D12 message 527), not as anything subtle. The first
        // Transition naming ANY entry of a point emits the WHOLE point.
        struct Point {
            const D3D12_RESOURCE_BARRIER* entries = nullptr;
            std::uint32_t                 count = 0;
            std::atomic<bool>             emitted{ false };
        };
        Point*                     points = nullptr;
        std::uint32_t              pointCount = 0;
        std::atomic<std::uint32_t> unmatched{ 0 };  // body wanted something not compiled
        int                        pass = -1;       // RenderPass enum, --barrier-flip-trace only
        // Pass-flow S1: the body used EmitPoint markers. The comparator then skips the benign
        // "INFO extra" direction and the SKIPPED heuristic for this pass — a marker body cannot
        // diverge from the compile by construction and does not feed the observation log.
        std::atomic<bool>          markerUsed{ false };
    };
    static void SetThreadCompiledBarriers(CompiledBarriers* cb);
    static CompiledBarriers* CurrentThreadCompiledBarriers();

    // Pass-flow S1 (docs/render_graph_pass_flow_plan.md): emit the compiled point with the given
    // DECLARATION index wholesale, without naming any resource or state — the marker realizes
    // the dormant ctx.Barrier(cl, point) design from A.1s. The index is absolute (the value of
    // *ctx.usePoint when the Prepare declared the point's uses), so a sub-block inside a shared
    // pass (several objects declare into one pass) never depends on other blocks' tails. Earlier
    // unemitted EMPTY points are swept emitted on the way (the compile ate their barriers);
    // an earlier unemitted NON-empty point, a marker past the end, a double-emitted point, or no
    // compiled barriers installed on this thread are invariant failures — each one is a
    // conversion bug that would otherwise lose or reorder barriers silently.
    void EmitPoint(ID3D12GraphicsCommandList* cl, std::uint32_t point);

    // Barrier-diagnostics sink for --barrier-cmp / --barrier-flip-trace: barrier_diag.log plus the
    // debugger. A file because the --scene-stress harness runs with no debugger attached.
    static void DiagLog(const char* line);
    // Same sink, dropping a line identical to one already written. These diagnostics report STATE
    // and are evaluated every frame, so without this one standing condition buries the log (3779
    // lines, 7 distinct, in an 8-second run). Keyed on the formatted text, so any moving number
    // still gets through.
    static void DiagLogOnce(const char* line);

    // Carries both the log and the compiled barriers onto a fan-out worker (see TransitionLogScope).
    struct CompiledBarrierScope {
        explicit CompiledBarrierScope(CompiledBarriers* cb) : prev_(CurrentThreadCompiledBarriers())
        {
            if (cb) { SetThreadCompiledBarriers(cb); }
        }
        ~CompiledBarrierScope() { SetThreadCompiledBarriers(prev_); }
        CompiledBarrierScope(const CompiledBarrierScope&) = delete;
        CompiledBarrierScope& operator=(const CompiledBarrierScope&) = delete;
    private:
        CompiledBarriers* prev_ = nullptr;
    };

    // Carries the dispatching thread's log onto a fan-out worker for the duration of a job.
    // Without this a pass that spreads its recording over several threads is invisible to the
    // comparator, and "no complaints" would mean "not observed" rather than "correct".
    //
    // Usage at a dispatch site — capture on the PASS thread, install inside the job:
    //   auto* log = Renderer::CurrentThreadTransitionLog();
    //   auto job = [log, ...](std::size_t i) { Renderer::TransitionLogScope s(log); ... };
    struct TransitionLogScope {
        explicit TransitionLogScope(TransitionLog* log) { SetThreadTransitionLog(log); }
        ~TransitionLogScope() { SetThreadTransitionLog(nullptr); }
        TransitionLogScope(const TransitionLogScope&) = delete;
        TransitionLogScope& operator=(const TransitionLogScope&) = delete;
    };
    void UAVBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* res);

    // Rung 0 GPU-driven shadows (Step 3): a shared, lazily-created command signature for a
    // single DRAW_INDEXED indirect argument with no per-draw root arguments (instance data
    // comes from a bound SRV indexed by SV_InstanceID + the arg's StartInstanceLocation).
    // Independent of any root signature (pRootSignature = nullptr at creation). Null on
    // failure. Owned by the Renderer for the lifetime of the device.
    ID3D12CommandSignature* GetDrawIndexedCommandSignature();
    // Thin ExecuteIndirect wrapper. countBuffer may be null (then all maxCommandCount commands
    // execute). No-op if cl/sig/argBuffer are null.
    void ExecuteIndirect(ID3D12GraphicsCommandList* cl, ID3D12CommandSignature* sig,
                         UINT maxCommandCount, ID3D12Resource* argBuffer, UINT64 argOffset,
                         ID3D12Resource* countBuffer = nullptr, UINT64 countOffset = 0);

    void SetReflectionTextureScale(Math::float2 scale);
    void SetReflectionTextureScale(float scale) { SetReflectionTextureScale(Math::float2(scale, scale)); }
    Math::float2 GetReflectionTextureScale() const { return reflectionTextureScale_; }
    UINT GetReflectionTextureWidth() const;
    UINT GetReflectionTextureHeight() const;
    void SetOceanReflectionTextureScale(Math::float2 scale);
    void SetOceanReflectionTextureScale(float scale) { SetOceanReflectionTextureScale(Math::float2(scale, scale)); }
    Math::float2 GetOceanReflectionTextureScale() const { return oceanReflectionTextureScale_; }
    UINT GetOceanReflectionTextureWidth() const;
    UINT GetOceanReflectionTextureHeight() const;
    void SetRenderResolutionScale(float scale);
    void UpdateDlssCameraData(const Camera& camera);
    bool EvaluateDLSS(ID3D12GraphicsCommandList* cl);
    bool IsDlssActive() const;
    // DLSS-split: the Main_DLSS builder's prediction — see DlssHandler::WillEvaluate.
    bool WillEvaluateDlss() const;
    // Debug: freeze the sub-pixel jitter so render-resolution targets stop shimmering in the
    // texture inspector. See DlssHandler::SetJitterPaused for what it costs.
    // The texture inspector's preview request, left during UI building and consumed by the
    // preview pass later in the SAME frame (the developer window is drawn before Scene::Render).
    // `resource` null = nothing to do. See shaders/debug_preview_cs.hlsl for why this exists.
    struct DebugPreviewRequest
    {
        ID3D12Resource* resource = nullptr;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        float gain = 1.0f;
        bool stretch = false;
        bool showAlpha = false;
    };
    void RequestDebugPreview(const DebugPreviewRequest& req) { debugPreviewRequest_ = req; }
    const DebugPreviewRequest& DebugPreviewRequestRef() const { return debugPreviewRequest_; }
    void ClearDebugPreviewRequest() { debugPreviewRequest_ = {}; }
    // Builds the requested view into the per-frame scratch slot and hands back its handle.
    D3D12_CPU_DESCRIPTOR_HANDLE MakeDebugPreviewSourceSrv(
        ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc);

    void SetJitterPaused(bool paused);
    bool IsJitterPaused() const;
    bool IsDlssAvailable() const;
    void SetDlssActive(bool active);
    void SetDlssMode(sl::DLSSMode mode);
    sl::DLSSMode GetDlssMode() const { return dlssMode_; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetTonemapSourceSrvCPU() const;
    void BindDescriptorHeaps(ID3D12GraphicsCommandList* cl) const;

    template<class Alloc, class It>
    inline GpuDescHandle StageDescriptorTableRange(
        Alloc& alloc,
        D3D12_DESCRIPTOR_HEAP_TYPE heapType,
        It first, It last)
    {
        const UINT count = static_cast<UINT>(std::distance(first, last));
        if (count == 0) {
            return {};
        }

        GpuDescHandle block = alloc.Alloc(count);

        // ONE DRIVER CALL, NOT ONE PER DESCRIPTOR. This used to loop CopyDescriptorsSimple(1, ...)
        // over the range, so a compute dispatch with three SRVs and three UAVs cost six calls into
        // the driver before it could even bind -- and the convolution issues ~18 dispatches a
        // frame. CopyDescriptors takes the whole set at once; passing null for the source range
        // SIZES is the documented way of saying "every source range is one descriptor", which is
        // exactly what these are. Both callers hand in contiguous storage (an initializer_list or
        // a std::array), so the sources can be pointed at directly rather than gathered.
        UINT dstRangeSize = count;
        const D3D12_CPU_DESCRIPTOR_HANDLE* srcStarts = &(*first);
        graphicsDevice_.Device()->CopyDescriptors(
            1, &block.cpu, &dstRangeSize,
            count, srcStarts, nullptr,
            heapType);
        return block;
    }

    inline GpuDescHandle StageSrvUavTable(std::initializer_list<D3D12_CPU_DESCRIPTOR_HANDLE> src)
    {
        return StageDescriptorTableRange(GetDescAlloc(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, src.begin(), src.end());
    }

    template<size_t N>
    inline GpuDescHandle StageSrvUavTable(const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, N>& src)
    {
        return StageSrvUavTable(src, N);
    }

    template<size_t N>
    inline GpuDescHandle StageSrvUavTable(const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, N>& src, size_t count)
    {
        const size_t clamped = std::min(count, src.size());
        const auto first = src.begin();
        const auto last = first + static_cast<std::ptrdiff_t>(clamped);
        return StageDescriptorTableRange(GetDescAlloc(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, first, last);
    }

    inline GpuDescHandle StageSamplerTable(std::initializer_list<D3D12_CPU_DESCRIPTOR_HANDLE> src)
    {
        return StageDescriptorTableRange(GetSamplerAlloc(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, src.begin(), src.end());
    }

    template<size_t N>
    inline GpuDescHandle StageSamplerTable(const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, N>& src)
    {
        return StageSamplerTable(src, N);
    }

    template<size_t N>
    inline GpuDescHandle StageSamplerTable(const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, N>& src, size_t count)
    {
        const size_t clamped = std::min(count, src.size());
        const auto first = src.begin();
        const auto last = first + static_cast<std::ptrdiff_t>(clamped);
        return StageDescriptorTableRange(GetSamplerAlloc(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, first, last);
    }

private:
    // Texture-inspector preview request; see RequestDebugPreview.
    DebugPreviewRequest debugPreviewRequest_{};
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void CreateSwapChainAndRTVs(UINT width, UINT height);
    void CreateDepthResources(UINT width, UINT height);
    void WaitForFrame(UINT frameIndex);   // wait for a specific frame (by that frame's fence value)
    void SignalFrame(UINT frameIndex);    // signal the fence for a frame
    void RefreshCurrentFrameCaches();
    // Async-compute step 1: the one-shot COMPUTE-lane probe (see render::g_computeLaneProbe).
    // Runs at the end of the first BeginFrame that has frame resources, then never again.
    void ProbeComputeLaneOnce();
    // Async-compute step 2: one empty COMPUTE submission per frame (see render::g_asyncEmptySubmit).
    void SubmitEmptyComputeWork();
    // Async-compute step 5/6: the emission-side queue-legality backstop, called from BOTH places
    // that emit a compiled barrier point (Transition and EmitPoint).
    void CheckCompiledPointOnQueue(ID3D12GraphicsCommandList* cl, const CompiledBarriers::Point& pt, int pass) const;
    // Async-compute step 6: dump one queue's submitted array by debug name (--dump-submit-order).
    static void DumpSubmitOrder(const char* queueName, const std::vector<ID3D12CommandList*>& lists);
    std::pair<UINT, UINT> ComputeScaledTextureSize(UINT referenceWidth, UINT referenceHeight, Math::float2 scale) const;
    std::pair<UINT, UINT> ComputeReflectionTextureSize(UINT referenceWidth, UINT referenceHeight) const;
#if WITH_EDITOR
    void ResetObjectIdPickState();
#endif
    void RecreateDeferredTargets();
    void UpdateRenderResolutionFromScale();


private:
    SubmitTimeline submitTimeline_;
    // Step 6: the frame's submissions, one per contiguous same-queue run of batches.
    std::vector<SubmitTimeline::Submission> submitSegmentsScratch_;
    // Step 8: (producer batch -> cross-queue fence value) for this frame, in submission order.
    std::vector<std::pair<size_t, UINT64>> crossQueueSignals_;

    // Per-frame deferred render targets + their CPU descriptor heaps
    RenderTargetManager rtManager_;

    // OS / dimensions
    HWND  hWnd_ = nullptr;
    UINT  width_ = 1600;
    UINT  height_ = 900;
    float renderResolutionScale_ = 1.0f;
    UINT  renderWidth_ = width_;
    UINT  renderHeight_ = height_;

#if WITH_EDITOR
    Microsoft::WRL::ComPtr<ID3D12Resource> objectIdReadback_;
    UINT objectIdPickX_ = 0;
    UINT objectIdPickY_ = 0;
    uint32_t objectIdPickResult_ = 0;
    bool objectIdPickRequested_ = false;
    bool objectIdPickInFlight_ = false;
    bool objectIdPickResultValid_ = false;
#endif

    Math::float2 reflectionTextureScale_ = Math::float2(0.5f, 0.5f);
    UINT reflectionTextureWidth_ = 1;
    UINT reflectionTextureHeight_ = 1;
    Math::float2 oceanReflectionTextureScale_ = Math::float2(0.5f, 0.5f);
    UINT oceanReflectionTextureWidth_ = 1;
    UINT oceanReflectionTextureHeight_ = 1;

    bool wireframeMode_ = false;
    std::vector<ID3D12Resource*> pendingImGuiTextureResources_;

    float fps_ = 0.0f;
    float fpsAlpha_ = 0.99f; // exponential smoothing: 0..1 (higher is smoother)

    uint64_t totalFrameNumber_ = 0;
    bool     shaderHotReloadEnabled_ = true;
    float    shaderWatchIntervalSec_ = 1.0f; // once per second
    float    shaderWatchAccumSec_ = 0.0f;
    bool     materialsHotReloaded_ = false;

    // D3D12 core (device/queue), presentation surface, frame pacing
    bool computeLaneProbed_ = false;  // async-compute step 1: the probe runs at most once
    UINT64 asyncOrderProbeValue_ = 0; // step 3: last frame's cross-queue signal (--async-order-probe)

    GraphicsDevice                    graphicsDevice_;
    SwapchainManager                  swapchain_;
    FrameScheduler                    frameScheduler_;

    UINT                              currentFrameIndex_ = 0;                   // 0..render::kFrameCount-1
    UINT                              lastPresentedIndex_ = 0;                  // backbuffer last shown (screenshots)
    FrameResource*                    currentFrameResource_ = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE       vsmPageTableSrv_{};                       // Step 21: glass VSM sampling
    D3D12_CPU_DESCRIPTOR_HANDLE       vsmPoolSrv_{};
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> vsmDummyHeap_;                 // Step 24b: inert VSM stand-in SRVs
    D3D12_CPU_DESCRIPTOR_HANDLE       vsmDummyBufferSrv_{};                     // null StructuredBuffer<uint> (t7/t9)
    D3D12_CPU_DESCRIPTOR_HANDLE       vsmDummyTexSrv_{};                        // null Texture2D (t8/t10)
    void EnsureVsmDummySrvs();                                                  // lazily create the two null SRVs
    static constexpr UINT             kFrameShaderVisibleHeapCount_ = 2;
    std::array<ID3D12DescriptorHeap*, kFrameShaderVisibleHeapCount_> currentFrameDescriptorHeaps_{};
    UINT                              currentFrameDescriptorHeapCount_ = 0;

    // Step 6/7: resource -> {canonical resting state, where the last compile left it}. This is ALL
    // the cross-frame resource-state the engine keeps now — one value per resource, written once
    // per frame by the single-threaded barrier compile. It replaced ResourceStateTracker's global
    // map, its per-command-list first-use/current maps, its per-thread lanes and its submit-time
    // barrier stitching.
    CanonicalStateRegistry canonicalStates_;
    // Reused by the frame-end check so the diagnostic does not allocate per frame.
    std::vector<std::pair<ID3D12Resource*, CanonicalStateRegistry::Entry>> canonicalScratch_;
    // Per-name high-water mark of live entries: a leak is growth PAST it, not a steady count.
    std::vector<std::pair<std::string, int>> canonicalNetScratch_;
    // MUST stay declared after canonicalStates_: ~ExposureMetering undeclares its buffers, and
    // members are destroyed in reverse declaration order, so this has to go first. Shutdown()
    // releases it explicitly anyway; this ordering only covers the path where Shutdown never ran.
    ExposureMetering exposureMetering_;
    robin_hood::unordered_map<std::string, int> canonicalNetPeak_;
    unsigned canonicalLastDrift_ = ~0u;    // summary prints on change only — see the note there
    unsigned canonicalLastDeclared_ = ~0u;

    // Rung 0 (Step 3): shared DRAW_INDEXED indirect command signature (lazy). See getter.
    ComPtr<ID3D12CommandSignature> drawIndexedCmdSig_;

    // Streamline / DLSS integration
    sl::DLSSMode dlssMode_ = sl::DLSSMode::eBalanced;
    std::unique_ptr<DlssHandler> dlssHandler_;
    bool streamlineInitialized_ = false;
    bool shutdown_ = false;

    void UpdateDlssSettings();
    void AllocateDlssResourcesIfNeeded();


    SamplerManager samplerManager_;
    MaterialManager materialManager_;
    InputLayoutManager inputLayoutManager_;
    MeshManager meshManager_;
    FontManager fontManager_;
    TextManager textManager_;
    MaterialDataManager materialDataManager_;
    RenderContextPool ctxPool_;
    DebugDrawSystem debugDrawSystem_;
    ImGuiLayer imguiLayer_;

    friend class DlssHandler;
};
