#pragma once

#include <vector>
#include <algorithm>
#include <array>

#include <d3d12.h>
#include <wrl/client.h>

#include "core/math/Math.h"
#include "rendering/core/RenderConstants.h"
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

    // Per-frame point-light equivalent of SelectShadowedSpots. Among point lights
    // whose influence sphere (position, radius) intersects the camera frustum and
    // which have shadows enabled, choose the highest projected-size up to
    // kMaxShadowedPointLights to cast (cube) shadows, assigning each a cube slot in
    // [0, GetShadowedPointCount()). Pass the SAME non-jittered frustum used for
    // spots. Must run before the point cube views are built / the point-light buffer
    // is filled.
    void SelectShadowedPoints(const Math::float3& cameraPos, const Frustum& cameraFrustum);

    // Number of point lights casting a shadow this frame (<= kMaxShadowedPointLights).
    size_t GetShadowedPointCount() const { return shadowedPointLightIndices_.size(); }
    // Cube-atlas slot for a point-light index, or -1 if it is not shadowed.
    int GetPointShadowSlot(size_t lightIndex) const
    {
        return lightIndex < pointShadowSlot_.size() ? pointShadowSlot_[lightIndex] : -1;
    }
    // Inverse of the slot map: which point-light index owns a given cube slot.
    size_t GetShadowedPointLightIndex(size_t slot) const
    {
        return slot < shadowedPointLightIndices_.size() ? shadowedPointLightIndices_[slot] : 0;
    }

    bool EnsurePointLightBuffer(Renderer* renderer, size_t requiredLights);
    bool EnsureSpotLightBuffer(Renderer* renderer, size_t requiredLights);

    // Barrier plan step 4: read-only "is the buffer already big enough" for the recording
    // passes. They used to call Ensure* themselves, which could FREE the previous allocation
    // while an in-flight frame still referenced it — the DXGI_DEVICE_HUNG the --scene-stress
    // harness was built for. Growth now happens once per frame, before any recording.
    bool HasPointLightBuffer(size_t requiredLights) const {
        return pointLightBuffer_ != nullptr && pointLightCapacity_ >= requiredLights;
    }
    bool HasSpotLightBuffer(size_t requiredLights) const {
        return spotLightBuffer_ != nullptr && spotLightCapacity_ >= requiredLights;
    }

    // The light buffers are ring-buffered per in-flight frame (kFrameCount regions
    // in one resource): the CPU rewrites the buffer every frame while up to
    // kFrameCount frames are still being read by the GPU, so each frame must
    // read/write its OWN region. Pass the renderer's current frame index. Writing
    // or reading a shared region would be a cross-frame WAR hazard (an in-flight
    // frame reading a newer frame's data — e.g. a stale shadow-slot index).
    PointLightGpu* GetPointLightBufferCPU(UINT frame) const {
        return pointLightBufferCPU_ ? pointLightBufferCPU_ + frame * pointLightCapacity_ : nullptr;
    }
    SpotLightGpu* GetSpotLightBufferCPU(UINT frame) const {
        return spotLightBufferCPU_ ? spotLightBufferCPU_ + frame * spotLightCapacity_ : nullptr;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE GetPointLightSrv(UINT frame) const { return pointLightSrvHandles_[frame]; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetSpotLightSrv(UINT frame) const { return spotLightSrvHandles_[frame]; }

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
    // Per-frame point cube-shadow-slot mapping (see SelectShadowedPoints).
    std::vector<int> pointShadowSlot_;                      // parallel to pointLights_; -1 = unshadowed
    std::vector<std::uint32_t> shadowedPointLightIndices_;  // slot -> point-light index
    // Each buffer holds render::kFrameCount contiguous regions of pointLightCapacity_
    // / spotLightCapacity_ elements; region f is written+read only by in-flight frame
    // f, with its own SRV in the *SrvHandles_ array. Capacity is PER REGION.
    Microsoft::WRL::ComPtr<ID3D12Resource> pointLightBuffer_;
    PointLightGpu* pointLightBufferCPU_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> pointLightSrvHeap_;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, render::kFrameCount> pointLightSrvHandles_{};
    size_t pointLightCapacity_ = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> spotLightBuffer_;
    SpotLightGpu* spotLightBufferCPU_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> spotLightSrvHeap_;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, render::kFrameCount> spotLightSrvHandles_{};
    size_t spotLightCapacity_ = 0;
};

