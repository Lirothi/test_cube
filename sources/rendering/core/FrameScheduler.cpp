#include "rendering/core/FrameScheduler.h"

#include "core/Helpers.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "core/diagnostics/ArtifactWriter.h"
#include "rendering/core/Renderer.h" // g_crossQueueWaits / g_asyncComputeLists

#include <cstdio>

namespace
{
// A fence wait that CANNOT hang silently.
//
// Every idle path in the engine funnels through these waits, and they were all
// WaitForSingleObject(..., INFINITE). When a frame stops completing -- a GPU stall on a machine
// with TDR off, a queue whose signal never ran -- the process parks here forever and produces NO
// artefact at all: the debugger shows "waiting on a fence" and nothing else, DRED never fires
// because the device was never removed, and there is no log to read afterwards.
//
// So: wait in slices, and the FIRST time a slice expires write down exactly what is stuck --
// which fence, the value waited for, and how far each fence has actually got. Then keep waiting,
// so behaviour is otherwise unchanged. One report per process; the point is to name the stall,
// not to spam.
void WaitFenceOrReport(ID3D12Fence* fence, HANDLE evt, UINT64 target, const char* what,
                       ID3D12Fence* directFence, ID3D12Fence* computeFence, UINT64 nextValue,
                       ID3D12Fence* crossGfx, UINT64 nextCrossGfx,
                       ID3D12Fence* crossCmp, UINT64 nextCrossCmp)
{
    static bool s_reported = false;
    constexpr DWORD kSliceMs = 4000;
    for (;;)
    {
        const DWORD r = WaitForSingleObject(evt, kSliceMs);
        if (r != WAIT_TIMEOUT) { return; }
        if (s_reported) { continue; }
        s_reported = true;
        diag::ArtifactFile f("fence_stall.log", diag::ArtifactMode::Append);
        if (f)
        {
            f.Printf("STALL in %s: waiting for %llu on %s\n",
                         what, static_cast<unsigned long long>(target),
                         (fence == directFence) ? "DIRECT fence" : "COMPUTE fence");
            f.Printf("  direct  completed = %llu\n",
                         directFence ? static_cast<unsigned long long>(directFence->GetCompletedValue()) : 0ull);
            f.Printf("  compute completed = %llu\n",
                         computeFence ? static_cast<unsigned long long>(computeFence->GetCompletedValue()) : 0ull);
            f.Printf("  next value to hand out = %llu\n",
                         static_cast<unsigned long long>(nextValue));
            // The cross-queue fence is the one that can park a QUEUE without the driver calling
            // it a hang: ID3D12CommandQueue::Wait is a legal wait, so TDR never fires, the device
            // is never removed and DRED stays empty. Everything queued behind such a Wait --
            // including the frame Signal this stall is waiting for -- can then never run.
            f.Printf("  cross gfx completed = %llu (next %llu) | cross cmp = %llu (next %llu)\n",
                         crossGfx ? static_cast<unsigned long long>(crossGfx->GetCompletedValue()) : 0ull,
                         static_cast<unsigned long long>(nextCrossGfx),
                         crossCmp ? static_cast<unsigned long long>(crossCmp->GetCompletedValue()) : 0ull,
                         static_cast<unsigned long long>(nextCrossCmp));
            f.Printf("  cross-queue waits issued = %u, async compute lists = %u\n",
                         render::g_crossQueueWaits, render::g_asyncComputeLists);
            f.Printf("  -> if `cross completed` < `next cross` - 1, a queue is parked on a\n"
                            "     cross-queue Wait nobody signalled: THAT is the deadlock, not a GPU\n"
                            "     stall. If they agree, the GPU really stopped on submitted work.\n");

            // The last frame's wait/signal edges, in submission order. Read it against
            // `cross completed`: the FIRST edge whose waitValue exceeds it is the parked one,
            // and its queue is the one holding everything behind it.
            // Walk the ring OLDEST first: the parked edge is the first whose wait exceeds what
            // the cross fence reached, and everything submitted after it on that queue is stuck
            // behind it regardless of how satisfiable it looks on its own.
            // Per-queue now, so an edge is judged against the fence of the queue that signals it.
            const unsigned long long doneGfx = crossGfx ? crossGfx->GetCompletedValue() : 0ull;
            const unsigned long long doneCmp = crossCmp ? crossCmp->GetCompletedValue() : 0ull;
            f.Printf("  submission history, oldest first (%u edges):\n",
                         render::g_submitEdgeCount);
            for (unsigned k = 0; k < render::g_submitEdgeCount; ++k)
            {
                const unsigned idx = (render::g_submitEdgeNext + render::kSubmitEdgeRing
                                      - render::g_submitEdgeCount + k) % render::kSubmitEdgeRing;
                const render::SubmitEdge& e = render::g_submitEdgeRing[idx];
                f.Printf("    f%-6llu %-8s lists=%-3u wait=%-6llu signal=%-6llu%s\n",
                             e.frame, e.compute ? "COMPUTE" : "graphics", e.lists,
                             e.waitValue, e.signalValue,
                             // a segment waits on the OTHER queue's fence and signals its own
                             (e.waitValue > (e.compute ? doneGfx : doneCmp)) ? "   <== NOT SATISFIED"
                             : ((e.signalValue > (e.compute ? doneCmp : doneGfx)) ? "   (signal pending)" : ""));
            }
        }
    }
}
} // namespace

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
    // One per PRODUCING queue -- see the members' comment. A fence with two writers is not
    // monotonic, and that was the deadlock.
    if (!crossQueueFenceGraphics_) {
        ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&crossQueueFenceGraphics_)));
        crossQueueFenceGraphics_->SetName(L"Fence.CrossQueue.Graphics");
    }
    if (!crossQueueFenceCompute_) {
        ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&crossQueueFenceCompute_)));
        crossQueueFenceCompute_->SetName(L"Fence.CrossQueue.Compute");
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
        WaitFenceOrReport(fence_.Get(), fenceEvent_, value, "WaitForFrame",
                          fence_.Get(), computeFence_.Get(), nextFenceValue_,
                          crossQueueFenceGraphics_.Get(), nextCrossGraphics_,
                          crossQueueFenceCompute_.Get(), nextCrossCompute_);
    }
    if (computeFence_ && computeFence_->GetCompletedValue() < value) {
        ThrowIfFailed(computeFence_->SetEventOnCompletion(value, computeFenceEvent_));
        WaitFenceOrReport(computeFence_.Get(), computeFenceEvent_, value, "WaitForFrame",
                          fence_.Get(), computeFence_.Get(), nextFenceValue_,
                          crossQueueFenceGraphics_.Get(), nextCrossGraphics_,
                          crossQueueFenceCompute_.Get(), nextCrossCompute_);
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
        WaitFenceOrReport(fence_.Get(), fenceEvent_, v, "WaitForGpuIdle",
                          fence_.Get(), computeFence_.Get(), nextFenceValue_,
                          crossQueueFenceGraphics_.Get(), nextCrossGraphics_,
                          crossQueueFenceCompute_.Get(), nextCrossCompute_);
    }
    if (computeFence_ && computeFence_->GetCompletedValue() < v) {
        ThrowIfFailed(computeFence_->SetEventOnCompletion(v, computeFenceEvent_));
        WaitFenceOrReport(computeFence_.Get(), computeFenceEvent_, v, "WaitForGpuIdle",
                          fence_.Get(), computeFence_.Get(), nextFenceValue_,
                          crossQueueFenceGraphics_.Get(), nextCrossGraphics_,
                          crossQueueFenceCompute_.Get(), nextCrossCompute_);
    }
}

FrameScheduler::CrossQueuePoint FrameScheduler::SignalCrossQueue(ID3D12CommandQueue* producer)
{
    if (!producer) {
        return {};
    }
    // The producer's OWN fence and its OWN counter: single writer, monotonic by construction.
    const bool isCompute = (producer->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_COMPUTE);
    ID3D12Fence* fence = isCompute ? crossQueueFenceCompute_.Get() : crossQueueFenceGraphics_.Get();
    if (!fence) {
        return {};
    }
    const UINT64 v = isCompute ? nextCrossCompute_++ : nextCrossGraphics_++;
    ThrowIfFailed(producer->Signal(fence, v));
    return CrossQueuePoint{ fence, v };
}

void FrameScheduler::WaitCrossQueue(ID3D12CommandQueue* consumer, const CrossQueuePoint& point)
{
    if (!consumer || !point.valid()) {
        return;
    }
    // GPU-side wait: the queue stalls until the fence reaches `value`. NOT a CPU wait — the whole
    // point of D2 is that a cross-queue dependency costs the CPU nothing.
    ThrowIfFailed(consumer->Wait(point.fence, point.value));
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
    nextCrossGraphics_ = 1;
    nextCrossCompute_ = 1;
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
    if (crossQueueFenceCompute_) {
        crossQueueFenceCompute_.Reset();
    }
    if (crossQueueFenceGraphics_) {
        crossQueueFenceGraphics_.Reset();
    }
}
