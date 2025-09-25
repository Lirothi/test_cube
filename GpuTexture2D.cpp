#include "GpuTexture2D.h"

#include "Renderer.h"
#include "Helpers.h"

#include <stdexcept>
#include <cstring>

using Microsoft::WRL::ComPtr;

bool GpuTexture2D::Create(Renderer* renderer,
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    UINT mipLevels,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initialState,
    const D3D12_CLEAR_VALUE* clearValue)
{
    if (!renderer) {
        return false;
    }

    auto* device = renderer->GetDevice();
    if (!device) {
        return false;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = static_cast<UINT16>(mipLevels);
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;

    HRESULT hr = device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        initialState,
        clearValue,
        IID_PPV_ARGS(&resource_));
    if (FAILED(hr)) {
        return false;
    }

    renderer->SetResourceState(resource_.Get(), initialState);

    format_ = format;
    width_ = width;
    height_ = height;
    mipLevels_ = mipLevels;
    flags_ = flags;

    return true;
}

bool GpuTexture2D::CreateFromData(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    DXGI_FORMAT format,
    UINT width,
    UINT height,
    UINT mipLevels,
    D3D12_RESOURCE_FLAGS flags,
    const void* data,
    size_t rowPitchBytes,
    D3D12_RESOURCE_STATES finalState,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    if (!renderer || !uploadCmdList || data == nullptr) {
        return false;
    }

    auto* device = renderer->GetDevice();
    if (!device) {
        return false;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = static_cast<UINT16>(mipLevels);
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;

    HRESULT hr = device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&resource_));
    if (FAILED(hr)) {
        return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSize = 0;
    UINT64 totalBytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &numRows, &rowSize, &totalBytes);

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = totalBytes;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> upload;
    hr = device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&upload));
    if (FAILED(hr)) {
        resource_.Reset();
        return false;
    }

    uint8_t* mapped = nullptr;
    D3D12_RANGE range{ 0, 0 };
    hr = upload->Map(0, &range, reinterpret_cast<void**>(&mapped));
    if (FAILED(hr)) {
        resource_.Reset();
        upload.Reset();
        return false;
    }

    const uint8_t* src = reinterpret_cast<const uint8_t*>(data);
    for (UINT row = 0; row < numRows; ++row) {
        std::memcpy(mapped + footprint.Offset + row * footprint.Footprint.RowPitch,
            src + row * rowPitchBytes,
            rowPitchBytes);
    }
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = resource_.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = upload.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = footprint;

    uploadCmdList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource_.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = finalState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    uploadCmdList->ResourceBarrier(1, &barrier);

    if (uploadKeepAlive) {
        uploadKeepAlive->push_back(upload);
    }

    renderer->SetResourceState(resource_.Get(), finalState);

    format_ = format;
    width_ = width;
    height_ = height;
    mipLevels_ = mipLevels;
    flags_ = flags;

    return true;
}

void GpuTexture2D::AllocateDescriptorHeap_(ID3D12Device* device, UINT count)
{
    if (count == 0) {
        cpuHeap_.Reset();
        srvCPU_ = {};
        uavCPU_ = {};
        return;
    }

    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.NumDescriptors = count;
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    ThrowIfFailed(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&cpuHeap_)));
    auto start = cpuHeap_->GetCPUDescriptorHandleForHeapStart();
    srvCPU_ = {};
    uavCPU_ = {};

    if (count >= 1) {
        srvCPU_ = start;
    }
    if (count >= 2) {
        const UINT incr = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        uavCPU_ = srvCPU_;
        uavCPU_.ptr += incr;
    }
}

void GpuTexture2D::CreateViews(Renderer* renderer,
    bool createSrv,
    bool createUav,
    DXGI_FORMAT srvFormat,
    DXGI_FORMAT uavFormat,
    UINT mipLevels)
{
    if (!renderer) {
        return;
    }
    auto* device = renderer->GetDevice();
    if (!device || !resource_) {
        return;
    }

    UINT descriptorCount = (createSrv ? 1u : 0u) + (createUav ? 1u : 0u);
    AllocateDescriptorHeap_(device, descriptorCount);

    D3D12_CPU_DESCRIPTOR_HANDLE cursor = {};
    if (createSrv) {
        cursor = srvCPU_;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = srvFormat;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = mipLevels;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.ResourceMinLODClamp = 0.0f;
        device->CreateShaderResourceView(resource_.Get(), &srv, srvCPU_);
    }

    if (createUav) {
        if (!createSrv) {
            uavCPU_ = cpuHeap_->GetCPUDescriptorHandleForHeapStart();
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = uavFormat;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Texture2D.MipSlice = 0;
        uav.Texture2D.PlaneSlice = 0;
        device->CreateUnorderedAccessView(resource_.Get(), nullptr, &uav, uavCPU_);
    }
}
