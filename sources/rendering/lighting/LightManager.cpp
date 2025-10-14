#include "rendering/lighting/LightManager.h"

#include <cstdint>
#include <cmath>

#include "core/Helpers.h"
#include "rendering/core/Renderer.h"

LightManager::~LightManager()
{
    Reset();
}

void LightManager::UpdateSpotLightCache()
{
    const size_t maxLights = static_cast<size_t>(kMaxSpotLights);
    cachedSpotLightCount_ = std::min(spotLights_.size(), maxLights);

    for (size_t i = 0; i < cachedSpotLightCount_; ++i)
    {
        spotLights_[i].UpdateCachedData();
    }
}

void LightManager::EnsurePointLightBuffer(Renderer* renderer, size_t requiredLights)
{
    if (!renderer || requiredLights == 0)
    {
        return;
    }

    if (pointLightBuffer_ && pointLightBufferCPU_ && pointLightSrvHandle_.ptr != 0 && pointLightCapacity_ >= requiredLights)
    {
        return;
    }

    if (pointLightBuffer_)
    {
        pointLightBuffer_->Unmap(0, nullptr);
        pointLightBuffer_.Reset();
        pointLightBufferCPU_ = nullptr;
        pointLightCapacity_ = 0;
    }
    pointLightSrvHeap_.Reset();
    pointLightSrvHandle_ = {};

    pointLightCapacity_ = std::max<size_t>(requiredLights, 1);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(pointLightCapacity_ * sizeof(PointLightGpu));
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    ThrowIfFailed(renderer->GetDevice()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(buffer.GetAddressOf())));

    D3D12_RANGE range{ 0, 0 };
    void* mapped = nullptr;
    ThrowIfFailed(buffer->Map(0, &range, &mapped));

    pointLightBuffer_ = buffer;
    pointLightBufferCPU_ = static_cast<PointLightGpu*>(mapped);
    renderer->SetResourceState(pointLightBuffer_.Get(), D3D12_RESOURCE_STATE_GENERIC_READ);

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = 1;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(renderer->GetDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(pointLightSrvHeap_.ReleaseAndGetAddressOf())));
    pointLightSrvHandle_ = pointLightSrvHeap_->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = static_cast<UINT>(pointLightCapacity_);
    srvDesc.Buffer.StructureByteStride = sizeof(PointLightGpu);
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    renderer->GetDevice()->CreateShaderResourceView(pointLightBuffer_.Get(), &srvDesc, pointLightSrvHandle_);
}

void LightManager::EnsureSpotLightBuffer(Renderer* renderer, size_t requiredLights)
{
    if (!renderer || requiredLights == 0)
    {
        return;
    }

    if (spotLightBuffer_ && spotLightBufferCPU_ && spotLightSrvHandle_.ptr != 0 && spotLightCapacity_ >= requiredLights)
    {
        return;
    }

    if (spotLightBuffer_)
    {
        spotLightBuffer_->Unmap(0, nullptr);
        spotLightBuffer_.Reset();
        spotLightBufferCPU_ = nullptr;
        spotLightCapacity_ = 0;
    }
    spotLightSrvHeap_.Reset();
    spotLightSrvHandle_ = {};

    spotLightCapacity_ = std::max<size_t>(requiredLights, 1);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(spotLightCapacity_ * sizeof(SpotLightGpu));
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    ThrowIfFailed(renderer->GetDevice()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(buffer.GetAddressOf())));

    D3D12_RANGE range{ 0, 0 };
    void* mapped = nullptr;
    ThrowIfFailed(buffer->Map(0, &range, &mapped));

    spotLightBuffer_ = buffer;
    spotLightBufferCPU_ = static_cast<SpotLightGpu*>(mapped);
    renderer->SetResourceState(spotLightBuffer_.Get(), D3D12_RESOURCE_STATE_GENERIC_READ);

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = 1;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(renderer->GetDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(spotLightSrvHeap_.ReleaseAndGetAddressOf())));
    spotLightSrvHandle_ = spotLightSrvHeap_->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = static_cast<UINT>(spotLightCapacity_);
    srvDesc.Buffer.StructureByteStride = sizeof(SpotLightGpu);
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    renderer->GetDevice()->CreateShaderResourceView(spotLightBuffer_.Get(), &srvDesc, spotLightSrvHandle_);
}

void LightManager::Reset()
{
    if (pointLightBuffer_)
    {
        pointLightBuffer_->Unmap(0, nullptr);
        pointLightBuffer_.Reset();
    }
    pointLightBufferCPU_ = nullptr;
    pointLightCapacity_ = 0;
    pointLightSrvHeap_.Reset();
    pointLightSrvHandle_ = {};

    if (spotLightBuffer_)
    {
        spotLightBuffer_->Unmap(0, nullptr);
        spotLightBuffer_.Reset();
    }
    spotLightBufferCPU_ = nullptr;
    spotLightCapacity_ = 0;
    spotLightSrvHeap_.Reset();
    spotLightSrvHandle_ = {};

    pointLights_.clear();
    spotLights_.clear();
    cachedSpotLightCount_ = 0;

}

