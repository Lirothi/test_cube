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
// (full amplitude out there).
//
// A boat's wake (OceanWake) rides along: its FREE height -- the field plus the hulls' own head, so a hull at rest does
// not float in its own hollow -- copied with the cascades, sampled where the vertex shader adds the wake's height.
// So does the surf sim's wave (OceanSurfSim, the shore's breakers; 2026-11-09, owner: "the surf sim's wave does not move
// the buoyancy"): its height field, copied on the frames its pass handed it over, added as the vertex shader adds it.
class OceanReadback
{
public:
    // The world's clock runs this many times faster (frame.timeScale): the cap on the copies -- 30 a second of the
    // world -- is that many times shorter by the wall clock.
    static void SetTimeScale(float scale);

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
    // frame's passes are built (Scene::Tick). With readbackCascades None it DROPS the adopted
    // surface and every copy still in flight: None means the flat water level from the frame it is
    // chosen, not the last wave that was copied before it.
    void Poll(Renderer* renderer, const OceanRenderable& ocean);

    // World y of the water surface at world (x, z). Before the first copy lands: the flat level.
    // Reads the adopted slot IN PLACE (no copy): BuildPass never lands a copy in the adopted slot,
    // so it stays intact until Poll adopts a newer one. `wakeShare` scales the boats' wake in it (a
    // boat under way leaves out the wake it makes itself: PhysicsWorld).
    float SampleHeight(float x, float z, float wakeShare = 1.0f) const;
    // The adopted copy carries a wake.
    bool HasWake() const { return HasSurface() && wake_.z > 0.0f; }
    // False before the first copy lands and while readbackCascades is None.
    bool HasSurface() const { return adoptedFrame_ != 0; }
    // The frame the adopted copy was taken on; changes exactly when a new copy is adopted.
    std::uint64_t AdoptedFrame() const { return adoptedFrame_; }
    const SurfaceParams& Surface() const { return surface_; }
    // Frames between the adopted copy's frame and the frame that adopted it.
    std::uint32_t LatencyFrames() const { return latency_; }
    // Diagnostics (get_readouts "floaters"): what the CPU's shore copy says at world (x, z) -- the water depth and the
    // vertical damping the surface takes there. False with no copy, outside its window, or where it holds nothing.
    bool ShoreAt(float x, float z, float& waterDepth, float& vertical) const;
    bool HasShore() const { return hasShore_; }
    const ShoreParams& Shore() const { return shore_; }

private:
    // The shore map's damping at the (warped) surface point, as the VS takes it; false where the VS takes none from the map.
    bool ShoreDamping(float uvX, float uvZ, float& waterDepth, float& vertical, float& horizontal) const;
    // `heights`: add the terms that move only y (the wake, the surf) -- the fixed-point iteration leaves them out.
    Math::float3 Displacement(float baseX, float baseZ, float wakeShare, bool heights) const;
    Math::float3 SampleCascade(std::uint32_t cascade, float u, float v) const;
    float SampleShoreDepth(float u, float v) const;
    float SampleWake(float u, float v) const;
    float SampleSurf(float u, float v) const;
    bool EnsureBuffer(Renderer* renderer, ID3D12Resource* displacement, ID3D12Resource* shore,
        std::uint32_t cascades);
    // Forget the adopted surface; copies in flight are marked to be freed, never adopted. Their
    // slots stay busy until their fence says the GPU is done writing them.
    void DropSurface();

    struct Slot
    {
        std::uint64_t frame = 0; // the frame number that recorded the copy (its fence: IsFrameNumberComplete); 0 = nothing pending
        SurfaceParams surface;
        bool hasShore = false;
        bool discard = false;    // recorded before DropSurface: free it when it lands, do not adopt
        ShoreParams shore;
        Math::float4 wake{};     // the wake's window as the copy saw it (GetWakeParams): z = 0 -> no wake copied
        Math::float4 surf{};     // the surf sim's window xyz (GetSurfSimParams) and its height scale w: z = 0 -> none copied
    };

    Microsoft::WRL::ComPtr<ID3D12Resource> buffer_; // render::kFrameCount slots
    const std::uint8_t* mapped_ = nullptr;          // persistently mapped (a READBACK heap may be)
    UINT64 slotBytes_ = 0;
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> cascadeFootprint_{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT shoreFootprint_{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT wakeFootprint_{}; // R16F, OceanWake::kResolution square: room in every slot
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT surfFootprint_{}; // RG16F, OceanSurfSim::kResolution square: room in every slot
    // BuildPass' copy as its body records it, read here rather than captured (~150 B of capture was a heap
    // allocation per copy). Written by the builder, read by the body in the same frame's graph run.
    struct CopyPass
    {
        ID3D12Resource* dst = nullptr;
        UINT64 base = 0;
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> cascadeFp{};
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT shoreFp{};
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT wakeFp{};
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT surfFp{};
        std::array<UINT, 2> subresource{};
        std::uint32_t cascades = 0;
        bool copyShore = false;
        ID3D12Resource* displacement = nullptr;
        ID3D12Resource* shore = nullptr;
        ID3D12Resource* wake = nullptr; // null: no wake this copy
        ID3D12Resource* surf = nullptr; // null: no surf this copy
        std::uint32_t copyPoint = 0, restorePoint = 0;
    };
    CopyPass copyPass_;
    std::uint32_t layoutResolution_ = 0;
    std::uint32_t layoutCascades_ = 0;
    std::uint32_t layoutShoreW_ = 0;
    std::uint32_t layoutShoreH_ = 0;
    std::array<Slot, render::kFrameCount> slots_{};
    // A buffer replaced on a settings change may still be a copy destination of frames in flight.
    std::vector<std::pair<std::uint64_t, Microsoft::WRL::ComPtr<ID3D12Resource>>> retired_;
    bool requested_ = false;
    bool shoreQueued_ = false; // the CURRENT shore map is in a slot (re-armed when it re-renders)
    std::uint32_t surfWaits_ = 0; // copies put off for want of the surf sim's wave this frame (BuildPass), at most 4 in a row
    std::chrono::steady_clock::time_point lastCopy_{};

    // The adopted surface: its slot in the ring (the texels are read from there in place).
    SurfaceParams surface_;
    UINT adoptedSlot_ = 0;
    ShoreParams shore_;
    Math::float4 wake_{};                                    // the adopted slot's wake window (z = 0: none)
    Math::float4 surf_{};                                    // the adopted slot's surf window and scale (z = 0: none)
    std::vector<std::uint16_t> shoreTexels_;                 // D16 unorm, width * height
    bool hasShore_ = false;
    std::uint64_t adoptedFrame_ = 0;
    std::uint32_t latency_ = 0;
};
