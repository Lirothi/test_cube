#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include <d3d12.h>
#include <wrl/client.h>

#include "core/math/Math.h"
#include "rendering/core/ResourceDeclarations.h"

class Material;
class Renderer;
struct RenderGraphPassContext;

// Persistent world-space wet-sand history. The visible ordinary ocean surface writes a per-frame
// coverage stamp; at the beginning of the next frame this class integrates it smoothly into the
// history, relocates both fields with the camera and applies the authored drying rate. SurfSim is
// deliberately not involved.
class OceanWetness
{
public:
    void EnsureResources(Renderer* renderer);
    void TickWindow(Math::float2 cameraXZ);

    std::function<void(RenderGraphPassContext)> BuildUpdatePass(
        RenderGraphPassContext& ctx,
        float timeSeconds,
        float wetTimeSeconds,
        float dryTimeSeconds);

    bool IsReady() const { return created_; }
    ID3D12Resource* GetCurrentResource() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentSrv() const;
    ID3D12Resource* GetCurrentStampResource() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentStampUav() const;

    // xy: centre in world XZ, z: 1 / half extent, w: texel size in metres.
    Math::float4 GetWindowParams() const;

private:
    static constexpr UINT kResolution = 512u;
    static constexpr float kHalfExtent = 103.0f;
    static constexpr float kTexelWorld = (kHalfExtent * 2.0f) / kResolution;
    static constexpr float kSnapTexels = 8.0f;

    GpuResource history_[2];
    GpuResource stamp_[2];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptorHeap_;
    D3D12_CPU_DESCRIPTOR_HANDLE historySrv_[2]{};
    D3D12_CPU_DESCRIPTOR_HANDLE historyUav_[2]{};
    D3D12_CPU_DESCRIPTOR_HANDLE stampSrv_[2]{};
    D3D12_CPU_DESCRIPTOR_HANDLE stampUav_[2]{};
    std::shared_ptr<Material> updateMaterial_;

    Math::float2 center_ = Math::float2(0.0f, 0.0f);
    Math::float2 pendingCenter_ = Math::float2(0.0f, 0.0f);
    int pendingShiftX_ = 0;
    int pendingShiftY_ = 0;
    std::uint32_t current_ = 0;
    float previousTime_ = -1.0f;
    bool created_ = false;
    bool hasCenter_ = false;
    bool needsInitialClear_ = true;
};
