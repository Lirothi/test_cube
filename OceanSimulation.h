#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

#include "Math.h"

class Renderer;
class Material;

class OceanSimulation
{
public:
    static constexpr UINT kResolution = 256;
    static constexpr UINT kCascadeCount = 4;
    static constexpr UINT kClipLevels = kCascadeCount;
    static constexpr UINT kArraySlices = kCascadeCount * 2;

    OceanSimulation();
    ~OceanSimulation() = default;

    void Initialize(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);

    void Update(Renderer* renderer, ID3D12GraphicsCommandList* cl, float timeSeconds);
    void OnHotReload(Renderer* renderer);

    float GetPatchLength() const { return basePatchLength_; }
    const Math::float4& GetLengthScales() const { return lengthScales_; }
    const Math::float4& GetInvLengthScales() const { return invLengthScales_; }
    float GetDisplacementAmplitude() const { return displacementAmplitude_; }

    D3D12_CPU_DESCRIPTOR_HANDLE GetDisplacementSRV() const { return displacementSrvs_.empty() ? D3D12_CPU_DESCRIPTOR_HANDLE{} : displacementSrvs_[0]; }
    ID3D12Resource* GetDisplacementResource() const { return displacement_.Get(); }

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

    static uint32_t FloatToBits(float value);

private:
    bool initialized_ = false;

    float basePatchLength_ = 200.0f;
    float windSpeed_ = 12.0f;
    Math::float2 windDir_ = Math::float2(1.0f, 0.0f);
    float spectrumScale_ = 3.0e-3f;
    float displacementAmplitude_ = 1.5f;
    float timeScale_ = 1.0f;

    Math::float4 lengthScales_ = Math::float4(0.0f, 0.0f, 0.0f, 0.0f);
    Math::float4 invLengthScales_ = Math::float4(0.0f, 0.0f, 0.0f, 0.0f);

    std::vector<Math::float4> h0Data_;
    std::vector<Math::float4> waveData_;

    Microsoft::WRL::ComPtr<ID3D12Resource> h0Buffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> waveDataBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> displacement_;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptorHeap_;
    UINT descriptorIncr_ = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE h0Srv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE waveDataSrv_{};
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> displacementSrvs_;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> displacementUavs_;

    UINT mipCount_ = 1u;

    std::shared_ptr<Material> spectrumMaterial_;
    std::shared_ptr<Material> fftMaterial_;
    std::shared_ptr<Material> fftPostMaterial_;
    std::shared_ptr<Material> mipMaterial_;
};

