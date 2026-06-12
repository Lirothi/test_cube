#pragma once
#include <d3d12.h>

#include <cstddef>
#include <mutex>
#include <vector>

// Per-frame submission timeline: the ordered pass batches that collect the
// command lists render-pass workers record (concurrently) for one frame.
// Batch order defines GPU execution order; within a batch the driver list
// comes first (its bundles execute inside it), then the direct lists.
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
// within a batch — fail fast via RendererInvariantFailure in every build
// config.
class SubmitTimeline {
public:
    struct PassBatch {
        ID3D12GraphicsCommandList* driver = nullptr;     // DIRECT; executes the bundles
        std::vector<ID3D12GraphicsCommandList*> bundles; // TYPE_BUNDLE
        std::vector<ID3D12CommandList*> directs;         // ready DIRECT command lists
    };

    // Start a new frame: deactivate every pooled batch (reset in place).
    void BeginTimeline();

    // Activate the next batch slot, reusing a pooled one when available.
    size_t BeginBatch();

    // Registration — callable from any worker thread.
    void RegisterDirect(ID3D12CommandList* cl, size_t batchIndex);
    void RegisterBundle(ID3D12GraphicsCommandList* cl, size_t batchIndex);
    void RegisterDriver(ID3D12GraphicsCommandList* cl, size_t batchIndex);

    // Submit-phase access — single-threaded by contract (every worker has
    // finished registering before the frame is gathered).
    size_t ActiveBatchCount() const { return activeBatchCount_; }
    const PassBatch& Batch(size_t index) const { return batches_[index]; }

    // Flatten the active batches in order into `out`: per batch the driver
    // (after executing its bundles into it), then the direct lists in
    // registration order. A batch with bundles but no driver gets one from
    // makeFallbackDriver(). Deactivates the timeline afterwards.
    template <class MakeFallbackDriver>
    void GatherFrameLists(std::vector<ID3D12CommandList*>& out, MakeFallbackDriver&& makeFallbackDriver)
    {
        std::lock_guard<std::mutex> lk(mtx_);
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
                for (ID3D12GraphicsCommandList* bundle : pb.bundles) {
                    driver->ExecuteBundle(bundle); // bundles are non-null (checked at registration)
                }
                out.push_back(driver);
            }
            out.insert(out.end(), pb.directs.begin(), pb.directs.end());
        }
        ResetBatchesInPlaceLocked_();
    }

    // Shutdown only: actually release the pooled memory.
    void Clear();

private:
    PassBatch& BatchForRegistrationLocked_(size_t batchIndex, const char* invariantMsg);
    static void CheckNotAlreadyRegisteredLocked_(const PassBatch& batch, ID3D12CommandList* cl, const char* invariantMsg);
    void ResetBatchesInPlaceLocked_();

    std::vector<PassBatch> batches_; // persistent pool; the first activeBatchCount_ belong to this frame
    size_t activeBatchCount_ = 0;
    std::mutex mtx_;
};
