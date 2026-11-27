#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <source_location>
#include <vector>

#include "rhi/Device.h"
#include "rhi/FrameScheduler.h"
#include "rhi/RhiQueue.h"

namespace rhi
{
// Architecture plan R4.5d (docs/architecture_review_plan.md, section 11.2, wave 3): the frame's synchronization --
// the frame fences and per-frame resources (FrameScheduler), the one road to the queues for a frame in flight
// (RhiQueue, game thread plan G10), the tickets of the last two submissions, the game half's deferred queue work
// (G7) and the waits for the GPU with their counters (R16.10). Was Renderer's; Renderer owns one (RhiFrame()) and
// its old methods forward here. Recording and submission (SubmitTimeline, SubmitFrame_) stay with Renderer (R4.5e).
class Frame
{
public:
    explicit Frame(Device& device) : device_(device) {}

    FrameScheduler& Scheduler() { return scheduler_; }
    const FrameScheduler& Scheduler() const { return scheduler_; }
    // Game thread plan G10: the one road to the GPU queues for a frame in flight (RhiQueue.h).
    RhiQueue& Queue() { return rhi_; }

    // The frame's own fence: wait for a slot's value, signal it on both queues (step 2).
    void WaitForFrame(UINT frameIndex);
    void SignalFrame(UINT frameIndex, std::uint64_t frameNumber);
    // Occlusion plan S3a: block until one frame SLOT's fence has passed -- the wait a query
    // latency below kFrameCount costs (UE's Map blocks the same way). Not the full idle below.
    // The last frame handed over first reaches the queues: under the RHI thread the slot of the frame before this one
    // still holds the fence of the frame kFrameCount before it until its submission runs (BeginFrame waits only for
    // the frame before that one), and the wait would return at once on a frame the GPU had not been given.
    void WaitForFrameSlot(UINT frameIndex) { rhi_.WaitFor(lastSubmit_); WaitForFrame(frameIndex); }
    // The non-blocking half: has the GPU finished the frame of this NUMBER? By number, never by slot: under the RHI
    // thread a slot answers for the frame kFrameCount before until its own submission signals.
    bool IsFrameNumberComplete(std::uint64_t frameNumber) const { return scheduler_.IsFrameNumberComplete(frameNumber); }

    // G10b: the newest frame's submission op and the one before it. BeginFrame waits for the one before: the RHI
    // thread at most one frame behind the recording (UE's GTSyncType 0), and that frame's slot (kFrameCount - 1 back,
    // or less) long submitted.
    void NoteSubmission(RhiQueue::Ticket ticket)
    {
        prevSubmit_ = lastSubmit_;
        lastSubmit_ = ticket;
        NoteWork(); // R8.1: a frame's lists are on their way to the queues
    }
    // Architecture plan R8.1: the GPU work handed out so far (a count: the frame's submission, every ExecuteOrDefer),
    // taken where it is handed out -- before the RHI thread runs it. A GPU-idle point that waited (GpuIdlePoint) finds the
    // GPU idle again while this has not moved since its wait.
    std::uint64_t WorkHandedOut() const { return workHandedOut_.load(std::memory_order_acquire); }
    void NoteWork() { workHandedOut_.fetch_add(1, std::memory_order_acq_rel); }
    void WaitForSubmissionBeforeLast() { rhi_.WaitFor(prevSubmit_); }

    // Full synchronization: from the game half it parks the render half, sends the hand-over submits, waits for the RHI
    // thread and both queues, exchanges the knobs (R16.10). Every call is counted by its caller's file:line and timed
    // (CPU scope "Renderer: GPU idle wait"); LogGpuIdleWaits prints them.
    void WaitForPreviousFrame(std::source_location where = std::source_location::current());
    // R16.10: the idle waits since the last call, by site ("gpu idle waits: ..."; nothing when there were none) and the
    // session's total; App's main loop every 5 s, beside threading::LogGameWaits, and once at the loop's end.
    static void LogGpuIdleWaits(double seconds);

    // Game thread plan G7: GPU work the game half records outside the frame (a thumbnail, the Mesh Editor's preview,
    // an upload batch) goes to the queue at the hand-over -- never while the render half records -- in the order it
    // was submitted. From the render half, at the hand-over or with the render half parked, it goes at once.
    void ExecuteOrDefer(ID3D12CommandList* list);
    bool SignalOrDefer(ID3D12Fence* fence, UINT64 value);
    void FlushHandOverSubmits(); // Scene::Sync, and where the game half parks the render half

private:
    Device& device_;
    FrameScheduler scheduler_;
    RhiQueue rhi_;
    RhiQueue::Ticket lastSubmit_ = 0;
    RhiQueue::Ticket prevSubmit_ = 0;
    std::atomic<std::uint64_t> workHandedOut_{ 0 }; // R8.1: WorkHandedOut
    // G7: the game half's deferred queue work (ExecuteOrDefer / SignalOrDefer), in submission order.
    struct HandOverOp
    {
        Microsoft::WRL::ComPtr<ID3D12CommandList> list;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        UINT64 value = 0;
    };
    std::mutex handOverMutex_;
    std::vector<HandOverOp> handOverOps_;
    bool HandOverSubmitsGoNow() const;
};
} // namespace rhi
