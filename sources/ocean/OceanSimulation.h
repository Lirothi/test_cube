#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

#include "core/math/Math.h"
#include "ocean/OceanSimulationConfig.h"
#include "ocean/OceanSimulationInputs.h"
#include "ocean/OceanSimulationSettings.h"
#include "app/scene/SceneView.h"

class Renderer;
class Material;
class Camera;

class OceanSimulation
{
public:
    OceanSimulation();
    ~OceanSimulation() = default;

    void Initialize(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);

    void Update(Renderer* renderer, ID3D12GraphicsCommandList* cl, float timeSeconds);
    void OnHotReload(Renderer* renderer);

    void SetSettings(Renderer* renderer, const OceanSimulationSettings& settings);
    void SetInputsProvider(Renderer* renderer, const OceanSimulationInputsProvider& provider);
    void ResetInitialSpectrum(Renderer* renderer);
    bool LoadConfig(Renderer* renderer, const std::wstring& path);
    bool SaveConfig(const std::wstring& path) const;
    void ApplyConfig(Renderer* renderer, const OceanSimulationConfig& config);
    OceanSimulationConfig GetConfigCopy() const;
    const std::wstring& GetConfigPath() const { return configPath_; }
    OceanSimulationInputsProvider& GetInputsProvider() { return inputsProvider_; }
    const OceanSimulationInputsProvider& GetInputsProvider() const { return inputsProvider_; }

    void SetSceneVariables(Renderer* renderer, float localWindDirectionDegrees, float swellDirectionDegrees, float windForce01);
    const OceanSimulationSettings& GetSettings() const { return settings_; }
    OceanSimulationInputs EvaluateInputs() const;

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
    void SetShoreViewSnapMultiplier(float multiplier)
    {
        shoreViewSnapMultiplier_ = std::max(multiplier, 1.0f);
    }

    const FoamParams& GetFoamParams() const { return inputs_.foam; }

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
    void InitializeDefaultAssets();
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
        Microsoft::WRL::ComPtr<ID3D12Resource> displacement;
        Microsoft::WRL::ComPtr<ID3D12Resource> prevDisplacement;
        Microsoft::WRL::ComPtr<ID3D12Resource> foamTurbulence;
    };

    struct RetiredUploadResources
    {
        uint64_t retireFrame = 0;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> resources;
    };

    Microsoft::WRL::ComPtr<ID3D12Resource> h0Buffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> waveDataBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> displacement_;
    Microsoft::WRL::ComPtr<ID3D12Resource> prevDisplacement_;
    Microsoft::WRL::ComPtr<ID3D12Resource> foamTurbulence_;
    Microsoft::WRL::ComPtr<ID3D12Resource> shoreDepth_;
    SceneView shoreDepthView_{};
    UINT shoreDepthWidth_ = 0u;
    UINT shoreDepthHeight_ = 0u;
    float shoreDepthHalfExtent_ = 250.0f;
    float2 prevShoreDepthPos_ = {FLT_MAX, FLT_MAX};

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
    std::vector<RetiredUploadResources> retiredUploadResources_;
};

