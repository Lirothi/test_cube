#pragma once

#include <array>
#include <memory>
#include <vector>
#include <wrl.h>

#include <d3d12.h>

#include "GpuTexture2D.h"

class Renderer;

struct OceanDisplaySpectrumSettings {
    float scale = 1.0f;
    float windSpeed = 0.0f;
    float windDirection = 0.0f; // degrees
    float fetch = 0.0f;
    float spreadBlend = 1.0f;
    float swell = 0.2f;
    float peakEnhancement = 3.3f;
    float shortWavesFade = 0.01f;
};

struct OceanWavesSettings {
    float gravity = 9.81f;
    float depth = 500.0f;
    float lambda = 1.0f;
    OceanDisplaySpectrumSettings local;
    OceanDisplaySpectrumSettings swell;
};

struct OceanCascadeConfig {
    float lengthScale = 250.0f;
    float cutoffLow = 0.0f;
    float cutoffHigh = 1.0f;
};

class OceanFFTSystem {
public:
    OceanFFTSystem();
    ~OceanFFTSystem();

    bool Initialize(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);

    void Tick(float deltaTime);
    void Dispatch(Renderer* renderer, ID3D12GraphicsCommandList* cl);

    void Reset();

    const GpuTexture2D& GetDisplacement(size_t cascadeIndex) const;
    const GpuTexture2D& GetDerivatives(size_t cascadeIndex) const;
    const GpuTexture2D& GetTurbulence(size_t cascadeIndex) const;

private:
    struct SpectrumBuffer;
    class FastFourierTransform;
    class Cascade;

    OceanWavesSettings settings_{};
    UINT size_ = 256;
    float time_ = 0.0f;
    float deltaTime_ = 0.0f;

    std::unique_ptr<FastFourierTransform> fft_;
    std::unique_ptr<SpectrumBuffer> spectrumBuffer_;
    GpuTexture2D gaussianNoise_;

    std::array<std::unique_ptr<Cascade>, 3> cascades_{};
};

