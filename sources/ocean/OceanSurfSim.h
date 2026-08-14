#pragma once

// Nearshore surf simulation (docs/ocean_surf_sim_plan.md).
//
// S0: infrastructure only. A sliding 500 m world-space window around the camera holds two
// ping-pong pairs — WaveSim (RG16F: height + vertical velocity) and SurfFoam (R16F) — updated by
// compute from the ocean's compute pass. The update kernel is a placeholder (world-anchored test
// pattern); the wave equation lands in S1, the SDF spawner in S2, breaking foam in S3.
//
// Detachability (the plan's hard contract): the OFF state records ZERO dispatches and allocates
// nothing — resources are created lazily on the first enabled frame, from
// OceanRenderable::EnsureSimulationResources (before the render graph, the engine's one sanctioned
// lazy-creation point). All touches outside OceanSurfSim.* / ocean_surf_sim_cs.hlsl are tagged
// "surf sim injection".

#include <cstdint>
#include <memory>

#include <d3d12.h>
#include <wrl/client.h>

#include "core/math/Math.h"
#include "rendering/core/ResourceDeclarations.h"

class Renderer;
class Material;
struct RenderGraphPassContext;

class OceanSurfSim
{
public:
    // Lazy-creates the textures/descriptors/materials. Call before the render graph is built and
    // only while the feature is enabled, so OFF never allocates.
    void EnsureResources(Renderer* renderer);
    bool IsReady() const { return created_; }

    // CPU-side window follow. Decides this frame's re-anchor BEFORE PrepareCompute/RecordCompute
    // run, so the barrier declarations and the recording cannot disagree about the ping-pong
    // order (both derive it from pendingShift_ via CurrentAfterFrame).
    void TickWindow(Math::float2 cameraXZ);

    // Barrier declarations for exactly what RecordCompute will transition this frame. Call only
    // on frames RecordCompute will actually run — registering resources a body never transitions
    // advances the barrier compile past barriers nobody emits.
    void PrepareCompute(RenderGraphPassContext& ctx);
    void RecordCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl, float timeSeconds);

    // x,y: window centre (world XZ), z: 1 / half extent, w: texel world size — the surface
    // shaders' window transform (debug view now, foam consumption at S4).
    Math::float4 GetWindowParams() const;
    // The height field the surface may sample THIS frame (left in a pixel-readable state by
    // RecordCompute).
    D3D12_CPU_DESCRIPTOR_HANDLE GetWaveSrv() const;

private:
    bool WillRelocate() const { return pendingShiftX_ != 0 || pendingShiftY_ != 0; }
    // The ping-pong index that will hold the CURRENT (freshest) data once RecordCompute has run:
    // a re-anchor frame swaps twice (relocate + update), a normal frame once.
    uint32_t CurrentAfterFrame() const { return WillRelocate() ? current_ : (current_ ^ 1u); }

    static constexpr UINT kResolution = 512u;
    static constexpr float kHalfExtent = 250.0f; // 500 m window — the shore-depth precedent
    static constexpr float kTexelWorld = (kHalfExtent * 2.0f) / static_cast<float>(kResolution);
    // Snap step of the window centre, whole texels so Relocate shifts losslessly. 8 texels
    // (~7.8 m) keeps re-anchors rare at walking speeds.
    static constexpr float kSnapTexels = 8.0f;

    GpuResource wave_[2];
    GpuResource foam_[2];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptorHeap_;
    D3D12_CPU_DESCRIPTOR_HANDLE waveUav_[2]{};
    D3D12_CPU_DESCRIPTOR_HANDLE foamUav_[2]{};
    D3D12_CPU_DESCRIPTOR_HANDLE waveSrv_[2]{};
    D3D12_CPU_DESCRIPTOR_HANDLE foamSrv_[2]{};
    std::shared_ptr<Material> updateMaterial_;
    std::shared_ptr<Material> relocateMaterial_;

    Math::float2 center_ = Math::float2(0.0f, 0.0f);
    // Texel shift decided by TickWindow for this frame's Relocate (0 = no re-anchor).
    int pendingShiftX_ = 0;
    int pendingShiftY_ = 0;
    Math::float2 pendingCenter_ = Math::float2(0.0f, 0.0f);
    uint32_t current_ = 0;
    float previousTime_ = -1.0f;
    bool created_ = false;
    bool hasCenter_ = false;
};
