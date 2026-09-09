#pragma once

#include <array>
#include <memory>
#include "rendering/core/RenderConstants.h"
#include "rendering/core/RenderPass.h"
#include "rendering/core/ResourceDeclarations.h"
#include "rendering/lighting/SkyAtmosphereSettings.h"

class Camera;
class Skybox;
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
                      const SkyAtmosphereSettings& settings, const Camera& camera, float startDepthMetres, size_t after, size_t luts);
    size_t BuildView(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph,
                     const SkyAtmosphereSettings& settings, const SkyViewFrameData& view, size_t after, size_t luts);
    size_t BuildAerial(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph,
                       const SkyAtmosphereSettings& settings, const SkyViewFrameData& view,
                       const Camera& camera, float startDepthMetres, size_t after);
    size_t BuildEnvironment(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph,
                            const SkyAtmosphereSettings& settings, SkyViewFrameData view, Skybox* sky, size_t luts);
    size_t BuildDistant(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph,
                        const SkyAtmosphereSettings& settings, SkyViewFrameData view, size_t after);
    bool DistantActive() const { return distantActive_; }
    unsigned DistantRevision() const { return distantBuilds_; }
    ID3D12Resource* DistantResource() const { return distant_.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE DistantSrv() const { return distantSrv_; }
    bool AerialBuilt() const { return aerialBuilt_; }
    ID3D12Resource* AerialResource() const { return aerial_.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE AerialSrv() const { return aerialSrv_; }
    ID3D12Resource* ViewResource() const { return skyView_.Get(); }
    ID3D12Resource* TransmittanceResource() const { return lut_[0].Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE ViewSrv() const { return viewSrv_; }
    bool Ready() const { return initialized_ && !failed_; }
    D3D12_CPU_DESCRIPTOR_HANDLE TransmittanceSrv() const { return srv_[0]; }
    D3D12_CPU_DESCRIPTOR_HANDLE MultiScatterSrv() const { return srv_[1]; }
    void Reset(); // caller has drained the GPU on level/device teardown

private:
    void PrepareDistant(Renderer* renderer);
    void ValidateDistant(UINT slot);
    GpuResource distant_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> distantHeap_;
    D3D12_CPU_DESCRIPTOR_HANDLE distantSrv_{}, distantUav_{};
    std::shared_ptr<Material> distantMaterial_;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, render::kFrameCount> distantReadback_;
    std::array<bool, render::kFrameCount> distantPending_{};
    SkyAtmosphereParameters distantParameters_{};
    SkyViewFrameData distantKey_{};
    bool distantActive_ = false, distantReady_ = false, distantFailed_ = false;
    unsigned distantBuilds_ = 0;
    void PrepareEnvironment(Renderer* renderer);
    void ValidateReadback(UINT slot);
    GpuResource environmentView_;
    std::array<GpuResource, 3> environment_; // radiance 128/8 mips, specular 128/8 mips, E/PI 32
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> environmentHeap_;
    D3D12_CPU_DESCRIPTOR_HANDLE environmentViewSrv_{}, environmentViewUav_{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 3> environmentSrv_{};
    // 8 radiance mips (the sky PICTURE the fog samples), 8 specular mips, 1 irradiance.
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 17> environmentUav_{};
    std::shared_ptr<Material> captureMaterial_, filterMaterial_;
    SkyAtmosphereParameters environmentParameters_{};
    SkyViewFrameData environmentKey_{};
    bool environmentReady_ = false, environmentFailed_ = false;
    unsigned environmentBuilds_ = 0;
    std::array<GpuResource, 2> lut_;
    GpuResource aerial_;
    D3D12_CPU_DESCRIPTOR_HANDLE aerialSrv_{}, aerialUav_{};
    std::shared_ptr<Material> aerialMaterial_;
    bool aerialBuilt_ = false; // reset/committed by each frame's serial builder
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
