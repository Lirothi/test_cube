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
#include <functional>
#include <memory>

#include <d3d12.h>
#include <wrl/client.h>

#include "core/math/Math.h"
#include "rendering/core/ResourceDeclarations.h"

class Renderer;
class Material;
struct RenderGraphPassContext;

namespace ocean
{
// surf sim S1: one-shot test-hump injection (the "Poke" button in the ocean window), consumed
// by the sim's pass builder on the next frame. "--ocean-surf-poke=<sec>" auto-pokes on that
// cadence so a headless phase series can watch a wave without a GUI.
inline bool g_surfSimPokeRequest = false;
inline float g_surfSimPokeInterval = 0.0f;
}

class OceanSurfSim
{
public:
    // Lazy-creates the textures/descriptors/materials. Call before the render graph is built and
    // only while the feature is enabled, so OFF never allocates.
    void EnsureResources(Renderer* renderer);
    bool IsReady() const { return created_; }

    // CPU-side window follow. Decides this frame's re-anchor BEFORE the render graph is built,
    // so the pass builder below sees the final answer.
    void TickWindow(Math::float2 cameraXZ);

    // S1: everything the wave equation needs to read the shore depth map — the window transform
    // (matching ShoreDepthUV in the surface shaders), the depth decode, and the SRV. Assembled
    // by OceanRenderable from OceanSimulation's getters.
    struct ShoreDepthWindow
    {
        Math::float2 center = Math::float2(0.0f, 0.0f);
        float invExtent = 0.0f; // uv = offset * invExtent + 0.5
        float zNear = 0.0f;
        float zFar = 0.0f;
        float camHeight = 0.0f;
        D3D12_CPU_DESCRIPTOR_HANDLE srv{};
        ID3D12Resource* resource = nullptr; // declared read-only at its canonical state
        // S2: the shore SDF the spawner refines candidates against. NOT declared to the barrier
        // compile: its canonical is the creation-time UAV while it actually rests shader-readable
        // after the one-shot jump flood — the caller instead GATES the sim on the SDF being built,
        // exactly like the modern surface which samples it undeclared.
        Math::float2 sdfCenter = Math::float2(0.0f, 0.0f);
        float sdfInvExtent = 0.0f;
        D3D12_CPU_DESCRIPTOR_HANDLE sdfSrv{};
        // S3: the shared ContactFoam breakup pattern that tears the deposit stripe.
        D3D12_CPU_DESCRIPTOR_HANDLE breakupSrv{};
    };

    // S2: authored spawner tuning + the frame's wind, assembled by OceanRenderable from the
    // ocean config (all knobs live there; the sim itself stays config-free).
    struct Tuning
    {
        float spawnDistance = 40.0f;  // metres seaward of the waterline
        float segmentLength = 30.0f;  // metres along the shore
        float amplitude = 0.35f;      // metres of injected height at full wind
        float interval = 3.0f;        // seconds between spawns at full wind
        float windCoupling = 1.0f;    // 0 = ignore wind, 1 = calm silences the spawner
        float windAmount = 0.0f;      // 0..1, the shared contact-foam wind remap
        // S3: breaking + foam.
        float depositStrength = 1.0f; // peak foam a breaking crest stamps (max() semantics)
        float breakerGamma = 0.78f;   // surf breaker index H/d
        float foamFadeRate = 0.4f;    // foam/s linear decay behind the crest
        float frontBreakup = 0.5f;    // 0..1 tear of the deposit stripe
        float runInland = 2.0f;       // metres past the SDF waterline the wave may live
        float minSpawnDepth = 1.2f;   // metres of bottom required under a disturber to be born
        // Shape of the injected wave (the peak width/height levers the user tunes):
        float waveSigma = 4.0f;       // m across-shore half-width of the hump - the wavelength
        float spawnDuration = 1.0f;   // s the hump inflates (shorter = more compact packet)
        float celerityFloor = 0.4f;   // m minimum depth for wave speed (bore march floor)
        float breakOnset = 0.5f;      // fraction of the breaker criterion where foam ramps in
        float waveDamping = 0.08f;    // 1/s open-water settle
    };

    // Pass-flow S3 pilot (docs/render_graph_pass_flow_plan.md): the whole pass is ONE builder.
    // It runs at Prepare time: makes every frame decision as a local (relocate, substep count,
    // ping-pong indices, poke), COMMITS the cross-frame state immediately, declares the barrier
    // points from those same locals, and returns the record lambda capturing them by value —
    // there is no separate Record to keep in sync, and the record body names no resource
    // (EmitPoint markers). Returns an empty function when the sim should not run this frame.
    std::function<void(RenderGraphPassContext)> BuildPass(
        RenderGraphPassContext& ctx, float timeSeconds, const ShoreDepthWindow& shore,
        const Tuning& tuning);

    // x,y: window centre (world XZ), z: 1 / half extent, w: texel world size — the surface
    // shaders' window transform (debug view now, foam consumption at S4).
    Math::float4 GetWindowParams() const;
    // The height field the surface may sample THIS frame (left in a pixel-readable state by
    // the pass).
    D3D12_CPU_DESCRIPTOR_HANDLE GetWaveSrv() const;
    // S3: the surf foam field, same handoff contract as the wave.
    D3D12_CPU_DESCRIPTOR_HANDLE GetSurfFoamSrv() const;

private:
    bool WillRelocate() const { return pendingShiftX_ != 0 || pendingShiftY_ != 0; }

    static constexpr UINT kResolution = 512u;
    static constexpr float kHalfExtent = 250.0f; // 500 m window — the shore-depth precedent
    static constexpr float kTexelWorld = (kHalfExtent * 2.0f) / static_cast<float>(kResolution);
    // Snap step of the window centre, whole texels so Relocate shifts losslessly. 8 texels
    // (~7.8 m) keeps re-anchors rare at walking speeds.
    static constexpr float kSnapTexels = 8.0f;
    // S1: the wave equation integrates on a FIXED substep (stability is a function of dt, so a
    // frame-rate dt would make it a function of the frame rate), catching up to real time with
    // at most kMaxSubsteps per frame — the Crest LodDataMgrPersistent cadence.
    static constexpr float kFixedDt = 1.0f / 120.0f;
    static constexpr int kMaxSubsteps = 4;
    // S2: disturbance slots, GPU-resident (the Spawn kernel writes them from the SDF; the CPU
    // only round-robins an index). 8 concurrent segments is plenty at seconds-long lifetimes.
    static constexpr UINT kMaxSpawners = 8u;
    static constexpr UINT kSpawnerStride = 32u; // float4 posDir + float4 params

    GpuResource wave_[2];
    GpuResource foam_[2];
    GpuResource spawners_; // kMaxSpawners x kSpawnerStride structured buffer, UAV-resident
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptorHeap_;
    D3D12_CPU_DESCRIPTOR_HANDLE waveUav_[2]{};
    D3D12_CPU_DESCRIPTOR_HANDLE foamUav_[2]{};
    D3D12_CPU_DESCRIPTOR_HANDLE waveSrv_[2]{};
    D3D12_CPU_DESCRIPTOR_HANDLE foamSrv_[2]{};
    D3D12_CPU_DESCRIPTOR_HANDLE spawnersUav_{};
    std::shared_ptr<Material> updateMaterial_;
    std::shared_ptr<Material> relocateMaterial_;
    std::shared_ptr<Material> spawnMaterial_;

    Math::float2 center_ = Math::float2(0.0f, 0.0f);
    // Texel shift decided by TickWindow for this frame's Relocate (0 = no re-anchor).
    int pendingShiftX_ = 0;
    int pendingShiftY_ = 0;
    Math::float2 pendingCenter_ = Math::float2(0.0f, 0.0f);
    uint32_t current_ = 0;
    float previousTime_ = -1.0f;
    float substepAccum_ = 0.0f;  // seconds of sim time not yet integrated
    float lastAutoPoke_ = -1.0e9f; // sim-clock time of the last --ocean-surf-poke injection
    float lastSpawnTime_ = -1.0e9f; // sim-clock time of the last spawner injection
    uint32_t nextSpawnSlot_ = 0;    // CPU round-robin into the GPU slot buffer
    uint32_t spawnSeed_ = 0x12345u; // xorshift state for candidate placement
    bool created_ = false;
    bool hasCenter_ = false;
    // First frame after creation runs a full-clear Relocate (shift >= Resolution): committed
    // heaps are NOT guaranteed zeroed — the foam pair held NaN garbage that survived every
    // read-modify-write frame and rendered as permanent black.
    bool needsInitialClear_ = true;
};
