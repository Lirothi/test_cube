#pragma once

#include <d3d12.h>
#include <source_location>
#include <vector>
#include <wrl/client.h>

class Renderer;

// RAII batch for one-off resource uploads (init-time uploads, level loads).
// Owns a DIRECT command allocator + command list and the keep-alive list that
// holds intermediate upload buffers until the GPU has consumed them.
//
// Usage:
//   UploadBatch batch;
//   batch.Begin(renderer);
//   ... record uploads into batch.CommandList(), park intermediates in batch.KeepAlive() ...
//   batch.SubmitAndWait(renderer);   // close, execute, full GPU wait, release intermediates
//
// A batch that is begun but never submitted (e.g. a failed level load) is closed
// and dropped on destruction without executing any GPU work.
class UploadBatch
{
public:
    UploadBatch() = default;
    ~UploadBatch();

    UploadBatch(const UploadBatch&) = delete;
    UploadBatch& operator=(const UploadBatch&) = delete;

    bool Begin(Renderer* renderer);
    // Close and enqueue without waiting. The list, its allocator and the intermediates go to the deferred-release
    // service (rendering/core/DeferredRelease.h, R16.8): released once the GPU has finished the frame the list
    // executes with -- the caller may drop this batch at once, and Begin it again.
    bool Submit(Renderer* renderer);
    // Close, enqueue, idle the GPU (Renderer::WaitForPreviousFrame) and release all of it at once -- not through the
    // service: a chain of loads with no frame between them would otherwise hold every batch's intermediates together.
    // `where` is the caller's: the idle is counted by it ("gpu idle waits:", R16.10), not by this file.
    void SubmitAndWait(Renderer* renderer, std::source_location where = std::source_location::current());

    bool IsOpen() const { return open_; }
    ID3D12GraphicsCommandList* CommandList() const { return cmdList_.Get(); }
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* KeepAlive() { return &keepAlive_; }

private:
    bool CloseAndExecute(Renderer* renderer);

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmdList_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> keepAlive_;
    bool open_ = false;
};
