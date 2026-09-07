#pragma once

#include <array>
#include <memory>
#include "rendering/core/RenderConstants.h"
#include "rendering/core/RenderPass.h"
#include "rendering/core/ResourceDeclarations.h"
#include "rendering/lighting/SkyAtmosphereSettings.h"

class Material;
class Renderer;
template <size_t MaxPasses> class RenderGraph;
struct RenderGraphPassContext;

// B1: fixed-size, view-independent LUTs. One GPU copy, ordered on the graphics queue.
// Changes overwrite in place only after previous graphics readers; resize never reallocates.
class SkyAtmosphere
{
public:
    SkyAtmosphere();
    ~SkyAtmosphere();
    void Prepare(Renderer* renderer, const SkyAtmosphereSettings& settings);
    size_t Build(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph, const SkyAtmosphereSettings& settings);
    size_t BuildDebug(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph,
                      const SkyAtmosphereSettings& settings, size_t after, size_t luts);
    bool Ready() const { return initialized_ && !failed_; }
    D3D12_CPU_DESCRIPTOR_HANDLE TransmittanceSrv() const { return srv_[0]; }
    D3D12_CPU_DESCRIPTOR_HANDLE MultiScatterSrv() const { return srv_[1]; }
    void Reset(); // caller has drained the GPU on level/device teardown

private:
    void ValidateReadback(UINT slot);
    std::array<GpuResource, 2> lut_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> srv_{}, uav_{};
    std::array<std::shared_ptr<Material>, 2> material_;
    std::shared_ptr<Material> debugMaterial_;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, render::kFrameCount> readback_;
    std::array<bool, render::kFrameCount> pending_{};
    std::array<SkyAtmosphereParameters, render::kFrameCount> pendingParameters_{};
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> footprint_{};
    UINT64 readbackBytes_ = 0;
    UINT64 ownedBytes_ = 0;
    SkyAtmosphereParameters cached_{};
    bool initialized_ = false, failed_ = false;
    unsigned builds_ = 0;
};
