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

// Fixed-size transfer LUTs (parameter-dirty) and SkyView (per frame). Graphics queue only.
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
    size_t BuildView(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph,
                     const SkyAtmosphereSettings& settings, const SkyViewFrameData& view, size_t after, size_t luts);
    ID3D12Resource* ViewResource() const { return skyView_.Get(); }
    ID3D12Resource* TransmittanceResource() const { return lut_[0].Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE ViewSrv() const { return viewSrv_; }
    bool Ready() const { return initialized_ && !failed_; }
    D3D12_CPU_DESCRIPTOR_HANDLE TransmittanceSrv() const { return srv_[0]; }
    D3D12_CPU_DESCRIPTOR_HANDLE MultiScatterSrv() const { return srv_[1]; }
    void Reset(); // caller has drained the GPU on level/device teardown

private:
    void ValidateReadback(UINT slot);
    std::array<GpuResource, 2> lut_;
    GpuResource skyView_;
    D3D12_CPU_DESCRIPTOR_HANDLE viewSrv_{}, viewUav_{};
    std::shared_ptr<Material> viewMaterial_;
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
