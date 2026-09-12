#pragma once

#include <array>
#include <memory>
#include "core/math/Math.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/core/RenderPass.h"
#include "rendering/core/ResourceDeclarations.h"
#include "rendering/lighting/SkyAtmosphere.h" // SkyEnvironmentOverlay (C4)
#include "rendering/lighting/VolumetricCloudSettings.h"

class Renderer;
class Material;
template <size_t MaxPasses> class RenderGraph;
struct RenderGraphPassContext;
namespace vfx { struct WindState; }

// Volumetric clouds (docs/volumetric_fog_sky_clouds_ssgi_plan.md, part C), the SkyAtmosphere shape:
// this owns the FIXED-SIZE resources -- the three noise textures (C1), the cloud shadow map and its
// filtered copy (C3) -- their descriptors and materials, and registers the passes. The half-res
// trace and its temporal history are render-size dependent and live in the Deferred ring
// (RenderTargetManager: cloudTrace / cloudTraceDepth / cloudResolved / cloudResolvedDepth), where
// the previous slot is the history exactly as it is for the fog and the AO.
//
// Cross-frame state (the shadow map's "built this frame", the noise's seed) is committed in the
// serial builder, never in a worker's record. Graphics queue only.
class VolumetricCloud
{
public:
    // Everything one frame's constants are made of. Filled by the SceneRenderer, which owns the
    // camera, the sun, the exposure and the sky's per-frame state; MakeConstants is pure.
    struct FrameInputs
    {
        const VolumetricCloudSettings* settings = nullptr;
        Math::mat4 invViewProjNoJitter{}, viewProjNoJitter{}, prevViewProjNoJitter{};
        Math::float3 cameraPos{};
        float projZ33 = 0.0f, projZ43 = 0.0f;
        Math::float3 sunDirection{}; // the direction the light TRAVELS (DirectionalLight::GetDirection)
        Math::float3 sunIlluminance{}; // post-transmittance effective colour
        float planetRadiusKm = 6360.0f;
        // The sky's per-frame products the trace reads (B3 aerial volume, B5 distant light): the
        // resource for the declaration, the SRV for the table. Null / off when the sky did not build them.
        bool aerialBuilt = false;
        float aerialStartKm = 0.1f;
        ID3D12Resource* aerialResource = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE aerialSrv{};
        bool distantActive = false;
        ID3D12Resource* distantResource = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE distantSrv{};
        float preExposure = 1.0f, prevPreExposure = 1.0f;
        Math::float2 windDirXZ{1.0f, 0.0f};
        float windTime = 0.0f;
        unsigned outputWidth = 1, outputHeight = 1, depthWidth = 1, depthHeight = 1;
        bool historyValid = false;
        unsigned frameIndex = 0;
        unsigned debugView = 0;
    };
    static VolumetricCloudConstants MakeConstants(const FrameInputs& in);

    VolumetricCloud();
    ~VolumetricCloud();
    void Prepare(Renderer* renderer, const VolumetricCloudSettings& settings);
    bool Ready() const { return initialized_ && !failed_; }
    void Reset(); // caller has drained the GPU on level/device teardown

    // C1: the noise set, rebuilt when the seed changes. kNone when nothing is registered.
    size_t BuildNoise(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph,
                      const VolumetricCloudSettings& settings);
    // C3: the shadow map from the sun. `after` = the noise pass (or kNone). Commits ShadowBuilt().
    size_t BuildShadow(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph,
                       const FrameInputs& in, size_t after);
    // C2: the half-res trace and its temporal resolve into the ring. `deps` = what it must follow
    // (the G-buffer, the aerial volume, the noise). The resolved pair is left NON_PIXEL for compose.
    size_t BuildTrace(Renderer* renderer, RenderGraph<static_cast<size_t>(RenderPass::Main_Count)>& graph,
                      const FrameInputs& in, const std::array<size_t, 4>& deps, size_t depCount);

    // C4: the clouds in the sky environment capture (UE's real-time sky light capture role). The
    // overlay the SkyAtmosphere composites between its capture and its probe filter; `dirty` when
    // the clouds changed or drifted enough since the last capture (the key is committed here, in the
    // serial builder). `noisePass` orders the capture after a noise rebuild; `distantRevision` is the
    // B5 light's rebuild count (the capture reads last frame's, and re-captures once it has changed).
    SkyEnvironmentOverlay MakeEnvironmentOverlay(Renderer* renderer, const FrameInputs& in, size_t noisePass,
                                                 unsigned distantRevision);

    // A frame that registers no cloud passes at all (clouds off, cubemap sky) clears the per-frame
    // flags the consumers read, so a stale "built" never outlives the frame that built it.
    void ClearFrameState() { shadowBuilt_ = false; }
    bool ShadowBuilt() const { return shadowBuilt_; }
    ID3D12Resource* ShadowResource() const { return shadowFiltered_.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE ShadowSrv() const { return shadowFilteredSrv_; }
    const Math::mat4& ShadowViewProj() const { return shadowViewProj_; }
    float ShadowFarDepthKm() const { return shadowFarKm_; }
    unsigned NoiseRevision() const { return noiseBuilds_; }

private:
    // The detail noise was 32^3 (Schneider's figure); at that size its finest Worley cells were one
    // texel wide and trilinear filtering drew the texel lattice as streaks across the clouds. 128^3
    // (8 MB) with the octaves capped at four texels per cell (cloud_noise_cs.hlsl) fixed it.
    static constexpr UINT kBaseSize = 128, kDetailSize = 128, kWeatherSize = 512, kShadowSize = 512;
    GpuResource base_, detail_, weather_, shadowRaw_, shadowFiltered_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    D3D12_CPU_DESCRIPTOR_HANDLE baseSrv_{}, baseUav_{}, detailSrv_{}, detailUav_{}, weatherSrv_{}, weatherUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE shadowRawSrv_{}, shadowRawUav_{}, shadowFilteredSrv_{}, shadowFilteredUav_{};
    std::shared_ptr<Material> noiseBase_, noiseDetail_, noiseWeather_, trace_, temporal_, shadowTrace_, shadowFilter_;
    UINT cbBytes_ = 0;
    // C4: the environment capture material and the key of the last capture. The cube refreshes when
    // the settings, the noise or the distant light changed, and otherwise at most every
    // kCaptureIntervalFrames while the clouds drift (wind) or the probe moves (the camera): a still
    // scene with frozen wind never recaptures (B4's rule).
    static constexpr unsigned kCaptureIntervalFrames = 4;
    static constexpr float kCaptureMoveMetres = 10.0f;
    std::shared_ptr<Material> capture_;
    UINT captureCbBytes_ = 0;
    bool captureValid_ = false;
    VolumetricCloudSettings captureSettings_{};
    unsigned captureNoiseRevision_ = 0, captureDistantRevision_ = 0;
    unsigned long long captureFrame_ = 0;
    Math::float2 captureProbeXZ_{};
    float captureWindTime_ = 0.0f;
    bool initialized_ = false, failed_ = false;
    bool noiseReady_ = false;
    std::uint32_t noiseSeed_ = 0;
    unsigned noiseBuilds_ = 0;
    bool shadowBuilt_ = false; // reset/committed by each frame's serial builder
    Math::mat4 shadowViewProj_{};
    float shadowFarKm_ = 1.0f;
    UINT64 ownedBytes_ = 0;
};
