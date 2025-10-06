#include "rendering/lighting/LightManager.h"

#include <cstdint>
#include <cmath>

#include "core/Helpers.h"
#include "rendering/core/Renderer.h"

using Math::float3;
using Math::float4;
using Math::mat4;

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
        const auto& desc = spotLights_[i].GetDesc();

        float3 dir = desc.direction;
        if (dir.Length() <= Math::EPS)
        {
            dir = float3(0.0f, -1.0f, 0.0f);
        }
        dir = dir.Normalized();
        cachedSpotDir_[i] = dir;

        float inner = std::max(0.0f, desc.innerAngle);
        float outer = std::max(inner + Math::EPS, desc.outerAngle);
        inner = std::min(inner, DirectX::XM_PIDIV2);
        outer = std::min(outer, DirectX::XM_PIDIV2);

        const float cosInner = std::cos(inner);
        const float cosOuter = std::cos(outer);
        const float denom = std::max(1e-4f, cosInner - cosOuter);

        cachedSpotCosInner_[i] = cosInner;
        cachedSpotCosOuter_[i] = cosOuter;
        cachedSpotInvAngleRange_[i] = 1.0f / denom;
        cachedSpotNormalBias_[i] = desc.shadowNormalBias;
        cachedSpotDepthBias_[i] = desc.shadowDepthBias;

        float3 up = std::abs(dir.y) > 0.99f ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
        cachedSpotView_[i] = mat4::LookAtLH(desc.position, desc.position + dir, up);

        const float fov = outer * 2.0f;
        const float aspect = 1.0f;
        const float nearPlane = std::max(desc.nearPlane, 0.01f);
        const float farPlane = std::max(desc.range, nearPlane + 0.1f);
        cachedSpotProj_[i] = mat4::PerspectiveFovLH(fov, aspect, nearPlane, farPlane);
    }

    for (size_t i = cachedSpotLightCount_; i < maxLights; ++i)
    {
        cachedSpotDir_[i] = float3(0.0f, -1.0f, 0.0f);
        cachedSpotCosInner_[i] = 0.0f;
        cachedSpotCosOuter_[i] = 0.0f;
        cachedSpotInvAngleRange_[i] = 0.0f;
        cachedSpotNormalBias_[i] = 0.0f;
        cachedSpotDepthBias_[i] = 0.0f;
        cachedSpotView_[i] = mat4::Identity();
        cachedSpotProj_[i] = mat4::Identity();
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

    for (size_t i = 0; i < kMaxSpotLights; ++i)
    {
        cachedSpotDir_[i] = float3(0.0f, -1.0f, 0.0f);
        cachedSpotCosInner_[i] = 0.0f;
        cachedSpotCosOuter_[i] = 0.0f;
        cachedSpotInvAngleRange_[i] = 0.0f;
        cachedSpotNormalBias_[i] = 0.0f;
        cachedSpotDepthBias_[i] = 0.0f;
        cachedSpotView_[i] = mat4::Identity();
        cachedSpotProj_[i] = mat4::Identity();
    }
}

