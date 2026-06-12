#include "rendering/core/SubmitTimeline.h"

#include "rendering/core/RendererInvariantFailure.h"

void SubmitTimeline::BeginTimeline()
{
    std::lock_guard<std::mutex> lk(mtx_);
    ResetBatchesInPlaceLocked_();
}

size_t SubmitTimeline::BeginBatch()
{
    std::lock_guard<std::mutex> lk(mtx_);
    const size_t idx = activeBatchCount_;
    if (idx == batches_.size()) {
        batches_.emplace_back();
    }
    ++activeBatchCount_;
    return idx;
}

void SubmitTimeline::RegisterDirect(ID3D12CommandList* cl, size_t batchIndex)
{
    if (cl == nullptr) {
        RendererInvariantFailure("SubmitTimeline::RegisterDirect: null command list");
    }
    std::lock_guard<std::mutex> lk(mtx_);
    PassBatch& batch = BatchForRegistrationLocked_(batchIndex,
        "SubmitTimeline::RegisterDirect: batch index outside the active range (command list would be lost)");
    CheckNotAlreadyRegisteredLocked_(batch, cl,
        "SubmitTimeline::RegisterDirect: command list already registered in this batch");
    batch.directs.push_back(cl);
}

void SubmitTimeline::RegisterBundle(ID3D12GraphicsCommandList* cl, size_t batchIndex)
{
    if (cl == nullptr) {
        RendererInvariantFailure("SubmitTimeline::RegisterBundle: null command list");
    }
    std::lock_guard<std::mutex> lk(mtx_);
    PassBatch& batch = BatchForRegistrationLocked_(batchIndex,
        "SubmitTimeline::RegisterBundle: batch index outside the active range (bundle would be lost)");
    CheckNotAlreadyRegisteredLocked_(batch, cl,
        "SubmitTimeline::RegisterBundle: bundle already registered in this batch");
    batch.bundles.push_back(cl);
}

void SubmitTimeline::RegisterDriver(ID3D12GraphicsCommandList* cl, size_t batchIndex)
{
    if (cl == nullptr) {
        RendererInvariantFailure("SubmitTimeline::RegisterDriver: null command list");
    }
    std::lock_guard<std::mutex> lk(mtx_);
    PassBatch& batch = BatchForRegistrationLocked_(batchIndex,
        "SubmitTimeline::RegisterDriver: batch index outside the active range (driver would be lost)");
    if (batch.driver != nullptr) {
        RendererInvariantFailure("SubmitTimeline::RegisterDriver: batch already has a driver (previous driver would be lost)");
    }
    CheckNotAlreadyRegisteredLocked_(batch, cl,
        "SubmitTimeline::RegisterDriver: command list already registered in this batch");
    batch.driver = cl;
}

void SubmitTimeline::Clear()
{
    std::lock_guard<std::mutex> lk(mtx_);
    batches_.clear(); // real destroy — releases the pooled per-batch vectors
    activeBatchCount_ = 0;
}

SubmitTimeline::PassBatch& SubmitTimeline::BatchForRegistrationLocked_(size_t batchIndex, const char* invariantMsg)
{
    // Checked against the ACTIVE count, not the pool size: stale pooled slots
    // beyond it are never gathered, so a write there is a silently lost CL.
    if (batchIndex >= activeBatchCount_) {
        RendererInvariantFailure(invariantMsg);
    }
    return batches_[batchIndex];
}

void SubmitTimeline::CheckNotAlreadyRegisteredLocked_(const PassBatch& batch, ID3D12CommandList* cl, const char* invariantMsg)
{
    // Linear pointer scan — batches hold at most tens of lists. If batches
    // ever grow large, switch to an epoch mark on the pooled entry; never
    // demote this to Debug-only (a duplicate submits the same CL twice).
    if (batch.driver == cl) {
        RendererInvariantFailure(invariantMsg);
    }
    for (ID3D12GraphicsCommandList* bundle : batch.bundles) {
        if (bundle == cl) {
            RendererInvariantFailure(invariantMsg);
        }
    }
    for (ID3D12CommandList* direct : batch.directs) {
        if (direct == cl) {
            RendererInvariantFailure(invariantMsg);
        }
    }
}

void SubmitTimeline::ResetBatchesInPlaceLocked_()
{
    for (PassBatch& batch : batches_) {
        batch.driver = nullptr;
        batch.bundles.clear(); // retains heap capacity
        batch.directs.clear();
    }
    activeBatchCount_ = 0;
}
