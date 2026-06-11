#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

// Owns the swapchain, its backbuffers + RTV heap, and the window-sized depth
// buffer + DSV heap. Resource-state bookkeeping for the backbuffers stays with
// the caller (Renderer), which wraps Create/Resize with the tracker updates.
class SwapchainManager
{
public:
    static constexpr UINT kFrameCount = 2;

    // (Re)creates the swapchain and backbuffer RTVs. Releases old buffers first.
    void Create(ID3D12Device* device, ID3D12CommandQueue* queue, HWND hwnd,
        UINT width, UINT height, DXGI_FORMAT resourceFormat, DXGI_FORMAT viewFormat);
    void CreateDepth(ID3D12Device* device, UINT width, UINT height,
        DXGI_FORMAT resourceFormat, DXGI_FORMAT viewFormat);

    // Resize path: drop backbuffer references + heaps, then ResizeBuffers.
    void ReleaseBuffersForResize();
    void ResizeBuffers(UINT width, UINT height);

    void Present();

    // Shutdown is staged to preserve the original release order.
    void ReleaseBuffers();   // backbuffers, depth, RTV/DSV heaps
    void ReleaseSwapchain(); // fullscreen exit + swapchain release

    bool IsValid() const { return swapChain_ != nullptr; }
    UINT CurrentBackBufferIndex() const { return swapChain_ ? swapChain_->GetCurrentBackBufferIndex() : 0; }
    ID3D12Resource* Backbuffer(UINT index) const { return index < kFrameCount ? renderTargets_[index].Get() : nullptr; }
    ID3D12Resource* DepthBuffer() const { return depthBuffer_.Get(); }

    D3D12_CPU_DESCRIPTOR_HANDLE BackbufferRTV(UINT index) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(index) * rtvDescriptorSize_;
        return rtv;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE DepthDSV() const
    {
        return dsvHeap_->GetCPUDescriptorHandleForHeapStart();
    }

private:
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain_;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> renderTargets_[kFrameCount];
    UINT rtvDescriptorSize_ = 0;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> depthBuffer_;
    UINT dsvDescriptorSize_ = 0;
};
