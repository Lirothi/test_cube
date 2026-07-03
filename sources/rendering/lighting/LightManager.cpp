#include "rendering/lighting/LightManager.h"

#include <cstdint>
#include <cmath>

#include "core/math/Frustum.h"
#include "rendering/core/Renderer.h"

LightManager::~LightManager()
{
    Reset();
}

void LightManager::UpdateSpotLightCache()
{
    // Step A3: the total spot-light count is uncapped — every spot lights the
    // scene. Shadow casting is separately capped per frame by SelectShadowedSpots
    // (kMaxShadowedSpotLights, highest projected importance); spots outside that set
    // render shadowless via the shadowParams.y = -1 sentinel.
    cachedSpotLightCount_ = spotLights_.size();

    for (size_t i = 0; i < cachedSpotLightCount_; ++i)
    {
        spotLights_[i].UpdateCachedData();
    }
}

void LightManager::SelectShadowedSpots(const Math::float3& cameraPos, const Frustum& cameraFrustum)
{
    // Reset every spot to "unshadowed"; sized to the full spot list so
    // GetSpotShadowSlot is safe for any lit index.
    spotShadowSlot_.assign(spotLights_.size(), -1);
    shadowedSpotLightIndices_.clear();

    const size_t candidateCount = cachedSpotLightCount_;
    if (candidateCount == 0)
    {
        return;
    }

    // Candidates = lit spots whose influence intersects the camera frustum. A spot
    // whose reach never touches the view cannot cast a visible shadow, so it never
    // consumes a scarce shadow slot even if it is physically close to the camera
    // (e.g. behind it). Among the survivors, pick the most important
    // min(count, kMaxShadowedSpotLights) by projected cone-bound size and brightness.
    //
    // Influence bound: the cached world-space AABB of the finite outer cone. It
    // encloses the real cone, so this remains conservative, while rejecting many
    // false positives from the previous apex-centered sphere.
    struct Candidate { std::uint32_t index; float score; };
    std::vector<Candidate> candidates;
    candidates.reserve(candidateCount);
    constexpr float kMinPriorityDistance = 0.25f;
    for (size_t i = 0; i < candidateCount; ++i)
    {
        const SpotLight& spot = spotLights_[i];
        const SpotLightDesc& desc = spot.GetDesc();
        const AABB& influenceBounds = spot.GetConeBounds();
        if (!cameraFrustum.Intersects(influenceBounds))
        {
            continue;
        }

        const Math::float3 boundsCenter = influenceBounds.GetCenter();
        const float boundsRadius = influenceBounds.GetRadius();
        const Math::float3 toBounds = boundsCenter - cameraPos;
        const float centerDistance = std::sqrt(toBounds.Dot(toBounds));
        const float priorityDistance = std::max(centerDistance - boundsRadius, kMinPriorityDistance);
        const float projectedSize = boundsRadius / priorityDistance;

        const float luminance = std::max(0.0f,
            desc.color.x * 0.2126f + desc.color.y * 0.7152f + desc.color.z * 0.0722f);
        const float brightness = std::max(desc.intensity, 0.0f) * luminance;
        const float score = projectedSize * projectedSize;// *brightness;
        candidates.push_back({ static_cast<std::uint32_t>(i), score });
    }

    const size_t shadowCount = std::min<size_t>(candidates.size(), kMaxShadowedSpotLights);
    std::partial_sort(candidates.begin(), candidates.begin() + shadowCount, candidates.end(),
        [](const Candidate& a, const Candidate& b)
        {
            if (a.score == b.score)
            {
                return a.index < b.index;
            }
            return a.score > b.score;
        });

    shadowedSpotLightIndices_.reserve(shadowCount);
    for (size_t s = 0; s < shadowCount; ++s)
    {
        const std::uint32_t lightIndex = candidates[s].index;
        shadowedSpotLightIndices_.push_back(lightIndex);
        spotShadowSlot_[lightIndex] = static_cast<int>(s);
    }
}

void LightManager::ReleasePointLightBuffer(Renderer* renderer)
{
    if (pointLightBuffer_)
    {
        if (renderer)
        {
            renderer->ClearResourceState(pointLightBuffer_.Get());
        }
        pointLightBuffer_->Unmap(0, nullptr);
        pointLightBuffer_.Reset();
    }
    pointLightBufferCPU_ = nullptr;
    pointLightCapacity_ = 0;
    pointLightSrvHeap_.Reset();
    pointLightSrvHandle_ = {};
}

void LightManager::ReleaseSpotLightBuffer(Renderer* renderer)
{
    if (spotLightBuffer_)
    {
        if (renderer)
        {
            renderer->ClearResourceState(spotLightBuffer_.Get());
        }
        spotLightBuffer_->Unmap(0, nullptr);
        spotLightBuffer_.Reset();
    }
    spotLightBufferCPU_ = nullptr;
    spotLightCapacity_ = 0;
    spotLightSrvHeap_.Reset();
    spotLightSrvHandle_ = {};
}

bool LightManager::EnsurePointLightBuffer(Renderer* renderer, size_t requiredLights)
{
    if (!renderer || !renderer->GetDevice() || requiredLights == 0)
    {
        return false;
    }

    if (pointLightBuffer_ && pointLightBufferCPU_ && pointLightSrvHandle_.ptr != 0 && pointLightCapacity_ >= requiredLights)
    {
        return true;
    }

    ReleasePointLightBuffer(renderer);
    const size_t newCapacity = std::max<size_t>(requiredLights, 1);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(newCapacity * sizeof(PointLightGpu));
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    HRESULT hr = renderer->GetDevice()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(buffer.GetAddressOf()));
    if (FAILED(hr) || !buffer)
    {
        return false;
    }

    D3D12_RANGE range{ 0, 0 };
    void* mapped = nullptr;
    hr = buffer->Map(0, &range, &mapped);
    if (FAILED(hr) || !mapped)
    {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = 1;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
    hr = renderer->GetDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(srvHeap.GetAddressOf()));
    if (FAILED(hr) || !srvHeap)
    {
        buffer->Unmap(0, nullptr);
        return false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    if (srvHandle.ptr == 0)
    {
        buffer->Unmap(0, nullptr);
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = static_cast<UINT>(newCapacity);
    srvDesc.Buffer.StructureByteStride = sizeof(PointLightGpu);
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    renderer->GetDevice()->CreateShaderResourceView(buffer.Get(), &srvDesc, srvHandle);

    renderer->SetResourceState(buffer.Get(), D3D12_RESOURCE_STATE_GENERIC_READ);
    if (buffer) { buffer->SetName(L"LightManager.PointLightBuffer"); }
    pointLightCapacity_ = newCapacity;
    pointLightBuffer_ = buffer;
    pointLightBufferCPU_ = static_cast<PointLightGpu*>(mapped);
    pointLightSrvHeap_ = srvHeap;
    pointLightSrvHandle_ = srvHandle;
    return true;
}

bool LightManager::EnsureSpotLightBuffer(Renderer* renderer, size_t requiredLights)
{
    if (!renderer || !renderer->GetDevice() || requiredLights == 0)
    {
        return false;
    }

    if (spotLightBuffer_ && spotLightBufferCPU_ && spotLightSrvHandle_.ptr != 0 && spotLightCapacity_ >= requiredLights)
    {
        return true;
    }

    ReleaseSpotLightBuffer(renderer);
    const size_t newCapacity = std::max<size_t>(requiredLights, 1);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<UINT64>(newCapacity * sizeof(SpotLightGpu));
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    HRESULT hr = renderer->GetDevice()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(buffer.GetAddressOf()));
    if (FAILED(hr) || !buffer)
    {
        return false;
    }

    D3D12_RANGE range{ 0, 0 };
    void* mapped = nullptr;
    hr = buffer->Map(0, &range, &mapped);
    if (FAILED(hr) || !mapped)
    {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = 1;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
    hr = renderer->GetDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(srvHeap.GetAddressOf()));
    if (FAILED(hr) || !srvHeap)
    {
        buffer->Unmap(0, nullptr);
        return false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    if (srvHandle.ptr == 0)
    {
        buffer->Unmap(0, nullptr);
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = static_cast<UINT>(newCapacity);
    srvDesc.Buffer.StructureByteStride = sizeof(SpotLightGpu);
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    renderer->GetDevice()->CreateShaderResourceView(buffer.Get(), &srvDesc, srvHandle);

    renderer->SetResourceState(buffer.Get(), D3D12_RESOURCE_STATE_GENERIC_READ);
    if (buffer) { buffer->SetName(L"LightManager.SpotLightBuffer"); }
    spotLightCapacity_ = newCapacity;
    spotLightBuffer_ = buffer;
    spotLightBufferCPU_ = static_cast<SpotLightGpu*>(mapped);
    spotLightSrvHeap_ = srvHeap;
    spotLightSrvHandle_ = srvHandle;
    return true;
}

void LightManager::Reset()
{
    // Clear the CPU-side light lists, but DELIBERATELY retain the GPU light
    // buffers + their SRV descriptors. Reset() runs on every level load/switch
    // (JsonLevel::Load) and unload (Scene::Clear). Releasing the spot/point
    // light buffers here dangles their SRV descriptor: the spot-lights compute
    // dispatch (Pass_SpotLights) reads the spot-light structured buffer via a
    // staged SRV (table index 6), and freeing + recreating that buffer across a
    // level change left the descriptor pointing at a destroyed resource
    // (GPU-based validation: "Invalid resource pointed to by descriptor ...
    // resource has been destroyed", spotlight_cs.hlsl(120)), which manifested as
    // an intermittent DXGI_ERROR_DEVICE_HUNG on the spot-lights dispatch during
    // scene-lifecycle churn. The buffers are fully rewritten from the CPU every
    // frame (EnsureSpotLightBuffer keeps them when capacity suffices; the pass
    // repopulates the contents), so retaining them across a level change is
    // correct and avoids the use-after-free. The buffers are still released
    // normally by the destructor (member ComPtr teardown).
    pointLights_.clear();
    spotLights_.clear();
    cachedSpotLightCount_ = 0;
    spotShadowSlot_.clear();
    shadowedSpotLightIndices_.clear();
}

