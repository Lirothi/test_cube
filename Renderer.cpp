#include "Renderer.h"
#include "Helpers.h"
#include <cassert>

Renderer::Renderer()
{

}

Renderer::~Renderer() {
    WaitForPreviousFrame();
    if (fenceEvent_ != nullptr) {
        CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }
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
            // при желании можно добавить фильтры на шумные сообщения
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
        frameResources_[i].GetDescAlloc().Init(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096);
        frameResources_[i].GetSamplerAlloc().Init(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 256);
        frameFenceValues_[i] = 0;
        frameResources_[i].InitUpload(device_.Get(), /*bytes*/ 2 * 1024 * 1024); // 2MB на кадр, под текст/мелочь
    }

    samplerManager_.Init(device_.Get(), 512);
}

void Renderer::InitFence() {
    // для совместимости с твоим main.cpp — безопасный no-op если уже инициализировано
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
    //if (auto* def = fontManager_.Get(L"courierb_32"))
    if (auto* def = fontManager_.Get(L"cons_32"))
    //if (auto* def = fontManager_.Get(L"consolas_32"))
    {
        textManager_.SetFont(def);
    }
}

void Renderer::CreateSwapChainAndRTVs(UINT width, UINT height) {
    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));

    // Уничтожим старый свап и RTV при переинициализации (если было)
    for (UINT i = 0; i < kFrameCount; ++i) {
        renderTargets_[i].Reset();
    }
    rtvHeap_.Reset();
    swapChain_.Reset();

    // Создаём swap chain (kFrameCount)
    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.BufferCount = kFrameCount;
    scd.Width = width;
    scd.Height = height;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.SampleDesc.Count = 1;

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
        device_->CreateRenderTargetView(renderTargets_[i].Get(), nullptr, rtv);
        rtv.ptr += rtvDescriptorSize_;
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
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CLEAR_VALUE cv{};
    cv.Format = DXGI_FORMAT_D32_FLOAT;
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
    const UINT64 value = frameFenceValues_[frameIndex];
    if (value == 0) {
        return; // ещё не сигналили этот кадр — ждать нечего
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

void Renderer::BeginFrame() {
    // Ждём GPU по своему backbuffer'у
    WaitForFrame(currentFrameIndex_);

    ++totalFrameNumber_;

    // Сброс кадровых пулов
    auto& fr = frameResources_[currentFrameIndex_];
    fr.ResetCommandAllocators(device_.Get());
    fr.ResetCommandListsUsage();

    frameResources_[currentFrameIndex_].GetDescAlloc().ResetPerFrame();
    frameResources_[currentFrameIndex_].GetSamplerAlloc().ResetPerFrame();
    frameResources_[currentFrameIndex_].ResetCommandListsUsage();
    frameResources_[currentFrameIndex_].ResetUpload();
}

void Renderer::EndFrame() {
    ExecuteTimelineAndPresent();
}

void Renderer::Update(float dt)
{
    if (fps_ <= 0.0f)
    {
        fps_ = 1.0f / dt;
    }
    else
    {
        fps_ = fps_ * fpsAlpha_ + (1.0f - fpsAlpha_) / dt;
    }

    // хот-релод: накопим время
    if (shaderHotReloadEnabled_) {
        shaderWatchAccumSec_ += dt;

        // 1) раз в N секунд — запустим одноразовую фоновую задачу сканирования ФС
        if (shaderWatchAccumSec_ >= shaderWatchIntervalSec_) {
            // не допускаем наложения сканов
            if (!materialManager_.IsProbeInFlight()) {
                (void)materialManager_.RequestFSProbeAsync();
            }
            // сохраняем остаток, чтоб не терять дробь
            shaderWatchAccumSec_ -= shaderWatchIntervalSec_;
            shaderWatchAccumSec_ = std::max(0.0f, shaderWatchAccumSec_);
        }

        // 2) применим pending-пересборки (если скан что-то нашёл и флаг выставлен)
        materialManager_.ApplyPendingHotReloads(this, totalFrameNumber_, /*keepAliveFrames=*/kFrameCount + 1);
    }
}

Renderer::ThreadCL Renderer::BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE type,
    ID3D12PipelineState* pso) {
    auto& fr = frameResources_[currentFrameIndex_];
    ID3D12CommandAllocator* alloc = fr.AcquireCommandAllocator(device_.Get(), type);
    ID3D12GraphicsCommandList* cl = fr.AcquireCommandList(device_.Get(), type, alloc, pso);

    ID3D12DescriptorHeap* heaps[] = {
        frameResources_[currentFrameIndex_].GetDescAlloc().GetShaderVisibleHeap(),
        frameResources_[currentFrameIndex_].GetSamplerAlloc().GetShaderVisibleHeap()
    };
    cl->SetDescriptorHeaps(_countof(heaps), heaps);

    ThreadCL t{};
    t.alloc = alloc;
    t.cl = cl;
    t.type = type;
    return t;
}

Renderer::ThreadCL Renderer::BeginThreadCommandBundle(ID3D12PipelineState* initialPSO)
{
	return BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_BUNDLE, initialPSO);
}

void Renderer::EndThreadCommandList(ThreadCL& t, size_t batchIndex) {
    if (t.cl != nullptr) {
        ThrowIfFailed(t.cl->Close());
        std::lock_guard<std::mutex> lk(submitMtx_);
        if (batchIndex < submitTimeline_.size()) {
            submitTimeline_[batchIndex].directs.push_back(t.cl);
        }
        t.cl = nullptr;
        t.alloc = nullptr;
    }
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
    std::vector<ID3D12CommandList*> lists;

    // собрать по порядку батчей
    {
        std::lock_guard<std::mutex> lk(submitMtx_);
        for (auto& pb : submitTimeline_) {
            // Если есть driver (создан в пассе) — дописываем в него ExecuteBundle(...)
            if (pb.driver != nullptr) {
                for (auto* b : pb.bundles) {
                    if (b != nullptr) {
                        pb.driver->ExecuteBundle(b);
                    }
                }
                ThrowIfFailed(pb.driver->Close());
                lists.push_back(pb.driver);
            }
            else if (!pb.bundles.empty()) {
                // fallback: нет driver’а — создадим временный
                auto& fr = frameResources_[currentFrameIndex_];
                ID3D12CommandAllocator* alloc =
                    fr.AcquireCommandAllocator(device_.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
                ID3D12GraphicsCommandList* cl =
                    fr.AcquireCommandList(device_.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, alloc);
                RecordBindDefaultsNoClear(cl);
                for (auto* b : pb.bundles) {
                    if (b != nullptr) {
                        cl->ExecuteBundle(b);
                    }
                }
                ThrowIfFailed(cl->Close());
                lists.push_back(cl);
            }

            // Также прикрепим любые готовые DIRECT-CL
            if (!pb.directs.empty()) {
                lists.insert(lists.end(), pb.directs.begin(), pb.directs.end());
            }
        }
        submitTimeline_.clear();
    }

    // Эпилог: RT→Present
    ID3D12GraphicsCommandList* epilogueCL = nullptr;
    {
        auto& fr = frameResources_[currentFrameIndex_];
        ID3D12CommandAllocator* alloc =
            fr.AcquireCommandAllocator(device_.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
        ID3D12GraphicsCommandList* cl =
            fr.AcquireCommandList(device_.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, alloc);

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = renderTargets_[currentFrameIndex_].Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        cl->ResourceBarrier(1, &b);
        ThrowIfFailed(cl->Close());
        epilogueCL = cl;
    }
    if (epilogueCL != nullptr) {
        lists.push_back(epilogueCL);
    }

    if (!lists.empty()) {
        commandQueue_->ExecuteCommandLists((UINT)lists.size(), lists.data());
    }

    ThrowIfFailed(swapChain_->Present(1, 0));
    SignalFrame(currentFrameIndex_);
    currentFrameIndex_ = swapChain_->GetCurrentBackBufferIndex();
}

void Renderer::WaitForPreviousFrame() {
    // Полностью дождаться GPU (для ресайза/деструктора)
    // Сигналим и ждём, пока фэнс не догонит значение
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

    // Важно: полностью дождаться GPU перед заменой ресурсов
    WaitForPreviousFrame();

    // Освобождаем старые RTV/DSV
    for (UINT i = 0; i < kFrameCount; ++i) {
        renderTargets_[i].Reset();
    }
    depthBuffer_.Reset();
    dsvHeap_.Reset();
    rtvHeap_.Reset();

    // ResizeBuffers
    DXGI_SWAP_CHAIN_DESC desc{};
    ThrowIfFailed(swapChain_->GetDesc(&desc));
    ThrowIfFailed(swapChain_->ResizeBuffers(kFrameCount, width_, height_, desc.BufferDesc.Format, desc.Flags));

    // Пересоздать RTV и DSV
    CreateSwapChainAndRTVs(width_, height_);
    CreateDepthResources(width_, height_);
    CreateDeferredTargets(width_, height_);
}

void Renderer::SetResourceState(ID3D12Resource* res, D3D12_RESOURCE_STATES state) {
    if (res == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lk(knownStatesMtx_);
    knownStates_[res] = state;
}

void Renderer::Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES after) {
    if (cl == nullptr || res == nullptr) {
        return;
    }

    D3D12_RESOURCE_STATES before = D3D12_RESOURCE_STATE_COMMON;
    {
        std::lock_guard<std::mutex> lk(knownStatesMtx_);
        auto it = knownStates_.find(res);
        before = (it == knownStates_.end()) ? D3D12_RESOURCE_STATE_COMMON : it->second;
        if (before == after) {
            return;
        }
        knownStates_[res] = after;
    }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    cl->ResourceBarrier(1, &b);
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
    // Barrier: Present -> RenderTarget (для текущего backbuffer)
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = renderTargets_[currentFrameIndex_].Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);

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
    const float clear[4] = { 0.3f, 0.3f, 0.8f, 1.0f };
    cl->ClearRenderTargetView(rtv, clear, 0, nullptr);
    cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
}

void Renderer::RecordBindDefaultsNoClear(ID3D12GraphicsCommandList* cl) {
    // Только bind RTV/DSV + viewport/scissor (без барьера и клира)
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

void Renderer::CreateDeferredTargets(UINT width, UINT height)
{
    // На всякий случай: убираем старые ресурсы/кучи
    DestroyDeferredTargets();

    ID3D12Device* dev = device_.Get();
    if (!dev) 
    {
        return;
    }

    // --- инкременты дескрипторов ---
    deferredRtvIncr_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    deferredDsvIncr_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    deferredSrvIncr_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // --- CPU-only дескрипторные кучи под offscreen (RTV/DSV/SRV) ---
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = kFrameCount * kDeferredRtvPerFrame;  // GB0,GB1,GB2, Light, Scene
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        desc.NodeMask = 0;
        ThrowIfFailed(dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&deferredRtvHeap_)));
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        desc.NumDescriptors = kFrameCount * kDeferredDsvPerFrame;  // Depth
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        desc.NodeMask = 0;
        ThrowIfFailed(dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&deferredDsvHeap_)));
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = kFrameCount * kDeferredSrvPerFrame;  // GB0,GB1,GB2,Depth,Light,Scene
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;              // CPU-only staging
        desc.NodeMask = 0;
        ThrowIfFailed(dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&deferredSrvCpuHeap_)));
    }

    // --- общие параметры размещения (Default heap) ---
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    auto MakeTex2DDesc = [&](DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Alignment = 0;
        rd.Width = width ? width : 1;
        rd.Height = height ? height : 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = fmt;
        rd.SampleDesc.Count = 1;
        rd.SampleDesc.Quality = 0;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = flags;
        return rd;
        };

    for (UINT f = 0; f < kFrameCount; ++f)
    {
        auto& D = deferred_[f];

        // =========================
        // GB0: Albedo+Metal (RGBA8)
        // =========================
        {
            const DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
            D3D12_RESOURCE_DESC rd = MakeTex2DDesc(fmt, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

            D3D12_CLEAR_VALUE cv = {};
            cv.Format = fmt;
            cv.Color[0] = 0.0f; cv.Color[1] = 0.0f; cv.Color[2] = 0.0f; cv.Color[3] = 0.0f;

            ThrowIfFailed(dev->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&D.gb0)));

            D.gbRTV[0] = DeferredRtvCPU(f, DeferredRtvSlot::GB0);
            dev->CreateRenderTargetView(D.gb0.Get(), nullptr, D.gbRTV[0]);

            D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format = fmt;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;

            D.gbSRV[0] = DeferredSrvCPU(f, DeferredSrvSlot::GB0);
            dev->CreateShaderResourceView(D.gb0.Get(), &sd, D.gbSRV[0]);
        }

        // ================================
        // GB1: NormalOcta+Rough (R10G10B10A2)
        // ================================
        {
            const DXGI_FORMAT fmt = DXGI_FORMAT_R10G10B10A2_UNORM;
            D3D12_RESOURCE_DESC rd = MakeTex2DDesc(fmt, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

            D3D12_CLEAR_VALUE cv = {};
            cv.Format = fmt;
            cv.Color[0] = 0.0f; cv.Color[1] = 0.0f; cv.Color[2] = 0.0f; cv.Color[3] = 0.0f;

            ThrowIfFailed(dev->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&D.gb1)));

            D.gbRTV[1] = DeferredRtvCPU(f, DeferredRtvSlot::GB1);
            dev->CreateRenderTargetView(D.gb1.Get(), nullptr, D.gbRTV[1]);

            D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format = fmt;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;

            D.gbSRV[1] = DeferredSrvCPU(f, DeferredSrvSlot::GB1);
            dev->CreateShaderResourceView(D.gb1.Get(), &sd, D.gbSRV[1]);
        }

        // =========================
        // GB2: Emissive (R11G11B10F)
        // =========================
        {
            const DXGI_FORMAT fmt = DXGI_FORMAT_R11G11B10_FLOAT;
            D3D12_RESOURCE_DESC rd = MakeTex2DDesc(fmt, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

            D3D12_CLEAR_VALUE cv = {};
            cv.Format = fmt;
            cv.Color[0] = 0.0f; cv.Color[1] = 0.0f; cv.Color[2] = 0.0f; cv.Color[3] = 0.0f;

            ThrowIfFailed(dev->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&D.gb2)));

            D.gbRTV[2] = DeferredRtvCPU(f, DeferredRtvSlot::GB2);
            dev->CreateRenderTargetView(D.gb2.Get(), nullptr, D.gbRTV[2]);

            D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format = fmt;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;

            D.gbSRV[2] = DeferredSrvCPU(f, DeferredSrvSlot::GB2);
            dev->CreateShaderResourceView(D.gb2.Get(), &sd, D.gbSRV[2]);
        }

        // ==============================
        // Depth: D32_FLOAT (+ SRV R32_FLOAT)
        // ==============================
        {
            const DXGI_FORMAT dsvFmt = DXGI_FORMAT_D32_FLOAT;
            D3D12_RESOURCE_DESC rd = MakeTex2DDesc(dsvFmt, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

            D3D12_CLEAR_VALUE cv = {};
            cv.Format = dsvFmt;
            cv.DepthStencil.Depth = 1.0f;
            cv.DepthStencil.Stencil = 0;

            ThrowIfFailed(dev->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&D.depth)));

            // DSV
            D.dsv = DeferredDsvCPU(f, DeferredDsvSlot::Depth);
            D3D12_DEPTH_STENCIL_VIEW_DESC dv = {};
            dv.Format = dsvFmt;
            dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            dv.Flags = D3D12_DSV_FLAG_NONE;
            dv.Texture2D.MipSlice = 0;
            dev->CreateDepthStencilView(D.depth.Get(), &dv, D.dsv);

            // SRV к depth как R32_FLOAT
            D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format = DXGI_FORMAT_R32_FLOAT;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MostDetailedMip = 0;
            sd.Texture2D.MipLevels = 1;
            sd.Texture2D.PlaneSlice = 0;
            sd.Texture2D.ResourceMinLODClamp = 0.0f;

            D.gbSRV[3] = DeferredSrvCPU(f, DeferredSrvSlot::Depth);
            dev->CreateShaderResourceView(D.depth.Get(), &sd, D.gbSRV[3]);
        }

        // =========================
        // LightTarget: RGBA16F
        // =========================
        {
            const DXGI_FORMAT fmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
            D3D12_RESOURCE_DESC rd = MakeTex2DDesc(fmt, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

            D3D12_CLEAR_VALUE cv = {};
            cv.Format = fmt;
            cv.Color[0] = 0.0f; cv.Color[1] = 0.0f; cv.Color[2] = 0.0f; cv.Color[3] = 0.0f;

            ThrowIfFailed(dev->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&D.light)));

            D.lightRTV = DeferredRtvCPU(f, DeferredRtvSlot::Light);
            dev->CreateRenderTargetView(D.light.Get(), nullptr, D.lightRTV);

            D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format = fmt;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;

            D.lightSRV = DeferredSrvCPU(f, DeferredSrvSlot::Light);
            dev->CreateShaderResourceView(D.light.Get(), &sd, D.lightSRV);
        }

        // =========================
        // SceneColor: RGBA16F
        // =========================
        {
            const DXGI_FORMAT fmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
            D3D12_RESOURCE_DESC rd = MakeTex2DDesc(fmt, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

            D3D12_CLEAR_VALUE cv = {};
            cv.Format = fmt;
            cv.Color[0] = 0.0f; cv.Color[1] = 0.0f; cv.Color[2] = 0.0f; cv.Color[3] = 0.0f;

            ThrowIfFailed(dev->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&D.scene)));

            D.sceneRTV = DeferredRtvCPU(f, DeferredRtvSlot::Scene);
            dev->CreateRenderTargetView(D.scene.Get(), nullptr, D.sceneRTV);

            D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format = fmt;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;

            D.sceneSRV = DeferredSrvCPU(f, DeferredSrvSlot::Scene);
            dev->CreateShaderResourceView(D.scene.Get(), &sd, D.sceneSRV);
        }

        SetResourceState(D.gb0.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        SetResourceState(D.gb1.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        SetResourceState(D.gb2.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        SetResourceState(D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        SetResourceState(D.light.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        SetResourceState(D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
}

void Renderer::DestroyDeferredTargets() {
    deferredRtvHeap_.Reset(); deferredDsvHeap_.Reset(); deferredSrvCpuHeap_.Reset();
    for (UINT f = 0; f < kFrameCount; ++f) {
        auto& D = deferred_[f];
        D.gb0.Reset(); D.gb1.Reset(); D.gb2.Reset(); D.depth.Reset(); D.light.Reset(); D.scene.Reset();
    }

    {
        std::lock_guard<std::mutex> lk(knownStatesMtx_);
        knownStates_.clear();
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
        for (int i = 0; i < 3; ++i) cl->ClearRenderTargetView(rtvs[i], c, 0, nullptr);
        if (mode == ClearMode::ColorDepth)
        {
            cl->ClearDepthStencilView(D.dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        }
    }
}

void Renderer::BindLightTarget(ID3D12GraphicsCommandList* cl, ClearMode mode) {
    auto& D = deferred_[currentFrameIndex_];
    cl->OMSetRenderTargets(1, &D.lightRTV, FALSE, nullptr);
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
    }
}

D3D12_GPU_DESCRIPTOR_HANDLE Renderer::StageGBufferSrvTable() {
    auto& D = deferred_[currentFrameIndex_];
    auto tbl = StageSrvUavTable({ D.gbSRV[0], D.gbSRV[1], D.gbSRV[2], D.gbSRV[3] });
    return tbl.gpu; // ключ t0 в шейдере
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