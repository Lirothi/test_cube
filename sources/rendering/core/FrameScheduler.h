#pragma once

#include <d3d12.h>
#include <cstdint>
#include <memory>
#include <wrl/client.h>

#include "rendering/core/FrameResource.h"
#include "rendering/core/RenderConstants.h"

// Owns frame pacing state: the frame fence + event, per-frame fence values,
// and the per-frame FrameResource pools (command allocators/lists, descriptor
// allocators, upload ring). The current frame index stays with the caller.
class FrameScheduler
{
public:
    ~FrameScheduler();

    void InitFence(ID3D12Device* device);     // safe no-op when already initialized
    void CreateFrameResources(ID3D12Device* device);

    // Waits until the GPU has finished the given frame slot — on BOTH queues.
    //
    // Async-compute step 2: R6 of the plan is that everything per-frame (command allocators, the
    // descriptor and sampler rings, the upload ring) is recycled per frame-in-flight SLOT on the
    // assumption that ONE fence decides when that slot is free. A compute queue running ahead of or
    // behind the direct one breaks that assumption, and this engine has already shipped one bug of
    // exactly that shape (a cache keyed on the slot instead of the frame number, handing out
    // descriptor addresses the ring had already reused). So the slot is free only when both queues
    // have passed the frame's value.
    void WaitForFrame(UINT frameIndex);
    // Signals BOTH queues for a frame slot with the same next global value. `compute` may be null
    // (device refused a compute queue) — then only the direct half is signalled and the compute
    // half counts as already complete.
    void SignalFrame(ID3D12CommandQueue* direct, ID3D12CommandQueue* compute, UINT frameIndex);
    // Has the GPU finished this slot on BOTH queues? Non-blocking; the Debug assert in
    // Renderer::BeginFrame uses it to catch a per-frame ring being reused before both fences.
    bool IsFrameComplete(UINT frameIndex) const;
    // Full synchronization: signal and wait, on BOTH queues (resize / level switch / shutdown).
    // Every idle path in the engine funnels through Renderer::WaitForPreviousFrame into this, so
    // this one function is what makes ~50 call sites queue-correct.
    void WaitForGpuIdle(ID3D12CommandQueue* direct, ID3D12CommandQueue* compute);

    // --- Step 2: the cross-queue signal/wait helper the graph will use at step 6 (D2) ---
    //
    // A dependency that crosses queues compiles into a Signal on the producer queue and a
    // GPU-side Wait on the consumer queue. Both halves live on ONE dedicated fence with a
    // monotonic counter, so a value handed out here is comparable across the whole frame.
    //
    // DORMANT: nothing calls these yet. They are here so step 6 adds scheduling rather than
    // fence plumbing, and so the ownership of that fence is settled now — a hand-rolled fence
    // inside a pass body is exactly what D2 forbids.
    // A cross-queue edge: WHICH fence, and the value on it. The fence is part of the answer
    // because there is one PER PRODUCING QUEUE -- see the comment on the members.
    struct CrossQueuePoint
    {
        ID3D12Fence* fence = nullptr;
        UINT64       value = 0;
        bool valid() const { return fence != nullptr && value != 0; }
    };
    CrossQueuePoint SignalCrossQueue(ID3D12CommandQueue* producer);
    void            WaitCrossQueue(ID3D12CommandQueue* consumer, const CrossQueuePoint& point);
    // Diagnostics only (the fence-stall report).
    ID3D12Fence* CrossQueueFenceGraphics() const { return crossQueueFenceGraphics_.Get(); }
    ID3D12Fence* CrossQueueFenceCompute() const { return crossQueueFenceCompute_.Get(); }
    UINT64 NextCrossQueueGraphics() const { return nextCrossGraphics_; }
    UINT64 NextCrossQueueCompute() const { return nextCrossCompute_; }

    bool HasFence() const { return fence_ != nullptr; }
    FrameResource* GetFrameResource(UINT frameIndex) const
    {
        return frameIndex < render::kFrameCount ? frameResources_[frameIndex].get() : nullptr;
    }

    // Shutdown is staged to preserve the original release order.
    void ResetFrameState(ID3D12Device* device); // reset pools, release frame resources
    void ReleaseFence();

private:
    // One value per slot, signalled on BOTH fences (step 2). Two fences rather than one shared by
    // both queues: a single fence signalled from two queues completes out of order, so
    // "GetCompletedValue() >= V" would no longer mean "both queues passed V" — which is the entire
    // question this class answers. Two fences and one value keeps the bookkeeping to one array.
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;         // direct queue
    Microsoft::WRL::ComPtr<ID3D12Fence> computeFence_;  // async-compute queue
    HANDLE fenceEvent_ = nullptr;
    HANDLE computeFenceEvent_ = nullptr;
    UINT64 nextFenceValue_ = 1;                  // global increment, shared by both fences
    UINT64 frameFenceValues_[render::kFrameCount] = {};  // last signal value for each frame

    // Step 2: the cross-queue edge fence (see SignalCrossQueue). Separate from the frame fences so
    // a mid-frame edge cannot perturb frame pacing, and so its counter stays readable in a capture.
    // ONE FENCE PER PRODUCING QUEUE, and that is load-bearing, not tidiness.
    //
    // There used to be a single `crossQueueFence_` with one shared counter, signalled by BOTH
    // queues. `ID3D12CommandQueue::Signal` ASSIGNS the fence value -- it does not take a maximum --
    // so with values handed out in submission order but executed concurrently on two queues, a
    // lower-valued signal can land AFTER a higher one and walk the fence BACKWARDS. A `Wait` on a
    // value the fence had already passed then blocks forever.
    //
    // It needed three async passes to deadlock, which is exactly why it looked like anything but a
    // fence bug: with A(compute) B(graphics) C(compute) D(graphics) E(compute), B landing before A
    // rewinds the fence once (survivable -- D pulls it forward again), but C then rewinds it a
    // SECOND time past D, and now compute waits for D while graphics waits for E that only compute
    // can signal. Observed exactly: cross fence stuck at C's value, `COMPUTE wait=D NOT SATISFIED`,
    // `graphics wait=E NOT SATISFIED`, both queues idle, device healthy, TDR silent.
    //
    // With one fence per producer each fence has a single writer, so its values are monotonic by
    // construction and no ordering of the two queues can rewind it.
    Microsoft::WRL::ComPtr<ID3D12Fence> crossQueueFenceGraphics_;
    Microsoft::WRL::ComPtr<ID3D12Fence> crossQueueFenceCompute_;
    UINT64 nextCrossGraphics_ = 1;
    UINT64 nextCrossCompute_ = 1;

    std::unique_ptr<FrameResource> frameResources_[render::kFrameCount];
};
