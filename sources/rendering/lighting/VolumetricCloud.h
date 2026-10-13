#pragma once

#include <array>
#include <memory>
#include <string>
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

namespace render
{
// FILE OVERRIDES for the density model's three textures, for an A/B against another set (they were
// first used to run Ultra Dynamic Sky's own, which must never enter the repository). Each replaces
// what the engine generates (cloud_noise_cs.hlsl CSProfile / CSFormation / CSCells); session-only,
// read once when the cloud resources are created.
//   `--cloud-profile=<image>`  the profile LUT (u = coverage column, v = 1 - height; R envelope,
//                              G density, B erosion);
//   `--cloud-formation=<dds>`  the formation volume (x/y the field over one tile, z a looping time
//                              axis; R the formation, B the floor variation) -- only with --cloud-cells;
//   `--cloud-cells=<dds>`      the erosion volume (R erosion, B the high-frequency mask).
// `--cloud-dump=<dir>` writes the GENERATED set, once it exists, as <dir>/profile.dds,
// formation.dds and cells.dds (8-bit RGBA, DX10 header -- the volumes load back through the flags
// above): the generator's output measured against a reference, instead of guessed from renders.
extern std::wstring g_cloudProfilePath;
extern std::wstring g_cloudFormationPath;
extern std::wstring g_cloudCellsPath;
extern std::wstring g_cloudDumpDir;
// Compose reads the (unjittered) clouds at uv - this x the camera jitter (SceneRenderer_Lighting.cpp).
// 1 is right; `--set=cloudCompose.jitterScale:0` (the old read) and -1 (the wrong sign) are the A/B
// that measured it (2026-10-12, DLSS Balanced vs the native shot of the same frozen pose, mid-sky
// luminance RMS: 1 -> 1.38, 0 -> 1.57, -1 -> 2.70; a Laplacian "sharpness" ranked -1 FIRST -- it
// counts the staircase edges a wrong shift leaves as detail).
extern float g_cloudComposeJitterScale;
}

// Volumetric clouds (docs/volumetric_fog_sky_clouds_ssgi_plan.md, part C), the SkyAtmosphere shape:
// this owns the FIXED-SIZE resources -- the density model's four generated textures (C1), the cloud
// shadow map and its filtered copy (C3) -- their descriptors and materials, and registers the passes.
// The half-res trace and its full-res reconstruction are render-size dependent and live in the Deferred ring
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
        Math::float3 sunDirection{}; // the direction the CLOUDS' light TRAVELS (DirectionalLight::GetDirection)
        // Its illuminance: outer space when `perSampleTransmittance` (the march applies the air at each
        // sample), else the colour after the air at the camera (no sky LUT to take it from).
        Math::float3 sunIlluminance{};
        // Time-of-day plan N3: the clouds' light and the ground's can differ -- after sunset the clouds
        // keep the sun while the moon lights the ground. The shadow map follows the GROUND's light.
        Math::float3 shadowDirection{}; // zero = sunDirection
        bool perSampleTransmittance = false;
        float atmosphereTopKm = 0.0f;
        ID3D12Resource* transmittanceResource = nullptr; // the sky's transmittance LUT (t7 of the trace)
        D3D12_CPU_DESCRIPTOR_HANDLE transmittanceSrv{};
        unsigned lightBody = 0; // 0 the sun, 1 the moon
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
    VolumetricCloudConstants MakeConstants(const FrameInputs& in) const;

    VolumetricCloud();
    ~VolumetricCloud();
    void Prepare(Renderer* renderer, const VolumetricCloudSettings& settings);
    bool Ready() const { return initialized_ && !failed_; }
    void Reset(); // caller has drained the GPU on level/device teardown

    // C1: the generated textures -- the weather map when the seed changes, the formation and cells
    // volumes when the seed or the roughness does (unless files replaced them), the profile LUT when
    // its shape knobs move. kNone when nothing is registered.
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
    void ClearFrameState() { shadowBuilt_ = false; shadowHistoryValid_ = false; }
    bool ShadowBuilt() const { return shadowBuilt_; }
    ID3D12Resource* ShadowResource() const { return shadowFiltered_.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE ShadowSrv() const { return shadowFilteredSrv_; }
    const Math::mat4& ShadowViewProj() const { return shadowViewProj_; }
    float ShadowFarDepthKm() const { return shadowFarKm_; }
    unsigned NoiseRevision() const { return noiseBuilds_; }

private:
    static constexpr UINT kWeatherSize = 512, kShadowSize = 512;
    GpuResource weather_, shadowRaw_, shadowFiltered_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    D3D12_CPU_DESCRIPTOR_HANDLE weatherSrv_{}, weatherUav_{};
    // The density model's textures (cloud_common.hlsli). Each is GENERATED here (cloud_noise_cs.hlsl
    // CSProfile / CSFormation / CSCells, again whenever its knobs or the seed change) unless a --cloud-*
    // file replaced it (uploaded once when the resources are created). All rest in NON_PIXEL like the
    // weather map: the shadow pass reads them on the async compute queue.
    static constexpr UINT kProfileWidth = 64, kProfileHeight = 256, kFormationSize = 256, kFormationDepth = 128, kCellsSize = 64;
    GpuResource profileGen_, formationGen_, cellsGen_;
    D3D12_CPU_DESCRIPTOR_HANDLE profileGenSrv_{}, profileGenUav_{}, formationGenSrv_{}, formationGenUav_{}, cellsGenSrv_{}, cellsGenUav_{};
    GpuResource profile_, formation_, cells_; // the file overrides
    D3D12_CPU_DESCRIPTOR_HANDLE profileSrv_{}, formationSrv_{}, cellsSrv_{};
    float profileInvSize_[2] = {1.0f / kProfileWidth, 1.0f / kProfileHeight};
    std::shared_ptr<Material> noiseProfile_, noiseFormation_, noiseCells_;
    bool profileGenReady_ = false, formationGenReady_ = false;
    float profileGenKey_[10] = {};
    std::uint32_t formationGenSeed_ = 0;
    float formationGenRoughness_ = 0.0f;
    bool CreateProfile(Renderer* renderer, D3D12_CPU_DESCRIPTOR_HANDLE srv); // the --cloud-profile file, if any
    // An 8-bit RGBA/BGRA volume DDS (DX10 header) into `target`; false (one warning) when it is not.
    bool LoadVolumeDds(Renderer* renderer, const std::wstring& path, GpuResource& target,
                       D3D12_CPU_DESCRIPTOR_HANDLE srv, const wchar_t* name);
    bool FormationFromFiles() const { return formation_.Get() != nullptr && cells_.Get() != nullptr; }
    bool ProfileFromFile() const { return profile_.Get() != nullptr; }
    // What the density code's three slots hold (every cloud pass binds them the same way): the
    // generated textures, or the files that replaced them.
    struct DensityBinding
    {
        ID3D12Resource* formation; ID3D12Resource* cells; ID3D12Resource* profile;
        D3D12_CPU_DESCRIPTOR_HANDLE formationSrv, cellsSrv, profileSrv;
    };
    DensityBinding Binding() const
    {
        const bool files = FormationFromFiles(), profileFile = ProfileFromFile();
        return {files ? formation_.Get() : formationGen_.Get(), files ? cells_.Get() : cellsGen_.Get(),
                profileFile ? profile_.Get() : profileGen_.Get(),
                files ? formationSrv_ : formationGenSrv_, files ? cellsSrv_ : cellsGenSrv_,
                profileFile ? profileSrv_ : profileGenSrv_};
    }
    void CreateResources(Renderer* renderer);
    // render::g_cloudDumpDir: armed by the rebuild that generated the set, done in a LATER frame's
    // Prepare (that rebuild has then been submitted; the dump waits for the GPU).
    unsigned long long dumpArmedFrame_ = 0;
    bool dumpArmed_ = false, dumpDone_ = false;
    void DumpGenerated(Renderer* renderer);
    D3D12_CPU_DESCRIPTOR_HANDLE shadowRawSrv_{}, shadowRawUav_{}, shadowFilteredSrv_{}, shadowFilteredUav_{};
    std::shared_ptr<Material> noiseWeather_, trace_, temporal_, shadowTrace_, shadowFilter_;
    UINT cbBytes_ = 0;
    // C4: the environment capture material and the key of the last capture. The cube refreshes when
    // the settings, the noise or the distant light changed, and otherwise at most every
    // kCaptureIntervalFrames while the clouds drift/evolve (shared wind clock) or the probe moves: a still
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
    // Time slicing (render::g_cloudShadowUpdateFrames): the raw map persists across frames, and
    // this is the key it was traced under. Anything in it changing forces a full trace; so does a
    // frame that built no map (the content is then of unknown age). Committed in BuildShadow.
    bool shadowHistoryValid_ = false;
    float shadowKeyViewProj_[16] = {};
    VolumetricCloudSettings shadowKeySettings_{}; // byte copy (memcpy), compared with memcmp
    unsigned shadowKeyNoise_ = 0;
    float shadowKeyPlanetKm_ = 0.0f;
    std::uint32_t shadowKeyFrames_ = 0;
    std::uint32_t shadowKeyCheap_ = ~0u; // cheap-density bits the map was traced with (never a valid value at start)
    Math::float2 shadowKeyWind_{}; // LAST frame's drift offset (m): a jump beyond a quarter texel re-traces
    std::uint32_t shadowSlicePhase_ = 0;
    UINT64 ownedBytes_ = 0;
};
