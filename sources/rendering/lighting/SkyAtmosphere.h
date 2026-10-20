#pragma once

#include <array>
#include <functional>
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

namespace render
{
// Time-of-day P1: the environment capture and the distant light keep their last build while the moving bodies
// and their light stay within a tolerance of it (SkyAtmosphere.cpp, NearlySameView). false = the exact keys
// of before -- every change rebuilds; the A/B arm (`--set=sky.keyTolerance`).
inline bool g_skyKeyTolerance = true;
}

// Plan C4: something composited INTO the sky's radiance cube between its capture and its probe
// filter -- the volumetric clouds, in UE's real-time sky light capture role. Its owner fills this
// per frame; the environment rebuilds when the sky's own key OR this says so, runs the material
// once per radiance mip (dispatch size x size*6, the cube's six faces down the Y), with the owner's
// four SRVs at t0..t3 and the sky's transmittance and multi-scatter LUTs appended at t4, t5, samplers
// linear wrap + linear clamp, and a constant buffer of the owner's `ownBytes` (writeConstants) followed
// by the sky's SkyAtmosphereParameters and SkyViewFrameData.
struct SkyEnvironmentOverlay
{
    bool active = false;   // composite this frame
    bool dirty = false;    // the overlay changed enough to rebuild even when the sky's key did not
    Material* material = nullptr;
    UINT cbBytes = 0;      // aligned size of the whole constant buffer
    UINT ownBytes = 0;     // the owner's blob at its head; the sky's params + view follow
    std::function<void(uint8_t* dst, unsigned mipSize)> writeConstants;
    std::array<ID3D12Resource*, 5> reads{};            // declared at rest for the pass (null = none)
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 5> srvs{}; // t0..t4; the sky's two LUTs follow at t5, t6
    size_t dependency = static_cast<size_t>(-1);       // a pass the overlay's inputs come from
    // How much of the sun reaches the ground under the overlay's clouds (CloudSunGroundVisibility):
    // the lower hemisphere's ground term takes it, or a closed deck kept a sunlit ground below the
    // probes' horizon -- which the far sea's grazing reflection picked up as a glowing line.
    float groundSunScale = 1.0f;
};

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
    // `night` (time-of-day N2): the night sky's layers, captured into the picture beside the LUT's sky;
    // `nightPass` the pass that generates the Milky Way's map this frame, if one does.
    size_t BuildEnvironment(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph,
                            const SkyAtmosphereSettings& settings, SkyViewFrameData view, Skybox* sky, size_t luts,
                            const SkyEnvironmentOverlay* overlay, const NightSkyFrameData& night, size_t nightPass);
    unsigned EnvironmentRevision() const { return environmentBuilds_; }
    size_t BuildDistant(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph,
                        const SkyAtmosphereSettings& settings, SkyViewFrameData view, size_t after);
    bool DistantActive() const { return distantActive_; }
    unsigned DistantRevision() const { return distantBuilds_; }
    // Bumps only when a rebuild's key moved FAR from the last such bump: other atmosphere parameters,
    // the sun turned more than ~2 degrees, a light's illuminance changed by more than 10 %, the sky's
    // scale or the planet changed (time-of-day plan T0). What drops HISTORY keys on this one -- the
    // volumetric fog and the cloud capture: a sun moving a little every frame rebuilds the distant
    // light every frame, and DistantRevision then threw both histories away every frame.
    unsigned DistantMajorRevision() const { return distantMajorBuilds_; }
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
    // Time-of-day plan N1: the moon's near side for the drawn disc -- its normal (xyz, the disc's own
    // frame: x right, y up, z toward the viewer) and its relative albedo (w, mean ~1) -- procedural
    // (the owner's decision: no downloaded maps), generated ONCE into a mipped texture. Every mip is
    // computed straight from the generator at its own resolution, the craters too small for it left
    // out, so no mip reads another and the chain is one barrier-free run. kNone when nothing to do.
    size_t BuildMoon(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph);
    bool MoonReady() const { return moonReady_; }
    ID3D12Resource* MoonResource() const { return moon_.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE MoonSrv() const { return moonSrv_; }
    // Time-of-day plan N2: the Milky Way, generated ONCE into an equirectangular map in equatorial
    // coordinates (sky_night_cs.hlsl; night_sky.hlsli's units), every mip straight from the model the
    // moon's way. kNone when nothing to do. NightNullSrv is a type-correct null for a table that must be full.
    size_t BuildNight(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph);
    bool NightReady() const { return nightReady_; }
    ID3D12Resource* NightResource() const { return night_.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE NightSrv() const { return nightSrv_; }
    D3D12_CPU_DESCRIPTOR_HANDLE NightNullSrv() const { return nightNullSrv_; }
    void Reset(); // caller has drained the GPU on level/device teardown

private:
    void PrepareMoon(Renderer* renderer);
    static constexpr UINT kMoonSize = 512, kMoonMips = 8;
    GpuResource moon_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> moonHeap_;
    D3D12_CPU_DESCRIPTOR_HANDLE moonSrv_{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMoonMips> moonUav_{};
    std::shared_ptr<Material> moonMaterial_;
    bool moonReady_ = false, moonFailed_ = false;
    void PrepareNight(Renderer* renderer);
    static constexpr UINT kNightWidth = 2048, kNightHeight = 1024, kNightMips = 7;
    GpuResource night_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> nightHeap_;
    D3D12_CPU_DESCRIPTOR_HANDLE nightSrv_{}, nightNullSrv_{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kNightMips> nightUav_{};
    std::shared_ptr<Material> nightMaterial_;
    bool nightReady_ = false, nightFailed_ = false;
    bool nightWanted_ = false; // a frame asked for the map: the next Prepare allocates it (8 MB, so only then)
    NightSkyFrameData environmentNightKey_{}; // what the last capture drew of the night sky (its clock zeroed)
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
    SkyAtmosphereParameters distantMajorParameters_{};
    SkyViewFrameData distantMajorKey_{};
    bool distantMajorValid_ = false;
    unsigned distantMajorBuilds_ = 0;
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
    bool environmentOverlayActive_ = false; // what the last build composited: a switch either way rebuilds
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
