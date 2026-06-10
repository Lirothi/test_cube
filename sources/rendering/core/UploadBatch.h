#pragma once

#include <d3d12.h>
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
    void SubmitAndWait(Renderer* renderer);

    bool IsOpen() const { return open_; }
    ID3D12GraphicsCommandList* CommandList() const { return cmdList_.Get(); }
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* KeepAlive() { return &keepAlive_; }

private:
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmdList_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> keepAlive_;
    bool open_ = false;
};
