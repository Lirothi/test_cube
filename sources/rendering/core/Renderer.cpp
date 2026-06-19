#include "rendering/core/Renderer.h"
#include "rendering/core/RendererInvariantFailure.h"
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

void Renderer::Shutdown()
{
    // Guard against repeated calls
    static bool inShutdown = false;
    if (inShutdown) {
        return;
    }
    inShutdown = true;

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

    ShutdownImGui();

    debugDrawSystem_.Shutdown();
    materialManager_.Clear();
    materialDataManager_.ClearAll();
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

    // 3) Back buffers and RTV/DSV heaps
    swapchain_.ReleaseBuffers();

    // 4) Safety measure: clear resource state tracking
    stateTracker_.ClearAllKnownStates();

    // 5) SwapChain — exit fullscreen (if needed) and release it
    swapchain_.ReleaseSwapchain();

    // 6) Frame resources: reset pool usage and clear the upload ring
    // (the actual ComPtrs release when Renderer is destroyed, but this removes dependencies)
    frameScheduler_.ResetFrameState(GetDevice());

    // 7) Fence/Queue
    frameScheduler_.ReleaseFence();
    graphicsDevice_.ReleaseQueue();

    if (dlssHandler_)
    {
        dlssHandler_->Shutdown();
    }

    slShutdown();

    // 8) Release the device last
    graphicsDevice_.ReleaseDevice();

    inShutdown = false;
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
    pref.logMessageCallback = &logFunctionCallback;
    pref.logLevel = sl::LogLevel::eDefault;
	//pref.logLevel = sl::LogLevel::eOff;
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
    //if (auto* def = fontManager_.Get(L"Consolas_32"))
    if (auto* def = fontManager_.Get(L"Consolas_32_coverage"))
    //if (auto* def = fontManager_.Get(L"cons_32"))
    {
        textManager_.SetFont(def);
    }
    auto shadowDesc = TextManager::ShadowDesc();
    shadowDesc.offsetX = 4.0f;
    shadowDesc.offsetY = 4.0f;
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
    }
}

void Renderer::CreateDepthResources(UINT width, UINT height) {
    swapchain_.CreateDepth(GetDevice(), width, height, render::kDepthBufferResourceFormat, render::kDepthBufferViewFormat);
}

void Renderer::WaitForFrame(UINT frameIndex) {
    frameScheduler_.WaitForFrame(frameIndex);
}

void Renderer::SignalFrame(UINT frameIndex) {
    frameScheduler_.SignalFrame(GetCommandQueue(), frameIndex);
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

void Renderer::BeginFrame() {
    CPU_SCOPE(ProfilerScopes::kRendererBeginFrame);
    // Wait for the GPU using its back buffer fence value
    WaitForFrame(currentFrameIndex_);

    if (dlssHandler_)
    {
        dlssHandler_->OnBeginFrame();
    }

    RefreshCurrentFrameCaches();
    FrameResource* fr = currentFrameResource_;

    ++totalFrameNumber_;

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
}

void Renderer::EndFrame() {
    ExecuteTimelineAndPresent();
}

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
    imguiLayer_.Render(commandList);
}

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

size_t Renderer::BeginSubmitBatch() {
    return submitTimeline_.BeginBatch();
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

    submitListsScratch_.clear();

    // Gather batches in order
    {
        CPU_SCOPE(ProfilerScopes::kService1);
        submitTimeline_.GatherFrameLists(submitListsScratch_, [this]() {
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

    fixedSubmitScratch_.clear();
    fixedSubmitScratch_.reserve(submitListsScratch_.size() * 2 + 3);

    // Acquire a fresh, open DIRECT command list from this frame's pool.
    auto acquireDirectCL = [this]() -> ID3D12GraphicsCommandList* {
        FrameResource* fr = frameScheduler_.GetFrameResource(currentFrameIndex_);
        ID3D12CommandAllocator* alloc =
            fr->AcquireCommandAllocator(GetDevice(), D3D12_COMMAND_LIST_TYPE_DIRECT);
        return fr->AcquireCommandList(GetDevice(), D3D12_COMMAND_LIST_TYPE_DIRECT, alloc);
    };

#if PROF_GPU_ENABLED
    {
        // GPU-profiler frame-begin: first list submitted this frame.
        ID3D12GraphicsCommandList* cl = acquireDirectCL();
        Profiler::Get().BeginGpuFrame(cl);
        ThrowIfFailed(cl->Close());
        fixedSubmitScratch_.push_back(cl);
    }
#endif

    {
        CPU_SCOPE(ProfilerScopes::kService2);

        // Step 4: work lists are already closed (directs at EndThreadCommandList,
        // drivers at gather time). For each in submission order, compute its
        // acquire barriers against the global state and, when non-empty, record
        // them into a small DEDICATED command list submitted immediately before
        // that work list. Per-list (never one prologue per batch): a later list
        // in a batch may transition out of an earlier list's final state, which
        // a single aggregated prologue could not represent.
        const ResourceStateTracker::CLState* previousState = nullptr;

        for (auto* cmd : submitListsScratch_) {
            const ResourceStateTracker::CLState* currentState = stateTracker_.FindCLStateForCmd(cmd);

            // Fold the previous list's final states into the global map first,
            // then resolve this list's acquire barriers against it.
            if (previousState) {
                stateTracker_.ApplyFinalStates(*previousState);
            }

            barrierScratch_.clear();
            if (currentState) {
                stateTracker_.AppendAcquireBarriers(*currentState, barrierScratch_);
            }

            if (!barrierScratch_.empty()) {
                ID3D12GraphicsCommandList* acquire = acquireDirectCL();
                acquire->ResourceBarrier(static_cast<UINT>(barrierScratch_.size()), barrierScratch_.data());
                ThrowIfFailed(acquire->Close());
                fixedSubmitScratch_.push_back(acquire);
            }

            fixedSubmitScratch_.push_back(cmd);
            previousState = currentState;
        }

        if (previousState) {
            stateTracker_.ApplyFinalStates(*previousState);
        }

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

        epilogueCmd->ResourceBarrier(1, &presentBarrier);
        SetResourceState(backbuffer, D3D12_RESOURCE_STATE_PRESENT);
#if PROF_GPU_ENABLED
        Profiler::Get().EndGpuFrame(epilogueCmd);
#endif
        ThrowIfFailed(epilogueCmd->Close());
        fixedSubmitScratch_.push_back(epilogueCmd);

        stateTracker_.SetKnownStateDirect(backbuffer, D3D12_RESOURCE_STATE_PRESENT);
    }

    {
        CPU_SCOPE(ProfilerScopes::kService3);
        if (!fixedSubmitScratch_.empty()) {
            GetCommandQueue()->ExecuteCommandLists(static_cast<UINT>(fixedSubmitScratch_.size()), fixedSubmitScratch_.data());
        }
    }

    stateTracker_.ResetLanesForFrame();

    {
        CPU_SCOPE(ProfilerScopes::kService4);
        swapchain_.Present();
    }
    SignalFrame(currentFrameIndex_);
    currentFrameIndex_ = swapchain_.CurrentBackBufferIndex();
    RefreshCurrentFrameCaches();
}

void Renderer::WaitForPreviousFrame() {
    // Fully wait for the GPU (for resize/destructor)
    frameScheduler_.WaitForGpuIdle(GetCommandQueue());
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

    RefreshCurrentFrameCaches();
}

void Renderer::SetResourceState(ID3D12Resource* res, D3D12_RESOURCE_STATES state) {
    stateTracker_.SetResourceState(res, state);
}

void Renderer::ClearResourceState(ID3D12Resource* res) {
    stateTracker_.ClearResourceState(res);
}

void Renderer::Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES after) {
    //CPU_SCOPE(ProfilerScopes::kRendererTransition);
    stateTracker_.Transition(cl, res, after);
}

void Renderer::UAVBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* res) {
    if (cl == nullptr || res == nullptr) {
        return;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = res;
    cl->ResourceBarrier(1, &b);
}

void Renderer::RecordBindAndClear(ID3D12GraphicsCommandList* cl) {
    // Barrier: Present -> RenderTarget (for the current back buffer)
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = swapchain_.Backbuffer(currentFrameIndex_);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
    SetResourceState(swapchain_.Backbuffer(currentFrameIndex_), D3D12_RESOURCE_STATE_RENDER_TARGET);

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

    const auto ssrSize = ComputeSsrTextureSize(rtWidth, rtHeight);
    ssrTextureWidth_ = ssrSize.first > 0 ? ssrSize.first : 1;
    ssrTextureHeight_ = ssrSize.second > 0 ? ssrSize.second : 1;

    RenderTargetManager::Formats formats{};
    formats.gb0 = render::kGBuffer0Format;
    formats.gb1 = render::kGBuffer1Format;
    formats.gb2 = render::kGBuffer2Format;
    formats.velocity = render::kGBufferVelocityFormat;
    formats.depth = render::kDeferredDepthFormat;
    formats.depthSrv = render::kDeferredDepthSrvFormat;
    formats.light = render::kLightTargetFormat;
    formats.sceneColor = render::kSceneColorFormat;
    formats.dlssBias = render::kDlssBiasFormat;
    formats.ssr = render::kSsrFormat;
    formats.ssrBlur = render::kSsrBlurFormat;
    formats.backbufferResource = render::kBackbufferResourceFormat;

    RenderTargetManager::Sizes sizes{};
    sizes.renderWidth = rtWidth;
    sizes.renderHeight = rtHeight;
    sizes.displayWidth = std::max(1u, width);
    sizes.displayHeight = std::max(1u, height);
    sizes.ssrWidth = ssrTextureWidth_;
    sizes.ssrHeight = ssrTextureHeight_;

    rtManager_.Create(GetDevice(), formats, sizes, stateTracker_);
}

void Renderer::DestroyDeferredTargets() {
    rtManager_.Destroy(stateTracker_);

    ssrTextureWidth_ = 1;
    ssrTextureHeight_ = 1;
}


std::pair<UINT, UINT> Renderer::ComputeSsrTextureSize(UINT baseWidth, UINT baseHeight) const
{
    const UINT refWidth = std::max(baseWidth, 1u);
    const UINT refHeight = std::max(baseHeight, 1u);

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

    const UINT ssrWidth = computeDim(refWidth, ssrTextureScale_.x);
    const UINT ssrHeight = computeDim(refHeight, ssrTextureScale_.y);
    return { ssrWidth, ssrHeight };
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
}

void Renderer::SetSsrTextureScale(Math::float2 scale)
{
    Math::float2 sanitized{ scale.x, scale.y };
    if (sanitized.x < 0.0f) { sanitized.x = 0.0f; }
    if (sanitized.y < 0.0f) { sanitized.y = 0.0f; }

    if (sanitized.x == ssrTextureScale_.x && sanitized.y == ssrTextureScale_.y)
    {
        return;
    }

    ssrTextureScale_ = sanitized;

    if (rtManager_.IsCreated())
    {
        RecreateDeferredTargets();
    }
}

UINT Renderer::GetSsrTextureWidth() const
{
    return std::max(1u, ssrTextureWidth_);
}

UINT Renderer::GetSsrTextureHeight() const
{
    return std::max(1u, ssrTextureHeight_);
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
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4] = { D.gbRTV[0], D.gbRTV[1], D.gbRTV[2], D.gbRTV[3] };
    cl->OMSetRenderTargets(4, rtvs, FALSE, &D.dsv);

    D3D12_VIEWPORT vp{ 0,0,float(renderWidth_),float(renderHeight_),0,1 };
    D3D12_RECT     sr{ 0,0,(LONG)renderWidth_,(LONG)renderHeight_ };
    cl->RSSetViewports(1, &vp); cl->RSSetScissorRects(1, &sr);

    if (mode != ClearMode::None) {
        const float c[4]{ 0,0,0,0 };
        for (int i = 0; i < 4; ++i) {
            cl->ClearRenderTargetView(rtvs[i], c, 0, nullptr);
        }
        if (mode == ClearMode::ColorDepth)
        {
            cl->ClearDepthStencilView(D.dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
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
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] = { D.sceneRTV, D.gbRTV[3], D.dlssBiasRTV };
    cl->OMSetRenderTargets(3, rtvs, FALSE, withDepth ? &D.dsv : nullptr);
    D3D12_VIEWPORT vp{ 0,0,float(renderWidth_),float(renderHeight_),0,1 };
    D3D12_RECT     sr{ 0,0,(LONG)renderWidth_,(LONG)renderHeight_ };
    cl->RSSetViewports(1, &vp); cl->RSSetScissorRects(1, &sr);
    if (mode != ClearMode::None) {
        const float c[4]{ 0,0,0,0 };
        cl->ClearRenderTargetView(rtvs[0], c, 0, nullptr);
        cl->ClearRenderTargetView(rtvs[1], c, 0, nullptr);
        cl->ClearRenderTargetView(rtvs[2], c, 0, nullptr);
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
    if (lightIndex >= LightManager::kMaxSpotLights)
    {
        lightIndex = LightManager::kMaxSpotLights - 1;
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

