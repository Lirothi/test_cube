#include "rendering/rt/ReflectionHistory.h"

namespace rt {

bool ReflectionHistory::EnsureSize(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format)
{
    if (!device || width == 0 || height == 0) {
        return false;
    }
    if (tex_[0] && w_ == width && h_ == height && fmt_ == format) {
        return false; // up to date
    }

    w_ = width; h_ = height; fmt_ = format;

    if (!heap_) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 4; // srv0, srv1, uav0, uav1
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // CPU-only (staged into the bindless heap)
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) {
            return false;
        }
    }
    const UINT incr = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE base = heap_->GetCPUDescriptorHandleForHeapStart();

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    for (int i = 0; i < 2; ++i) {
        tex_[i].Reset();
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&tex_[i]));

        srv_[i] = base; srv_[i].ptr += static_cast<SIZE_T>(i) * incr;        // slots 0,1
        uav_[i] = base; uav_[i].ptr += static_cast<SIZE_T>(2 + i) * incr;    // slots 2,3

        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = format;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(tex_[i].Get(), &sd, srv_[i]);

        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = format;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(tex_[i].Get(), nullptr, &ud, uav_[i]);
    }
    return true;
}

void ReflectionHistory::Reset()
{
    tex_[0].Reset();
    tex_[1].Reset();
    heap_.Reset();
    srv_[0] = srv_[1] = uav_[0] = uav_[1] = D3D12_CPU_DESCRIPTOR_HANDLE{};
    w_ = h_ = 0;
    fmt_ = DXGI_FORMAT_UNKNOWN;
}

} // namespace rt
