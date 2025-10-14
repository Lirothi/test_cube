#pragma once

#include <cstdint>

#include "core/math/Math.h"

class OceanSimulationSettings
{
public:
    enum class ResolutionValue : uint32_t
    {
        Six = 64,
        Seven = 128,
        Eight = 256,
        Nine = 512
    };

    enum class CascadesNumberValue : uint32_t
    {
        Two = 2,
        Three = 3,
        Four = 4,
    };

    enum class ReadbackCascadesMode : uint32_t
    {
        None = 0,
        One = 1,
        Two = 2,
    };

    enum class CascadeDomainsMode
    {
        Auto,
        Manual
    };

    OceanSimulationSettings();

    uint32_t GetResolution() const { return static_cast<uint32_t>(resolution_); }
    uint32_t GetCascadeCount() const { return static_cast<uint32_t>(cascadesNumber_); }
    uint32_t GetAnisotropyLevel() const { return static_cast<uint32_t>(anisoLevel_); }
    bool ShouldUpdateSpectrum() const { return updateSpectrum_; }
    bool ShouldSimulateFoam() const { return simulateFoam_; }
    ReadbackCascadesMode GetReadbackMode() const { return readbackCascades_; }
    uint32_t GetSamplingIterations() const { return static_cast<uint32_t>(samplingIterations_); }
    CascadeDomainsMode GetDomainsMode() const { return domainsMode_; }
    float GetSimulationScale() const { return simulationScale_; }
    bool AllowOverlap() const { return allowOverlap_; }
    float GetMinWavesInCascade() const { return minWavesInCascade_; }
    Math::float4 GetManualLengthScales() const { return manualScales_; }

    void SetResolution(ResolutionValue value) { resolution_ = value; }
    void SetCascadeCount(CascadesNumberValue value) { cascadesNumber_ = value; }
    void SetAnisotropyLevel(uint32_t value) { anisoLevel_ = static_cast<int>(value); }
    void SetUpdateSpectrum(bool value) { updateSpectrum_ = value; }
    void SetSimulateFoam(bool value) { simulateFoam_ = value; }
    void SetReadbackMode(ReadbackCascadesMode value) { readbackCascades_ = value; }
    void SetSamplingIterations(uint32_t value) { samplingIterations_ = static_cast<int>(value); }
    void SetDomainsMode(CascadeDomainsMode value) { domainsMode_ = value; }
    void SetSimulationScale(float value) { simulationScale_ = value; }
    void SetAllowOverlap(bool value) { allowOverlap_ = value; }
    void SetMinWavesInCascade(float value) { minWavesInCascade_ = value; }
    void SetManualLengthScales(const Math::float4& value) { manualScales_ = value; }

    Math::float4 ComputeLengthScales() const;
    void CalculateCascadeDomains(Math::float4& cutoffsLow, Math::float4& cutoffsHigh) const;

private:
    void CalculateCascadeDomainsManual(const Math::float4& lengthScales,
        bool allowOverlap,
        float minWavesInCascade,
        Math::float4& cutoffsLow,
        Math::float4& cutoffsHigh) const;

private:
    static constexpr float kSmallestWaveMultiplierAuto = 4.0f;
    static constexpr float kMinWavesInCascadeAuto = 6.0f;

    ResolutionValue resolution_;
    CascadesNumberValue cascadesNumber_;
    int anisoLevel_;
    bool simulateFoam_;
    bool updateSpectrum_;
    ReadbackCascadesMode readbackCascades_;
    int samplingIterations_;
    CascadeDomainsMode domainsMode_;
    float simulationScale_;
    bool allowOverlap_;
    float minWavesInCascade_;
    Math::float4 manualScales_;
};

