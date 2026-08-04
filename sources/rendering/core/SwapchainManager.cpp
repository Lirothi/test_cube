#include "rendering/core/SwapchainManager.h"

#include "core/Helpers.h"
#include "rendering/core/TextureCreate.h"

void SwapchainManager::Create(ID3D12Device* device, ID3D12CommandQueue* queue, HWND hwnd,
    UINT width, UINT height, DXGI_FORMAT resourceFormat, DXGI_FORMAT viewFormat)
{
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));

    // Destroy the old swap chain and RTVs when reinitializing (if any)
    for (UINT i = 0; i < render::kFrameCount; ++i) {
        renderTargets_[i].Reset();
    }
    rtvHeap_.Reset();
    swapChain_.Reset();

    // Create the swap chain (render::kFrameCount)
    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.BufferCount = render::kFrameCount;
    scd.Width = width;
    scd.Height = height;
    scd.Format = resourceFormat;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.SampleDesc.Count = 1;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap1;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(
        queue, hwnd, &scd, nullptr, nullptr, &swap1));
    ThrowIfFailed(swap1.As(&swapChain_));

    // RTV heap
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.NumDescriptors = render::kFrameCount;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap_)));
    rtvDescriptorSize_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // RTVs
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < render::kFrameCount; ++i) {
        ThrowIfFailed(swapChain_->GetBuffer(i, IID_PPV_ARGS(&renderTargets_[i])));

        D3D12_RENDER_TARGET_VIEW_DESC rtvFmt{};
        rtvFmt.Format = viewFormat;
        rtvFmt.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rtvFmt.Texture2D.MipSlice   = 0;
        rtvFmt.Texture2D.PlaneSlice = 0;

        device->CreateRenderTargetView(renderTargets_[i].Get(), &rtvFmt, rtv);
        rtv.ptr += rtvDescriptorSize_;
    }
}

void SwapchainManager::CreateDepth(ID3D12Device* device, UINT width, UINT height,
    DXGI_FORMAT resourceFormat, DXGI_FORMAT viewFormat)
{
    dsvHeap_.Reset();
    depthBuffer_.Reset();

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dsvHeap_)));
    dsvDescriptorSize_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    D3D12_RESOURCE_DESC depthDesc{};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = resourceFormat;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CLEAR_VALUE cv{};
    cv.Format = viewFormat;
    cv.DepthStencil.Depth = 0.0f;
    cv.DepthStencil.Stencil = 0;

    ThrowIfFailed(render::CreateCommittedTexture(device, heap, D3D12_HEAP_FLAG_NONE, depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, &depthBuffer_));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsv.Flags = D3D12_DSV_FLAG_NONE;
    device->CreateDepthStencilView(depthBuffer_.Get(), &dsv, dsvHeap_->GetCPUDescriptorHandleForHeapStart());
}

void SwapchainManager::ReleaseBuffersForResize()
{
    for (UINT i = 0; i < render::kFrameCount; ++i) {
        renderTargets_[i].Reset();
    }
    depthBuffer_.Reset();
    dsvHeap_.Reset();
    rtvHeap_.Reset();
}

void SwapchainManager::ResizeBuffers(UINT width, UINT height)
{
    DXGI_SWAP_CHAIN_DESC desc{};
    ThrowIfFailed(swapChain_->GetDesc(&desc));
    ThrowIfFailed(swapChain_->ResizeBuffers(render::kFrameCount, width, height, desc.BufferDesc.Format, desc.Flags));
}

void SwapchainManager::Present()
{
    ThrowIfFailed(swapChain_->Present(0, DXGI_PRESENT_ALLOW_TEARING));
}

void SwapchainManager::ReleaseBuffers()
{
    for (UINT i = 0; i < render::kFrameCount; ++i) {
        renderTargets_[i].Reset();
    }
    depthBuffer_.Reset();
    dsvHeap_.Reset();
    rtvHeap_.Reset();
    rtvDescriptorSize_ = 0;
    dsvDescriptorSize_ = 0;
}

void SwapchainManager::ReleaseSwapchain()
{
    if (swapChain_) {
        BOOL fs = FALSE;
        Microsoft::WRL::ComPtr<IDXGIOutput> out;
        if (SUCCEEDED(swapChain_->GetFullscreenState(&fs, &out)) && fs) {
            (void)swapChain_->SetFullscreenState(FALSE, nullptr);
        }
        swapChain_.Reset();
    }
}
