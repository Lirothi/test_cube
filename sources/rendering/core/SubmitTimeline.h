#pragma once
#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "rendering/core/RendererInvariantFailure.h"
#include "rendering/core/RenderPass.h" // step 6: RenderQueue

// Per-frame submission timeline: the ordered pass batches that collect the
// command lists render-pass workers record (concurrently) for one frame.
// Batch order defines GPU execution order; within a batch the driver list
// comes first (its bundles execute inside it), then the direct lists.
//
// DETERMINISTIC ORDER (step 3): worker completion order is NOT used as GPU
// order. Each direct and bundle carries a localOrder assigned BEFORE the work
// is dispatched (chunk index, cascade index, spot-light index; single-CL pass
// bodies use 0). Gathering sorts each batch's directs and bundles by
// localOrder, so the GPU order is reproducible run-to-run regardless of which
// worker finished first. The driver, when present, is conceptually work-list
// order 0 and is always gathered first (its bundles execute inside it);
// directs follow in localOrder order. localOrder lives in two independent
// namespaces per batch — directs and bundles — and must be unique within each:
// a duplicate is a non-deterministic tie and fails fast.
//
// The batches form a PERSISTENT POOL: BeginTimeline/GatherFrameLists reset
// them in place (the inner clear() retains heap capacity) instead of
// destroying them, so steady-state frames register command lists without
// allocating — a push_back under the registration mutex is a store into warm
// capacity. The pool only grows the first frame that uses more batches (or
// more lists in a batch) than any previous frame; only Clear() (shutdown)
// actually releases memory.
//
// Registration is thread-safe. Invariant violations — null list, an index
// past the ACTIVE batch count (the pool may hold more stale batches than this
// frame activated; writing into one is a silently lost CL), a duplicate list
// within a batch, a duplicate localOrder within a namespace — fail fast via
// RendererInvariantFailure in every build config.
class SubmitTimeline {
public:
    template <class T>
    struct Ordered {
        T cl = nullptr;
        uint32_t order = 0; // pre-assigned local order within its namespace
    };

    struct PassBatch {
        ID3D12GraphicsCommandList* driver = nullptr;                  // DIRECT; executes the bundles
        std::vector<Ordered<ID3D12GraphicsCommandList*>> bundles;     // TYPE_BUNDLE
        std::vector<Ordered<ID3D12CommandList*>> directs;             // ready command lists
        // Async-compute step 6: which queue this batch's lists are submitted to. A batch belongs to
        // exactly one pass node, and a pass has exactly one queue (D1), so this is well-defined.
        RenderQueue queue = RenderQueue::Graphics;
        // The latest batch on the OTHER queue that this batch must follow, or kNoCrossQueueWait.
        // Derived by the graph from the pass's prereqs/mtDeps — NOT from batch order, because
        // "later in the list" is not a dependency and treating it as one would serialise the two
        // queues and delete the overlap this whole plan exists to create (D2).
        size_t crossQueueWait = kNoCrossQueueWait;
    };

    static constexpr size_t kNoCrossQueueWait = (size_t)-1;

    // One ExecuteCommandLists' worth of work: a contiguous run of batches on ONE queue, plus the
    // cross-queue synchronisation that must bracket it.
    //
    // Segments rather than two flat arrays, because a fence edge lands BETWEEN submissions: the
    // producer queue must be told to signal after the work its consumer waits on, and a flat array
    // has no place to express "here". With no async pass there is exactly ONE segment and it is
    // today's array.
    struct Submission {
        RenderQueue queue = RenderQueue::Graphics;
        std::vector<ID3D12CommandList*> lists;
        size_t waitForBatch = kNoCrossQueueWait; // wait for the other queue's progress past this
        size_t lastBatch = kNoCrossQueueWait;    // highest batch index in this segment
        // Someone on the other queue waits for THIS segment's last batch, so the producer must
        // signal right after it — not at the end of whatever else happens to be submitted to the
        // same queue. See the note on segment boundaries in GatherFrameLists.
        bool signalAfter = false;
    };

    // Start a new frame: deactivate every pooled batch (reset in place).
    void BeginTimeline();

    // Activate the next batch slot, reusing a pooled one when available.
    // `queue` fixes which queue the batch's lists will be submitted to (step 6).
    size_t BeginBatch(RenderQueue queue = RenderQueue::Graphics);

    // Step 6: record that `batchIndex` must not start before the other queue has finished
    // `waitForBatch`. Called by the graph while it unrolls, where the prereq information lives.
    void SetCrossQueueWait(size_t batchIndex, size_t waitForBatch);

    // Registration — callable from any worker thread. localOrder positions the
    // list within its batch namespace (directs / bundles).
    void RegisterDirect(ID3D12CommandList* cl, size_t batchIndex, uint32_t localOrder);
    void RegisterBundle(ID3D12GraphicsCommandList* cl, size_t batchIndex, uint32_t localOrder);
    void RegisterDriver(ID3D12GraphicsCommandList* cl, size_t batchIndex);

    // Submit-phase access — single-threaded by contract (every worker has
    // finished registering before the frame is gathered).
    size_t ActiveBatchCount() const { return activeBatchCount_; }
    const PassBatch& Batch(size_t index) const { return batches_[index]; }

    // Sort + validate each active batch's directs and bundles by localOrder.
    // GatherFrameLists runs this first; exposed so the stress harness can check
    // ordering (and trigger the duplicate-order failure) without executing
    // bundles, which needs a real driver command list.
    void ApplySubmitOrder();

    // Flatten the active batches in order into `out`: per batch the driver
    // (after executing its bundles into it, in localOrder order), then the
    // direct lists in localOrder order. A batch with bundles but no driver
    // gets one from makeFallbackDriver(). Deactivates the timeline afterwards.
    //
    // The driver's lifecycle ends here (step 4): bundles are appended in sorted
    // order and the driver is CLOSED exactly once, at gather time. Direct lists
    // were already closed by EndThreadCommandList; this only collects them.
    template <class MakeFallbackDriver>
    void GatherFrameLists(std::vector<Submission>& out, MakeFallbackDriver&& makeFallbackDriver)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        ApplySubmitOrderLocked_();
        out.clear();
        // Which batches are the TARGET of some cross-queue wait. Computed first, because a segment
        // boundary is needed AFTER each of them and the walk below is single-pass.
        waitTargets_.assign(activeBatchCount_, false);
        for (size_t i = 0; i < activeBatchCount_; ++i) {
            const size_t w = batches_[i].crossQueueWait;
            if (w != kNoCrossQueueWait && w < activeBatchCount_) { waitTargets_[w] = true; }
        }
        const std::vector<bool>& waitTargets = waitTargets_;
        for (size_t i = 0; i < activeBatchCount_; ++i) {
            PassBatch& pb = batches_[i];
            // Step 6: open a new SEGMENT when the queue changes, or when this batch introduces a
            // cross-queue wait (the wait has to be issued before the work that needs it, and a
            // queue Wait applies to everything submitted after it). Otherwise keep appending, so
            // the common case — every batch on Graphics, no waits — produces exactly ONE segment
            // holding exactly today's array.
            const bool needsNewSegment =
                out.empty() ||
                out.back().queue != pb.queue ||
                pb.crossQueueWait != kNoCrossQueueWait ||
                (!out.empty() && out.back().signalAfter); // previous segment ends at a signal point
            if (needsNewSegment) {
                out.push_back(Submission{});
                out.back().queue = pb.queue;
                out.back().waitForBatch = pb.crossQueueWait;
            }
            Submission& seg = out.back();
            seg.lastBatch = i;
            // A batch that someone on the other queue waits FOR ends its segment, so the signal can
            // be placed immediately after it. Without this cut the signal lands after everything
            // else already submitted to that queue, and the consumer waits for far more than the
            // graph said — measured: the first cut of this made RTTrace wait for the whole graphics
            // prefix and it overlapped the small light passes instead of VsmPageRender.
            if (waitTargets[i]) { seg.signalAfter = true; }
            ID3D12GraphicsCommandList* driver = pb.driver;
            if (driver == nullptr && !pb.bundles.empty()) {
                driver = makeFallbackDriver();
            }
            if (driver != nullptr) {
                if (!pb.bundles.empty()) {
                    // The ONE legal place to time bundled work. A bundle cannot contain a
                    // timestamp query, and this loop runs after the pass body closed its own
                    // scope — so without this bracket every bundled draw executed inside the
                    // driver list but outside every scope, and showed in the GPU trace as a hole
                    // rather than as work. Guarded on non-empty so batches without bundles do not
                    // each pay for an empty scope.
                    GPU_SCOPE(driver, ProfilerScopes::kExecuteBundles);
                    for (const auto& bundle : pb.bundles) {
                        driver->ExecuteBundle(bundle.cl); // non-null (checked at registration)
                    }
                }
                if (FAILED(driver->Close())) {
                    RendererInvariantFailure("SubmitTimeline::GatherFrameLists: driver Close() failed");
                }
                seg.lists.push_back(driver);
            }
            for (const auto& direct : pb.directs) {
                seg.lists.push_back(direct.cl);
            }
        }
        ResetBatchesInPlaceLocked_();
    }

    // Shutdown only: actually release the pooled memory.
    void Clear();

private:
    PassBatch& BatchForRegistrationLocked_(size_t batchIndex, const char* invariantMsg);
    static void CheckNotAlreadyRegisteredLocked_(const PassBatch& batch, ID3D12CommandList* cl, const char* invariantMsg);
    void ApplySubmitOrderLocked_();
    void ResetBatchesInPlaceLocked_();

    std::vector<PassBatch> batches_; // persistent pool; the first activeBatchCount_ belong to this frame
    std::vector<bool> waitTargets_;  // step 8: batches some other queue waits for (reused per frame)
    size_t activeBatchCount_ = 0;
    std::mutex mtx_;
};
