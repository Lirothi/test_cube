#pragma once
#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "rendering/core/RendererInvariantFailure.h"

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
        std::vector<Ordered<ID3D12CommandList*>> directs;             // ready DIRECT command lists
    };

    // Start a new frame: deactivate every pooled batch (reset in place).
    void BeginTimeline();

    // Activate the next batch slot, reusing a pooled one when available.
    size_t BeginBatch();

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
    void GatherFrameLists(std::vector<ID3D12CommandList*>& out, MakeFallbackDriver&& makeFallbackDriver)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        ApplySubmitOrderLocked_();
        size_t expectedListCount = 0;
        for (size_t i = 0; i < activeBatchCount_; ++i) {
            const PassBatch& pb = batches_[i];
            expectedListCount += pb.directs.size();
            if (pb.driver != nullptr || !pb.bundles.empty()) {
                ++expectedListCount;
            }
        }
        out.reserve(out.size() + expectedListCount);
        for (size_t i = 0; i < activeBatchCount_; ++i) {
            PassBatch& pb = batches_[i];
            ID3D12GraphicsCommandList* driver = pb.driver;
            if (driver == nullptr && !pb.bundles.empty()) {
                driver = makeFallbackDriver();
            }
            if (driver != nullptr) {
                for (const auto& bundle : pb.bundles) {
                    driver->ExecuteBundle(bundle.cl); // bundles are non-null (checked at registration)
                }
                if (FAILED(driver->Close())) {
                    RendererInvariantFailure("SubmitTimeline::GatherFrameLists: driver Close() failed");
                }
                out.push_back(driver);
            }
            for (const auto& direct : pb.directs) {
                out.push_back(direct.cl);
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
    size_t activeBatchCount_ = 0;
    std::mutex mtx_;
};
