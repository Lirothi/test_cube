#pragma once

#include <d3d12.h>
#include <cstdint>
#include <memory>
#include <wrl/client.h>

#include "rendering/core/FrameResource.h"

// Owns frame pacing state: the frame fence + event, per-frame fence values,
// and the per-frame FrameResource pools (command allocators/lists, descriptor
// allocators, upload ring). The current frame index stays with the caller.
class FrameScheduler
{
public:
    static constexpr UINT kFrameCount = 2;

    ~FrameScheduler();

    void InitFence(ID3D12Device* device);     // safe no-op when already initialized
    void CreateFrameResources(ID3D12Device* device);

    // Waits until the GPU has finished the given frame slot (by its fence value).
    void WaitForFrame(UINT frameIndex);
    // Signals the fence for a frame slot with the next global value.
    void SignalFrame(ID3D12CommandQueue* queue, UINT frameIndex);
    // Full synchronization: signal and wait (resize/shutdown).
    void WaitForGpuIdle(ID3D12CommandQueue* queue);

    bool HasFence() const { return fence_ != nullptr; }
    FrameResource* GetFrameResource(UINT frameIndex) const
    {
        return frameIndex < kFrameCount ? frameResources_[frameIndex].get() : nullptr;
    }

    // Shutdown is staged to preserve the original release order.
    void ResetFrameState(ID3D12Device* device); // reset pools, release frame resources
    void ReleaseFence();

private:
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    UINT64 nextFenceValue_ = 1;                  // global increment
    UINT64 frameFenceValues_[kFrameCount] = {};  // last signal value for each frame

    std::unique_ptr<FrameResource> frameResources_[kFrameCount];
};
