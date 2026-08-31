#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>

#include "rendering/core/UploadBatch.h" // held by value in a unique_ptr member below
#include <string>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

#include "core/math/Math.h"
#include "ocean/OceanSimulationConfig.h"
#include "ocean/OceanSimulationInputs.h"
#include "ocean/OceanSimulationSettings.h"
#include "app/scene/SceneView.h"
#include "rendering/core/ResourceDeclarations.h"

class Renderer;
class Material;
class Camera;
struct RenderGraphPassContext;

class OceanSimulation
{
public:
    OceanSimulation();
    explicit OceanSimulation(const std::wstring& configPath);
    ~OceanSimulation() = default;

    void Initialize(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);

    void Update(Renderer* renderer, ID3D12GraphicsCommandList* cl, float timeSeconds);
    // Barrier plan step 5: Update's transition sequence, registered ahead of recording.
    // Keep in step with Update / DispatchSpectrum / DispatchFFT* / GenerateMips / DispatchFoam.
    void PrepareUpdate(RenderGraphPassContext& ctx);
    // D1.1: shared by PrepareUpdate and Update so the two cannot disagree about whether
    // this frame performs the displacement-history copy.
    bool WillCopyDisplacementHistory() const;
    // Called from SceneRenderer::EnsureFrameResources, before the graph is built: re-creates the
    // GPU resources after an invalidating settings change so Prepare and Record see the SAME
    // pointers. See the definition for what went wrong when this was lazy inside Update().
    void EnsureFrameResources(Renderer* renderer);
    void OnHotReload(Renderer* renderer);

    void SetSettings(Renderer* renderer, const OceanSimulationSettings& settings);
    void SetInputsProvider(Renderer* renderer, const OceanSimulationInputsProvider& provider);
    void ResetInitialSpectrum(Renderer* renderer);
    bool LoadConfig(Renderer* renderer, const std::wstring& path);
    bool SaveConfig(const std::wstring& path) const;
    void ApplyConfig(Renderer* renderer, const OceanSimulationConfig& config);
    OceanSimulationConfig GetConfigCopy() const;
    const OceanRenderConfig& GetRenderConfig() const { return config_.render; }
    void SetRenderConfig(const OceanRenderConfig& render) { config_.render = render; }
    const std::wstring& GetConfigPath() const { return configPath_; }
    OceanSimulationInputsProvider& GetInputsProvider() { return inputsProvider_; }
    const OceanSimulationInputsProvider& GetInputsProvider() const { return inputsProvider_; }

    void SetSceneVariables(Renderer* renderer, float localWindDirectionDegrees, float swellDirectionDegrees, float windForce01);
    const OceanSimulationSettings& GetSettings() const { return settings_; }
    OceanSimulationInputs EvaluateInputs() const;
    // Same evaluation at an ARBITRARY wind force, without touching the live state. Read-only:
    // used by the shore run-up to learn the reference wave height at "full at wind", so the
    // nearshore wave drive can stop growing past that force while the open-sea FFT keeps its size.
    OceanSimulationInputs EvaluateInputsAt(float windForce01) const;

    UINT GetResolution() const { return resolution_; }
    UINT GetCascadeCount() const { return cascadeCount_; }

    float GetPatchLength() const { return basePatchLength_; }
    const Math::float4& GetLengthScales() const { return lengthScales_; }
    const Math::float4& GetInvLengthScales() const { return invLengthScales_; }
    float GetDisplacementAmplitude() const { return displacementAmplitude_; }
    float GetLocalWindDirectionDegrees() const { return localWindDirection_; }
    float GetSwellDirectionDegrees() const { return swellDirection_; }
    float GetWindForce01() const { return windForce01_; }
    float GetLocalWindDirectionRadians() const;
    Math::float2 GetLocalWindDirectionVector() const;
    float GetFoamTrailUpdateTime() const;

    D3D12_CPU_DESCRIPTOR_HANDLE GetDisplacementSRV() const
    {
        if (displacementFullSrv_.ptr != 0)
        {
            return displacementFullSrv_;
        }

        return displacementSrvs_.empty() ? D3D12_CPU_DESCRIPTOR_HANDLE{} : displacementSrvs_[0];
    }
    D3D12_CPU_DESCRIPTOR_HANDLE GetPreviousDisplacementSRV() const { return prevDisplacementSrv_; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetFoamTurbulenceSRV() const { return foamSrv_; }
    ID3D12Resource* GetDisplacementResource() const { return displacement_.Get(); }
    ID3D12Resource* GetPreviousDisplacementResource() const { return prevDisplacement_.Get(); }
    bool HasPreviousDisplacement() const { return prevDisplacementValid_; }
    ID3D12Resource* GetFoamResource() const { return foamTurbulence_.Get(); }
    ID3D12Resource* GetShoreDepthResource() const { return shoreDepth_.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetShoreDepthDsv() const { return shoreDepthDsv_; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetShoreDepthSrv() const { return shoreDepthSrv_; }
    SceneView& GetShoreDepthView() { return shoreDepthView_; }
    const SceneView& GetShoreDepthView() const { return shoreDepthView_; }
    UINT GetShoreDepthWidth() const { return shoreDepthWidth_; }
    UINT GetShoreDepthHeight() const { return shoreDepthHeight_; }
    float2 GetShoreViewCenter() const;
    float GetShoreViewHeight() const;
    float2 GetShoreDepthRange() const;
    float GetShoreDepthHalfExtent() const { return shoreDepthHalfExtent_; }
    void UpdateShoreView(const Camera& camera);
    bool ShouldRenderShoreDepth() const { return shouldRenderShoreDepth_; }

    // Shore SDF — a STATIC map of the whole level holding plan-view distance to the waterline.
    // Scene calls SetShoreArea once the terrain's footprint is known; that centres the map and
    // marks it for a single rebuild. Unlike the depth window there is no "outside the field" case,
    // so a distant island is still known about.
    void SetShoreArea(float2 centerXZ);
    bool ShouldBuildShoreSdf() const { return shoreSdfDirty_; }
    void MarkShoreSdfBuilt() { shoreSdfDirty_ = false; }
    ID3D12Resource* GetShoreSdfSourceResource() const { return shoreSdfSource_.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetShoreSdfSourceDsv() const { return shoreSdfSourceDsv_; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetShoreSdfSrv() const { return shoreSdfSrv_; }
    ID3D12Resource* GetShoreSdfResource() const { return shoreSdfJump_[1].Get(); }
    ID3D12Resource* GetShoreSdfScratchResource() const { return shoreSdfJump_[0].Get(); }
    SceneView& GetShoreSdfView() { return shoreSdfView_; }
    const SceneView& GetShoreSdfView() const { return shoreSdfView_; }
    float2 GetShoreSdfCenter() const { return shoreSdfCenter_; }
    float GetShoreSdfHalfExtent() const { return shoreSdfHalfExtent_; }
    // The jump-flood itself. Called with the source already rendered and readable.
    void BuildShoreSdf(Renderer* renderer, ID3D12GraphicsCommandList* cl);
    // pass-flow S6: everything BuildShoreSdf needs before it will record anything. The pass
    // builder asks BEFORE declaring, because the flood's own early-out used to fire mid-record,
    // after the pass had declared the SDF's UAV/SRV points — and it also cleared `shoreSdfDirty_`
    // on the way out, so a level whose SDF materials were not up yet lost its one rebuild.
    bool CanBuildShoreSdf() const
    {
        return shoreSdfSeedMaterial_ && shoreSdfJumpMaterial_ && shoreSdfResolveMaterial_ &&
               shoreSdfJump_[0] && shoreSdfJump_[1];
    }
    void SetShoreViewSnapMultiplier(float multiplier)
    {
        shoreViewSnapMultiplier_ = std::max(multiplier, 1.0f);
    }

    const FoamParams& GetFoamParams() const { return inputs_.foam; }

    // The read state the sim leaves its displacement/foam maps in, in ONE place.
    //
    // It was written out longhand in three: PrepareUpdate's declarations, Update's mirroring
    // transitions, and a third hard-coded copy inside DispatchFoam. Moving Main_ObjectCompute to
    // the compute queue meant dropping the PIXEL bit (direct-queue-exclusive), and the third copy
    // was missed — the pass then failed Close() with E_INVALIDARG, which is a much worse way to
    // learn it than a compile error. A declaration and the transition that mirrors it are ONE fact;
    // they get one name.
    //
    // NON_PIXEL, not NON_PIXEL|PIXEL: the pixel-shader bit is added by the consumer that actually
    // needs it (OceanRenderable::PrepareRender) and stripped again by Main_PrologueClear.
    static constexpr D3D12_RESOURCE_STATES kSimMapReadState =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

private:
    void BuildSpectrum();
    void CreateResources(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
    void CreateDescriptors(ID3D12Device* device);
    void CreateMaterials(Renderer* renderer);
    void DispatchSpectrum(Renderer* renderer, ID3D12GraphicsCommandList* cl, float timeSeconds);
    void DispatchFFT(Renderer* renderer, ID3D12GraphicsCommandList* cl);
    void DispatchFFTPost(Renderer* renderer, ID3D12GraphicsCommandList* cl);
    void GenerateMips(Renderer* renderer, ID3D12GraphicsCommandList* cl);
    void DispatchFoam(Renderer* renderer, ID3D12GraphicsCommandList* cl, float simTime);
    void InitializeFoamTexture(Renderer* renderer, ID3D12GraphicsCommandList* cl);
    void RefreshDerivedSettings();
    void ResetGpuResources(Renderer* renderer, bool resetMaterials);
    void RetireGpuResources(Renderer* renderer);
    void RetireUploadResources(Renderer* renderer, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> resources);
    void CollectRetiredResources(Renderer* renderer);
    float ComputeCascadeContribution(float kLength, UINT cascade) const;
    void InitializeFromConfig(const std::wstring& configPath);
    void ApplyConfigInternal(Renderer* renderer, const OceanSimulationConfig& config, bool resetResources);
    void ReleaseCpuData();
    void CreateShoreDepth(Renderer* renderer);

private:
    bool initialized_ = false;

    OceanSimulationSettings defaultSettings_;
    OceanSimulationSettings settings_;
    float basePatchLength_ = 0.0f;
    float displacementAmplitude_ = 1.5f;
    float timeScale_ = 1.0f;
    float localWindDirection_ = 0.0f;
    float swellDirection_ = 0.0f;
    float windForce01_ = 0.0f;
    float waterDepth_ = 1000.0f;
    float chopValue_ = 1.0f;
    float shoreViewSnapMultiplier_ = 4.0f;

    std::shared_ptr<EqualizerPreset> defaultEqualizerPreset_;
    std::shared_ptr<SwellPreset> defaultSwellPreset_;
    std::shared_ptr<LocalWavesPreset> defaultLocalPreset_;
    std::vector<std::shared_ptr<LocalWavesPreset>> defaultLocalPresets_;
    OceanSimulationConfig config_;
    std::wstring configPath_;

    OceanSimulationInputsProvider inputsProvider_;
    OceanSimulationInputs inputs_;

    Math::float4 lengthScales_ = Math::float4(0.0f, 0.0f, 0.0f, 0.0f);
    Math::float4 invLengthScales_ = Math::float4(0.0f, 0.0f, 0.0f, 0.0f);
    Math::float4 cutoffsLow_ = Math::float4(0.0f, 0.0f, 0.0f, 0.0f);
    Math::float4 cutoffsHigh_ = Math::float4(0.0f, 0.0f, 0.0f, 0.0f);

    UINT resolution_ = 0;
    UINT cascadeCount_ = 0;
    UINT arraySliceCount_ = 0;

    std::vector<Math::float4> h0Data_;
    std::vector<Math::float4> waveData_;

    struct RetiredGpuResources
    {
        uint64_t retireFrame = 0;
        Microsoft::WRL::ComPtr<ID3D12Resource> h0Buffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> waveDataBuffer;
        GpuResource displacement;
        GpuResource prevDisplacement;
        GpuResource foamTurbulence;
    };

    struct RetiredUploadResources
    {
        uint64_t retireFrame = 0;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> resources;
    };

    Microsoft::WRL::ComPtr<ID3D12Resource> h0Buffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> waveDataBuffer_;
    GpuResource displacement_;
    GpuResource prevDisplacement_;
    GpuResource foamTurbulence_;
    // Step 6b part 2: first owner on the wrapper. Chosen because it was a MEASURED leaker for a
    // telling reason — it is the one ocean resource RetireGpuResources' hand-written clear list
    // forgets. The wrapper makes that impossible: registration dies with the resource.
    GpuResource shoreDepth_;
    SceneView shoreDepthView_{};
    UINT shoreDepthWidth_ = 0u;
    UINT shoreDepthHeight_ = 0u;
    // 500 m window following the camera, 512^2 = 0.98 m per texel. This is the DETAIL map: foam
    // widths, run-up, sink and water colour all read its depth, so its budget goes where the
    // camera is. Whole-level coverage is the SDF's job.
    float shoreDepthHalfExtent_ = 250.0f;
    float2 prevShoreDepthPos_ = {FLT_MAX, FLT_MAX};

    // Shore SDF: 2 x 2 km, STATIC, centred on the terrain, holding the plan-view distance to the
    // waterline. It answers only "how far is land from here", which is what the wave's vertical
    // damping needs — and unlike the depth window it covers the whole level, so there is no
    // "outside the field" case for a distant island to fall through.
    float shoreSdfHalfExtent_ = 1000.0f;
    float2 shoreSdfCenter_ = {0.0f, 0.0f};
    bool shoreSdfDirty_ = true;
    // 1024^2 over 2 km = 1.95 m per texel. Coarser than the depth window on purpose: this feeds
    // wave damping, whose query is widened by half a clipmap quad anyway, so sub-metre precision
    // here would buy nothing.
    static constexpr UINT kShoreSdfSize = 1024u;
    GpuResource shoreSdfSource_;   // terrain rendered from above, the flood's input
    GpuResource shoreSdfJump_[2];  // jump-flood ping-pong; [1] ends up holding the distance
    SceneView shoreSdfView_{};
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> shoreSdfDsvHeap_;
    D3D12_CPU_DESCRIPTOR_HANDLE shoreSdfSourceDsv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE shoreSdfSourceSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE shoreSdfJumpUav_[2]{};
    D3D12_CPU_DESCRIPTOR_HANDLE shoreSdfSrv_{};
    std::shared_ptr<Material> shoreSdfSeedMaterial_;
    std::shared_ptr<Material> shoreSdfJumpMaterial_;
    std::shared_ptr<Material> shoreSdfResolveMaterial_;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> shoreDepthDsvHeap_;
    D3D12_CPU_DESCRIPTOR_HANDLE shoreDepthDsv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE shoreDepthSrv_{};


    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptorHeap_;
    UINT descriptorIncr_ = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE h0Srv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE waveDataSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE displacementFullSrv_{};
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> displacementSrvs_;
    D3D12_CPU_DESCRIPTOR_HANDLE prevDisplacementSrv_{};
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> displacementUavs_;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> foamSrvs_;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> foamUavs_;
    D3D12_CPU_DESCRIPTOR_HANDLE foamSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE foamUav_{};

    UINT mipCount_ = 1u;
    std::vector<UINT> mipExtents_;

    std::shared_ptr<Material> spectrumMaterial_;
    std::shared_ptr<Material> fftMaterial_;
    std::shared_ptr<Material> fftPostMaterial_;
    std::shared_ptr<Material> mipMaterial_;
    std::shared_ptr<Material> foamSimMaterial_;
    std::shared_ptr<Material> foamInitMaterial_;

    float lastFoamSimTime_ = 0.0f;
    bool foamNeedsInit_ = true;
    bool hasDisplacementHistory_ = false;
    bool prevDisplacementValid_ = false;
    bool shouldRenderShoreDepth_ = true;
    std::vector<RetiredGpuResources> retiredGpuResources_;

    // Upload batches from EnsureFrameResources, kept alive until their GPU work has certainly
    // finished (drained in CollectRetiredResources by the same rule as the resources above).
    struct PendingInitBatch
    {
        std::unique_ptr<UploadBatch> batch;
        uint64_t submitFrame = 0;
    };
    std::vector<PendingInitBatch> pendingInitBatches_;
    std::vector<RetiredUploadResources> retiredUploadResources_;
};

