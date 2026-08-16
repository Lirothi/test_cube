#include "rendering/core/ExposureMetering.h"

#include "core/Helpers.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/TextureCreate.h" // P3B base log-luminance texture

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
    heapDesc.NumDescriptors = 6; // histogram SRV/UAV + exposure SRV/UAV + base-lum SRV/UAV
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

    // P3B base log-luminance. Bilinear-sampled by the tonemap, so it needs a filterable format;
    // R16_FLOAT is ample for a log quantity spanning roughly 24 stops.
    {
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = kBaseLumWidth;
        desc.Height = kBaseLumHeight;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        ComPtr<ID3D12Resource> resource;
        ThrowIfFailed(render::CreateCommittedTexture(device, heapProps, D3D12_HEAP_FLAG_NONE,
            desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, &resource));
        baseLum_.Attach(renderer->Declarations(), std::move(resource),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"Exposure.BaseLogLum");

        baseLumSrv_ = next();
        D3D12_SHADER_RESOURCE_VIEW_DESC baseSrv{};
        baseSrv.Format = DXGI_FORMAT_R16_FLOAT;
        baseSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        baseSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        baseSrv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(baseLum_.Get(), &baseSrv, baseLumSrv_);

        baseLumUav_ = next();
        D3D12_UNORDERED_ACCESS_VIEW_DESC baseUav{};
        baseUav.Format = DXGI_FORMAT_R16_FLOAT;
        baseUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(baseLum_.Get(), nullptr, &baseUav, baseLumUav_);
    }

    // Readback ring for the dev UI. Tiny and persistently mapped, in the same shape the particle
    // alive-count debug readback uses.
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = kExposureRecordBytes * kReadbackSlots;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_)));
        readback_->SetName(L"Exposure.Readback");

        void* mapped = nullptr;
        ThrowIfFailed(readback_->Map(0, nullptr, &mapped));
        readbackPtr_ = static_cast<const float*>(mapped);

        rd.Width = static_cast<UINT64>(kHistogramBins) * sizeof(std::uint32_t) * kReadbackSlots;
        ThrowIfFailed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&histogramReadback_)));
        histogramReadback_->SetName(L"Exposure.HistogramReadback");
        void* mappedHist = nullptr;
        ThrowIfFailed(histogramReadback_->Map(0, nullptr, &mappedHist));
        histogramReadbackPtr_ = static_cast<const std::uint32_t*>(mappedHist);
    }

    created_ = true;
    // Contents are undefined until something writes them, so the first P2 solve must seed.
    resetRequested_ = true;
}

void ExposureMetering::RecordReadbackCopy(ID3D12GraphicsCommandList* cl)
{
    if (!created_ || !readback_ || !cl)
    {
        return;
    }
    const UINT64 slot = readbackFrame_ % kReadbackSlots;
    cl->CopyBufferRegion(readback_.Get(), slot * kExposureRecordBytes,
        exposure_.Get(), 0, kExposureRecordBytes);
    if (histogramReadback_)
    {
        constexpr UINT64 kHistogramBytes =
            static_cast<UINT64>(kHistogramBins) * sizeof(std::uint32_t);
        cl->CopyBufferRegion(histogramReadback_.Get(), slot * kHistogramBytes,
            histogram_.Get(), 0, kHistogramBytes);
    }
    ++readbackFrame_;
}

bool ExposureMetering::LatestHistogram(float* outBins, UINT binCount, UINT* outTotal) const
{
    if (!histogramReadbackPtr_ || !outBins || binCount == 0 || readbackFrame_ <= kReadbackSlots)
    {
        return false;
    }
    // Same oldest-slot rule as LatestReadback: it retired frames ago, so no fence is needed.
    const std::uint64_t slot = (readbackFrame_ + 1u) % kReadbackSlots;
    const std::uint32_t* bins = histogramReadbackPtr_ + slot * kHistogramBins;

    const UINT count = (binCount < kHistogramBins) ? binCount : kHistogramBins;
    std::uint32_t total = 0;
    std::uint32_t peak = 0;
    for (UINT i = 0; i < kHistogramBins; ++i)
    {
        total += bins[i];
        peak = (bins[i] > peak) ? bins[i] : peak;
    }
    // Normalised to the PEAK, not the total: a histogram is read for its shape, and one tall bin
    // (a big flat sky) would otherwise flatten everything else into the axis.
    const float inv = (peak > 0u) ? (1.0f / static_cast<float>(peak)) : 0.0f;
    for (UINT i = 0; i < count; ++i)
    {
        outBins[i] = static_cast<float>(bins[i]) * inv;
    }
    if (outTotal) { *outTotal = total; }
    return true;
}

ExposureMetering::Readback ExposureMetering::LatestReadback() const
{
    Readback out{};
    if (!readbackPtr_ || readbackFrame_ <= kReadbackSlots)
    {
        return out;
    }
    // Oldest slot: written kReadbackSlots-1 frames ago, so it has certainly retired. Reading the
    // newest one would race the GPU.
    const std::uint64_t slot = (readbackFrame_ + 1u) % kReadbackSlots;
    const float* record = readbackPtr_ + slot * (kExposureRecordBytes / sizeof(float));
    out.adaptedEv100 = record[0];
    out.lowLuminance = record[1];
    out.highLuminance = record[2];
    out.targetEv100 = record[3];
    out.valid = true;
    return out;
}

void ExposureMetering::Release()
{
    // GpuResource::Reset undeclares before dropping the reference, which is what keeps the
    // canonical-state registry from holding a dangling pointer after a device loss.
    histogram_.Reset();
    exposure_.Reset();
    baseLum_.Reset();
    baseLumUav_ = {};
    baseLumSrv_ = {};
    if (readback_ && readbackPtr_)
    {
        readback_->Unmap(0, nullptr);
    }
    readbackPtr_ = nullptr;
    readback_.Reset();
    if (histogramReadback_ && histogramReadbackPtr_)
    {
        histogramReadback_->Unmap(0, nullptr);
    }
    histogramReadbackPtr_ = nullptr;
    histogramReadback_.Reset();
    readbackFrame_ = 0;
    descriptorHeap_.Reset();
    histogramSrv_ = {};
    histogramUav_ = {};
    exposureSrv_ = {};
    exposureUav_ = {};
    created_ = false;
    resetRequested_ = true;
}
