#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

#include "core/math/Math.h"
#include "ocean/OceanSimulationInputs.h"
#include "ocean/OceanSimulationSettings.h"

class Renderer;
class Material;

class OceanSimulation
{
public:
    static constexpr UINT kClipLevels = 5;

    OceanSimulation();
    ~OceanSimulation() = default;

    void Initialize(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);

    void Update(Renderer* renderer, ID3D12GraphicsCommandList* cl, float timeSeconds);
    void OnHotReload(Renderer* renderer);

    void SetSettings(const OceanSimulationSettings& settings);
    void SetInputsProvider(const OceanSimulationInputsProvider& provider);
    OceanSimulationInputsProvider& GetInputsProvider() { return inputsProvider_; }
    const OceanSimulationInputsProvider& GetInputsProvider() const { return inputsProvider_; }

    void SetSceneVariables(float localWindDirectionDegrees, float swellDirectionDegrees, float windForce01);
    const OceanSimulationSettings& GetSettings() const { return settings_; }

    UINT GetResolution() const { return resolution_; }
    UINT GetCascadeCount() const { return cascadeCount_; }

    float GetPatchLength() const { return basePatchLength_; }
    const Math::float4& GetLengthScales() const { return lengthScales_; }
    const Math::float4& GetInvLengthScales() const { return invLengthScales_; }
    float GetDisplacementAmplitude() const { return displacementAmplitude_; }
    float GetLocalWindDirectionRadians() const;
    Math::float2 GetLocalWindDirectionVector() const;

    D3D12_CPU_DESCRIPTOR_HANDLE GetDisplacementSRV() const { return displacementSrvs_.empty() ? D3D12_CPU_DESCRIPTOR_HANDLE{} : displacementSrvs_[0]; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetFoamTurbulenceSRV() const { return foamSrv_; }
    ID3D12Resource* GetDisplacementResource() const { return displacement_.Get(); }
    ID3D12Resource* GetFoamResource() const { return foamTurbulence_.Get(); }

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
    float ComputeCascadeContribution(float kLength, UINT cascade) const;
    void InitializeDefaultAssets();
    void ReleaseCpuData();

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

    std::shared_ptr<EqualizerPreset> defaultEqualizerPreset_;
    std::shared_ptr<SwellPreset> defaultSwellPreset_;
    std::shared_ptr<LocalWavesPreset> defaultLocalPreset_;
    std::vector<std::shared_ptr<LocalWavesPreset>> defaultLocalPresets_;

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

    Microsoft::WRL::ComPtr<ID3D12Resource> h0Buffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> waveDataBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> displacement_;
    Microsoft::WRL::ComPtr<ID3D12Resource> foamTurbulence_;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptorHeap_;
    UINT descriptorIncr_ = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE h0Srv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE waveDataSrv_{};
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> displacementSrvs_;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> displacementUavs_;
    D3D12_CPU_DESCRIPTOR_HANDLE foamSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE foamUav_{};

    UINT mipCount_ = 1u;

    std::shared_ptr<Material> spectrumMaterial_;
    std::shared_ptr<Material> fftMaterial_;
    std::shared_ptr<Material> fftPostMaterial_;
    std::shared_ptr<Material> mipMaterial_;
    std::shared_ptr<Material> foamSimMaterial_;
    std::shared_ptr<Material> foamInitMaterial_;

    float lastFoamSimTime_ = 0.0f;
    bool foamNeedsInit_ = true;
};

