#include "rendering/core/Renderer.h"
#include "core/Helpers.h"
#include <cassert>
#include <vector>
#include <dxgidebug.h>
#pragma comment(lib, "dxguid.lib")
#include <d3d12sdklayers.h> // ID3D12Debug*, ID3D12InfoQueue
#include <mimalloc.h>
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "app/Systems.h"

thread_local uint32_t Renderer::tlLaneIndex_ = UINT32_MAX;
thread_local Renderer::CLStateEntry* Renderer::tlCurrentEntry_ = nullptr;

Renderer::Renderer()
{

}

Renderer::~Renderer() {
    // Report after everything has been reset
    ReportLiveObjects(); // when the debug build includes this helper

    // Close the event at the very end
    if (fenceEvent_ != nullptr) {
        CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }
}

static void MiOut(const char* msg, void* /*arg*/) { OutputDebugStringA(msg); }

void Renderer::Shutdown()
{
    // Guard against repeated calls
    static bool inShutdown = false;
    if (inShutdown) {
        return;
    }
    inShutdown = true;

    // Nothing to do if the device is missing
    const bool hasDevice = (device_ != nullptr);
    const bool hasQueue = (commandQueue_ != nullptr);
    const bool hasFence = (fence_ != nullptr);

    // 0) Fully wait for the GPU (and close all outstanding command lists)
    if (hasDevice && hasQueue && hasFence) {
        WaitForPreviousFrame(); // your full-synchronization helper :contentReference[oaicite:2]{index=2}
    }

#if PROF_GPU_ENABLED
    Profiler::Get().ShutdownGpu();
#endif

    materialManager_.Clear();
    materialDataManager_.ClearAll();
    meshManager_.Clear();
    textManager_.Clear();
    fontManager_.Clear();
    samplerManager_.Clear();

    // 1) Stop the command “timeline”: prevent further submissions
    {
        std::lock_guard<std::mutex> lk(submitMtx_);
        submitTimeline_.clear(); // PassBatch_ refers only to command lists from the frame pools :contentReference[oaicite:3]{index=3}
    }

    // 2) Offscreen targets (G-Buffer/Light/Scene/Depth) — destroy these first
    DestroyDeferredTargets(); // properly resets resources and heaps, and clears knownStates_ :contentReference[oaicite:4]{index=4}

    // 3) Back buffers and RTV/DSV heaps
    for (UINT i = 0; i < kFrameCount; ++i) {
        renderTargets_[i].Reset();
    }
    depthBuffer_.Reset();
    dsvHeap_.Reset();
    rtvHeap_.Reset();
    rtvDescriptorSize_ = 0;
    dsvDescriptorSize_ = 0;

    // 4) Safety measure: clear resource state tracking
    {
        std::lock_guard<std::mutex> lk(knownStatesMtx_);
        knownStates_.clear();
    }

    // 5) SwapChain — exit fullscreen (if needed) and release it
    if (swapChain_) {
        BOOL fs = FALSE;
        Microsoft::WRL::ComPtr<IDXGIOutput> out;
        if (SUCCEEDED(swapChain_->GetFullscreenState(&fs, &out)) && fs) {
            (void)swapChain_->SetFullscreenState(FALSE, nullptr);
        }
        swapChain_.Reset();
    }

    // 6) Frame resources: reset pool usage and clear the upload ring
    // (the actual ComPtrs release when Renderer is destroyed, but this removes dependencies)
    for (UINT i = 0; i < kFrameCount; ++i) {
        frameResources_[i]->ResetCommandAllocators(device_.Get());
        frameResources_[i]->ResetCommandListsUsage();
        frameResources_[i]->ResetUpload(); // clears fallback chunks and resets per-frame pointers :contentReference[oaicite:5]{index=5}
        frameResources_[i].reset();
        frameFenceValues_[i] = 0;
    }
    nextFenceValue_ = 1;

    // 7) Fence/Queue
    if (fence_) {
        fence_.Reset();
    }
    if (commandQueue_) {
        commandQueue_.Reset();
    }

    // 8) Release the device last
    if (device_) {
        device_.Reset();
    }

    inShutdown = false;

    mi_register_output(MiOut, nullptr);
    mi_collect(true);
    mi_option_set(mi_option_show_stats, 1);
    //mi_stats_print(nullptr);
}

void Renderer::InitD3D12(HWND window) {
    hWnd_ = window;

#ifdef _DEBUG
    {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
            dbg->EnableDebugLayer();
        }
    }
#endif

    // --- Device ---
    ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)));

#ifdef _DEBUG
    {
        ComPtr<ID3D12InfoQueue> info;
        if (SUCCEEDED(device_.As(&info))) {
            info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
            info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
            // Add filters for noisy messages if desired
        }
    }
#endif

    // --- Queue ---
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&commandQueue_)));

    // --- SwapChain + RTVs (kFrameCount) ---
    CreateSwapChainAndRTVs(width_, height_);

    // --- Depth ---
    CreateDepthResources(width_, height_);

    CreateDeferredTargets(width_, height_);

    // --- Fence + event ---
    if (!fence_) {
        ThrowIfFailed(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));
    }
    if (!fenceEvent_) {
        fenceEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent_) {
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
        }
    }

    // --- Frame resources ---
    for (UINT i = 0; i < kFrameCount; ++i) {
        // per-frame shader-visible heaps
        frameResources_[i] = std::make_unique<FrameResource>();
        frameResources_[i]->GetDescAlloc().Init(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096);
        frameResources_[i]->GetSamplerAlloc().Init(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 256);
        frameFenceValues_[i] = 0;
        frameResources_[i]->InitUpload(device_.Get(), /*bytes*/ 4 * 1024 * 1024);
    }

    RefreshCurrentFrameCaches();

    samplerManager_.Init(device_.Get(), 512);

    InitFence();
}

void Renderer::InitFence() {
    // Compatibility with your main.cpp — safe no-op if initialization already happened
    if (!fence_) {
        ThrowIfFailed(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));
    }
    if (!fenceEvent_) {
        fenceEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent_) {
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
        }
    }
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
    shadowDesc.offsetX = 2.0f;
    shadowDesc.offsetY = 2.0f;
    shadowDesc.color.w = 0.9f;
    shadowDesc.scaleWithTextSize = false;
    textManager_.SetShadow(shadowDesc);
}

void Renderer::CreateSwapChainAndRTVs(UINT width, UINT height) {
    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));

    // Destroy the old swap chain and RTVs when reinitializing (if any)
    for (UINT i = 0; i < kFrameCount; ++i) {
        ClearResourceState(renderTargets_[i].Get());
        renderTargets_[i].Reset();
    }
    rtvHeap_.Reset();
    swapChain_.Reset();

    // Create the swap chain (kFrameCount)
    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.BufferCount = kFrameCount;
    scd.Width = width;
    scd.Height = height;
    scd.Format = GetBackbufferResourceFormat();
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.SampleDesc.Count = 1;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    ComPtr<IDXGISwapChain1> swap1;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(
        commandQueue_.Get(), hWnd_, &scd, nullptr, nullptr, &swap1));
    ThrowIfFailed(swap1.As(&swapChain_));

    currentFrameIndex_ = swapChain_->GetCurrentBackBufferIndex();

    // RTV heap
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.NumDescriptors = kFrameCount;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(device_->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap_)));
    rtvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // RTVs
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i) {
        ThrowIfFailed(swapChain_->GetBuffer(i, IID_PPV_ARGS(&renderTargets_[i])));

        D3D12_RENDER_TARGET_VIEW_DESC rtvFmt{};
        rtvFmt.Format = GetBackbufferFormat();        // <- key
        rtvFmt.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rtvFmt.Texture2D.MipSlice   = 0;
        rtvFmt.Texture2D.PlaneSlice = 0;

        device_->CreateRenderTargetView(renderTargets_[i].Get(), &rtvFmt, rtv);
        rtv.ptr += rtvDescriptorSize_;
        SetResourceState(renderTargets_[i].Get(), D3D12_RESOURCE_STATE_PRESENT);
    }
}

void Renderer::CreateDepthResources(UINT width, UINT height) {
    dsvHeap_.Reset();
    depthBuffer_.Reset();

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(device_->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dsvHeap_)));
    dsvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    D3D12_RESOURCE_DESC depthDesc{};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = kDepthBufferResourceFormat;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CLEAR_VALUE cv{};
    cv.Format = kDepthBufferViewFormat;
    cv.DepthStencil.Depth = 1.0f;
    cv.DepthStencil.Stencil = 0;

    ThrowIfFailed(device_->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
        IID_PPV_ARGS(&depthBuffer_)));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsv.Flags = D3D12_DSV_FLAG_NONE;
    device_->CreateDepthStencilView(depthBuffer_.Get(), &dsv, dsvHeap_->GetCPUDescriptorHandleForHeapStart());
}

void Renderer::WaitForFrame(UINT frameIndex) {
    CPU_SCOPE(ProfilerScopes::kRendererWaitForFrame);
    const UINT64 value = frameFenceValues_[frameIndex];
    if (value == 0) {
        return; // frame has not been signaled yet — nothing to wait for
    }
    if (fence_->GetCompletedValue() < value) {
        ThrowIfFailed(fence_->SetEventOnCompletion(value, fenceEvent_));
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
}

void Renderer::SignalFrame(UINT frameIndex) {
    const UINT64 v = nextFenceValue_++;
    ThrowIfFailed(commandQueue_->Signal(fence_.Get(), v));
    frameFenceValues_[frameIndex] = v;
}

void Renderer::RefreshCurrentFrameCaches() {
    currentFrameResource_ = nullptr;
    currentFrameDescriptorHeapCount_ = 0;
    currentFrameDescriptorHeaps_.fill(nullptr);

    if (currentFrameIndex_ >= kFrameCount) {
        return;
    }

    FrameResource* fr = frameResources_[currentFrameIndex_].get();
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

    RefreshCurrentFrameCaches();
    FrameResource* fr = currentFrameResource_;

    ++totalFrameNumber_;

    // Reset per-frame pools
    if (fr) {
        fr->ResetCommandAllocators(device_.Get());
        fr->ResetCommandListsUsage();
        fr->GetDescAlloc().ResetPerFrame();
        fr->GetSamplerAlloc().ResetPerFrame();
        fr->ResetUpload();
    }

    ctxPool_.ResetForFrame();
}

void Renderer::EndFrame() {
    ExecuteTimelineAndPresent();
}

void Renderer::ReportLiveObjects()
{
#if defined(_DEBUG)
    // 1) Detailed report from the device
    if (device_) {
        Microsoft::WRL::ComPtr<ID3D12DebugDevice> ddev;
        if (SUCCEEDED(device_.As(&ddev))) {
            ddev->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL);
        }
    }
    // 2) DXGI report (optional)
    {
        Microsoft::WRL::ComPtr<IDXGIDebug1> dxgiDbg;
        if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDbg)))) {
            dxgiDbg->ReportLiveObjects(DXGI_DEBUG_ALL,
                (DXGI_DEBUG_RLO_FLAGS)(DXGI_DEBUG_RLO_DETAIL | DXGI_DEBUG_RLO_IGNORE_INTERNAL));
        }
    }
#endif
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
        if (materialManager_.ApplyPendingHotReloads(this, totalFrameNumber_, /*keepAliveFrames=*/kFrameCount + 1)) {
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
        fr = frameResources_[currentFrameIndex_].get();
    }
    if (!fr) {
        return {};
    }

    //CPU_SCOPE(L"Renderer::BeginThreadCommandList.1");
    alloc = fr->AcquireCommandAllocator(device_.Get(), type);
    cl = fr->AcquireCommandList(device_.Get(), type, alloc, pso);

    if ((type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE || type == D3D12_COMMAND_LIST_TYPE_BUNDLE) &&
        currentFrameDescriptorHeapCount_ > 0)
    {
        cl->SetDescriptorHeaps(currentFrameDescriptorHeapCount_, currentFrameDescriptorHeaps_.data());
    }

    // Register the command list in the lock-free state tracker
    //RegisterCurrentThreadCL(cl);

    return ThreadCL{ alloc, cl, type };
}

Renderer::ThreadCL Renderer::BeginThreadCommandBundle(ID3D12PipelineState* initialPSO)
{
	return BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_BUNDLE, initialPSO);
}

void Renderer::EndThreadCommandList(ThreadCL& t, size_t batchIndex) {
    CPU_SCOPE(ProfilerScopes::kRendererEndThreadCommandList);
    if (!t.cl) { return; }
    ThrowIfFailed(t.cl->Close());

    {
        std::lock_guard<std::mutex> lk(submitMtx_);
        if (batchIndex < submitTimeline_.size()) {
            auto& b = submitTimeline_[batchIndex];
            b.directs.push_back(t.cl);
        }
    }

    // Clear the TLS binding for this command list
    //UnregisterCurrentThreadCL();

    t.cl = nullptr;
    t.alloc = nullptr;
}

void Renderer::BeginSubmitTimeline() {
    std::lock_guard<std::mutex> lk(submitMtx_);
    submitTimeline_.clear();
}

size_t Renderer::BeginSubmitBatch(const std::string& passName) {
    std::lock_guard<std::mutex> lk(submitMtx_);
    const size_t idx = submitTimeline_.size();
    submitTimeline_.push_back({});
    submitTimeline_.back().name = passName;
    return idx;
}

void Renderer::RegisterPassDriver(ID3D12GraphicsCommandList* cl, size_t batchIndex)
{
    std::lock_guard<std::mutex> lk(submitMtx_);
    if (batchIndex < submitTimeline_.size()) {
        submitTimeline_[batchIndex].driver = cl;
    }
}

void Renderer::EndThreadCommandBundle(ThreadCL& b, size_t batchIndex)
{
    CPU_SCOPE(ProfilerScopes::kRendererEndThreadCommandBundle);
    if (b.cl != nullptr) {
        ThrowIfFailed(b.cl->Close());
        std::lock_guard<std::mutex> lk(submitMtx_);
        if (batchIndex < submitTimeline_.size()) {
            submitTimeline_[batchIndex].bundles.push_back(b.cl);
        }
        b.cl = nullptr;
        b.alloc = nullptr;
    }
}

void Renderer::ExecuteTimelineAndPresent() {
    CPU_SCOPE(ProfilerScopes::kRendererExecuteTimelineAndPresent);

    submitListsScratch_.clear();

    // Gather batches in order
    {
        CPU_SCOPE(ProfilerScopes::kService1);
        std::lock_guard<std::mutex> lk(submitMtx_);
        size_t expectedListCount = 0;
        for (const auto& pb : submitTimeline_) {
            expectedListCount += pb.directs.size();
            if (pb.driver != nullptr || !pb.bundles.empty()) {
                ++expectedListCount;
            }
        }
        submitListsScratch_.reserve(expectedListCount);
        for (auto& pb : submitTimeline_) {
            // If the driver exists (created in the pass) — append ExecuteBundle(...)
            if (pb.driver != nullptr) {
                for (auto* b : pb.bundles) {
                    if (b != nullptr) {
                        pb.driver->ExecuteBundle(b);
                    }
                }
                ThrowIfFailed(pb.driver->Close());
                submitListsScratch_.push_back(pb.driver);
            }
            else if (!pb.bundles.empty()) {
                // Fallback: no driver available — create a temporary one
                auto& fr = frameResources_[currentFrameIndex_];
                ID3D12CommandAllocator* alloc =
                    fr->AcquireCommandAllocator(device_.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
                ID3D12GraphicsCommandList* cl =
                    fr->AcquireCommandList(device_.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, alloc);
                RecordBindDefaultsNoClear(cl);
                for (auto* b : pb.bundles) {
                    if (b != nullptr) {
                        cl->ExecuteBundle(b);
                    }
                }
                ThrowIfFailed(cl->Close());
                submitListsScratch_.push_back(cl);
            }

            // Also attach any prepared DIRECT command lists
            if (!pb.directs.empty()) {
                submitListsScratch_.insert(submitListsScratch_.end(), pb.directs.begin(), pb.directs.end());
            }
        }
        submitTimeline_.clear();
    }

    fixedSubmitScratch_.clear();
    fixedSubmitScratch_.reserve(submitListsScratch_.size() * 2 + 3);
#if PROF_GPU_ENABLED
    {
        auto& fr = frameResources_[currentFrameIndex_];
        ID3D12CommandAllocator* alloc =
            fr->AcquireCommandAllocator(device_.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
        ID3D12GraphicsCommandList* cl =
            fr->AcquireCommandList(device_.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, alloc);
        Profiler::Get().BeginGpuFrame(cl);
        ThrowIfFailed(cl->Close());
        fixedSubmitScratch_.push_back(cl);
    }
#endif

    {
        CPU_SCOPE(ProfilerScopes::kService2);

        for (auto* cmd : submitListsScratch_) {
            const CLState* st = FindCLStateForCmd(cmd);

            // 3.1: if a command list needs transitions on first use — insert a prologue with prev→firstUse barriers
            if (st && !st->firstUse.empty()) {

                // Gather the barrier list
                barrierScratch_.clear();
                barrierScratch_.reserve(st->firstUse.size());

                for (auto& kv : st->firstUse) {
                    ID3D12Resource* res = kv.first;
                    const D3D12_RESOURCE_STATES want = kv.second;

                    D3D12_RESOURCE_STATES before = D3D12_RESOURCE_STATE_COMMON;
                    if (auto ig = knownStates_.find(res); ig != knownStates_.end()) {
                        before = ig->second;
                    }

                    if (before != want) {
                        D3D12_RESOURCE_BARRIER b{};
                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        b.Transition.pResource = res;
                        b.Transition.StateBefore = before;
                        b.Transition.StateAfter = want;
                        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        barrierScratch_.push_back(b);

                    }
                    knownStates_[res] = want;
                }

                // Create the prologue ONLY when there is something to transition
                if (!barrierScratch_.empty()) {
                    auto& fr = frameResources_[currentFrameIndex_];
                    ID3D12CommandAllocator* alloc =
                        fr->AcquireCommandAllocator(device_.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
                    ID3D12GraphicsCommandList* prologue =
                        fr->AcquireCommandList(device_.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, alloc);

                    prologue->ResourceBarrier(static_cast<UINT>(barrierScratch_.size()), barrierScratch_.data());
                    ThrowIfFailed(prologue->Close());
                    fixedSubmitScratch_.push_back(prologue);
                }
            }

            // 3.2: the command list itself
            fixedSubmitScratch_.push_back(cmd);

            // 3.3: after executing, update the global final state of its resources
            if (st && !st->current.empty()) {
                for (auto& kv : st->current) {
                    knownStates_[kv.first] = kv.second;
                }
            }
        }
    }

    // Epilogue: transition RT → Present
    ID3D12GraphicsCommandList* epilogueCL = nullptr;
    {
        auto& fr = frameResources_[currentFrameIndex_];
        ID3D12CommandAllocator* alloc =
            fr->AcquireCommandAllocator(device_.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
        ID3D12GraphicsCommandList* cl =
            fr->AcquireCommandList(device_.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, alloc);

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = renderTargets_[currentFrameIndex_].Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        cl->ResourceBarrier(1, &b);
        SetResourceState(renderTargets_[currentFrameIndex_].Get(), D3D12_RESOURCE_STATE_PRESENT);
#if PROF_GPU_ENABLED
        Profiler::Get().EndGpuFrame(cl);
#endif
        ThrowIfFailed(cl->Close());
        epilogueCL = cl;
    }
    if (epilogueCL)
    {
        fixedSubmitScratch_.push_back(epilogueCL);
    }

	{
        CPU_SCOPE(ProfilerScopes::kService3);
		if (!fixedSubmitScratch_.empty()) {
			commandQueue_->ExecuteCommandLists(static_cast<UINT>(fixedSubmitScratch_.size()), fixedSubmitScratch_.data());
		}
	}

    const uint32_t lanes = clLaneCount_.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < std::min<uint32_t>(lanes, kCLStateLanes); ++i) {
        clLanes_[i].entries.clear();
        clLanes_[i].epoch++;
    }

    {
        CPU_SCOPE(ProfilerScopes::kService4);
        ThrowIfFailed(swapChain_->Present(0, DXGI_PRESENT_ALLOW_TEARING));
    }
    //ThrowIfFailed(swapChain_->Present(1, 0));
    SignalFrame(currentFrameIndex_);
    currentFrameIndex_ = swapChain_->GetCurrentBackBufferIndex();
    RefreshCurrentFrameCaches();
}

void Renderer::WaitForPreviousFrame() {
    // Fully wait for the GPU (for resize/destructor)
    // Signal and wait until the fence reaches the target value
    const UINT64 v = nextFenceValue_++;
    ThrowIfFailed(commandQueue_->Signal(fence_.Get(), v));
    if (fence_->GetCompletedValue() < v) {
        ThrowIfFailed(fence_->SetEventOnCompletion(v, fenceEvent_));
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
}

void Renderer::OnResize(UINT width, UINT height) {
    if (width == 0 || height == 0) {
        return;
    }
    width_ = width;
    height_ = height;

    // Important: wait for the GPU before replacing resources
    WaitForPreviousFrame();

    // Release the old RTV/DSV objects
    for (UINT i = 0; i < kFrameCount; ++i) {
        ClearResourceState(renderTargets_[i].Get());
        renderTargets_[i].Reset();
    }
    ClearResourceState(depthBuffer_.Get());
    depthBuffer_.Reset();
    dsvHeap_.Reset();
    rtvHeap_.Reset();

    // ResizeBuffers
    DXGI_SWAP_CHAIN_DESC desc{};
    ThrowIfFailed(swapChain_->GetDesc(&desc));
    ThrowIfFailed(swapChain_->ResizeBuffers(kFrameCount, width_, height_, desc.BufferDesc.Format, desc.Flags));

    // Recreate RTV and DSV
    CreateSwapChainAndRTVs(width_, height_);
    CreateDepthResources(width_, height_);
    CreateDeferredTargets(width_, height_);

    RefreshCurrentFrameCaches();
}

void Renderer::SetResourceState(ID3D12Resource* res, D3D12_RESOURCE_STATES state) {
    if (res == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lk(knownStatesMtx_);
    knownStates_[res] = state;
}

D3D12_RESOURCE_STATES Renderer::GetGlobalKnownState(ID3D12Resource* res)
{
    std::lock_guard<std::mutex> lk(knownStatesMtx_);
    auto it = knownStates_.find(res);
    return (it == knownStates_.end()) ? D3D12_RESOURCE_STATE_COMMON : it->second;
}

void Renderer::ClearResourceState(ID3D12Resource* res) {
    if (res == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lk(knownStatesMtx_);
    knownStates_.erase(res);
}

void Renderer::Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES after) {
    if (!cl || !res) { return; }
    //CPU_SCOPE(ProfilerScopes::kRendererTransition);
    ID3D12CommandList* base = static_cast<ID3D12CommandList*>(cl);

    // Fast path — the active command list is stored in TLS
    CLStateEntry* entry = tlCurrentEntry_;
    const uint32_t lane = tlLaneIndex_;
    if (entry == nullptr || lane == UINT32_MAX ||
        entry->epoch != clLanes_[lane].epoch || entry->cmd != base) {
        if (lane != UINT32_MAX) {
            CLStateLane& ln = clLanes_[lane];
            auto found = ln.entries.find(base);
            if (found != ln.entries.end()) {
                entry = &found->second;
                entry->epoch = ln.epoch;
                tlCurrentEntry_ = entry;
            } else {
                RegisterCurrentThreadCL(cl);
                entry = tlCurrentEntry_;
            }
        } else {
            // Command list not yet registered on this thread — register it on the fly
            RegisterCurrentThreadCL(cl);
            entry = tlCurrentEntry_;
        }
    }

    if (!entry) {
        return;
    }
    auto& st = entry->st;

    auto itCur = st.current.find(res);
    if (itCur == st.current.end()) {
        // First use in this command list — no intra-CL barrier required
        st.firstUse.emplace(res, after);
        st.current.emplace(res, after);
        return;
    }

    const D3D12_RESOURCE_STATES before = itCur->second;
    if (before == after) { return; }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);

    itCur->second = after;
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
    b.Transition.pResource = renderTargets_[currentFrameIndex_].Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
    SetResourceState(renderTargets_[currentFrameIndex_].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);

    // RTV/DSV
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += SIZE_T(currentFrameIndex_) * rtvDescriptorSize_;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
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
    cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
}

void Renderer::RecordBindDefaultsNoClear(ID3D12GraphicsCommandList* cl) {
    // Only bind RTV/DSV + viewport/scissor (no barrier or clear)
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += SIZE_T(currentFrameIndex_) * rtvDescriptorSize_;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
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

D3D12_CPU_DESCRIPTOR_HANDLE Renderer::DeferredRtvAt(UINT idx) const {
    auto h = deferredRtvHeap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(idx) * deferredRtvIncr_; return h;
}
D3D12_CPU_DESCRIPTOR_HANDLE Renderer::DeferredDsvAt(UINT idx) const {
    auto h = deferredDsvHeap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(idx) * deferredDsvIncr_; return h;
}
D3D12_CPU_DESCRIPTOR_HANDLE Renderer::DeferredSrvAt(UINT idx) const {
    auto h = deferredSrvCpuHeap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(idx) * deferredSrvIncr_; return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE Renderer::DeferredRtvCPU(UINT frame, DeferredRtvSlot slot) const {
    const UINT idx = frame * kDeferredRtvPerFrame + static_cast<UINT>(slot);
    return DeferredRtvAt(idx);
}
D3D12_CPU_DESCRIPTOR_HANDLE Renderer::DeferredSrvCPU(UINT frame, DeferredSrvSlot slot) const {
    const UINT idx = frame * kDeferredSrvPerFrame + static_cast<UINT>(slot);
    return DeferredSrvAt(idx);
}
D3D12_CPU_DESCRIPTOR_HANDLE Renderer::DeferredDsvCPU(UINT frame, DeferredDsvSlot slot) const {
    const UINT idx = frame * kDeferredDsvPerFrame + static_cast<UINT>(slot);
    return DeferredDsvAt(idx);
}
D3D12_CPU_DESCRIPTOR_HANDLE Renderer::DeferredSpotShadowDsvCPU(UINT frame, UINT lightIndex) const {
    const UINT base = frame * kDeferredDsvPerFrame + static_cast<UINT>(DeferredDsvSlot::Count);
    return DeferredDsvAt(base + lightIndex);
}

void Renderer::CreateDeferredTargets(UINT width, UINT height)
{
    // Just in case: release old resources/heaps
    DestroyDeferredTargets();

    ID3D12Device* dev = device_.Get();
    if (!dev) { return; }

    // --- Descriptor increments ---
    deferredRtvIncr_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    deferredDsvIncr_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    deferredSrvIncr_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // --- CPU-only descriptor heaps for offscreen targets (RTV/DSV/SRV) ---
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = kFrameCount * kDeferredRtvPerFrame;  // GB0,GB1,GB2, Light, Scene
        ThrowIfFailed(dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&deferredRtvHeap_)));
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        desc.NumDescriptors = kFrameCount * kDeferredDsvPerFrame;  // Depth
        ThrowIfFailed(dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&deferredDsvHeap_)));
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = kFrameCount * kDeferredSrvPerFrame;  // GB0,GB1,GB2,Depth,Light,LightUAV,Scene,SceneUAV,SSR,SSRBlur,Shadow,SSRUAV,SSRBlurUAV,Tonemap,TonemapUAV
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // CPU-only staging
        ThrowIfFailed(dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&deferredSrvCpuHeap_)));
    }

    // --- Common placement parameters (Default heap) ---
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    auto MakeTex2DDesc = [&](DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = width ? width : 1;
        rd.Height = height ? height : 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = fmt;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = flags;
        return rd;
        };

    // ---- Shared factories ----
    auto CreateRT = [&](DXGI_FORMAT fmt,
        DeferredRtvSlot rtvSlot,
        DeferredSrvSlot srvSlot,
        DeferredSrvSlot uavSlot,
        UINT f,
        ComPtr<ID3D12Resource>& outRes,
        D3D12_CPU_DESCRIPTOR_HANDLE& outRTV,
        D3D12_CPU_DESCRIPTOR_HANDLE& outSRV,
        float4 clear = float4(0, 0, 0, 0))
        {
            D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (uavSlot != DeferredSrvSlot::Count)
            {
                flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            }
            D3D12_RESOURCE_DESC rd = MakeTex2DDesc(fmt, flags);

            D3D12_CLEAR_VALUE cv{}; cv.Format = fmt;
            cv.Color[0] = clear.x; cv.Color[1] = clear.y; cv.Color[2] = clear.z; cv.Color[3] = clear.w;

            ThrowIfFailed(dev->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&outRes)));

            // RTV/SRV — ONLY for frame f
            outRTV = DeferredRtvCPU(f, rtvSlot);
            dev->CreateRenderTargetView(outRes.Get(), nullptr, outRTV);

            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = fmt;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;

            outSRV = DeferredSrvCPU(f, srvSlot);
            dev->CreateShaderResourceView(outRes.Get(), &sd, outSRV);

            D3D12_CPU_DESCRIPTOR_HANDLE outUAV{};
            if (uavSlot != DeferredSrvSlot::Count)
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.Format = fmt;
                ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                outUAV = DeferredSrvCPU(f, uavSlot);
                dev->CreateUnorderedAccessView(outRes.Get(), nullptr, &ud, outUAV);
            }

            // Store the handles in deferred_[f]
            auto& D = deferred_[f];
            switch (rtvSlot) {
            case DeferredRtvSlot::GB0:   D.gbRTV[0] = outRTV; break;
            case DeferredRtvSlot::GB1:   D.gbRTV[1] = outRTV; break;
            case DeferredRtvSlot::GB2:   D.gbRTV[2] = outRTV; break;
            case DeferredRtvSlot::Light: D.lightRTV = outRTV; break;
            case DeferredRtvSlot::Scene: D.sceneRTV = outRTV; break;
            default: break;
            }
            switch (srvSlot) {
            case DeferredSrvSlot::GB0:    D.gbSRV[0] = outSRV; break;
            case DeferredSrvSlot::GB1:    D.gbSRV[1] = outSRV; break;
            case DeferredSrvSlot::GB2:    D.gbSRV[2] = outSRV; break;
            case DeferredSrvSlot::Depth:  D.gbSRV[3] = outSRV; break;
            case DeferredSrvSlot::Light:  D.lightSRV = outSRV; break;
            case DeferredSrvSlot::Scene:  D.sceneSRV = outSRV; break;
            default: break;
            }
            if (uavSlot != DeferredSrvSlot::Count)
            {
                switch (uavSlot)
                {
                case DeferredSrvSlot::LightUAV: D.lightUAV = outUAV; break;
                case DeferredSrvSlot::SceneUAV: D.sceneUAV = outUAV; break;
                default: break;
                }
            }

            SetResourceState(outRes.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        };

    auto CreateSrvUavTexture = [&](DXGI_FORMAT fmt,
        DeferredSrvSlot srvSlot,
        DeferredSrvSlot uavSlot,
        UINT f,
        ComPtr<ID3D12Resource>& outRes,
        D3D12_CPU_DESCRIPTOR_HANDLE& outSRV,
        D3D12_CPU_DESCRIPTOR_HANDLE& outUAV)
        {
            D3D12_RESOURCE_DESC rd = MakeTex2DDesc(fmt, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

            ThrowIfFailed(dev->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&outRes)));

            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = fmt;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;

            outSRV = DeferredSrvCPU(f, srvSlot);
            dev->CreateShaderResourceView(outRes.Get(), &sd, outSRV);

            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = fmt;
            ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

            outUAV = DeferredSrvCPU(f, uavSlot);
            dev->CreateUnorderedAccessView(outRes.Get(), nullptr, &ud, outUAV);

            auto& D = deferred_[f];
            if (srvSlot == DeferredSrvSlot::SSR)
            {
                D.ssrSRV = outSRV;
                D.ssrUAV = outUAV;
            }
            else if (srvSlot == DeferredSrvSlot::SSRBlur)
            {
                D.ssrBlurSRV = outSRV;
                D.ssrBlurUAV = outUAV;
            }
            else if (srvSlot == DeferredSrvSlot::Tonemap)
            {
                D.tonemapSRV = outSRV;
                D.tonemapUAV = outUAV;
            }

            SetResourceState(outRes.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        };

    auto CreateDepth = [&](DXGI_FORMAT dsvFmt,
        UINT f,
        ComPtr<ID3D12Resource>& outRes,
        D3D12_CPU_DESCRIPTOR_HANDLE& outDSV,
        D3D12_CPU_DESCRIPTOR_HANDLE& outDepthSRV)
        {
            D3D12_RESOURCE_DESC rd = MakeTex2DDesc(dsvFmt, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

            D3D12_CLEAR_VALUE cv{}; cv.Format = dsvFmt; cv.DepthStencil.Depth = 1.0f; cv.DepthStencil.Stencil = 0;
            ThrowIfFailed(dev->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&outRes)));

            auto& D = deferred_[f];

            // DSV
            outDSV = DeferredDsvCPU(f, DeferredDsvSlot::Depth);
            D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
            dv.Format = dsvFmt;
            dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            dev->CreateDepthStencilView(outRes.Get(), &dv, outDSV);
            D.dsv = outDSV;

            // Create an SRV for depth as R32_FLOAT
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = GetDepthSrvFormat();
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            outDepthSRV = DeferredSrvCPU(f, DeferredSrvSlot::Depth);
            dev->CreateShaderResourceView(outRes.Get(), &sd, outDepthSRV);
            D.gbSRV[3] = outDepthSRV;

            SetResourceState(outRes.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        };

    auto CreateShadow = [&](UINT f,
        Microsoft::WRL::ComPtr<ID3D12Resource>& outRes,
        D3D12_CPU_DESCRIPTOR_HANDLE& outDSV,
        D3D12_CPU_DESCRIPTOR_HANDLE& outSRV,
        UINT resolution)
        {
            // Shadows use a typeless texture with DSV=D32F and SRV=R32F
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = resolution;
            rd.Height = resolution;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = DXGI_FORMAT_R16_TYPELESS;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

            D3D12_CLEAR_VALUE cv{};
            cv.Format = DXGI_FORMAT_D16_UNORM;
            cv.DepthStencil.Depth = 1.0f;
            cv.DepthStencil.Stencil = 0;

            ThrowIfFailed(dev->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&outRes)));

            // DSV — goes into its dedicated shadow slot
            outDSV = DeferredDsvCPU(f, DeferredDsvSlot::Shadow);
            D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
            dsv.Format = DXGI_FORMAT_D16_UNORM;
            dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            dev->CreateDepthStencilView(outRes.Get(), &dsv, outDSV);

            // SRV — also stored in the shadow slot
            outSRV = DeferredSrvCPU(f, DeferredSrvSlot::Shadow);
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R16_UNORM;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            dev->CreateShaderResourceView(outRes.Get(), &sd, outSRV);

            SetResourceState(outRes.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        };

    auto CreateSpotShadow = [&](UINT frameIndex,
        ComPtr<ID3D12Resource>& outRes,
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, LightManager::kMaxSpotLights>& outDSV,
        D3D12_CPU_DESCRIPTOR_HANDLE& outSRV,
        UINT resolution)
        {
            if (resolution == 0) { resolution = 512; }

            D3D12_CLEAR_VALUE clear{};
            clear.Format = DXGI_FORMAT_D16_UNORM;
            clear.DepthStencil.Depth = 1.0f;
            clear.DepthStencil.Stencil = 0;

            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Alignment = 0;
            desc.Width = resolution;
            desc.Height = resolution;
            desc.DepthOrArraySize = static_cast<UINT16>(LightManager::kMaxSpotLights);
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R16_TYPELESS;
            desc.SampleDesc.Count = 1;
            desc.SampleDesc.Quality = 0;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

            ThrowIfFailed(dev->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(outRes.ReleaseAndGetAddressOf())));

            outSRV = DeferredSrvCPU(frameIndex, DeferredSrvSlot::SpotShadow);
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_R16_UNORM;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2DArray.MipLevels = 1;
            srvDesc.Texture2DArray.FirstArraySlice = 0;
            srvDesc.Texture2DArray.ArraySize = LightManager::kMaxSpotLights;
            srvDesc.Texture2DArray.MostDetailedMip = 0;
            srvDesc.Texture2DArray.PlaneSlice = 0;
            srvDesc.Texture2DArray.ResourceMinLODClamp = 0.0f;
            dev->CreateShaderResourceView(outRes.Get(), &srvDesc, outSRV);

            for (UINT i = 0; i < LightManager::kMaxSpotLights; ++i)
            {
                outDSV[i] = DeferredSpotShadowDsvCPU(frameIndex, i);
                D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
                dsv.Format = DXGI_FORMAT_D16_UNORM;
                dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                dsv.Flags = D3D12_DSV_FLAG_NONE;
                dsv.Texture2DArray.ArraySize = 1;
                dsv.Texture2DArray.FirstArraySlice = i;
                dsv.Texture2DArray.MipSlice = 0;
                dev->CreateDepthStencilView(outRes.Get(), &dsv, outDSV[i]);
            }

            SetResourceState(outRes.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        };

    for (UINT f = 0; f < kFrameCount; ++f)
    {
        auto& D = deferred_[f];

        CreateRT(kGBuffer0Format, DeferredRtvSlot::GB0, DeferredSrvSlot::GB0, DeferredSrvSlot::Count, f, D.gb0, D.gbRTV[0], D.gbSRV[0]);
        CreateRT(kGBuffer1Format, DeferredRtvSlot::GB1, DeferredSrvSlot::GB1, DeferredSrvSlot::Count, f, D.gb1, D.gbRTV[1], D.gbSRV[1]);
        CreateRT(kGBuffer2Format, DeferredRtvSlot::GB2, DeferredSrvSlot::GB2, DeferredSrvSlot::Count, f, D.gb2, D.gbRTV[2], D.gbSRV[2]);

        CreateDepth(kDeferredDepthFormat, f, D.depth, D.dsv, /*outDepthSRV*/ D.gbSRV[3]);

        D.shadowRes = 4096; // could be driven by config/parameter
        CreateShadow(f, D.shadow, D.shadowDSV, D.shadowSRV, D.shadowRes);

        D.spotShadowRes = 512;
        CreateSpotShadow(f, D.spotShadow, D.spotShadowDSV, D.spotShadowSRV, D.spotShadowRes);

        CreateRT(kLightTargetFormat, DeferredRtvSlot::Light, DeferredSrvSlot::Light, DeferredSrvSlot::LightUAV, f, D.light, D.lightRTV, D.lightSRV);
        CreateRT(kSceneColorFormat, DeferredRtvSlot::Scene, DeferredSrvSlot::Scene, DeferredSrvSlot::SceneUAV, f, D.scene, D.sceneRTV, D.sceneSRV);
        CreateSrvUavTexture(kSsrFormat, DeferredSrvSlot::SSR, DeferredSrvSlot::SSRUAV, f, D.ssr, D.ssrSRV, D.ssrUAV);
        CreateSrvUavTexture(kSsrBlurFormat, DeferredSrvSlot::SSRBlur, DeferredSrvSlot::SSRBlurUAV, f, D.ssrBlur, D.ssrBlurSRV, D.ssrBlurUAV);
        CreateSrvUavTexture(kBackbufferResourceFormat, DeferredSrvSlot::Tonemap, DeferredSrvSlot::TonemapUAV, f, D.tonemap, D.tonemapSRV, D.tonemapUAV);
    }
}

void Renderer::DestroyDeferredTargets() {
    deferredRtvHeap_.Reset(); deferredDsvHeap_.Reset(); deferredSrvCpuHeap_.Reset();
    std::vector<ID3D12Resource*> released;
    released.reserve(kFrameCount * Renderer::DeferredTargets::kResourceCount);

    auto collect = [&released](ComPtr<ID3D12Resource>& res) {
        if (ID3D12Resource* ptr = res.Get()) {
            released.push_back(ptr);
            res.Reset();
        }
    };

    for (UINT f = 0; f < kFrameCount; ++f) {
        auto& D = deferred_[f];
        collect(D.gb0);
        collect(D.gb1);
        collect(D.gb2);
        collect(D.depth);
        collect(D.light);
        collect(D.scene);
        collect(D.tonemap);
        collect(D.ssr);
        collect(D.ssrBlur);
        collect(D.shadow);
        collect(D.spotShadow);
    }

    if (!released.empty()) {
        std::lock_guard<std::mutex> lk(knownStatesMtx_);
        for (auto* res : released) {
            knownStates_.erase(res);
        }
    }
}

void Renderer::BindGBuffer(ID3D12GraphicsCommandList* cl, ClearMode mode) {
    auto& D = deferred_[currentFrameIndex_];
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] = { D.gbRTV[0], D.gbRTV[1], D.gbRTV[2] };
    cl->OMSetRenderTargets(3, rtvs, FALSE, &D.dsv);

    D3D12_VIEWPORT vp{ 0,0,float(width_),float(height_),0,1 };
    D3D12_RECT     sr{ 0,0,(LONG)width_,(LONG)height_ };
    cl->RSSetViewports(1, &vp); cl->RSSetScissorRects(1, &sr);

    if (mode != ClearMode::None) {
        const float c[4]{ 0,0,0,0 };
        for (int i = 0; i < 3; ++i) {
            cl->ClearRenderTargetView(rtvs[i], c, 0, nullptr);
        }
        if (mode == ClearMode::ColorDepth)
        {
            cl->ClearDepthStencilView(D.dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        }
    }
}

void Renderer::BindLightTarget(ID3D12GraphicsCommandList* cl, ClearMode mode, bool withDepth) {
    auto& D = deferred_[currentFrameIndex_];
    cl->OMSetRenderTargets(1, &D.lightRTV, FALSE, withDepth ? &D.dsv : nullptr);
    D3D12_VIEWPORT vp{ 0,0,float(width_),float(height_),0,1 };
    D3D12_RECT     sr{ 0,0,(LONG)width_,(LONG)height_ };
    cl->RSSetViewports(1, &vp); cl->RSSetScissorRects(1, &sr);
    if (mode != ClearMode::None) {
        const float c[4]{ 0,0,0,0 };
        cl->ClearRenderTargetView(D.lightRTV, c, 0, nullptr);
    }
}

void Renderer::BindSceneColor(ID3D12GraphicsCommandList* cl, ClearMode mode, bool withDepth) {
    auto& D = deferred_[currentFrameIndex_];
    cl->OMSetRenderTargets(1, &D.sceneRTV, FALSE, withDepth ? &D.dsv : nullptr);
    D3D12_VIEWPORT vp{ 0,0,float(width_),float(height_),0,1 };
    D3D12_RECT     sr{ 0,0,(LONG)width_,(LONG)height_ };
    cl->RSSetViewports(1, &vp); cl->RSSetScissorRects(1, &sr);
    if (mode != ClearMode::None) {
        const float c[4]{ 0,0,0,0 };
        cl->ClearRenderTargetView(D.sceneRTV, c, 0, nullptr);
        if (mode == ClearMode::ColorDepth && withDepth) {
            cl->ClearDepthStencilView(D.dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        }
    }
}

void Renderer::BindShadowTarget(ID3D12GraphicsCommandList* cl, int cascadeIndex, bool clearDepth)
{
    auto& D = deferred_[currentFrameIndex_];

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
    auto& D = deferred_[currentFrameIndex_];
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
    auto& D = deferred_[currentFrameIndex_];
    auto tbl = StageSrvUavTable({ D.gbSRV[0], D.gbSRV[1], D.gbSRV[2], D.gbSRV[3] });
    return tbl.gpu; // shader key t0
}
D3D12_GPU_DESCRIPTOR_HANDLE Renderer::StageComposeSrvTable() {
    auto& D = deferred_[currentFrameIndex_];
    auto tbl = StageSrvUavTable({ D.lightSRV, D.gbSRV[2] }); // Light, Emissive
    return tbl.gpu;
}
D3D12_GPU_DESCRIPTOR_HANDLE Renderer::StageTonemapSrvTable() {
    auto& D = deferred_[currentFrameIndex_];
    auto tbl = StageSrvUavTable({ D.sceneSRV });
    return tbl.gpu;
}

void Renderer::RegisterCurrentThreadCL(ID3D12GraphicsCommandList* cl) {
    uint32_t lane = tlLaneIndex_;
    if (lane == UINT32_MAX) {
        lane = clLaneCount_.fetch_add(1, std::memory_order_relaxed);
        if (lane >= kCLStateLanes) { lane = kCLStateLanes - 1; }
        tlLaneIndex_ = lane;
    }
    CLStateLane& ln = clLanes_[lane];
    if (ln.entries.size() == 0)
    {
        ln.entries.reserve(32);
    }
    CLStateEntry& e = ln.entries[static_cast<ID3D12CommandList*>(cl)];
    e.cmd = static_cast<ID3D12CommandList*>(cl);
    e.st.firstUse.clear(); e.st.firstUse.reserve(8);
    e.st.current.clear(); e.st.current.reserve(16);
    e.epoch = ln.epoch;
    tlCurrentEntry_ = &e;
}

void Renderer::UnregisterCurrentThreadCL() {
    tlCurrentEntry_ = nullptr;
}

const Renderer::CLState* Renderer::FindCLStateForCmd(ID3D12CommandList* cmd) const {
    const uint32_t lanes = clLaneCount_.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < std::min<uint32_t>(lanes, kCLStateLanes); ++i) {
        auto found = clLanes_[i].entries.find(cmd);
        if (found != clLanes_[i].entries.end()) {
            return &found->second.st;
        }
    }
    return nullptr;
}