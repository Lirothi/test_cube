#pragma once

#include <vector>
#include <algorithm>

#include <d3d12.h>
#include <wrl/client.h>

#include "core/Math.h"
#include "rendering/lighting/PointLight.h"
#include "rendering/lighting/SpotLight.h"

class Renderer;

class LightManager {
public:
    static constexpr std::uint32_t kMaxSpotLights = 4;

    struct alignas(16) PointLightGpu
    {
        Math::float3 position;
        float radius;
        Math::float3 color;
        float intensity;
    };

    struct alignas(16) SpotLightGpu
    {
        Math::float4 positionRange;      // xyz = position, w = range
        Math::float4 directionCosOuter;  // xyz = direction (normalized), w = cos(outer)
        Math::float4 colorIntensity;     // xyz = color, w = intensity
        Math::float4 shadowParams;       // x = cos(inner), y = shadow index, z = invAngleRange, w = depth bias
        Math::float4 shadowParams2;      // x = normal bias (WS)
        Math::mat4   viewProj;
    };

    LightManager() = default;
    ~LightManager();

    std::vector<PointLight>& PointLights() { return pointLights_; }
    const std::vector<PointLight>& PointLights() const { return pointLights_; }

    std::vector<SpotLight>& SpotLights() { return spotLights_; }
    const std::vector<SpotLight>& SpotLights() const { return spotLights_; }

    void UpdateSpotLightCache();

    size_t GetSpotLightCount() const { return cachedSpotLightCount_; }
    void EnsurePointLightBuffer(Renderer* renderer, size_t requiredLights);
    void EnsureSpotLightBuffer(Renderer* renderer, size_t requiredLights);

    PointLightGpu* GetPointLightBufferCPU() const { return pointLightBufferCPU_; }
    SpotLightGpu* GetSpotLightBufferCPU() const { return spotLightBufferCPU_; }

    D3D12_CPU_DESCRIPTOR_HANDLE GetPointLightSrv() const { return pointLightSrvHandle_; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetSpotLightSrv() const { return spotLightSrvHandle_; }

    void Reset();

private:
    using ComPtr = Microsoft::WRL::ComPtr<ID3D12Resource>;

    std::vector<PointLight> pointLights_;
    std::vector<SpotLight>  spotLights_;

    size_t cachedSpotLightCount_ = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> pointLightBuffer_;
    PointLightGpu* pointLightBufferCPU_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> pointLightSrvHeap_;
    D3D12_CPU_DESCRIPTOR_HANDLE pointLightSrvHandle_{};
    size_t pointLightCapacity_ = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> spotLightBuffer_;
    SpotLightGpu* spotLightBufferCPU_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> spotLightSrvHeap_;
    D3D12_CPU_DESCRIPTOR_HANDLE spotLightSrvHandle_{};
    size_t spotLightCapacity_ = 0;
};

