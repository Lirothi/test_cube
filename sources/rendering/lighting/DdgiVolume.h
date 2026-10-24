#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <wrl/client.h>
#include <d3d12.h>

#include "core/math/Math.h"
#include "rendering/core/ResourceDeclarations.h"

class Material;
class Renderer;

// Interior lighting plan D: the GI probe volume (DDGI, Majercik et al. 2019 / NVIDIA RTXGI) -- its
// resources, its PSOs and the CPU half of each frame (where the volume stands, which probes update,
// the rays' rotation). The passes themselves are SceneRenderer's (SceneRenderer_Gi.cpp): they need
// the TLAS, the bindless table and the frame's lights. Shaders: ddgi_common.hlsli (the layout and the
// sampling), ddgi_trace_cs.hlsl, ddgi_update_cs.hlsl.
//
// A camera-centred grid of kDims probes `spacing` apart that SCROLLS with the camera (a slot keeps its
// texels until the cell it stands for changes); each frame 1 / divisor of them trace kRaysPerProbe rays
// and blend them in. What they hold is the BOUNCE only -- lighting_cs adds it to the sky term plan S
// already occludes, so nothing is counted twice and the volume's edge fades an addition.
class DdgiVolume
{
public:
    static constexpr int kDimX = 32, kDimY = 16, kDimZ = 32; // mirrors ddgi_common.hlsli kDdgiDims
    static constexpr uint32_t kProbeCount = uint32_t(kDimX) * kDimY * kDimZ;
    static constexpr uint32_t kRaysPerProbe = 64;          // ddgi_common.hlsli kDdgiRays
    static constexpr uint32_t kIrradianceTile = 6 + 2;     // octahedral interior + border
    static constexpr uint32_t kDepthTile = 14 + 2;
    static constexpr uint32_t kAtlasTilesX = uint32_t(kDimX) * kDimY;
    static constexpr uint32_t kAtlasTilesY = uint32_t(kDimZ);
    // Every resource rests here: the trace reads the atlases and the state for its infinite bounce,
    // lighting reads them, and the passes that write flip them to UAV and back.
    static constexpr D3D12_RESOURCE_STATES kRestState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // One frame's placement and schedule, computed once in the graph's serial build and handed to
    // both passes (they must agree on every field).
    struct Frame
    {
        int baseCell[3] = { 0, 0, 0 }; // the volume's first world cell
        uint32_t generation = 1;       // bumped when the volume starts over
        float spacing = 0.5f;
        uint32_t divisor = 4;          // 1 / divisor of the probes update per frame...
        uint32_t phase = 0;            // ...those whose slot index is phase mod divisor
        uint32_t probes = 0;           // = kProbeCount / divisor (rounded up)
        uint32_t seed = 0;             // per-frame random seed (the sky rays)
        Math::mat4 rayRotation;        // this update's random rotation of the ray set
    };

    DdgiVolume();
    ~DdgiVolume();

    // Creates the atlases, the ray buffer, the probe state and the two PSOs once; idempotent.
    void Prepare(Renderer* renderer);
    bool Ready() const { return ready_ && !failed_; }
    void Reset(); // the caller has drained the GPU (level/device teardown)

    // Places the volume around `cameraPos` and picks this frame's probes. A new spacing starts a new
    // generation: every probe is then untraced until its turn comes.
    Frame BeginFrame(const Math::float3& cameraPos, float spacing, uint32_t divisor);
    // The generation lighting reads with (0 = nothing traced yet, the volume is not applied).
    uint32_t AppliedGeneration() const { return tracedGeneration_.load(std::memory_order_acquire); }
    // From the update's recording body (a worker thread); read by the next frame's serial decisions.
    void MarkTraced(uint32_t generation) { tracedGeneration_.store(generation, std::memory_order_release); }
    float Spacing() const { return spacing_; }
    const int* BaseCell() const { return lastBase_; }

    Material* TraceMaterial() const { return traceMaterial_.get(); }
    Material* UpdateMaterial() const { return updateMaterial_.get(); }

    ID3D12Resource* Rays() const { return rays_.Get(); }
    ID3D12Resource* Irradiance() const { return irradiance_.Get(); }
    ID3D12Resource* Depth() const { return depth_.Get(); }
    ID3D12Resource* State() const { return state_.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE RaysSrv() const { return handles_[0]; }
    D3D12_CPU_DESCRIPTOR_HANDLE RaysUav() const { return handles_[1]; }
    D3D12_CPU_DESCRIPTOR_HANDLE IrradianceSrv() const { return handles_[2]; }
    D3D12_CPU_DESCRIPTOR_HANDLE IrradianceUav() const { return handles_[3]; }
    D3D12_CPU_DESCRIPTOR_HANDLE DepthSrv() const { return handles_[4]; }
    D3D12_CPU_DESCRIPTOR_HANDLE DepthUav() const { return handles_[5]; }
    D3D12_CPU_DESCRIPTOR_HANDLE StateSrv() const { return handles_[6]; }
    D3D12_CPU_DESCRIPTOR_HANDLE StateUav() const { return handles_[7]; }

private:
    GpuResource rays_;
    GpuResource irradiance_;
    GpuResource depth_;
    GpuResource state_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> handles_{};
    std::shared_ptr<Material> traceMaterial_;
    std::shared_ptr<Material> updateMaterial_;
    bool ready_ = false;
    bool failed_ = false;

    float spacing_ = 0.0f;
    uint32_t generation_ = 1;
    std::atomic<uint32_t> tracedGeneration_{ 0 };
    uint32_t frameCounter_ = 0;
    int lastBase_[3] = { 0, 0, 0 };
};
