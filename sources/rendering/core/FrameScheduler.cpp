#include "rendering/core/FrameScheduler.h"

#include "core/Helpers.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"

FrameScheduler::~FrameScheduler()
{
    if (fenceEvent_ != nullptr) {
        CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }
}

void FrameScheduler::InitFence(ID3D12Device* device)
{
    if (!fence_) {
        ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));
    }
    if (!fenceEvent_) {
        fenceEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent_) {
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
        }
    }
}

void FrameScheduler::CreateFrameResources(ID3D12Device* device)
{
    for (UINT i = 0; i < kFrameCount; ++i) {
        // per-frame shader-visible heaps
        frameResources_[i] = std::make_unique<FrameResource>();
        frameResources_[i]->GetDescAlloc().Init(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096);
        frameResources_[i]->GetSamplerAlloc().Init(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 256);
        frameFenceValues_[i] = 0;
        frameResources_[i]->InitUpload(device, /*bytes*/ 4 * 1024 * 1024);
    }
}

void FrameScheduler::WaitForFrame(UINT frameIndex)
{
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

void FrameScheduler::SignalFrame(ID3D12CommandQueue* queue, UINT frameIndex)
{
    const UINT64 v = nextFenceValue_++;
    ThrowIfFailed(queue->Signal(fence_.Get(), v));
    frameFenceValues_[frameIndex] = v;
}

void FrameScheduler::WaitForGpuIdle(ID3D12CommandQueue* queue)
{
    // Signal and wait until the fence reaches the target value
    const UINT64 v = nextFenceValue_++;
    ThrowIfFailed(queue->Signal(fence_.Get(), v));
    if (fence_->GetCompletedValue() < v) {
        ThrowIfFailed(fence_->SetEventOnCompletion(v, fenceEvent_));
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
}

void FrameScheduler::ResetFrameState(ID3D12Device* device)
{
    for (UINT i = 0; i < kFrameCount; ++i) {
        if (frameResources_[i]) {
            frameResources_[i]->ResetCommandAllocators(device);
            frameResources_[i]->ResetCommandListsUsage();
            frameResources_[i]->ResetUpload();
            frameResources_[i].reset();
        }
        frameFenceValues_[i] = 0;
    }
    nextFenceValue_ = 1;
}

void FrameScheduler::ReleaseFence()
{
    if (fence_) {
        fence_.Reset();
    }
}
