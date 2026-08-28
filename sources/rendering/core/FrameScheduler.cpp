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
    if (computeFenceEvent_ != nullptr) {
        CloseHandle(computeFenceEvent_);
        computeFenceEvent_ = nullptr;
    }
}

void FrameScheduler::InitFence(ID3D12Device* device)
{
    if (!fence_) {
        ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));
        fence_->SetName(L"Fence.Frame.Direct");
    }
    if (!fenceEvent_) {
        fenceEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent_) {
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
        }
    }
    // Step 2. Created unconditionally, even when the device refused a compute queue: the fence
    // costs nothing, and an always-present fence means the wait path has ONE shape instead of two.
    if (!computeFence_) {
        ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&computeFence_)));
        computeFence_->SetName(L"Fence.Frame.AsyncCompute");
    }
    if (!computeFenceEvent_) {
        computeFenceEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!computeFenceEvent_) {
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
        }
    }
    if (!crossQueueFence_) {
        ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&crossQueueFence_)));
        crossQueueFence_->SetName(L"Fence.CrossQueue");
    }
}

void FrameScheduler::CreateFrameResources(ID3D12Device* device)
{
    for (UINT i = 0; i < render::kFrameCount; ++i) {
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
    // Step 2: BOTH queues. The compute fence is signalled with the same value by SignalFrame, so
    // one stored value covers both halves. Waiting on the direct fence first is deliberate — it is
    // the one that is almost always the later of the two, so the second wait usually returns
    // immediately rather than blocking twice.
    if (fence_->GetCompletedValue() < value) {
        ThrowIfFailed(fence_->SetEventOnCompletion(value, fenceEvent_));
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
    if (computeFence_ && computeFence_->GetCompletedValue() < value) {
        ThrowIfFailed(computeFence_->SetEventOnCompletion(value, computeFenceEvent_));
        WaitForSingleObject(computeFenceEvent_, INFINITE);
    }
}

bool FrameScheduler::IsFrameComplete(UINT frameIndex) const
{
    if (frameIndex >= render::kFrameCount) {
        return true;
    }
    const UINT64 value = frameFenceValues_[frameIndex];
    if (value == 0) {
        return true; // never signalled — nothing has used this slot yet
    }
    if (!fence_ || fence_->GetCompletedValue() < value) {
        return false;
    }
    if (computeFence_ && computeFence_->GetCompletedValue() < value) {
        return false;
    }
    return true;
}

void FrameScheduler::SignalFrame(ID3D12CommandQueue* direct, ID3D12CommandQueue* compute, UINT frameIndex)
{
    // ONE value on BOTH fences. The compute queue is signalled every frame whether or not anything
    // was submitted to it: a Signal on an idle queue completes immediately, and paying that
    // uniformly is what keeps the slot's release rule a single expression instead of a special case
    // per frame. When the device refused a compute queue there is nothing to signal, and the
    // compute half of every later comparison is then satisfied by a fence that never moves off its
    // own signalled values — so it must NOT be signalled here, or the wait would hang.
    const UINT64 v = nextFenceValue_++;
    ThrowIfFailed(direct->Signal(fence_.Get(), v));
    if (compute && computeFence_) {
        ThrowIfFailed(compute->Signal(computeFence_.Get(), v));
    }
    else if (computeFence_) {
        // No compute queue: advance the fence from the CPU so "both passed V" stays true.
        ThrowIfFailed(computeFence_->Signal(v));
    }
    frameFenceValues_[frameIndex] = v;
}

void FrameScheduler::WaitForGpuIdle(ID3D12CommandQueue* direct, ID3D12CommandQueue* compute)
{
    // Signal and wait until BOTH fences reach the target value. Every resize, level switch and
    // shutdown path in the engine ends up here, which is why step 2 fixes it rather than the
    // hardening step: the gates for every intervening step ARE that churn.
    const UINT64 v = nextFenceValue_++;
    if (direct) {
        ThrowIfFailed(direct->Signal(fence_.Get(), v));
    }
    if (compute && computeFence_) {
        ThrowIfFailed(compute->Signal(computeFence_.Get(), v));
    }
    else if (computeFence_) {
        ThrowIfFailed(computeFence_->Signal(v));
    }
    if (direct && fence_->GetCompletedValue() < v) {
        ThrowIfFailed(fence_->SetEventOnCompletion(v, fenceEvent_));
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
    if (computeFence_ && computeFence_->GetCompletedValue() < v) {
        ThrowIfFailed(computeFence_->SetEventOnCompletion(v, computeFenceEvent_));
        WaitForSingleObject(computeFenceEvent_, INFINITE);
    }
}

UINT64 FrameScheduler::SignalCrossQueue(ID3D12CommandQueue* producer)
{
    if (!producer || !crossQueueFence_) {
        return 0;
    }
    const UINT64 v = nextCrossQueueValue_++;
    ThrowIfFailed(producer->Signal(crossQueueFence_.Get(), v));
    return v;
}

void FrameScheduler::WaitCrossQueue(ID3D12CommandQueue* consumer, UINT64 value)
{
    if (!consumer || !crossQueueFence_ || value == 0) {
        return;
    }
    // GPU-side wait: the queue stalls until the fence reaches `value`. NOT a CPU wait — the whole
    // point of D2 is that a cross-queue dependency costs the CPU nothing.
    ThrowIfFailed(consumer->Wait(crossQueueFence_.Get(), value));
}

void FrameScheduler::ResetFrameState(ID3D12Device* device)
{
    for (UINT i = 0; i < render::kFrameCount; ++i) {
        if (frameResources_[i]) {
            frameResources_[i]->ResetCommandAllocators(device);
            frameResources_[i]->ResetCommandListsUsage();
            frameResources_[i]->ResetUpload();
            frameResources_[i].reset();
        }
        frameFenceValues_[i] = 0;
    }
    nextFenceValue_ = 1;
    nextCrossQueueValue_ = 1;
    // Both counters restart from 1, which is only sound because this runs at SHUTDOWN and
    // ReleaseFence follows immediately (see Renderer's staged teardown). Signalling 1 on a fence
    // whose completed value is already in the hundreds would make every later wait pass instantly.
}

void FrameScheduler::ReleaseFence()
{
    if (fence_) {
        fence_.Reset();
    }
    // Step 2: the two fences added for the second queue. Released here rather than in the
    // destructor for the same reason the direct one is — shutdown order is staged deliberately
    // (see Renderer's teardown), and a fence outliving its queue shows up as a live-object warning.
    if (computeFence_) {
        computeFence_.Reset();
    }
    if (crossQueueFence_) {
        crossQueueFence_.Reset();
    }
}
