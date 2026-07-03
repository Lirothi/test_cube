#pragma once

#include <vector>
#include <algorithm>

#include <d3d12.h>
#include <wrl/client.h>

#include "core/math/Math.h"
#include "rendering/lighting/PointLight.h"
#include "rendering/lighting/SpotLight.h"

class Renderer;
class Frustum;

class LightManager {
public:
    // The total spot-light count is unbounded (the structured buffer grows on
    // demand). Only shadow CASTERS are capped per frame - the highest-priority
    // kMaxShadowedSpotLights for the camera - which sizes the shadow atlas /
    // shadow-view array / DSV reservation. Distant spots still light the scene,
    // just without a shadow.
    static constexpr std::uint32_t kMaxShadowedSpotLights = 8;
    // Max point lights that can cast (omnidirectional cube) shadows in one frame.
    // Each shadowed point uses 6 cube faces, so this sizes the point shadow atlas
    // at 6 * kMaxShadowedPointLights slices. Chosen per frame by projected size,
    // like the spot casters. Distant/opted-out points still light, just no shadow.
    static constexpr std::uint32_t kMaxShadowedPointLights = 4;

    struct alignas(16) PointLightGpu
    {
        Math::float3 position;
        float radius;
        Math::float3 color;
        float intensity;
        Math::float4 shadowParams; // x = shadow slot (-1 = none), y = bias, z = nearPlane, w = farPlane (radius)
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

    // Per-frame: among lit spots whose influence intersects the camera frustum,
    // choose the highest-priority projected/bright spots up to
    // kMaxShadowedSpotLights to cast shadows, and assign each a shadow slot in
    // [0, GetShadowedSpotCount()) in descending-priority order. Frustum-culling
    // first means a nearby spot that can't affect any visible pixel (e.g. behind
    // the camera, out of reach) never consumes a scarce shadow slot. Must run
    // after UpdateSpotLightCache and before the spot shadow views are built /
    // the spot-light buffer is filled.
    void SelectShadowedSpots(const Math::float3& cameraPos, const Frustum& cameraFrustum);

    size_t GetSpotLightCount() const { return cachedSpotLightCount_; }

    // Number of spots casting a shadow this frame (<= kMaxShadowedSpotLights).
    size_t GetShadowedSpotCount() const { return shadowedSpotLightIndices_.size(); }
    // Shadow-atlas slot for a spot-light index, or -1 if it is not shadowed.
    int GetSpotShadowSlot(size_t lightIndex) const
    {
        return lightIndex < spotShadowSlot_.size() ? spotShadowSlot_[lightIndex] : -1;
    }
    // Inverse of the slot map: which spot-light index owns a given shadow slot.
    size_t GetShadowedSpotLightIndex(size_t slot) const
    {
        return slot < shadowedSpotLightIndices_.size() ? shadowedSpotLightIndices_[slot] : 0;
    }
    bool EnsurePointLightBuffer(Renderer* renderer, size_t requiredLights);
    bool EnsureSpotLightBuffer(Renderer* renderer, size_t requiredLights);

    PointLightGpu* GetPointLightBufferCPU() const { return pointLightBufferCPU_; }
    SpotLightGpu* GetSpotLightBufferCPU() const { return spotLightBufferCPU_; }

    D3D12_CPU_DESCRIPTOR_HANDLE GetPointLightSrv() const { return pointLightSrvHandle_; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetSpotLightSrv() const { return spotLightSrvHandle_; }

    void Reset();

private:
    using ComPtr = Microsoft::WRL::ComPtr<ID3D12Resource>;

    void ReleasePointLightBuffer(Renderer* renderer = nullptr);
    void ReleaseSpotLightBuffer(Renderer* renderer = nullptr);

    std::vector<PointLight> pointLights_;
    std::vector<SpotLight>  spotLights_;

    size_t cachedSpotLightCount_ = 0;
    // Per-frame shadow-slot mapping (see SelectShadowedSpots).
    std::vector<int> spotShadowSlot_;                       // parallel to spotLights_; -1 = unshadowed
    std::vector<std::uint32_t> shadowedSpotLightIndices_;   // slot -> spot-light index
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

