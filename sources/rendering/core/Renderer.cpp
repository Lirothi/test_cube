#include "rendering/core/Renderer.h"
#include <unordered_set>
#include "core/diagnostics/DiagPaths.h"
#include "rendering/core/RendererInvariantFailure.h"
#include "rendering/core/BarrierTranslation.h"
#include "rendering/core/CommandListBindState.h"
#include "core/Helpers.h"
#include <cassert>
#include <cmath>
#include <vector>
#include <utility>
#include <dxgidebug.h>
#include <d3d12sdklayers.h> // ID3D12Debug*, ID3D12InfoQueue
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "app/Systems.h"
#include "app/camera/Camera.h"
#include "materials/Texture2D.h" // shared-texture cache stats reported at shutdown
#include "rendering/core/DlssHandler.h"
#include "streamline/include/sl.h"

#pragma comment(lib, "dxguid.lib")

Renderer::Renderer()
    : dlssHandler_(std::make_unique<DlssHandler>(*this))
{
}

Renderer::~Renderer() {
    // Report after everything has been reset
    ReportLiveObjects(); // when the debug build includes this helper
    // (the frame fence event is closed by FrameScheduler's destructor)
}

static void logFunctionCallback(sl::LogType type, const char* msg)
{
    OutputDebugStringA(msg);
}

// Step 9: the enhanced-barrier command list. Same borrowed-view contract as AsCmdList4 — the
// QueryInterface ref is released immediately and the pointer is valid only as long as `cl` is.
// Null when the runtime predates ID3D12GraphicsCommandList7, which is why every future caller
// must check it rather than assume AreEnhancedBarriersSupported() implies it.
ID3D12GraphicsCommandList7* Renderer::AsCmdList7(ID3D12GraphicsCommandList* cl) const
{
    if (!cl) {
        return nullptr;
    }
    ID3D12GraphicsCommandList7* cl7 = nullptr;
    if (SUCCEEDED(cl->QueryInterface(IID_PPV_ARGS(&cl7)))) {
        cl7->Release();
        return cl7;
    }
    return nullptr;
}

ID3D12GraphicsCommandList4* Renderer::AsCmdList4(ID3D12GraphicsCommandList* cl) const
{
    if (!cl) {
        return nullptr;
    }
    // QueryInterface AddRefs the same underlying object; it stays alive through
    // the caller's `cl`, so release the extra ref immediately and hand back the
    // borrowed view (valid for as long as `cl` is).
    ID3D12GraphicsCommandList4* cl4 = nullptr;
    if (SUCCEEDED(cl->QueryInterface(IID_PPV_ARGS(&cl4)))) {
        cl4->Release();
        return cl4;
    }
    return nullptr;
}

void Renderer::Shutdown()
{
    if (shutdown_) {
        return;
    }
    shutdown_ = true;

    // Nothing to do if the device is missing
    const bool hasDevice = (GetDevice() != nullptr);
    const bool hasQueue = (GetCommandQueue() != nullptr);
    const bool hasFence = frameScheduler_.HasFence();

    // 0) Fully wait for the GPU (and close all outstanding command lists)
    if (hasDevice && hasQueue && hasFence) {
        WaitForPreviousFrame(); // your full-synchronization helper :contentReference[oaicite:2]{index=2}
    }

#if PROF_GPU_ENABLED
    Profiler::Get().ShutdownGpu();
#endif

    // Streamline must release its feature resources and interposed D3D/DXGI
    // objects while the device, queue, swap chain and all tagged resources are
    // still alive. After slShutdown(), proxy calls fall back to the native
    // implementation, so the remaining renderer teardown can proceed normally.
    if (streamlineInitialized_)
    {
        if (dlssHandler_)
        {
            dlssHandler_->Shutdown();
        }

        slShutdown();
        streamlineInitialized_ = false;
    }

    ShutdownImGui();

    debugDrawSystem_.Shutdown();
    materialManager_.Clear();
    // Report the shared-texture cache BEFORE the material caches drop their last references, or
    // the live-entry count is always zero and the line says nothing.
    {
        std::uint32_t saved = 0, loaded = 0;
        std::size_t entries = 0;
        Texture2D::CacheStats(saved, loaded, entries);
        char line[192];
        std::snprintf(line, sizeof(line),
                      "[texcache] %u loads, %u shared (GPU copies avoided), %zu live entries\n",
                      loaded, saved, entries);
        OutputDebugStringA(line);
        if (FILE* f = nullptr; fopen_s(&f, diag::LogPath("texcache.log").c_str(), "w") == 0 && f) {
            std::fputs(line, f);
            std::fclose(f);
        }
    }
    materialDataManager_.ClearAll();
    Texture2D::ClearCache();
    meshManager_.Clear();
    textManager_.Clear();
    fontManager_.Clear();
    samplerManager_.Clear();

    // 1) Stop the command “timeline”: prevent further submissions. Real clear —
    // releases the pooled per-batch memory (batches refer only to command lists
    // from the frame pools).
    submitTimeline_.Clear();

    // 2) Offscreen targets (G-Buffer/Light/Scene/Depth) — destroy these first
    DestroyDeferredTargets(); // properly resets resources and heaps, and clears knownStates_ :contentReference[oaicite:4]{index=4}

    // 2b) P1: the persistent exposure buffers. Before the canonical clear below, so they undeclare
    // themselves properly instead of being swept by it.
    exposureMetering_.Release();

    // 3) Back buffers and RTV/DSV heaps
    swapchain_.ReleaseBuffers();

    // 4) Safety measure: drop every canonical declaration — each resource they describe has just
    // been destroyed, and a surviving entry would hand a recycled address someone else's resting
    // state (and its stale `predicted`).
    canonicalStates_.Clear();

    // 5) SwapChain — exit fullscreen (if needed) and release it
    swapchain_.ReleaseSwapchain();

    // 6) Frame resources: reset pool usage and clear the upload ring
    // (the actual ComPtrs release when Renderer is destroyed, but this removes dependencies)
    frameScheduler_.ResetFrameState(GetDevice());

    // 6b) The two lazily-created, renderer-lifetime objects. Left to ~Renderer they were still
    // alive when ReportLiveObjects() runs — that call sits in the DESTRUCTOR BODY, which executes
    // before any member is destroyed — so every debug run ended with three "Live ..." warnings:
    // these two plus the device, whose refcount of 2 was exactly the references they held. Three
    // permanent entries in a leak report is how a real leak goes unnoticed, which is the same
    // reason the barrier log now collapses its repeats. Both are recreated on demand, so nothing
    // downstream depends on them surviving this far.
    vsmDummyHeap_.Reset();
    drawIndexedCmdSig_.Reset();

    // 7) Fence/Queue
    frameScheduler_.ReleaseFence();
    graphicsDevice_.ReleaseQueue();

    // 8) Release the device last
    graphicsDevice_.ReleaseDevice();
}

void Renderer::InitD3D12(HWND window, UINT width, UINT height) {
    hWnd_ = window;
    width_ = width;
    height_ = height;

    sl::Preferences pref;

    pref.applicationId = 0x12345678U;

#if _DEBUG
    pref.showConsole = true;
    pref.logMessageCallback = &logFunctionCallback;
    pref.logLevel = sl::LogLevel::eDefault;
#else
    pref.showConsole = false;
    // Perf: in release keep Streamline silent. eDefault makes slEvaluateFeature format log
    // strings and invoke the callback (which calls OutputDebugStringA — a global-locked syscall)
    // every frame; measured ~80us/frame inside Pass_Tonemap. eOff + no callback removes it.
    pref.logMessageCallback = nullptr;
    pref.logLevel = sl::LogLevel::eOff;
#endif

    pref.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;

    sl::Feature featuresToLoad[] = {
        sl::kFeatureDLSS,
#ifdef STREAMLINE_FEATURE_NIS
        sl::kFeatureNIS,
#endif
#ifdef STREAMLINE_FEATURE_DLSS_FG
        sl::kFeatureDLSS_G,
#endif
#ifdef STREAMLINE_FEATURE_REFLEX
        sl::kFeatureReflex,
#endif
        // PCL is always implicitly loaded, but request it to ensure we never have 0-sized array
        sl::kFeaturePCL
    };
    pref.featuresToLoad = featuresToLoad;
    pref.numFeaturesToLoad = static_cast<uint32_t>(std::size(featuresToLoad));

    pref.renderAPI = sl::RenderAPI::eD3D12;

    auto slRes = slInit(pref, sl::kSDKVersion);
    streamlineInitialized_ = (slRes == sl::Result::eOk);

    // --- Device (debug layer enabled inside, in debug builds) ---
    graphicsDevice_.InitDevice();

    slSetD3DDevice(GetDevice());

    if (dlssHandler_)
    {
        dlssHandler_->OnStreamlineInitialized(slRes);
    }

    UpdateRenderResolutionFromScale();

    //sl::AdapterInfo adapterInfo{};
    //if (SL_FAILED(result, slIsFeatureSupported(sl::kFeatureDLSS, adapterInfo)))
    //{
    //    int a = 0;
    //}

    graphicsDevice_.SetupDebugBreaks();

    // --- Queue ---
    graphicsDevice_.InitQueue();

    // --- SwapChain + RTVs (render::kFrameCount) ---
    CreateSwapChainAndRTVs(width_, height_);

    // --- Depth ---
    CreateDepthResources(width_, height_);
    CreateDeferredTargets(width_, height_);

    // --- Fence + event ---
    frameScheduler_.InitFence(GetDevice());

    // --- Frame resources ---
    frameScheduler_.CreateFrameResources(GetDevice());

    AllocateDlssResourcesIfNeeded();

    RefreshCurrentFrameCaches();

    samplerManager_.Init(GetDevice(), 512);
    InitImGui();

    // P1: the persistent exposure buffers. Created unconditionally rather than when the feature is
    // switched on -- they are about 1 KB, and gating them would mean the default (dormant) state
    // exercises none of the lifecycle this step exists to prove.
    exposureMetering_.EnsureResources(this);

    InitFence();
}

void Renderer::InitFence() {
    // Compatibility with your main.cpp — safe no-op if initialization already happened
    frameScheduler_.InitFence(GetDevice());
}

void Renderer::InitTextSystem(ID3D12GraphicsCommandList* uploadCl,
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
    const std::wstring& folder)
{
    fontManager_.Init(this);
    fontManager_.LoadFromFolder(this, uploadCl, uploadKeepAlive, folder);

    textManager_.Init(this);
    if (auto* def = fontManager_.Get(L"Consolas_32"))
    //if (auto* def = fontManager_.Get(L"Consolas_32_coverage"))
    //if (auto* def = fontManager_.Get(L"cons_32"))
    {
        textManager_.SetFont(def);
    }
    auto shadowDesc = TextManager::ShadowDesc();
    shadowDesc.offsetX = 2.0f;
    shadowDesc.offsetY = 2.0f;
    shadowDesc.color.w = 0.9f;
    shadowDesc.scaleWithTextSize = true;
    textManager_.SetShadow(shadowDesc);
}

void Renderer::CreateSwapChainAndRTVs(UINT width, UINT height) {
    // Forget tracked states of the old backbuffers before they are released
    for (UINT i = 0; i < render::kFrameCount; ++i) {
        ClearResourceState(swapchain_.Backbuffer(i));
    }

    swapchain_.Create(GetDevice(), GetCommandQueue(), hWnd_, width, height,
        GetBackbufferResourceFormat(), GetBackbufferFormat());

    currentFrameIndex_ = swapchain_.CurrentBackBufferIndex();

    for (UINT i = 0; i < render::kFrameCount; ++i) {
        SetResourceState(swapchain_.Backbuffer(i), D3D12_RESOURCE_STATE_PRESENT);
        // Step 7: the backbuffer is written by hand-rolled barriers in RecordBindAndClear
        // and the present epilogue, which the compile never sees. Excluded from it.
        SetResourceUnmanaged(swapchain_.Backbuffer(i));
    }
}

void Renderer::CreateDepthResources(UINT width, UINT height) {
    swapchain_.CreateDepth(GetDevice(), width, height, render::kDepthBufferResourceFormat, render::kDepthBufferViewFormat);
}

void Renderer::WaitForFrame(UINT frameIndex) {
    frameScheduler_.WaitForFrame(frameIndex);
}

void Renderer::SignalFrame(UINT frameIndex) {
    // Step 2: both queues, one value. GetComputeQueue() is null when the device refused one, which
    // FrameScheduler handles by advancing the compute fence from the CPU instead.
    frameScheduler_.SignalFrame(GetCommandQueue(), GetComputeQueue(), frameIndex);
}

void Renderer::RefreshCurrentFrameCaches() {
    currentFrameResource_ = nullptr;
    currentFrameDescriptorHeapCount_ = 0;
    currentFrameDescriptorHeaps_.fill(nullptr);

    if (currentFrameIndex_ >= render::kFrameCount) {
        return;
    }

    FrameResource* fr = frameScheduler_.GetFrameResource(currentFrameIndex_);
    currentFrameResource_ = fr;
    if (!fr) {
        return;
    }

    if (auto* heap = fr->GetDescAlloc().GetShaderVisibleHeap()) {
        currentFrameDescriptorHeaps_[currentFrameDescriptorHeapCount_++] = heap;
    }
    if (auto* heap = fr->GetSamplerAlloc().GetShaderVisibleHeap()) {
        currentFrameDescriptorHeaps_[currentFrameDescriptorHeapCount_++] = heap;
    }
}

// Mirrors `render::g_noAsyncCompute` as the frame loop last APPLIED it, and a latch so the device
// removal is reported once rather than every frame after it happens. File statics rather than
// Renderer members on purpose: the flag one of them shadows is itself a global, and adding a member
// to this widely-included header changes the class layout — which an incremental Release build does
// not notice and which then fails as "batch index outside the active range" (paid for in step 8).
static bool s_asyncModeApplied = false;
static bool s_deviceRemovalReported = false;

// Async-compute: apply a change to the async switch, draining BOTH queues first.
//
// Called from the ONE place the new value first matters — immediately before the frame's graph is
// built. NOT from BeginFrame, which is where this started and where it was a frame LATE: the
// developer window is drawn between BeginFrame and the graph build, so a click lands after the
// former, and the first frame under the new topology went out undrained.
//
// Why drain at all: the switch changes which queue owns which pass. Every guard in the compile
// validates ONE frame's graph, so none of them can see two in-flight frames disagreeing about queue
// ownership. This removes that class.
//
// HONEST STATUS: unproven as a fix. The reported repro is rapid clicking of the developer-window
// checkbox; flipping the flag every frame in this same phase, with the drain both enabled and
// bypassed, stayed clean over six Release_Editor runs. So this is hygiene that happens to be
// correct, not a demonstrated cure.
void Renderer::SyncAsyncQueueMode()
{
    if (render::g_noAsyncCompute != s_asyncModeApplied) {
        WaitForPreviousFrame();
        s_asyncModeApplied = render::g_noAsyncCompute;
    }
}

// Write the device-removed reason to logs/device_removed.log, once per process.
void Renderer::ReportDeviceRemovalOnce()
{
    if (s_deviceRemovalReported || !GetDevice()) {
        return;
    }
    const HRESULT reason = GetDevice()->GetDeviceRemovedReason();
    if (SUCCEEDED(reason)) {
        return;
    }
    s_deviceRemovalReported = true;
    FILE* f = nullptr;
    if (fopen_s(&f, diag::LogPath("device_removed.log").c_str(), "a") == 0 && f) {
        // The reason CLASS is most of the answer: DXGI_ERROR_DEVICE_HUNG (0x887A0006) is a wait
        // whose signal never came, DXGI_ERROR_DEVICE_RESET (0x887A0007) a fault in this app's own
        // work, DXGI_ERROR_DRIVER_INTERNAL_ERROR (0x887A0020) a malformed command reaching the
        // driver. The async state and the queue counters say which topology was live when it died.
        std::fprintf(f,
            "device removed: reason=0x%08X frame=%llu asyncCompute=%s computeLists=%u crossQueueWaits=%u\n",
            static_cast<unsigned>(reason),
            static_cast<unsigned long long>(totalFrameNumber_),
            render::g_noAsyncCompute ? "off" : "on",
            render::g_asyncComputeLists, render::g_crossQueueWaits);
        std::fclose(f);
    }
}

void Renderer::BeginFrame() {
    CPU_SCOPE(ProfilerScopes::kRendererBeginFrame);

    // A device removal can surface anywhere — a failed Present, a fence wait that never completes,
    // a TDR that kills the queue between frames — and until now it surfaced as a bare throw with
    // nothing written to disk, which is exactly why the one real report of the async toggle removing
    // the device left no evidence at all. One call per frame catches it wherever it happened, for a
    // measured 0.207 us (see g_deviceRemovalCheck). `--no-dr-check` skips the poll.
    if (render::g_deviceRemovalCheck) {
        ReportDeviceRemovalOnce();
    }

    // Wait for the GPU using its back buffer fence value
    WaitForFrame(currentFrameIndex_);

    // The inspector re-states its preview request every frame it is open, so clearing here means a
    // CLOSED inspector stops the pass instead of leaving it resampling the last target forever.
    // Order matters and holds: BeginFrame -> UI building (which may re-request) -> Scene::Render.
    debugPreviewRequest_ = {};

    if (dlssHandler_)
    {
        dlssHandler_->OnBeginFrame();
    }

    RefreshCurrentFrameCaches();
    FrameResource* fr = currentFrameResource_;

    ++totalFrameNumber_;

    // Step 2: the invariant this whole step exists to protect (R6). Everything below recycles
    // per-frame state — command allocators, the descriptor and sampler rings, the upload ring — on
    // the assumption that the GPU is done with this SLOT. That is now a two-queue question, and a
    // path that resets the pools without having waited would corrupt silently: the symptom is a
    // descriptor address handed out twice, which GBV reports as a wrong descriptor TYPE somewhere
    // else entirely. Debug-only; WaitForFrame above is what makes it true.
    assert(frameScheduler_.IsFrameComplete(currentFrameIndex_) &&
           "BeginFrame: per-frame pools reused before BOTH queues passed this slot's fence");

    // Reset per-frame pools
    if (fr) {
        fr->ResetCommandAllocators(GetDevice());
        fr->ResetCommandListsUsage();
        fr->GetDescAlloc().ResetPerFrame();
        fr->GetSamplerAlloc().ResetPerFrame();
        fr->ResetUpload();
    }

    ctxPool_.ResetForFrame();
    debugDrawSystem_.BeginFrame();
    pendingImGuiTextureResources_.clear();

    // Async-compute step 1. Last thing in BeginFrame: the pools have just been reset and the
    // frame's descriptor heaps are published, which is exactly the state a real compute pass would
    // find. Costs one bool test per frame with the flag off.
    ProbeComputeLaneOnce();
}

// Async-compute plan step 1 — prove the COMPUTE lane of the FrameResource pools works.
//
// R2 of the plan: `FrameResource` has pooled allocators and command lists per
// D3D12_COMMAND_LIST_TYPE since long before this work, but `D3D12_COMMAND_LIST_TYPE_COMPUTE`
// appeared in exactly two files across sources/ and both were the plumbing itself — no code had
// ever acquired one. So this acquires exactly one, on purpose, in isolation, and throws it away.
//
// It deliberately does NOT submit. Step 1's whole claim is "the queue exists and is idle"; a probe
// that executed anything would make the frame's unchanged-ness unprovable, which is the only thing
// this step is judged on.
void Renderer::ProbeComputeLaneOnce()
{
    if (!render::g_computeLaneProbe || computeLaneProbed_) {
        return;
    }
    FrameResource* fr = currentFrameResource_;
    if (!fr || !GetDevice()) {
        return; // no frame resources yet — try again next frame rather than reporting a failure
    }
    computeLaneProbed_ = true;

    const char* verdict = "unknown";
    char detail[160] = {};
    try {
        // Both throw on failure (see FrameResource::CommandAllocPools_/CommandListPools_), and a
        // COMPUTE list is created against a COMPUTE allocator — mismatching the two is the classic
        // way this fails, so the pools' own type-indexed lanes are what is being tested here.
        ID3D12CommandAllocator* alloc =
            fr->AcquireCommandAllocator(GetDevice(), D3D12_COMMAND_LIST_TYPE_COMPUTE);
        ID3D12GraphicsCommandList* cl =
            fr->AcquireCommandList(GetDevice(), D3D12_COMMAND_LIST_TYPE_COMPUTE, alloc);
        if (!alloc || !cl) {
            verdict = "FAILED";
            std::snprintf(detail, sizeof(detail), " (acquire returned null)");
        }
        else {
            // The same call BeginThreadCommandList makes for a COMPUTE list. A compute queue
            // accepts CBV_SRV_UAV and SAMPLER heaps, so this must succeed; it is here because it
            // is the first thing a real async pass would do and it has never run.
            if (currentFrameDescriptorHeapCount_ > 0) {
                cl->SetDescriptorHeaps(currentFrameDescriptorHeapCount_,
                                       currentFrameDescriptorHeaps_.data());
            }
            const HRESULT closeHr = cl->Close();
            if (FAILED(closeHr)) {
                verdict = "FAILED";
                std::snprintf(detail, sizeof(detail), " (Close hr=0x%08X)",
                              static_cast<unsigned int>(closeHr));
            }
            else {
                verdict = "OK";
                std::snprintf(detail, sizeof(detail), " (allocator+list acquired, %u heaps bound, closed)",
                              currentFrameDescriptorHeapCount_);
            }
        }
    }
    catch (const std::exception& e) {
        verdict = "FAILED";
        std::snprintf(detail, sizeof(detail), " (%s)", e.what());
    }

    char msg[288];
    std::snprintf(msg, sizeof(msg), "[caps] compute-lane probe: %s%s | compute queue: %s\n",
                  verdict, detail, GetComputeQueue() ? "present" : "absent");
    OutputDebugStringA(msg);
    FILE* f = nullptr;
    if (fopen_s(&f, diag::LogPath("device_caps.log").c_str(), "a") == 0 && f) {
        std::fputs(msg, f);
        std::fclose(f);
    }
}

void Renderer::EndFrame() {
    ExecuteTimelineAndPresent();
}

#if WITH_EDITOR
bool Renderer::RequestObjectIdPick(float displayX, float displayY)
{
    if (!GetDevice() || !rtManager_.IsCreated() || objectIdPickRequested_ || objectIdPickInFlight_)
    {
        return false;
    }
    if (width_ == 0 || height_ == 0 || renderWidth_ == 0 || renderHeight_ == 0)
    {
        return false;
    }
    if (!std::isfinite(displayX) || !std::isfinite(displayY))
    {
        return false;
    }
    if (displayX < 0.0f || displayY < 0.0f ||
        displayX >= static_cast<float>(width_) || displayY >= static_cast<float>(height_))
    {
        return false;
    }

    const float renderX = displayX * static_cast<float>(renderWidth_) / static_cast<float>(width_);
    const float renderY = displayY * static_cast<float>(renderHeight_) / static_cast<float>(height_);
    objectIdPickX_ = std::min(renderWidth_ - 1u, static_cast<UINT>(std::max(renderX, 0.0f)));
    objectIdPickY_ = std::min(renderHeight_ - 1u, static_cast<UINT>(std::max(renderY, 0.0f)));
    objectIdPickRequested_ = true;
    objectIdPickResultValid_ = false;
    objectIdPickResult_ = 0;
    return true;
}

void Renderer::RecordObjectIdPickReadback(ID3D12GraphicsCommandList* cl)
{
    if (!cl || !objectIdPickRequested_ || objectIdPickInFlight_ || !GetDevice())
    {
        return;
    }

    const auto& D = rtManager_.Deferred(currentFrameIndex_);
    if (!D.objectID.Get())
    {
        objectIdPickRequested_ = false;
        return;
    }

    if (!objectIdReadback_)
    {
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_READBACK;
        heapProps.CreationNodeMask = 1;
        heapProps.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(GetDevice()->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&objectIdReadback_)));
        objectIdReadback_->SetName(L"Editor.ObjectIdReadback");
    }

    // No transition here. This used to be an out-of-graph readback that moved objectID itself,
    // and the comment claimed the before-state was "known". It is not: Main_ObjectIdReadback now
    // DECLARES objectID as COPY_SOURCE and its body calls ApplyDeclaredStates, so by the time this
    // runs the compile has already moved it. The leftover explicit transition then asserted
    // before=RENDER_TARGET against a resource sitting in COPY_SOURCE — harmless under legacy
    // barriers, a hard debug-layer error under enhanced ones (id=1334, INCOMPATIBLE_BARRIER_LAYOUT).
    // Only reachable from that pass; see the single call site in SceneRenderer.

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = objectIdReadback_.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = render::kObjectIdFormat;
    dst.PlacedFootprint.Footprint.Width = 1;
    dst.PlacedFootprint.Footprint.Height = 1;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = D.objectID.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    const UINT x = std::min(objectIdPickX_, renderWidth_ - 1u);
    const UINT y = std::min(objectIdPickY_, renderHeight_ - 1u);
    D3D12_BOX srcBox{ x, y, 0, x + 1u, y + 1u, 1 };
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &srcBox);

    objectIdPickRequested_ = false;
    objectIdPickInFlight_ = true;
}

void Renderer::ResolveObjectIdPickReadback()
{
    if (!objectIdPickInFlight_)
    {
        return;
    }

    WaitForPreviousFrame();

    objectIdPickResult_ = 0;
    if (objectIdReadback_)
    {
        D3D12_RANGE readRange{ 0, sizeof(uint32_t) };
        void* mapped = nullptr;
        if (SUCCEEDED(objectIdReadback_->Map(0, &readRange, &mapped)) && mapped)
        {
            objectIdPickResult_ = *static_cast<const uint32_t*>(mapped);
            D3D12_RANGE writtenRange{ 0, 0 };
            objectIdReadback_->Unmap(0, &writtenRange);
            objectIdPickResultValid_ = true;
        }
    }

    objectIdPickInFlight_ = false;
}

bool Renderer::ConsumeObjectIdPick(uint32_t& outObjectId)
{
    if (!objectIdPickResultValid_)
    {
        return false;
    }
    outObjectId = objectIdPickResult_;
    objectIdPickResult_ = 0;
    objectIdPickResultValid_ = false;
    return true;
}

void Renderer::ResetObjectIdPickState()
{
    objectIdReadback_.Reset();
    objectIdPickX_ = 0;
    objectIdPickY_ = 0;
    objectIdPickResult_ = 0;
    objectIdPickRequested_ = false;
    objectIdPickInFlight_ = false;
    objectIdPickResultValid_ = false;
}
#endif

void Renderer::InitImGui()
{
    if (imguiLayer_.IsInitialized())
    {
        return;
    }

    if (!hWnd_ || !GetDevice() || !GetCommandQueue())
    {
        return;
    }

    imguiLayer_.Init(hWnd_, *this);
}

void Renderer::BeginImGuiFrame()
{
    imguiLayer_.BeginFrame();
}

void Renderer::RenderImGui(ID3D12GraphicsCommandList* commandList)
{
    // Step 7 — out-of-graph, explicit before state. The list holds two kinds: editor-owned
    // textures (created shader-readable, so this is a no-op) and engine render targets shown by
    // TextureDebugViewer (left at their canonical by the frame's graph work, since the overlay
    // runs last). Both answers come from the canonical registry, so no tracker is involved.
    for (ID3D12Resource* resource : pendingImGuiTextureResources_)
    {
        TransitionExplicit(commandList, resource, GetCanonicalState(resource),
                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    imguiLayer_.Render(commandList);
    // ...and put every one of them BACK. This used to be left undone, which was silently fine only
    // while every previewable target rested in a state sharing PIXEL_SHADER_RESOURCE's barrier
    // LAYOUT — NON_PIXEL_SHADER_RESOURCE does (both are D3D12_BARRIER_LAYOUT_SHADER_RESOURCE), so
    // the leftover state was invisible to the enhanced-barrier validator. It is NOT fine for a
    // target resting in UNORDERED_ACCESS (tonemap, fxaa, and briefly the GTAO output): the next
    // frame's compiled barrier claims a UAV before-state against a resource the inspector left in
    // the SHADER_RESOURCE layout, which is INCOMPATIBLE_BARRIER_LAYOUT and a debug-layer break.
    //
    // Restoring here rather than constraining what a target may rest in keeps the rule where it
    // belongs: the graph decides resting states, and this out-of-graph peek borrows and returns.
    // For editor-owned textures the canonical IS PixelShaderResource, so both calls are no-ops.
    for (ID3D12Resource* resource : pendingImGuiTextureResources_)
    {
        TransitionExplicit(commandList, resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                           GetCanonicalState(resource));
    }
    pendingImGuiTextureResources_.clear();
}

void Renderer::RestoreGraphicsStateAfterExternalDraw(ID3D12GraphicsCommandList* commandList)
{
    if (!commandList)
    {
        return;
    }

    // ImGui directly records D3D12 state, bypassing the engine's material bind
    // cache. Rebind the main output and engine heaps, then make the next native
    // Material::Bind fully establish its pipeline and root bindings.
    RecordBindDefaultsNoClear(commandList);
    BindDescriptorHeaps(commandList);
    render::g_clBindState.Reset();
}

ImTextureID Renderer::CreateImGuiTextureId(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc)
{
    if (!resource)
    {
        return ImTextureID_Invalid;
    }

    if (std::find(pendingImGuiTextureResources_.begin(),
            pendingImGuiTextureResources_.end(),
            resource) == pendingImGuiTextureResources_.end())
    {
        pendingImGuiTextureResources_.push_back(resource);
    }
    return imguiLayer_.CreateTextureIdForSrv(GetDevice(), resource, srvDesc, currentFrameIndex_);
}

#if WITH_EDITOR
void Renderer::ReleaseImGuiTextureDescriptors(ID3D12Resource* resource)
{
    imguiLayer_.ReleasePreviewDescriptorsForResource(resource);
    canonicalStates_.Forget(resource);
}

void Renderer::MarkImGuiTextureShaderReadable(ID3D12Resource* resource)
{
    // Editor textures shown through CreateImGuiTextureId (icon atlas, asset
    // thumbnails, viewport billboards) are created in a pixel-shader-readable
    // state outside the state tracker. Register that so RenderImGui does not
    // record a COMMON->PSR barrier with a wrong before-state, which the D3D12
    // debug layer rejects. Call each frame before RenderImGui, since a resize
    // clears all known states. Engine render targets go through TextureDebugViewer
    // instead and keep their real tracked state, so they are unaffected.
    if (resource)
    {
        // Step 7: this now DECLARES the texture's canonical (it is created shader-readable and
        // stays there), rather than papering over the tracker's global map. That is why it
        // survives the tracker's deletion instead of going with it as D2 assumed.
        SetResourceState(resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
}
#endif

void Renderer::ShutdownImGui()
{
    imguiLayer_.Shutdown();
}

bool Renderer::HandleImGuiWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    return imguiLayer_.HandleWndProc(hwnd, message, wParam, lParam);
}

bool Renderer::ImGuiWantsMouse() const
{
    return imguiLayer_.WantsMouse();
}

bool Renderer::ImGuiWantsKeyboard() const
{
    return imguiLayer_.WantsKeyboard();
}

void Renderer::ReportLiveObjects()
{
    graphicsDevice_.ReportLiveObjects();
}

void Renderer::Tick(float dt)
{
    CPU_SCOPE(ProfilerScopes::kRendererTick);
    fontManager_.Tick();

    if (fps_ <= 0.0f)
    {
        fps_ = 1.0f / dt;
    }
    else
    {
        fps_ = fps_ * fpsAlpha_ + (1.0f - fpsAlpha_) / dt;
    }

    // Hot reload: accumulate elapsed time
    if (shaderHotReloadEnabled_) {
        shaderWatchAccumSec_ += dt;

        // 1) Every N seconds — launch a one-off background task to scan the filesystem
        if (shaderWatchAccumSec_ >= shaderWatchIntervalSec_) {
            // Prevent overlapping scans
            if (!materialManager_.IsProbeInFlight()) {
                (void)materialManager_.RequestFSProbeAsync();
            }
            // Keep the fractional remainder so we do not lose time
            shaderWatchAccumSec_ -= shaderWatchIntervalSec_;
            shaderWatchAccumSec_ = std::max(0.0f, shaderWatchAccumSec_);
        }

        // 2) Apply pending rebuilds (if the scan found changes and set the flag)
        if (materialManager_.ApplyPendingHotReloads(this, totalFrameNumber_, /*keepAliveFrames=*/render::kFrameCount + 1)) {
            materialsHotReloaded_ = true;
        }
    }
}

bool Renderer::ConsumeMaterialHotReloadFlag()
{
    bool wasReloaded = materialsHotReloaded_;
    materialsHotReloaded_ = false;
    return wasReloaded;
}

Renderer::ThreadCL Renderer::BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE type, ID3D12PipelineState* pso)
{
    CPU_SCOPE(ProfilerScopes::kRendererBeginThreadCommandList);
    ID3D12GraphicsCommandList* cl = 0;
    ID3D12CommandAllocator* alloc = 0;
    FrameResource* fr = currentFrameResource_;
    if (!fr) {
        fr = frameScheduler_.GetFrameResource(currentFrameIndex_);
    }
    if (!fr) {
        return {};
    }

    //CPU_SCOPE(L"Renderer::BeginThreadCommandList.1");
    alloc = fr->AcquireCommandAllocator(GetDevice(), type);
    cl = fr->AcquireCommandList(GetDevice(), type, alloc, pso);

    if ((type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE || type == D3D12_COMMAND_LIST_TYPE_BUNDLE) &&
        currentFrameDescriptorHeapCount_ > 0)
    {
        cl->SetDescriptorHeaps(currentFrameDescriptorHeapCount_, currentFrameDescriptorHeaps_.data());
    }

    // Register the command list in the lock-free state tracker
    //RegisterCurrentThreadCL(cl);

    // Step 3: a fresh command list / bundle inherits no pipeline/root state, so reset
    // this thread's bind cache — the first draw recorded into it binds fully.
    render::g_clBindState.Reset();

    return ThreadCL{ alloc, cl, type };
}

void Renderer::BindDescriptorHeaps(ID3D12GraphicsCommandList* cl) const
{
    if (!cl)
    {
        return;
    }

    if (currentFrameDescriptorHeapCount_ > 0)
    {
        cl->SetDescriptorHeaps(currentFrameDescriptorHeapCount_, currentFrameDescriptorHeaps_.data());
    }
}

Renderer::ThreadCL Renderer::BeginThreadCommandBundle(ID3D12PipelineState* initialPSO)
{
    return BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_BUNDLE, initialPSO);
}

void Renderer::EndThreadCommandList(ThreadCL& t, size_t batchIndex, uint32_t localOrder) {
    CPU_SCOPE(ProfilerScopes::kRendererEndThreadCommandList);
    if (t.cl == nullptr) {
        RendererInvariantFailure("Renderer::EndThreadCommandList: null command list");
    }
    // Step 4: work lists close at End time (no longer at submit time, when the
    // fixup used to append the next list's acquire barriers to this one's tail).
    // Runs on worker tasks, which must not throw — check the HRESULT explicitly;
    // a list that failed to Close would lose its GPU work.
    const HRESULT hr = t.cl->Close();
    if (FAILED(hr)) {
        RendererInvariantFailure("Renderer::EndThreadCommandList: Close() failed");
    }
    submitTimeline_.RegisterDirect(t.cl, batchIndex, localOrder);

    // Clear the TLS binding for this command list
    //UnregisterCurrentThreadCL();

    t.cl = nullptr;
    t.alloc = nullptr;
}

void Renderer::BeginSubmitTimeline() {
    submitTimeline_.BeginTimeline();
}

size_t Renderer::BeginSubmitBatch(RenderQueue queue) {
    return submitTimeline_.BeginBatch(queue);
}

void Renderer::SetSubmitBatchCrossQueueWait(size_t batchIndex, size_t waitForBatch) {
    submitTimeline_.SetCrossQueueWait(batchIndex, waitForBatch);
}

void Renderer::RegisterPassDriver(ID3D12GraphicsCommandList* cl, size_t batchIndex)
{
    submitTimeline_.RegisterDriver(cl, batchIndex);
}

void Renderer::EndThreadCommandBundle(ThreadCL& b, size_t batchIndex, uint32_t localOrder)
{
    CPU_SCOPE(ProfilerScopes::kRendererEndThreadCommandBundle);
    if (b.cl == nullptr) {
        RendererInvariantFailure("Renderer::EndThreadCommandBundle: null command list");
    }
    // Runs on worker tasks, which must not throw (the task system swallows task
    // exceptions) — check the HRESULT explicitly; a bundle that failed to Close
    // would lose its GPU work.
    const HRESULT hr = b.cl->Close();
    if (FAILED(hr)) {
        RendererInvariantFailure("Renderer::EndThreadCommandBundle: Close() failed");
    }
    submitTimeline_.RegisterBundle(b.cl, batchIndex, localOrder);
    b.cl = nullptr;
    b.alloc = nullptr;
}

void Renderer::ExecuteTimelineAndPresent() {
    CPU_SCOPE(ProfilerScopes::kRendererExecuteTimelineAndPresent);

    submitSegmentsScratch_.clear();

    // Gather batches in order, split into per-queue SEGMENTS (step 6). With every pass on the
    // graphics queue — which is every pass until step 8 — this yields exactly ONE segment holding
    // exactly the array a single-queue submit produced.
    {
        CPU_SCOPE(ProfilerScopes::kService1);
        submitTimeline_.GatherFrameLists(submitSegmentsScratch_, [this]() {
            // Fallback: a batch has bundles but no driver — create a temporary one
            FrameResource* fr = frameScheduler_.GetFrameResource(currentFrameIndex_);
            ID3D12CommandAllocator* alloc =
                fr->AcquireCommandAllocator(GetDevice(), D3D12_COMMAND_LIST_TYPE_DIRECT);
            ID3D12GraphicsCommandList* cl =
                fr->AcquireCommandList(GetDevice(), D3D12_COMMAND_LIST_TYPE_DIRECT, alloc);
            RecordBindDefaultsNoClear(cl);
            return cl;
        });
    }

    // Step 6: the GRAPHICS segments are what the wrapping lists bracket. The profiler-begin list
    // goes at the front of the first graphics segment and the epilogue at the end of the last, so
    // "the frame" still means the same span on the direct queue's timeline as it did before.
    size_t firstGraphicsSeg = (size_t)-1;
    size_t lastGraphicsSeg = (size_t)-1;
    size_t graphicsListCount = 0;
    for (size_t i = 0; i < submitSegmentsScratch_.size(); ++i) {
        if (submitSegmentsScratch_[i].queue != RenderQueue::Graphics) { continue; }
        if (firstGraphicsSeg == (size_t)-1) { firstGraphicsSeg = i; }
        lastGraphicsSeg = i;
        graphicsListCount += submitSegmentsScratch_[i].lists.size();
    }

    (void)graphicsListCount;

    // Acquire a fresh, open DIRECT command list from this frame's pool.
    auto acquireDirectCL = [this]() -> ID3D12GraphicsCommandList* {
        FrameResource* fr = frameScheduler_.GetFrameResource(currentFrameIndex_);
        ID3D12CommandAllocator* alloc =
            fr->AcquireCommandAllocator(GetDevice(), D3D12_COMMAND_LIST_TYPE_DIRECT);
        return fr->AcquireCommandList(GetDevice(), D3D12_COMMAND_LIST_TYPE_DIRECT, alloc);
    };

    // Step 6: with no graphics segment at all there is nowhere to put the present transition, so
    // make one. Cannot happen today (the frame always has graphics work) but the wrapping must not
    // depend on that.
    if (firstGraphicsSeg == (size_t)-1) {
        submitSegmentsScratch_.push_back(SubmitTimeline::Submission{});
        firstGraphicsSeg = lastGraphicsSeg = submitSegmentsScratch_.size() - 1;
    }

#if PROF_GPU_ENABLED
    {
        // GPU-profiler frame-begin: first list submitted this frame — i.e. the front of the FIRST
        // graphics segment, so GPU.Frame still brackets the same span of the direct queue.
        ID3D12GraphicsCommandList* cl = acquireDirectCL();
        Profiler::Get().BeginGpuFrame(cl);
        ThrowIfFailed(cl->Close());
        auto& lists = submitSegmentsScratch_[firstGraphicsSeg].lists;
        lists.insert(lists.begin(), cl);
    }
#endif

    {
        CPU_SCOPE(ProfilerScopes::kService2);

        // Step 7: work lists are already closed (directs at EndThreadCommandList, drivers at
        // gather time) and each one now carries its OWN barriers, compiled ahead of execution
        // by RenderGraph::CompileBarriers. This loop used to resolve every list's acquire
        // barriers against a live global state map and, when non-empty, record them into a
        // DEDICATED command list submitted immediately before that work list. Measured just
        // before the deletion: one such list per frame carrying exactly one barrier, all of it
        // for the swapchain backbuffer -- which now uses explicit before-states like the rest
        // of the out-of-graph paths. So the whole thing is a straight append.
        //
        // Step 6: the work lists now live in the SEGMENTS the gather produced, already in order,
        // so there is nothing to copy — only the two wrapping lists still have to be placed.

        // Dedicated epilogue list submitted last: present transition (+ GPU
        // profiler frame-end). Never reopen or append to the final work list.
        ID3D12GraphicsCommandList* epilogueCmd = acquireDirectCL();

        ID3D12Resource* backbuffer = swapchain_.Backbuffer(currentFrameIndex_);

        D3D12_RESOURCE_BARRIER presentBarrier{};
        presentBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        presentBarrier.Transition.pResource = backbuffer;
        presentBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        presentBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        presentBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        barriers::EmitOne(epilogueCmd, presentBarrier);
        // The engine's one return-to-canonical transition (D2's frame epilogue): PRESENT is
        // the backbuffer's declared resting state, so the frame ends where it began.
#if PROF_GPU_ENABLED
        Profiler::Get().EndGpuFrame(epilogueCmd);
#endif
        ThrowIfFailed(epilogueCmd->Close());
        submitSegmentsScratch_[lastGraphicsSeg].lists.push_back(epilogueCmd);
    }

    // Step 6: the frame's compiles have all run by here, so `predicted` is the frame's true
    // end state. Default off.
    ReportOffCanonicalStates();

    // Step 6 acceptance: dump the submitted arrays, ONCE, by debug name and in order. Placed here —
    // after the wrapping lists are placed and before any ExecuteCommandLists — so what is dumped is
    // literally what is submitted, not a reconstruction of it.
    if (render::g_dumpSubmitOrder) {
        render::g_dumpSubmitOrder = false; // once per process; a per-frame dump drowns the diff
        for (const auto& seg : submitSegmentsScratch_) {
            DumpSubmitOrder(seg.queue == RenderQueue::AsyncCompute ? "compute" : "graphics", seg.lists);
        }
    }

    {
        CPU_SCOPE(ProfilerScopes::kService3);
        // Step 3's ordering probe: make the graphics queue wait for LAST frame's compute signal
        // before it starts this frame's work. A GPU-side wait — the CPU does not block. The value
        // is from the previous frame, so the signal was always submitted before this wait and the
        // pair can never deadlock.
        if (render::g_asyncOrderProbe && asyncOrderProbeValue_ != 0) {
            frameScheduler_.WaitCrossQueue(GetCommandQueue(), asyncOrderProbeValue_);
        }

        // --- Step 6: per-queue submission with the graph's cross-queue fence edges (D2) ---
        //
        // Segments are submitted in BATCH ORDER, which is the order the graph unrolled them in.
        // Before a segment that carries a cross-queue wait, the producing queue is told to signal
        // (SignalCrossQueue) and this queue to wait (WaitCrossQueue) — both GPU-side, so the CPU
        // never blocks on a dependency between queues.
        //
        // A segment WITHOUT a wait is submitted with no synchronisation at all. That is the whole
        // point: two adjacent segments on different queues run concurrently unless the graph said
        // they must not. Ordering them "just in case" would delete the overlap this plan exists to
        // create.
        //
        // With every pass on Graphics there is exactly ONE segment, no waits, and this collapses to
        // the single ExecuteCommandLists it replaced.
        // The signal for batch B is enqueued IMMEDIATELY AFTER B's own segment, not at the moment
        // its consumer is submitted. That distinction is the whole difference between a fence edge
        // and a barrier across the frame: a signal issued later means "everything submitted to this
        // queue so far", which is almost always far more than the graph asked for. Measured on the
        // first cut of this loop — RTTrace ended up waiting for the entire graphics prefix and
        // overlapped the small light passes instead of Pass_VsmPageRender.
        crossQueueSignals_.clear();
        // Counted here rather than at graph-build time: what the developer window's async toggle
        // has to answer for is what reached the QUEUE, not what the graph intended. Reset per
        // frame, so the numbers describe this frame and cannot go stale when the toggle flips.
        render::g_asyncComputeLists = 0;
        render::g_crossQueueWaits = 0;
        for (const auto& seg : submitSegmentsScratch_) {
            const bool isCompute = (seg.queue == RenderQueue::AsyncCompute);
            ID3D12CommandQueue* q = isCompute ? GetComputeQueue() : GetCommandQueue();
            if (q == nullptr) {
                RendererInvariantFailure("Renderer::ExecuteTimelineAndPresent: a segment targets a queue that does not exist");
            }
            if (seg.waitForBatch != SubmitTimeline::kNoCrossQueueWait) {
                UINT64 value = 0;
                for (const auto& sig : crossQueueSignals_) {
                    if (sig.first == seg.waitForBatch) { value = sig.second; break; }
                }
                if (value == 0) {
                    RendererInvariantFailure("Renderer::ExecuteTimelineAndPresent: cross-queue wait "
                                             "for a batch that was never signalled (segment order broken)");
                }
                frameScheduler_.WaitCrossQueue(q, value);
                ++render::g_crossQueueWaits;
            }
            if (!seg.lists.empty()) {
                q->ExecuteCommandLists(static_cast<UINT>(seg.lists.size()), seg.lists.data());
                if (isCompute) {
                    render::g_asyncComputeLists += static_cast<std::uint32_t>(seg.lists.size());
                }
            }
            if (seg.signalAfter) {
                crossQueueSignals_.emplace_back(seg.lastBatch, frameScheduler_.SignalCrossQueue(q));
            }
        }
    }

    // Step 2's proof device: a deliberately EMPTY compute submission, so the two-queue frame fence
    // is exercised by something real rather than by a queue that never receives work. Opens a
    // COMPUTE list, closes it, submits it — no barriers, no dispatches, nothing to get wrong. The
    // signal that follows in SignalFrame then genuinely orders behind a GPU submission on that
    // queue, which is what makes "the slot waits for both" a claim with evidence.
    //
    // Off by default (`--async-empty-submit`); step 8 replaces it with the first real pass.
    if (render::g_asyncEmptySubmit) {
        SubmitEmptyComputeWork();
    }

    {
        CPU_SCOPE(ProfilerScopes::kService4);
        lastPresentedIndex_ = currentFrameIndex_; // the buffer about to be shown (screenshots)
        // Present is the most likely place a device removal surfaces, and it was a bare throw: the
        // process died leaving nothing on disk. Report before rethrowing — BeginFrame's per-frame
        // check catches the cases that never reach here (a fence wait that never returns, a TDR
        // between frames), and the latch inside means only the first one is written.
        try {
            swapchain_.Present();
        }
        catch (...) {
            ReportDeviceRemovalOnce();
            throw;
        }
    }
    SignalFrame(currentFrameIndex_);
    currentFrameIndex_ = swapchain_.CurrentBackBufferIndex();
    RefreshCurrentFrameCaches();
}

// Async-compute plan step 6 — write one queue's submitted command-list array to
// logs/submit_order.log, by debug name, in submission order.
//
// By NAME, not by pointer: pointers are per-run and comparing them across two builds proves
// nothing, while the names are the pass identities — which is exactly what "the graphics array is
// unchanged" is a statement about. Appends, so the graphics and compute arrays of one frame land in
// one file in the order they are submitted.
void Renderer::DumpSubmitOrder(const char* queueName,
                               const std::vector<ID3D12CommandList*>& lists)
{
    FILE* f = nullptr;
    if (fopen_s(&f, diag::LogPath("submit_order.log").c_str(), "a") != 0 || !f) {
        return;
    }
    std::fprintf(f, "== queue=%s count=%zu ==\n", queueName, lists.size());
    for (size_t i = 0; i < lists.size(); ++i) {
        char label[160] = {};
        render::DebugObjectLabel(lists[i], label, sizeof(label));
        std::fprintf(f, "%3zu %s\n", i, label);
    }
    std::fclose(f);
}

// Async-compute plan step 2 — submit one empty COMPUTE command list to the async queue.
//
// The point is NOT the work (there is none). It is that the compute queue receives a real
// submission every frame, so the Signal that follows has something to order behind and the
// "a frame slot is free only when BOTH queues passed its value" rule is exercised rather than
// merely written. Without this the compute fence would be signalled on an idle queue and complete
// instantly, and every wait added in this step would be vacuously true.
void Renderer::SubmitEmptyComputeWork()
{
    ID3D12CommandQueue* computeQueue = GetComputeQueue();
    FrameResource* fr = currentFrameResource_;
    if (!computeQueue || !fr || !GetDevice()) {
        return;
    }
    ID3D12CommandAllocator* alloc =
        fr->AcquireCommandAllocator(GetDevice(), D3D12_COMMAND_LIST_TYPE_COMPUTE);
    ID3D12GraphicsCommandList* cl =
        fr->AcquireCommandList(GetDevice(), D3D12_COMMAND_LIST_TYPE_COMPUTE, alloc);
    if (!cl) {
        return;
    }
    // Records no WORK — but step 3 adds a GPU scope around the nothing, because an empty command
    // list produces no timestamps and therefore an EMPTY second trace row, which proves nothing
    // about the compute queue's calibration, frequency or drain fence. One timestamped pair per
    // frame on that row is what makes the second track evidence instead of decoration.
    {
        GPU_SCOPE(cl, ProfilerScopes::kAsyncEmptySubmit);
    }
    if (FAILED(cl->Close())) {
        RendererInvariantFailure("Renderer::SubmitEmptyComputeWork: Close() failed");
    }
    ID3D12CommandList* lists[] = { cl };
    computeQueue->ExecuteCommandLists(1, lists);

    // Step 3 acceptance: a KNOWN-ORDERED cross-queue pair, so the shared timebase can be checked
    // rather than assumed. The compute queue signals here; the next frame's graphics submission
    // waits on that value before it starts (see ExecuteTimelineAndPresent). The trace must then
    // show every `Async.EmptySubmit` ending before the following `GPU.Frame` begins — if the two
    // rows were calibrated independently and wrongly, that ordering would not hold.
    //
    // This deliberately SERIALISES the two queues, so it is its own flag and never on by default.
    // It also gives step 2's dormant SignalCrossQueue/WaitCrossQueue their first real exercise.
    if (render::g_asyncOrderProbe) {
        asyncOrderProbeValue_ = frameScheduler_.SignalCrossQueue(computeQueue);
    }
}

void Renderer::WaitForPreviousFrame() {
    // Fully wait for the GPU (for resize/destructor) — step 2: BOTH queues.
    //
    // This is the ONE function every idle path in the engine funnels through: ~50 call sites across
    // level switch, resize, editor commands, thumbnail eviction, screenshots and shutdown all call
    // it rather than touching the scheduler. That is why making it queue-correct here makes all of
    // them queue-correct, and why the plan puts this in step 2 instead of the hardening step — the
    // gates for every step in between are exactly that churn.
    frameScheduler_.WaitForGpuIdle(GetCommandQueue(), GetComputeQueue());
}

void Renderer::OnResize(UINT width, UINT height) {
    if (width == 0 || height == 0) {
        return;
    }
    width_ = width;
    height_ = height;
    if (dlssHandler_)
    {
        dlssHandler_->OnDisplaySizeChanged();
    }
    else
    {
        UpdateRenderResolutionFromScale();
    }

    // Important: wait for the GPU before replacing resources
    WaitForPreviousFrame();

    // Release the old RTV/DSV objects
    for (UINT i = 0; i < render::kFrameCount; ++i) {
        ClearResourceState(swapchain_.Backbuffer(i));
    }
    ClearResourceState(swapchain_.DepthBuffer());
    swapchain_.ReleaseBuffersForResize();

    // ResizeBuffers
    swapchain_.ResizeBuffers(width_, height_);

    // Recreate RTV and DSV
    CreateSwapChainAndRTVs(width_, height_);
    CreateDepthResources(width_, height_);
    CreateDeferredTargets(width_, height_);
    AllocateDlssResourcesIfNeeded();

    // P1 / plan section 6.4: the exposure buffers are resolution-independent, so a resize must NOT
    // recreate them -- but the adapted value was metered from the old resolution's source, so the
    // history is stale and has to be re-seeded rather than adapted from.
    exposureMetering_.RequestReset();

    RefreshCurrentFrameCaches();
}

void Renderer::SetResourceState(ID3D12Resource* res, D3D12_RESOURCE_STATES state) {
    // Step 6: this IS the canonical declaration (see the header).
    Declarations().Declare(res, state);
}

void Renderer::SetResourceState(ID3D12Resource* res, D3D12_RESOURCE_STATES creationState,
                                D3D12_RESOURCE_STATES canonicalState) {
    Declarations().Declare(res, creationState, canonicalState);
}

void Renderer::ClearResourceState(ID3D12Resource* res) {
    Declarations().Forget(res);
}

D3D12_RESOURCE_STATES Renderer::GetCanonicalState(ID3D12Resource* res) const {
    return canonicalStates_.Get(res);
}

void Renderer::ReportOffCanonicalStates() {
    if (!render::g_canonicalCheck) { return; }

    // Snapshot first: the report walks the whole table and the registry's lock is also taken by
    // texture loads on other threads, so it is copied out rather than held across the loop.
    canonicalStates_.Snapshot(canonicalScratch_);

    unsigned drifted = 0;
    for (const auto& [res, entry] : canonicalScratch_) {
        // NEVER dereference `res`. Not every release path unregisters, so this table can hold
        // dangling keys; both the name and the state come from the ENTRY for exactly that reason.
        (void)res;
        const D3D12_RESOURCE_STATES actual = entry.predicted;
        if (actual == entry.state) { continue; }
        ++drifted;
        char msg[320];
        std::snprintf(msg, sizeof(msg), "[canonical] off-canonical res=%s canonical=0x%X actual=0x%X\n",
                      entry.name, static_cast<unsigned>(entry.state), static_cast<unsigned>(actual));
        Renderer::DiagLogOnce(msg);
    }

    // Step 6b part 2: name the resources that share a debug name. The original premise here was
    // "every resource has a unique debug name, so two live entries sharing one means an earlier one
    // was never unregistered" — the dangling keys that crashed the reporter and let a recycled
    // address inherit a stale canonical. That premise is FALSE for assets: a debug name is the
    // asset path, and two materials naming one texture file are two legitimate live entries. So
    // this reports the duplication and leaves the verdict to the reader (see below). It still says
    // exactly WHICH names to look at, instead of guessing at ~60 declaration sites.
    // Throttled: this walks the table twice.
    // Triggered by the table CHANGING SIZE, which is precisely when something was declared or
    // failed to be forgotten. A fixed frame-count throttle was wrong: the stress harness runs
    // only a handful of frames per churn op, so a 240-frame tick never fired.
    if (canonicalScratch_.size() != canonicalLastDeclared_) {
        // A steady count is NOT a leak: two GPU-instanced clouds, or one texture legitimately
        // loaded twice, sit at a constant 2 forever. Only a count that keeps CLIMBING past every
        // value it has held before means declares are outrunning forgets. Reporting bare
        // duplicates cannot tell those apart and twice made me call a steady x2 a leak.
        canonicalStates_.NetByName(canonicalNetScratch_);
        for (const auto& [name, net] : canonicalNetScratch_) {
            // Skip pointer-"named" entries. Their identity is an ADDRESS, which the allocator
            // reuses, so a per-name net count means nothing for them — the answer for an unnamed
            // resource is to give it a debug name, not to count it.
            if (name.find('.') == std::string::npos && name.find(':') == std::string::npos) { continue; }
            auto it = canonicalNetPeak_.find(name);
            const int peak = (it == canonicalNetPeak_.end()) ? 0 : it->second;
            if (net <= peak) { continue; }
            canonicalNetPeak_[name] = net;
            // Growing past a previous high-water mark. The first sighting of a name is not
            // interesting (every resource declares once), so only report from the second on.
            if (net < 2) { continue; }
            // NOT called a leak, because this cannot tell one apart from a duplicate. Both look
            // identical here -- N live entries under one debug name -- and the honest reading is
            // usually the boring one. Measured on `demo`: damaged_plaster_normal.dds sat at 4
            // because FOUR distinct materials name that file (two presets plus three inline object
            // materials) and MaterialDataManager caches by MATERIAL NAME, so each loads its own
            // copy; bronze_*.dds sat at 2 for the same reason. Nothing was leaking.
            //
            // HOW TO TELL THEM APART: count the referrers in data/ first. A count that MATCHES the
            // number of things naming the asset is duplication -- wasted VRAM, fixable only by a
            // texture cache keyed on path+usage, not by chasing lifetimes. A count that keeps
            // climbing with no new referrer, or that survives the thing that owned it, is the leak.
            // A level-switching `--scene-stress` run is the discriminator: declares outrunning
            // forgets climb across iterations, duplication stays flat at the single-level value.
            char dup[240];
            std::snprintf(dup, sizeof(dup),
                          "[canonical] duplicate-name %s: %d live entries "
                          "(check the referrer count before reading this as a leak)\n",
                          name.c_str(), net);
            Renderer::DiagLogOnce(dup);
        }

        unsigned anonymous = 0;
        for (const auto& [res, entry] : canonicalScratch_) {
            (void)res;
            // A resource with no debug name falls back to its address, unique by construction, so
            // it can never pair up with anything. Counted separately or the leak hides in them.
            if (std::strchr(entry.name, '.') == nullptr && std::strchr(entry.name, ':') == nullptr) { ++anonymous; }
        }
        char anon[160];
        std::snprintf(anon, sizeof(anon), "[canonical] %u named + %u UNNAMED entries declared\n",
                      static_cast<unsigned>(canonicalNetScratch_.size()), anonymous);
        Renderer::DiagLogOnce(anon);
    }

    // One summary line so an empty log is distinguishable from a check that never ran — the same
    // mistake the comparator's SKIPPED line exists to prevent. Printed only when the numbers
    // CHANGE: at one line per frame it floods DBWIN's single shared buffer and the listener drops
    // the leak lines above, which is exactly how the first leak measurement came back empty.
    const unsigned declaredCount = static_cast<unsigned>(canonicalScratch_.size());
    if (drifted != canonicalLastDrift_ || declaredCount != canonicalLastDeclared_) {
        canonicalLastDrift_ = drifted;
        canonicalLastDeclared_ = declaredCount;
        char summary[160];
        std::snprintf(summary, sizeof(summary), "[canonical] frame end: %u of %u declared resources off-canonical\n",
                      drifted, declaredCount);
        Renderer::DiagLogOnce(summary);
    }
}

// Barrier plan step 3: null unless a converted pass's body is running on this thread.
static thread_local Renderer::TransitionLog* tlTransitionLog = nullptr;

void Renderer::SetThreadTransitionLog(TransitionLog* log) {
    tlTransitionLog = log;
}

Renderer::TransitionLog* Renderer::CurrentThreadTransitionLog() {
    return tlTransitionLog;
}

// Step 7: the pass whose compiled barriers this thread should emit. Null outside a converted
// pass body, which is what keeps unconverted paths (uploads, editor, present) on the old route.
static thread_local Renderer::CompiledBarriers* tlCompiledBarriers = nullptr;

void Renderer::SetThreadCompiledBarriers(CompiledBarriers* cb) { tlCompiledBarriers = cb; }
Renderer::CompiledBarriers* Renderer::CurrentThreadCompiledBarriers() { return tlCompiledBarriers; }

// Barrier-diagnostics sink, shared by --barrier-flip-trace and --barrier-cmp. Goes to a FILE as
// well as the debugger: the --scene-stress runs that reproduce the residual mismatches have no
// debugger attached, so DBWIN output is simply lost (the same trap as the stress harness's own
// verdict). Flushed per line — a crash mid-frame must not take the last line with it, which is the
// one that matters.
void Renderer::DiagLog(const char* line) {
    static std::mutex mtx;
    static FILE* f = nullptr;
    std::lock_guard<std::mutex> lk(mtx);
    if (f == nullptr) { fopen_s(&f, diag::LogPath("barrier_diag.log").c_str(), "w"); }
    if (f != nullptr) { std::fputs(line, f); std::fflush(f); }
    OutputDebugStringA(line);
}

// Same sink, but a line that is IDENTICAL to one already written is dropped.
//
// Every one of these diagnostics is evaluated per frame, so a single standing condition writes a
// line per frame forever: an eight-second headless run produced 3779 lines of which SEVEN were
// distinct, and the two that mattered (an off-canonical resource and a leak) sat in the middle of
// three thousand copies of "0 of 176 off-canonical". A per-frame repeat carries no information the
// first line did not — these report STATE, not events — so the whole log becomes the set of
// conditions that occurred, which is what anyone reading it actually wants.
//
// Deliberately keyed on the FORMATTED TEXT: any number that moves (a leak count climbing, a drift
// count changing) makes a new line and still gets through, so a condition that is developing is
// never the thing that gets swallowed.
void Renderer::DiagLogOnce(const char* line) {
    static std::mutex mtx;
    static std::unordered_set<std::string> seen;
    {
        std::lock_guard<std::mutex> lk(mtx);
        // Past the cap everything prints again: losing a diagnostic is worse than repeating one,
        // so the overflow direction is "say too much", never "go quiet".
        if (seen.size() < 8192 && !seen.insert(line).second) { return; }
    }
    DiagLog(line);
}

void Renderer::TransitionExplicit(ID3D12GraphicsCommandList* cl, ID3D12Resource* res,
                                  D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (cl == nullptr || res == nullptr || before == after) { return; }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers::EmitOne(cl, b);
}

void Renderer::Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES after) {
    //CPU_SCOPE(ProfilerScopes::kRendererTransition);
    if (tlTransitionLog != nullptr && res != nullptr) {
        TransitionLog& log = *tlTransitionLog;
        // Claim a slot atomically: a fan-out pass appends from several worker threads at once.
        const std::uint32_t slot = log.count->fetch_add(1, std::memory_order_relaxed);
        if (slot < log.capacity) {
            log.entries[slot] = ObservedTransition{ res, after };
        }
        else {
            // Truncating would make the comparator report a false "missing" entry.
            log.overflowed.store(true, std::memory_order_relaxed);
        }
    }

    // Step 7 — emit the barrier the compile already produced for this pass. This is the engine's
    // only barrier path for graph resources; there is no state tracking behind it any more.
    // It owns a transition only for resources the compile actually MODELS: an undeclared or
    // unmanaged resource has no compiled entry, and treating that as "no barrier needed" would
    // silently drop it — so it falls to the invariant failure at the bottom instead, which says
    // to use TransitionExplicit.
    if (tlCompiledBarriers != nullptr && res != nullptr && cl != nullptr &&
        canonicalStates_.IsCompileManaged(res)) {
        CompiledBarriers& cb = *tlCompiledBarriers;
        // A request may only match the CURRENT point — the first not yet emitted. Never a later
        // one.
        //
        // Searching all points for (resource, state) is ambiguous, because the same transition
        // appears at several of them: Pass_Tonemap moves `tonemap` UAV -> COPY_SOURCE -> UAV, and
        // its very first request (ApplyDeclaredStates asking for UAV, which the compile turned
        // into nothing because UAV is already canonical) matched the RESTORE point at the end and
        // dragged the whole pass's barriers to the top of the command list. Measured with
        // --barrier-flip-trace: "point 3 emit 0x800->0x8" printed before "point 2 emit 0x8->0x800".
        //
        // Registrations are made in body order, so the body's requests arrive in point order and
        // the current point is the only one it can legitimately be asking for. Anything else is a
        // transition the compile decided needs no barrier.
        for (std::uint32_t p = 0; p < cb.pointCount; ++p) {
            CompiledBarriers::Point& pt = cb.points[p];
            if (pt.emitted.load(std::memory_order_relaxed)) { continue; }
            // Pass-flow S1: the view now keeps EMPTY points (the compile ate every barrier of a
            // declared point) so EmitPoint markers stay 1:1 with declarations. They are
            // transparent to this matcher: mark and move on, exactly as if they were not there —
            // which is what the view used to enforce by omitting them.
            if (pt.count == 0) {
                bool notYetEmpty = false;
                pt.emitted.compare_exchange_strong(notYetEmpty, true, std::memory_order_relaxed);
                continue;
            }
            bool names = false;
            for (std::uint32_t i2 = 0; i2 < pt.count && !names; ++i2) {
                names = pt.entries[i2].Transition.pResource == res &&
                        pt.entries[i2].Transition.StateAfter == after;
            }
            if (!names) { break; } // current point is not this request -> nothing to emit
            bool notYet = false;
            if (pt.emitted.compare_exchange_strong(notYet, true, std::memory_order_relaxed)) {
                CheckCompiledPointOnQueue(cl, pt, cb.pass);
                // Step 12: the ONE place compiled barriers reach the GPU, which is exactly why the
                // enhanced branch is this small — steps 1-7 collapsed every emission site into it.
                // A refusal from EmitEnhanced (a state it cannot express, too many entries, a
                // non-transition) falls through to ResourceBarrier: a gap in the translation must
                // never become a LOST barrier.
                bool emitted = false;
                if (graphicsDevice_.UseEnhancedBarriers()) {
                    const auto isBufferFn = [](void* ctx, ID3D12Resource* r) {
                        return static_cast<Renderer*>(ctx)->IsResourceBuffer(r);
                    };
                    emitted = barriers::EmitEnhanced(AsCmdList7(cl), pt.entries, pt.count,
                                                     isBufferFn, this);
                }
                if (!emitted) {
                    cl->ResourceBarrier(pt.count, pt.entries);
                    barriers::NoteLegacyEmit();
                }
                if (render::g_barrierFlipTrace) {
                    char label[96];
                    canonicalStates_.NameOf(res, label, sizeof(label));
                    char m[280];
                    std::snprintf(m, sizeof(m), "[flip] pass=%d point %u/%u (%u barriers) asked %s 0x%X\n",
                                  cb.pass, p, cb.pointCount, pt.count, label, static_cast<unsigned>(after));
                    Renderer::DiagLog(m);
                }
            }
            return;
        }
        // Nothing to emit. Either the compile decided this transition needs no barrier (the
        // resource is already in `after`), or Prepare and the body disagree about what this frame
        // does — and only the second is a bug. The trace prints the CURRENT point so the two can
        // be told apart by eye: a benign miss sits at a point that names other resources, a real
        // one sits at a point that names THIS resource with a different state, or past the end.
        if (render::g_barrierFlipTrace) {
            std::uint32_t cur = cb.pointCount;
            for (std::uint32_t p = 0; p < cb.pointCount; ++p) {
                if (!cb.points[p].emitted.load(std::memory_order_relaxed)) { cur = p; break; }
            }
            char label[96];
            canonicalStates_.NameOf(res, label, sizeof(label));
            char detail[320] = {};
            int off = 0;
            if (cur < cb.pointCount) {
                const CompiledBarriers::Point& pt = cb.points[cur];
                for (std::uint32_t i2 = 0; i2 < pt.count && off >= 0 && off < static_cast<int>(sizeof(detail)) - 1; ++i2) {
                    char n2[96];
                    canonicalStates_.NameOf(pt.entries[i2].Transition.pResource, n2, sizeof(n2));
                    off += std::snprintf(detail + off, sizeof(detail) - static_cast<size_t>(off), "%s%s:0x%X->0x%X",
                                         (i2 == 0) ? "" : ", ", n2,
                                         static_cast<unsigned>(pt.entries[i2].Transition.StateBefore),
                                         static_cast<unsigned>(pt.entries[i2].Transition.StateAfter));
                }
            }
            char m[520];
            std::snprintf(m, sizeof(m), "[flip-miss] pass=%d res=%s want=0x%X cur=%u/%u [%s]\n",
                          cb.pass, label, static_cast<unsigned>(after), cur, cb.pointCount, detail);
            Renderer::DiagLog(m);
        }
        cb.unmatched.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Nothing may reach here. `Renderer::Transition` is the RENDER GRAPH's entry point: it emits
    // the barriers the compile produced for the pass recording on this thread. A call for a
    // resource the compile does not model, or from outside a pass body, has no barrier to emit and
    // used to fall through to ResourceStateTracker — which is now deleted, so falling through
    // would SILENTLY DROP the barrier. Out-of-graph code uses TransitionExplicit and supplies its
    // own before-state. Measured EMPTY before the tracker was removed (the last client was the
    // backbuffer resolve in Pass_Tonemap); loud rather than silent if that ever changes.
    char label[96];
    canonicalStates_.NameOf(res, label, sizeof(label));
    char msg[240];
    std::snprintf(msg, sizeof(msg),
                  "Renderer::Transition: no compiled barrier for res=%s after=0x%X "
                  "(undeclared/unmanaged, or called outside a render-graph pass) - "
                  "use TransitionExplicit instead",
                  label, static_cast<unsigned>(after));
    RendererInvariantFailure(msg);
}

// Pass-flow S1 (docs/render_graph_pass_flow_plan.md): the marker realization of the dormant
// ctx.Barrier(cl, point) design — see the header comment for the contract.
// Async-compute step 5 (D6) — the EMISSION-side backstop for queue legality.
//
// The registration-time rule in RenderGraph::Use is the primary check; this is the one that cannot
// be bypassed, because it reads the queue off the COMMAND LIST ITSELF rather than off anything the
// pass declared. Same trick as step 3's queue labelling, and same reason: the truth is in the thing
// doing the recording, so nothing has to be plumbed and nothing can drift.
//
// ONE function, called from BOTH emission sites. Step 5 first guarded only `Transition` and missed
// `EmitPoint` — and `EmitPoint` is the path every AddPass2 body actually takes, so the backstop was
// unreachable in practice. Compiled barriers reach the GPU from exactly these two places; a check
// that lives in one of them is not a check.
void Renderer::CheckCompiledPointOnQueue(ID3D12GraphicsCommandList* cl,
                                         const CompiledBarriers::Point& pt, int pass) const
{
    if (cl == nullptr || cl->GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE) { return; }
    for (std::uint32_t i = 0; i < pt.count; ++i) {
        const D3D12_RESOURCE_BARRIER& b = pt.entries[i];
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) { continue; }
        if (!barriers::IsDirectQueueExclusiveState(b.Transition.StateAfter) &&
            !barriers::IsDirectQueueExclusiveState(b.Transition.StateBefore)) {
            continue;
        }
        char label[96];
        canonicalStates_.NameOf(b.Transition.pResource, label, sizeof(label));
        char m[320];
        std::snprintf(m, sizeof(m),
                      "Renderer: compiled barrier for res=%s (0x%X -> 0x%X) is being emitted on a "
                      "COMPUTE command list, but that state is DIRECT-queue only (pass=%d).",
                      label,
                      static_cast<unsigned>(b.Transition.StateBefore),
                      static_cast<unsigned>(b.Transition.StateAfter), pass);
        RendererInvariantFailure(m);
    }
}

void Renderer::EmitPoint(ID3D12GraphicsCommandList* cl, std::uint32_t point) {
    CompiledBarriers* installed = CurrentThreadCompiledBarriers();
    if (installed == nullptr || cl == nullptr) {
        RendererInvariantFailure(
            "Renderer::EmitPoint: no compiled barriers installed on this thread - "
            "a marker outside a converted render-graph pass body");
        return;
    }
    CompiledBarriers& cb = *installed;
    cb.markerUsed.store(true, std::memory_order_relaxed);
    if (point >= cb.pointCount) {
        char msg[200];
        std::snprintf(msg, sizeof(msg),
                      "Renderer::EmitPoint: pass=%d marker point %u past the compiled count %u - "
                      "Prepare declared fewer points than the body marks",
                      cb.pass, point, cb.pointCount);
        RendererInvariantFailure(msg);
        return;
    }
    // Sweep everything before the marker: an empty point is a pure advance (the compile ate its
    // barriers), a non-empty unemitted one means barriers were skipped — loud, not silent.
    for (std::uint32_t p = 0; p < point; ++p) {
        CompiledBarriers::Point& prev = cb.points[p];
        if (prev.emitted.load(std::memory_order_relaxed)) { continue; }
        if (prev.count == 0) {
            bool notYetEmpty = false;
            prev.emitted.compare_exchange_strong(notYetEmpty, true, std::memory_order_relaxed);
            continue;
        }
        char msg[200];
        std::snprintf(msg, sizeof(msg),
                      "Renderer::EmitPoint: pass=%d marker point %u but earlier non-empty point %u "
                      "was never emitted - barriers would be lost",
                      cb.pass, point, p);
        RendererInvariantFailure(msg);
        return;
    }
    CompiledBarriers::Point& pt = cb.points[point];
    bool notYet = false;
    if (!pt.emitted.compare_exchange_strong(notYet, true, std::memory_order_relaxed)) {
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "Renderer::EmitPoint: pass=%d point %u emitted twice - duplicate marker or a "
                      "named Transition already claimed it",
                      cb.pass, point);
        RendererInvariantFailure(msg);
        return;
    }
    CheckCompiledPointOnQueue(cl, pt, cb.pass);
    if (pt.count > 0) {
        // Same emission as Transition's claimed branch: enhanced when available, legacy fallback
        // so a translation gap can never become a LOST barrier.
        bool emitted = false;
        if (graphicsDevice_.UseEnhancedBarriers()) {
            const auto isBufferFn = [](void* ctx, ID3D12Resource* r) {
                return static_cast<Renderer*>(ctx)->IsResourceBuffer(r);
            };
            emitted = barriers::EmitEnhanced(AsCmdList7(cl), pt.entries, pt.count, isBufferFn, this);
        }
        if (!emitted) {
            cl->ResourceBarrier(pt.count, pt.entries);
            barriers::NoteLegacyEmit();
        }
    }
    if (render::g_barrierFlipTrace) {
        char m[160];
        std::snprintf(m, sizeof(m), "[flip] pass=%d point %u/%u (%u barriers) marker\n",
                      cb.pass, point, cb.pointCount, pt.count);
        Renderer::DiagLog(m);
    }
}

void Renderer::UAVBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* res) {
    if (cl == nullptr || res == nullptr) {
        return;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = res;
    barriers::EmitOne(cl, b);
}

void Renderer::EnsureVsmDummySrvs() {
    if (vsmDummyHeap_) { return; }
    ID3D12Device* dev = GetDevice();
    if (!dev) { return; }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // CPU-only: these are copy SOURCES for StageSrvUavTable
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(vsmDummyHeap_.GetAddressOf()))) || !vsmDummyHeap_) {
        vsmDummyHeap_.Reset();
        return;
    }
    vsmDummyHeap_->SetName(L"VSM.DummySrvs");

    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE base = vsmDummyHeap_->GetCPUDescriptorHandleForHeapStart();

    // Slot 0: null StructuredBuffer<uint> — matches VsmPageTable (t7/t9). Inert (never sampled: the
    // shaders read it only under useVsm/vsmParams != 0, which is false whenever VSM isn't resident).
    vsmDummyBufferSrv_ = base;
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.NumElements = 1;
        d.Buffer.StructureByteStride = 4;
        d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        dev->CreateShaderResourceView(nullptr, &d, vsmDummyBufferSrv_);
    }

    // Slot 1: null Texture2D (R32_FLOAT) — matches VsmPool (t8/t10), D32 since 2026-08-21.
    vsmDummyTexSrv_ = base;
    vsmDummyTexSrv_.ptr += inc;
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Format = DXGI_FORMAT_R32_FLOAT;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(nullptr, &d, vsmDummyTexSrv_);
    }
}

ID3D12CommandSignature* Renderer::GetDrawIndexedCommandSignature() {
    if (drawIndexedCmdSig_) {
        return drawIndexedCmdSig_.Get();
    }
    ID3D12Device* device = GetDevice();
    if (!device) {
        return nullptr;
    }
    // One DRAW_INDEXED argument, no per-draw root-argument changes -> pRootSignature = nullptr.
    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC desc{};
    desc.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    desc.NumArgumentDescs = 1;
    desc.pArgumentDescs = &arg;
    desc.NodeMask = 0;
    if (FAILED(device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(drawIndexedCmdSig_.GetAddressOf())))) {
        drawIndexedCmdSig_.Reset();
        return nullptr;
    }
    drawIndexedCmdSig_->SetName(L"Renderer.DrawIndexedIndirectSig");
    return drawIndexedCmdSig_.Get();
}

void Renderer::ExecuteIndirect(ID3D12GraphicsCommandList* cl, ID3D12CommandSignature* sig,
                               UINT maxCommandCount, ID3D12Resource* argBuffer, UINT64 argOffset,
                               ID3D12Resource* countBuffer, UINT64 countOffset) {
    if (cl == nullptr || sig == nullptr || argBuffer == nullptr) {
        return;
    }
    cl->ExecuteIndirect(sig, maxCommandCount, argBuffer, argOffset, countBuffer, countOffset);
}

void Renderer::RecordBindAndClear(ID3D12GraphicsCommandList* cl) {
    // Barrier: Present -> RenderTarget (for the current back buffer)
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = swapchain_.Backbuffer(currentFrameIndex_);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers::EmitOne(cl, b);

    // RTV/DSV
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = swapchain_.BackbufferRTV(currentFrameIndex_);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = swapchain_.DepthDSV();
    cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    // viewport/scissor
    D3D12_VIEWPORT vp{};
    vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
    vp.Width = static_cast<float>(width_);
    vp.Height = static_cast<float>(height_);
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;

    D3D12_RECT sr{ 0, 0, (LONG)width_, (LONG)height_ };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sr);

    // Clear
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    cl->ClearRenderTargetView(rtv, clear, 0, nullptr);
    cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
}

void Renderer::RecordBindDefaultsNoClear(ID3D12GraphicsCommandList* cl) {
    // Only bind RTV/DSV + viewport/scissor (no barrier or clear)
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = swapchain_.BackbufferRTV(currentFrameIndex_);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = swapchain_.DepthDSV();
    cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    D3D12_VIEWPORT vp{};
    vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
    vp.Width = static_cast<float>(width_);
    vp.Height = static_cast<float>(height_);
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    D3D12_RECT sr{ 0, 0, (LONG)width_, (LONG)height_ };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sr);
}

void Renderer::CreateDeferredTargets(UINT width, UINT height)
{
    const UINT rtWidth = std::max(1u, renderWidth_);
    const UINT rtHeight = std::max(1u, renderHeight_);
    const UINT displayWidth = std::max(1u, width);
    const UINT displayHeight = std::max(1u, height);

    const auto reflectionSize = ComputeReflectionTextureSize(displayWidth, displayHeight);
    reflectionTextureWidth_ = reflectionSize.first > 0 ? reflectionSize.first : 1;
    reflectionTextureHeight_ = reflectionSize.second > 0 ? reflectionSize.second : 1;
    const auto oceanReflectionSize = ComputeScaledTextureSize(displayWidth, displayHeight, oceanReflectionTextureScale_);
    oceanReflectionTextureWidth_ = oceanReflectionSize.first > 0 ? oceanReflectionSize.first : 1;
    oceanReflectionTextureHeight_ = oceanReflectionSize.second > 0 ? oceanReflectionSize.second : 1;

    RenderTargetManager::Formats formats{};
    formats.gb0 = render::kGBuffer0Format;
    formats.gb1 = render::kGBuffer1Format;
    formats.gb2 = render::kGBuffer2Format;
    formats.gbAux = render::kGBufferAuxFormat;
    formats.velocity = render::kGBufferVelocityFormat;
#if WITH_EDITOR
    formats.objectID = render::kObjectIdFormat;
#endif
    formats.depth = render::kDeferredDepthFormat;
    formats.depthSrv = render::kDeferredDepthSrvFormat;
    formats.light = render::kLightTargetFormat;
    formats.sceneColor = render::kSceneColorFormat;
    formats.reflection = render::kReflectionFormat;
    formats.reflectionScratch = render::kReflectionScratchFormat;
    formats.oceanReflection = render::kReflectionFormat;
    formats.backbufferResource = render::kBackbufferResourceFormat;
    formats.gtao = render::kGtaoFormat;
    formats.hzb = render::kHzbFormat;
    formats.bloom = render::kBloomFormat;
    formats.bloomFft = render::kBloomFftFormat;
    formats.debugPreview = render::kDebugPreviewFormat;

    RenderTargetManager::Sizes sizes{};
    sizes.renderWidth = rtWidth;
    sizes.renderHeight = rtHeight;
    sizes.displayWidth = displayWidth;
    sizes.displayHeight = displayHeight;
    sizes.reflectionWidth = reflectionTextureWidth_;
    sizes.reflectionHeight = reflectionTextureHeight_;
    sizes.oceanReflectionWidth = oceanReflectionTextureWidth_;
    sizes.oceanReflectionHeight = oceanReflectionTextureHeight_;
    // P6B: half the RENDER resolution, rounded up so a 1-pixel target never becomes 0.
    sizes.gtaoWidth = std::max(1u, (rtWidth + 1u) / 2u);
    sizes.gtaoHeight = std::max(1u, (rtHeight + 1u) / 2u);
    // P6C: the pyramid deliberately shares mip 0 with the GTAO grid, so a horizon search can move
    // between "the depth I sampled" and "the tile that contains it" without a second mapping.
    sizes.hzbWidth = sizes.gtaoWidth;
    sizes.hzbHeight = sizes.gtaoHeight;
    // P8: half the DISPLAY resolution, not the render one. Bloom runs after the upscaler, on the
    // same image the tonemap reads -- sizing it off `rtWidth` would make the pyramid change shape
    // with the DLSS quality mode while the image it describes did not.
    sizes.bloomWidth = std::max(1u, (displayWidth + 1u) / 2u);
    sizes.bloomHeight = std::max(1u, (displayHeight + 1u) / 2u);
    // P8C-2: the convolution's ALLOCATION ceiling is a HALF-resolution frame (bloom.convPercent
    // 50); the first P8C ran at a quarter, and a 1-2 texel diffraction ray upscaled 4x per axis
    // was a dashed line of squares -- the "ragged crown". The grid is the image rounded UP to a
    // power of two with 25% headroom, and the headroom IS the zero pad: without it the transform's
    // circular convolution would wrap a streak from one screen edge to the other. At 2560x1440
    // this lands on a 1280x720 image inside a 2048x1024 grid (100 MB of grids at FP32 -- the
    // memory/precision pair the plan says to revisit on measurement).
    //
    // These are the MAXIMUM sizes; the per-frame ACTIVE grid follows bloom.convPercent inside
    // Bloom_Convolve, running the transform on a sub-grid of the same textures, so lowering the
    // percent buys the cost back without recreating targets.
    {
        const UINT imageW = std::max(16u, (displayWidth + 1u) / 2u);
        const UINT imageH = std::max(16u, (displayHeight + 1u) / 2u);
        const auto nextPow2 = [](UINT v) {
            UINT p = 16u;
            while (p < v && p < 2048u) { p <<= 1u; }
            return p;
        };
        sizes.bloomFftImageWidth = imageW;
        sizes.bloomFftImageHeight = imageH;
        sizes.bloomFftWidth = nextPow2((imageW * 5u) / 4u);
        sizes.bloomFftHeight = nextPow2((imageH * 5u) / 4u);
        // P8C-2: the lens-flare accumulation target, a quarter of the display -- the scatter's
        // tile grid runs at this resolution too, which is where its cost model starts.
        sizes.lensFlareWidth = std::max(16u, (displayWidth + 3u) / 4u);
        sizes.lensFlareHeight = std::max(16u, (displayHeight + 3u) / 4u);
        // P8C-2l: the streak pyramid shares that quarter-display base -- level 0 is one half,
        // levels 1..N packed into the other, and the halving widths always fit.
        sizes.streakWidth = sizes.lensFlareWidth;
        sizes.streakHeight = sizes.lensFlareHeight;
    }

    rtManager_.Create(GetDevice(), formats, sizes, Declarations());
}

void Renderer::DestroyDeferredTargets() {
    rtManager_.Destroy(Declarations());
#if WITH_EDITOR
    ResetObjectIdPickState();
#endif

    reflectionTextureWidth_ = 1;
    reflectionTextureHeight_ = 1;
    oceanReflectionTextureWidth_ = 1;
    oceanReflectionTextureHeight_ = 1;
}


std::pair<UINT, UINT> Renderer::ComputeScaledTextureSize(UINT referenceWidth, UINT referenceHeight, Math::float2 scale) const
{
    const UINT refWidth = std::max(referenceWidth, 1u);
    const UINT refHeight = std::max(referenceHeight, 1u);

    auto computeDim = [](UINT dim, float scale) -> UINT
    {
        if (scale <= 0.0f)
        {
            return 1u;
        }
        const float scaled = static_cast<float>(dim) * scale;
        const UINT value = static_cast<UINT>(scaled + 0.5f);
        return std::max(1u, value);
    };

    const UINT reflectionWidth = computeDim(refWidth, scale.x);
    const UINT reflectionHeight = computeDim(refHeight, scale.y);
    return { reflectionWidth, reflectionHeight };
}

std::pair<UINT, UINT> Renderer::ComputeReflectionTextureSize(UINT referenceWidth, UINT referenceHeight) const
{
    return ComputeScaledTextureSize(referenceWidth, referenceHeight, reflectionTextureScale_);
}

void Renderer::RecreateDeferredTargets()
{
    if (!GetDevice() || width_ == 0 || height_ == 0)
    {
        return;
    }

    WaitForPreviousFrame();
    DestroyDeferredTargets();
    CreateDeferredTargets(width_, height_);
    RefreshCurrentFrameCaches();
}

void Renderer::SetReflectionTextureScale(Math::float2 scale)
{
    Math::float2 sanitized{ scale.x, scale.y };
    if (sanitized.x < 0.0f) { sanitized.x = 0.0f; }
    if (sanitized.y < 0.0f) { sanitized.y = 0.0f; }

    if (sanitized.x == reflectionTextureScale_.x && sanitized.y == reflectionTextureScale_.y)
    {
        return;
    }

    reflectionTextureScale_ = sanitized;

    if (rtManager_.IsCreated())
    {
        RecreateDeferredTargets();
    }
}

UINT Renderer::GetReflectionTextureWidth() const
{
    return std::max(1u, reflectionTextureWidth_);
}

UINT Renderer::GetReflectionTextureHeight() const
{
    return std::max(1u, reflectionTextureHeight_);
}

void Renderer::SetOceanReflectionTextureScale(Math::float2 scale)
{
    Math::float2 sanitized{ scale.x, scale.y };
    if (sanitized.x < 0.0f) { sanitized.x = 0.0f; }
    if (sanitized.y < 0.0f) { sanitized.y = 0.0f; }

    if (sanitized.x == oceanReflectionTextureScale_.x && sanitized.y == oceanReflectionTextureScale_.y)
    {
        return;
    }

    oceanReflectionTextureScale_ = sanitized;

    if (rtManager_.IsCreated())
    {
        RecreateDeferredTargets();
    }
}

UINT Renderer::GetOceanReflectionTextureWidth() const
{
    return std::max(1u, oceanReflectionTextureWidth_);
}

UINT Renderer::GetOceanReflectionTextureHeight() const
{
    return std::max(1u, oceanReflectionTextureHeight_);
}

void Renderer::UpdateRenderResolutionFromScale()
{
    if (dlssHandler_)
    {
        dlssHandler_->RefreshRenderResolution();
        return;
    }

    const float clampedScale = std::clamp(renderResolutionScale_, 0.1f, 1.0f);
    renderResolutionScale_ = clampedScale;
    const float baseWidth = static_cast<float>(std::max(width_, 1u));
    const float baseHeight = static_cast<float>(std::max(height_, 1u));

    renderWidth_ = std::max(1u, static_cast<UINT>(baseWidth * clampedScale + 0.5f));
    renderHeight_ = std::max(1u, static_cast<UINT>(baseHeight * clampedScale + 0.5f));
}

void Renderer::SetRenderResolutionScale(float scale)
{
    float sanitized = scale;
    if (!std::isfinite(sanitized))
    {
        sanitized = 1.0f;
    }
    sanitized = std::clamp(sanitized, 0.1f, 1.0f);

    if (std::abs(sanitized - renderResolutionScale_) < 1e-4f)
    {
        return;
    }

    renderResolutionScale_ = sanitized;
    if (dlssHandler_)
    {
        dlssHandler_->OnRenderResolutionScaleChanged();
    }
    else
    {
        UpdateRenderResolutionFromScale();
        if (rtManager_.IsCreated())
        {
            RecreateDeferredTargets();
        }
    }
}

void Renderer::UpdateDlssSettings()
{
    if (dlssHandler_)
    {
        dlssHandler_->UpdateSettings();
    }
    else
    {
        UpdateRenderResolutionFromScale();
    }
}

void Renderer::AllocateDlssResourcesIfNeeded()
{
    if (dlssHandler_)
    {
        dlssHandler_->AllocateResourcesIfNeeded();
    }
}

void Renderer::UpdateDlssCameraData(const Camera& camera)
{
    if (dlssHandler_)
    {
        dlssHandler_->UpdateCameraData(camera);
    }
}

Math::float2 Renderer::GetCameraJitter() const
{
    if (dlssHandler_ && dlssHandler_->IsActive())
    {
        const Math::float2 jitterPixels = dlssHandler_->GetCurrentJitterPixels();
        const float renderWidth = static_cast<float>(std::max(renderWidth_, 1u));
        const float renderHeight = static_cast<float>(std::max(renderHeight_, 1u));
        return Math::float2(jitterPixels.x / renderWidth, jitterPixels.y / renderHeight);
    }

    return Math::float2(0.0f, 0.0f);
}

bool Renderer::EvaluateDLSS(ID3D12GraphicsCommandList* cl)
{
    if (!dlssHandler_)
    {
        return false;
    }

    return dlssHandler_->Evaluate(cl);
}

bool Renderer::IsDlssActive() const
{
    return dlssHandler_ && dlssHandler_->IsActive();
}

bool Renderer::WillEvaluateDlss() const
{
    return dlssHandler_ && dlssHandler_->WillEvaluate();
}

bool Renderer::IsDlssAvailable() const
{
    return dlssHandler_ && dlssHandler_->IsAvailable();
}

D3D12_CPU_DESCRIPTOR_HANDLE Renderer::MakeDebugPreviewSourceSrv(
    ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc)
{
    const D3D12_CPU_DESCRIPTOR_HANDLE handle =
        rtManager_.Deferred(currentFrameIndex_).debugPreviewSrcSRV;
    if (resource && handle.ptr != 0 && GetDevice())
    {
        GetDevice()->CreateShaderResourceView(resource, &desc, handle);
    }
    return handle;
}

void Renderer::SetJitterPaused(bool paused)
{
    if (dlssHandler_) { dlssHandler_->SetJitterPaused(paused); }
}

bool Renderer::IsJitterPaused() const
{
    return dlssHandler_ && dlssHandler_->IsJitterPaused();
}

void Renderer::SetDlssActive(bool active)
{
    if (dlssHandler_)
    {
        dlssHandler_->SetActive(active);
        if (active && dlssMode_ != sl::DLSSMode::eOff)
        {
            UpdateDlssSettings();
            AllocateDlssResourcesIfNeeded();
        }
    }
}

void Renderer::SetDlssMode(sl::DLSSMode mode)
{
    if (dlssMode_ == mode)
    {
        return;
    }

    const UINT previousRenderWidth = renderWidth_;
    const UINT previousRenderHeight = renderHeight_;

    dlssMode_ = mode;

    if (mode == sl::DLSSMode::eOff)
    {
        if (dlssHandler_)
        {
            dlssHandler_->SetActive(false);
        }
        SetRenderResolutionScale(1.0f);
    }

    UpdateDlssSettings();

    if (mode != sl::DLSSMode::eOff)
    {
        AllocateDlssResourcesIfNeeded();

        if (rtManager_.IsCreated() &&
            (renderWidth_ != previousRenderWidth || renderHeight_ != previousRenderHeight))
        {
            RecreateDeferredTargets();
        }
    }
}

void Renderer::BindGBuffer(ID3D12GraphicsCommandList* cl, ClearMode mode) {
    auto& D = rtManager_.Deferred(currentFrameIndex_);
#if WITH_EDITOR
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[6] = {
        D.gbRTV[0], D.gbRTV[1], D.gbRTV[2], D.gbRTV[3], D.objectIDRTV, D.gbAuxRTV
    };
    cl->OMSetRenderTargets(6, rtvs, FALSE, &D.dsv);
#else
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[5] = {
        D.gbRTV[0], D.gbRTV[1], D.gbRTV[2], D.gbRTV[3], D.gbAuxRTV
    };
    cl->OMSetRenderTargets(5, rtvs, FALSE, &D.dsv);
#endif
    cl->OMSetStencilRef(0);

    D3D12_VIEWPORT vp{ 0,0,float(renderWidth_),float(renderHeight_),0,1 };
    D3D12_RECT     sr{ 0,0,(LONG)renderWidth_,(LONG)renderHeight_ };
    cl->RSSetViewports(1, &vp); cl->RSSetScissorRects(1, &sr);

    if (mode != ClearMode::None) {
        const float c[4]{ 0,0,0,0 };
        for (int i = 0; i < 4; ++i) {
            cl->ClearRenderTargetView(rtvs[i], c, 0, nullptr);
        }
#if WITH_EDITOR
        cl->ClearRenderTargetView(rtvs[4], c, 0, nullptr);
        constexpr int kAuxRtvIndex = 5;
#else
        constexpr int kAuxRtvIndex = 4;
#endif
        const float auxNeutral[4]{ 1,1,0,0 };
        cl->ClearRenderTargetView(rtvs[kAuxRtvIndex], auxNeutral, 0, nullptr);
        if (mode == ClearMode::ColorDepth)
        {
            cl->ClearDepthStencilView(D.dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, nullptr);
        }
    }
}

void Renderer::BindLightTarget(ID3D12GraphicsCommandList* cl, ClearMode mode, bool withDepth) {
    auto& D = rtManager_.Deferred(currentFrameIndex_);
    cl->OMSetRenderTargets(1, &D.lightRTV, FALSE, withDepth ? &D.dsv : nullptr);
    D3D12_VIEWPORT vp{ 0,0,float(renderWidth_),float(renderHeight_),0,1 };
    D3D12_RECT     sr{ 0,0,(LONG)renderWidth_,(LONG)renderHeight_ };
    cl->RSSetViewports(1, &vp); cl->RSSetScissorRects(1, &sr);
    if (mode != ClearMode::None) {
        const float c[4]{ 0,0,0,0 };
        cl->ClearRenderTargetView(D.lightRTV, c, 0, nullptr);
        if (mode == ClearMode::ColorDepth && withDepth)
        {
            cl->ClearDepthStencilView(D.dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        }
    }
}

void Renderer::BindSceneColor(ID3D12GraphicsCommandList* cl, ClearMode mode, bool withDepth) {
    auto& D = rtManager_.Deferred(currentFrameIndex_);
    cl->OMSetRenderTargets(1, &D.sceneRTV, FALSE, withDepth ? &D.dsv : nullptr);
    D3D12_VIEWPORT vp{ 0,0,float(renderWidth_),float(renderHeight_),0,1 };
    D3D12_RECT     sr{ 0,0,(LONG)renderWidth_,(LONG)renderHeight_ };
    cl->RSSetViewports(1, &vp); cl->RSSetScissorRects(1, &sr);
    if (mode != ClearMode::None) {
        const float c[4]{ 0,0,0,0 };
        cl->ClearRenderTargetView(D.sceneRTV, c, 0, nullptr);
        if (mode == ClearMode::ColorDepth && withDepth) {
            cl->ClearDepthStencilView(D.dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        }
    }
}

void Renderer::BindLightTargetWithVelocity(ID3D12GraphicsCommandList* cl, ClearMode mode, bool withDepth) {
    auto& D = rtManager_.Deferred(currentFrameIndex_);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = { D.lightRTV, D.gbRTV[3] };
    cl->OMSetRenderTargets(2, rtvs, FALSE, withDepth ? &D.dsv : nullptr);
    D3D12_VIEWPORT vp{ 0,0,float(renderWidth_),float(renderHeight_),0,1 };
    D3D12_RECT     sr{ 0,0,(LONG)renderWidth_,(LONG)renderHeight_ };
    cl->RSSetViewports(1, &vp); cl->RSSetScissorRects(1, &sr);
    if (mode != ClearMode::None) {
        const float c[4]{ 0,0,0,0 };
        cl->ClearRenderTargetView(rtvs[0], c, 0, nullptr);
        cl->ClearRenderTargetView(rtvs[1], c, 0, nullptr);
        if (mode == ClearMode::ColorDepth && withDepth)
        {
            cl->ClearDepthStencilView(D.dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        }
    }
}

void Renderer::BindSceneColorWithVelocity(ID3D12GraphicsCommandList* cl, ClearMode mode, bool withDepth) {
    auto& D = rtManager_.Deferred(currentFrameIndex_);
#if WITH_EDITOR
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] = { D.sceneRTV, D.gbRTV[3], D.objectIDRTV };
    cl->OMSetRenderTargets(3, rtvs, FALSE, withDepth ? &D.dsv : nullptr);
#else
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = { D.sceneRTV, D.gbRTV[3] };
    cl->OMSetRenderTargets(2, rtvs, FALSE, withDepth ? &D.dsv : nullptr);
#endif
    D3D12_VIEWPORT vp{ 0,0,float(renderWidth_),float(renderHeight_),0,1 };
    D3D12_RECT     sr{ 0,0,(LONG)renderWidth_,(LONG)renderHeight_ };
    cl->RSSetViewports(1, &vp); cl->RSSetScissorRects(1, &sr);
    if (mode != ClearMode::None) {
        const float c[4]{ 0,0,0,0 };
        cl->ClearRenderTargetView(rtvs[0], c, 0, nullptr);
        cl->ClearRenderTargetView(rtvs[1], c, 0, nullptr);
#if WITH_EDITOR
        cl->ClearRenderTargetView(rtvs[2], c, 0, nullptr);
#endif
        if (mode == ClearMode::ColorDepth && withDepth)
        {
            cl->ClearDepthStencilView(D.dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        }
    }
}

void Renderer::BindShadowTarget(ID3D12GraphicsCommandList* cl, int cascadeIndex, bool clearDepth)
{
    auto& D = rtManager_.Deferred(currentFrameIndex_);

    // Single DSV for the entire atlas
    cl->OMSetRenderTargets(0, nullptr, FALSE, &D.shadowDSV);

    if (!clearDepth)
    {
	    const float tile = float(D.shadowRes) * 0.5f; // 2048
           // Layout: 0:(0,0)  1:(2048,0)  2:(0,2048)
    	float topLeftX = 0.0f;
    	float topLeftY = 0.0f;
    	if (cascadeIndex == 1) { topLeftX = tile; topLeftY = 0.0f; }
    	if (cascadeIndex == 2) { topLeftX = 0.0f; topLeftY = tile; }
    	if (cascadeIndex == 3) { topLeftX = tile; topLeftY = tile; }

    	D3D12_VIEWPORT vp{ topLeftX, topLeftY, tile, tile, 0.0f, 1.0f };
    	D3D12_RECT sc{ (LONG)topLeftX, (LONG)topLeftY, (LONG)(topLeftX + tile), (LONG)(topLeftY + tile) };
    	cl->RSSetViewports(1, &vp);
    	cl->RSSetScissorRects(1, &sc);
    }
	else
    {
        cl->ClearDepthStencilView(D.shadowDSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }
}

void Renderer::BindSpotShadowTarget(ID3D12GraphicsCommandList* cl, UINT lightIndex, bool clearDepth)
{
    auto& D = rtManager_.Deferred(currentFrameIndex_);
    if (lightIndex >= LightManager::kMaxShadowedSpotLights)
    {
        lightIndex = LightManager::kMaxShadowedSpotLights - 1;
    }

    cl->OMSetRenderTargets(0, nullptr, FALSE, &D.spotShadowDSV[lightIndex]);

    const float res = static_cast<float>(std::max(D.spotShadowRes, 1u));
    D3D12_VIEWPORT vp{ 0.0f, 0.0f, res, res, 0.0f, 1.0f };
    D3D12_RECT sc{ 0, 0, static_cast<LONG>(res), static_cast<LONG>(res) };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);

    if (clearDepth)
    {
        cl->ClearDepthStencilView(D.spotShadowDSV[lightIndex], D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }
}

void Renderer::BindPointShadowTarget(ID3D12GraphicsCommandList* cl, UINT cubeSlot, UINT face, bool clear)
{
    auto& D = rtManager_.Deferred(currentFrameIndex_);
    if (cubeSlot >= LightManager::kMaxShadowedPointLights)
    {
        cubeSlot = LightManager::kMaxShadowedPointLights - 1;
    }
    if (face >= 6) { face = 5; }
    const UINT faceIndex = cubeSlot * 6u + face;

    // Depth-cube: this cube face IS a depth slice — bind it as the DSV (no color RT),
    // exactly like BindSpotShadowTarget binds a spot atlas slice.
    cl->OMSetRenderTargets(0, nullptr, FALSE, &D.pointShadowDSV[faceIndex]);

    const float res = static_cast<float>(std::max(D.pointShadowRes, 1u));
    D3D12_VIEWPORT vp{ 0.0f, 0.0f, res, res, 0.0f, 1.0f };
    D3D12_RECT sc{ 0, 0, static_cast<LONG>(res), static_cast<LONG>(res) };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);

    if (clear)
    {
        cl->ClearDepthStencilView(D.pointShadowDSV[faceIndex], D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }
}

D3D12_GPU_DESCRIPTOR_HANDLE Renderer::StageGBufferSrvTable() {
    auto& D = rtManager_.Deferred(currentFrameIndex_);
    auto tbl = StageSrvUavTable({ D.gbSRV[0], D.gbSRV[1], D.gbSRV[2], D.depthSRV });
    return tbl.gpu; // shader key t0
}
D3D12_GPU_DESCRIPTOR_HANDLE Renderer::StageComposeSrvTable() {
    auto& D = rtManager_.Deferred(currentFrameIndex_);
    auto tbl = StageSrvUavTable({ D.lightSRV, D.gbSRV[2] }); // Light, Emissive
    return tbl.gpu;
}
D3D12_GPU_DESCRIPTOR_HANDLE Renderer::StageTonemapSrvTable() {
    auto& D = rtManager_.Deferred(currentFrameIndex_);
    D3D12_CPU_DESCRIPTOR_HANDLE src = D.sceneSRV;
    if (dlssHandler_ && dlssHandler_->ShouldUseUpscaledOutput() && D.dlssOutputSRV.ptr != 0)
    {
        src = D.dlssOutputSRV;
    }
    auto tbl = StageSrvUavTable({ src });
    return tbl.gpu;
}

D3D12_CPU_DESCRIPTOR_HANDLE Renderer::GetTonemapSourceSrvCPU() const
{
    const auto& D = rtManager_.Deferred(currentFrameIndex_);
    if (dlssHandler_ && dlssHandler_->ShouldUseUpscaledOutput() && D.dlssOutputSRV.ptr != 0)
    {
        return D.dlssOutputSRV;
    }
    return D.sceneSRV;
}

