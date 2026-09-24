#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

#include "core/math/Math.h"
#include "rendering/core/RenderConstants.h"

class Renderer;
class OceanRenderable;
struct RenderGraphPassContext;

// The ocean surface on the CPU -- what buoyancy floats on, and what anything else that asks
// "where is the water" will read.
//
// The first 1-2 FFT cascades (the preset's `readbackCascades`, None/One/Two: the long swell, where
// a hull's motion lives -- the 5 m and 0.5 m cascades average out under anything boat-sized) and
// the shore depth map are copied into a readback ring by Main_OceanReadback. Poll adopts the
// NEWEST copy whose frame the GPU has already finished, checked with a fence read: it never waits.
// Copies are taken at most 30 times a second, so the surface is up to ~33 ms plus the GPU's own lag
// old, and the reader carries it forward (see OceanBuoyancy).
//
// SampleHeight is the vertex shader's surface, term for term (ocean_surface_surf_sim.hlsli VSMain):
// the camera-distance cascade weights, the clipmap UV warp, the shore damping from the depth map,
// and -- because the displacement is CHOPPY, a surface point P sits where some other grid point X
// was pushed to -- the fixed-point inverse X = P - D.xz(X), `samplingIterations` steps. Not
// reproduced: the depth-buffer damping the shader falls back to outside the 500 m shore window
// (full amplitude out there) and the surf sim's height (off by default).
class OceanReadback
{
public:
    // Everything the displacement depends on besides the texels, as it stood the frame the copy
    // was taken (the OceanCB fields of the same names).
    struct SurfaceParams
    {
        Math::float4 lengthScales{};   // cascadeLengthScales
        Math::float3 viewer{};         // clipMapViewer: the cascade weights' distance origin
        float fadeScale = 20.0f;       // clipMapParams.w
        float warpStrength = 0.0f;     // windParams0.w
        float waterLevel = 0.0f;
        float time = 0.0f;             // the ocean clock the copied cascades were simulated at
        std::uint32_t cascades = 0;    // copied (readbackCascades)
        std::uint32_t simCascades = 0; // simulated (the weights' loop bound)
        std::uint32_t resolution = 0;
        std::uint32_t iterations = 3;  // samplingIterations
    };
    struct ShoreParams
    {
        Math::float4 view{};           // shoreViewParams: centre xz, camera height, 1/extent
        Math::float2 depthRange{};     // shoreDepthParams.xy: ortho near / far
        Math::float4 damp{};           // shoreLegacyDampParams: vertical, XZ, fade depth
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    // Someone wants water heights (buoyancy asks every Tick while anything floats). No request ->
    // nothing is declared and nothing is copied: an ocean nothing floats on costs nothing here.
    void Request() { requested_ = true; }

    // Main_OceanReadback's builder (graphics queue -- see the pass for the measurement that put it
    // there): the cascades on a requested frame at most 30 times a second, the shore map once per
    // re-rendering.
    std::function<void(RenderGraphPassContext)> BuildPass(RenderGraphPassContext& ctx, const OceanRenderable& ocean);

    // Adopt the newest finished copy. Never waits. Call after Renderer::BeginFrame, before this
    // frame's passes are built (Scene::Tick).
    void Poll(Renderer* renderer);

    // World y of the water surface at world (x, z). Before the first copy lands: the flat level.
    // Reads the adopted slot IN PLACE (no copy): BuildPass never lands a copy in the adopted slot,
    // so it stays intact until Poll adopts a newer one.
    float SampleHeight(float x, float z) const;
    bool HasSurface() const { return adoptedFrame_ != 0; }
    // The frame the adopted copy was taken on; changes exactly when a new copy is adopted.
    std::uint64_t AdoptedFrame() const { return adoptedFrame_; }
    const SurfaceParams& Surface() const { return surface_; }
    // Frames between the adopted copy's frame and the frame that adopted it.
    std::uint32_t LatencyFrames() const { return latency_; }

private:
    Math::float3 Displacement(float baseX, float baseZ) const;
    Math::float3 SampleCascade(std::uint32_t cascade, float u, float v) const;
    float SampleShoreDepth(float u, float v) const;
    bool EnsureBuffer(Renderer* renderer, ID3D12Resource* displacement, ID3D12Resource* shore,
        std::uint32_t cascades);

    struct Slot
    {
        std::uint64_t frame = 0; // 0 = nothing pending
        UINT frameSlot = 0;      // the renderer frame slot that recorded the copy (its fence)
        SurfaceParams surface;
        bool hasShore = false;
        ShoreParams shore;
    };

    Microsoft::WRL::ComPtr<ID3D12Resource> buffer_; // render::kFrameCount slots
    const std::uint8_t* mapped_ = nullptr;          // persistently mapped (a READBACK heap may be)
    UINT64 slotBytes_ = 0;
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> cascadeFootprint_{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT shoreFootprint_{};
    std::uint32_t layoutResolution_ = 0;
    std::uint32_t layoutCascades_ = 0;
    std::uint32_t layoutShoreW_ = 0;
    std::uint32_t layoutShoreH_ = 0;
    std::array<Slot, render::kFrameCount> slots_{};
    // A buffer replaced on a settings change may still be a copy destination of frames in flight.
    std::vector<std::pair<std::uint64_t, Microsoft::WRL::ComPtr<ID3D12Resource>>> retired_;
    bool requested_ = false;
    bool shoreQueued_ = false; // the CURRENT shore map is in a slot (re-armed when it re-renders)
    std::chrono::steady_clock::time_point lastCopy_{};

    // The adopted surface: its slot in the ring (the texels are read from there in place).
    SurfaceParams surface_;
    UINT adoptedSlot_ = 0;
    ShoreParams shore_;
    std::vector<std::uint16_t> shoreTexels_;                 // D16 unorm, width * height
    bool hasShore_ = false;
    std::uint64_t adoptedFrame_ = 0;
    std::uint32_t latency_ = 0;
};
