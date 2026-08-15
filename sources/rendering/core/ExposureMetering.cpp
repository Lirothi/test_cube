#include "rendering/core/ExposureMetering.h"

#include "core/Helpers.h"
#include "rendering/core/Renderer.h"

using Microsoft::WRL::ComPtr;

namespace
{
    // Four floats: adapted EV100, plus the three lanes the section 6.5 debug contract will fill in
    // (metered low percentile, metered high percentile, target EV100). Sized now so P2 does not
    // have to grow the resource and re-plumb its descriptors.
    constexpr UINT64 kExposureRecordBytes = 16u;

    ComPtr<ID3D12Resource> CreateRawUavBuffer(ID3D12Device* device, UINT64 bytes)
    {
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        ComPtr<ID3D12Resource> resource;
        ThrowIfFailed(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(resource.GetAddressOf())));
        return resource;
    }
}

void ExposureMetering::EnsureResources(Renderer* renderer)
{
    if (created_ || !renderer || !renderer->GetDevice())
    {
        return;
    }

    ID3D12Device* device = renderer->GetDevice();

    // UNORDERED_ACCESS is both the creation and the canonical resting state: every future consumer
    // is a compute dispatch, so declaring anything else would just add a barrier at each end.
    histogram_.Attach(renderer->Declarations(),
        CreateRawUavBuffer(device, static_cast<UINT64>(kHistogramBins) * sizeof(std::uint32_t)),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"Exposure.Histogram");
    exposure_.Attach(renderer->Declarations(),
        CreateRawUavBuffer(device, kExposureRecordBytes),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"Exposure.Value");

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = 4; // histogram SRV/UAV + exposure SRV/UAV
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap_)));

    const UINT increment =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptorHeap_->GetCPUDescriptorHandleForHeapStart();
    const auto next = [&]()
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE result = handle;
        handle.ptr += increment;
        return result;
    };

    // Raw views throughout: R32_TYPELESS + BUFFER_*_FLAG_RAW, with sizes in 32-bit words.
    const auto rawSrv = [&](ID3D12Resource* resource, UINT words, D3D12_CPU_DESCRIPTOR_HANDLE dst)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.NumElements = words;
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device->CreateShaderResourceView(resource, &srvDesc, dst);
    };
    const auto rawUav = [&](ID3D12Resource* resource, UINT words, D3D12_CPU_DESCRIPTOR_HANDLE dst)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = words;
        uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(resource, nullptr, &uavDesc, dst);
    };

    constexpr UINT kExposureWords = static_cast<UINT>(kExposureRecordBytes / sizeof(std::uint32_t));
    histogramSrv_ = next();
    rawSrv(histogram_.Get(), kHistogramBins, histogramSrv_);
    histogramUav_ = next();
    rawUav(histogram_.Get(), kHistogramBins, histogramUav_);
    exposureSrv_ = next();
    rawSrv(exposure_.Get(), kExposureWords, exposureSrv_);
    exposureUav_ = next();
    rawUav(exposure_.Get(), kExposureWords, exposureUav_);

    created_ = true;
    // Contents are undefined until something writes them, so the first P2 solve must seed.
    resetRequested_ = true;
}

void ExposureMetering::Release()
{
    // GpuResource::Reset undeclares before dropping the reference, which is what keeps the
    // canonical-state registry from holding a dangling pointer after a device loss.
    histogram_.Reset();
    exposure_.Reset();
    descriptorHeap_.Reset();
    histogramSrv_ = {};
    histogramUav_ = {};
    exposureSrv_ = {};
    exposureUav_ = {};
    created_ = false;
    resetRequested_ = true;
}
