#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace rt {

// Ping-pong history textures for temporal reflection denoise (S11). Two SSR-res
// textures (kSsrFormat, UAV+SRV); each frame one is "current" (written) and the
// other is "previous" (reprojected + read). Recreated on size change. Their
// SRV/UAV live in a CPU-only heap and are copied into the bindless heap per frame.
// State tracking is the caller's job (register both textures after a realloc).
class ReflectionHistory
{
public:
    // (Re)create on size/format change. Returns true if it (re)allocated.
    bool EnsureSize(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format);
    void Reset();

    bool Ready() const { return tex_[0] != nullptr; }

    // parity (e.g. frame number & 1) picks the current texture; the other is prev.
    ID3D12Resource* Curr(uint64_t parity) const { return tex_[parity & 1u].Get(); }
    ID3D12Resource* Prev(uint64_t parity) const { return tex_[(parity ^ 1u) & 1u].Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE CurrUav(uint64_t parity) const { return uav_[parity & 1u]; }
    D3D12_CPU_DESCRIPTOR_HANDLE PrevSrv(uint64_t parity) const { return srv_[(parity ^ 1u) & 1u]; }

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> tex_[2];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_; // CPU-only: srv0,srv1,uav0,uav1
    D3D12_CPU_DESCRIPTOR_HANDLE srv_[2]{};
    D3D12_CPU_DESCRIPTOR_HANDLE uav_[2]{};
    UINT w_ = 0, h_ = 0;
    DXGI_FORMAT fmt_ = DXGI_FORMAT_UNKNOWN;
};

} // namespace rt
