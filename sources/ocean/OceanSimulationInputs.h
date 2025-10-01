#pragma once

#include <memory>
#include <vector>

#include "core/Math.h"

class EqualizerPreset;
class LocalWavesPreset;
class SwellPreset;

struct FoamParams
{
    float decayRate = 0.02f;
    float coverage = 0.0f;
    float density = 8.4f;
    float sharpness = 0.5f;
    float persistence = 0.5f;
    float trail = 0.0f;
    float trailTextureStrength = 0.5f;
    Math::float2 trailTextureSize = Math::float2(100.0f, 50.0f);
    float underwater = 0.0f;
    Math::float4 cascadesWeights = Math::float4(1.0f, 1.0f, 1.0f, 1.0f);

    static FoamParams GetDefault();
    static FoamParams Lerp(const FoamParams& lhs, const FoamParams& rhs, float t);
};

struct SpectrumParams
{
    enum class EnergySpectrumModel
    {
        PM,
        JONSWAP,
        TMA
    };

    EnergySpectrumModel energySpectrum = EnergySpectrumModel::PM;
    float windSpeed = 5.0f;
    float fetch = 100.0f;
    float peaking = 3.3f;
    float scale = 1.0f;
    float cutoffWavelength = 0.01f;
    float alignment = 1.0f;
    float extraAlignment = 0.0f;

    static SpectrumParams Lerp(const SpectrumParams& lhs, const SpectrumParams& rhs, float t);
    static SpectrumParams GetDefaultLocal();
    static SpectrumParams GetDefaultSwell();
};

class EqualizerPreset
{
public:
    enum class FilterType
    {
        Bell,
        Highshelf,
        Lowshelf
    };

    struct Filter
    {
        FilterType type = FilterType::Bell;
        float center = 0.0f;
        float value = 0.0f;
        float width = 0.5f;
    };

    static constexpr float kXMin = -1.5f;
    static constexpr float kXMax = 3.5f;
    static constexpr float kMinWidth = 0.1f;

    EqualizerPreset();
    EqualizerPreset(std::vector<Filter> scaleFilters, std::vector<Filter> chopFilters);

    Math::float2 Sample(float normalizedU) const;

    void SetScaleFilters(std::vector<Filter> filters);
    void SetChopFilters(std::vector<Filter> filters);

    static std::shared_ptr<EqualizerPreset> CreateDefault();

private:
    static float EvaluateFilters(const std::vector<Filter>& filters, float x);

private:
    std::vector<Filter> scaleFilters_;
    std::vector<Filter> chopFilters_;
};

class SwellPreset
{
public:
    SwellPreset();

    void SetSpectrum(const SpectrumParams& spectrum) { spectrum_ = spectrum; }
    void SetReferenceWaveHeight(float value) { referenceWaveHeight_ = value; }

    const SpectrumParams& GetSpectrum() const { return spectrum_; }
    float GetReferenceWaveHeight() const { return referenceWaveHeight_; }

private:
    SpectrumParams spectrum_;
    float referenceWaveHeight_ = 0.0f;
};

class LocalWavesPreset
{
public:
    LocalWavesPreset();

    void SetSpectrum(const SpectrumParams& spectrum) { spectrum_ = spectrum; }
    void SetReferenceWaveHeight(float value) { referenceWaveHeight_ = value; }
    void SetChop(float value) { chop_ = value; }
    void SetFoam(const FoamParams& foam) { foam_ = foam; }
    void SetEqualizer(const std::shared_ptr<EqualizerPreset>& equalizer) { equalizer_ = equalizer; }
    void SetWindForce(float value) { windForce_ = value; }

    float GetWindForce() const { return windForce_; }
    float GetReferenceWaveHeight() const { return referenceWaveHeight_; }
    const SpectrumParams& GetSpectrum() const { return spectrum_; }
    const FoamParams& GetFoam() const { return foam_; }
    float GetChop() const { return chop_; }
    std::shared_ptr<EqualizerPreset> GetEqualizer() const { return equalizer_; }

private:
    SpectrumParams spectrum_;
    FoamParams foam_;
    std::shared_ptr<EqualizerPreset> equalizer_;
    float referenceWaveHeight_ = 1.0f;
    float chop_ = 1.0f;
    float windForce_ = 0.0f;
};

struct OceanSimulationInputs
{
    float timeScale = 1.0f;
    float depth = 1000.0f;
    float chop = 1.0f;
    FoamParams foam = FoamParams::GetDefault();
    SpectrumParams local = SpectrumParams::GetDefaultLocal();
    SpectrumParams swell = SpectrumParams::GetDefaultSwell();
    std::shared_ptr<EqualizerPreset> equalizerRamp0 = EqualizerPreset::CreateDefault();
    std::shared_ptr<EqualizerPreset> equalizerRamp1 = EqualizerPreset::CreateDefault();
    float equalizerLerpValue = 0.0f;
    float foamTrailUpdateTime = 0.0f;
    float referenceWaveHeight = 0.0f;
};

class OceanSimulationInputsProvider
{
public:
    enum class InputsProviderMode
    {
        Fixed,
        Scale
    };

    OceanSimulationInputsProvider();

    void SetMode(InputsProviderMode mode) { mode_ = mode; }
    void SetTimeScale(float value) { timeScale_ = value; }
    void SetDepth(float value) { depth_ = value; }
    void SetDisplayWindForce(float value);
    void SetSwellPreset(const std::shared_ptr<SwellPreset>& swell);
    void SetLocalWavesPreset(const std::shared_ptr<LocalWavesPreset>& preset);
    void SetLocalWavesArray(const std::vector<std::shared_ptr<LocalWavesPreset>>& presets);
    void SetDefaultEqualizer(const std::shared_ptr<EqualizerPreset>& preset);

    void PopulateInputs(OceanSimulationInputs& target) const;
    void PopulateInputs(OceanSimulationInputs& target, float windForce01) const;

private:
    struct LerpVars
    {
        const LocalWavesPreset* start = nullptr;
        const LocalWavesPreset* end = nullptr;
        float t = 0.0f;
    };

    static LerpVars GetLerpVars(float windForce01, float maxWindForce,
        const std::vector<std::shared_ptr<LocalWavesPreset>>& presets);

    void ApplyPreset(OceanSimulationInputs& target, const LocalWavesPreset& preset) const;
    void ApplyPreset(OceanSimulationInputs& target,
        const LocalWavesPreset& start,
        const LocalWavesPreset& end,
        float t) const;
    std::shared_ptr<EqualizerPreset> GetSafeEqualizer(const std::shared_ptr<EqualizerPreset>& preset) const;

    void UpdateMaxWindForce();

private:
    InputsProviderMode mode_ = InputsProviderMode::Fixed;
    float timeScale_ = 1.0f;
    float depth_ = 1000.0f;
    float displayWindForce01_ = 0.0f;
    std::shared_ptr<SwellPreset> swell_;
    std::shared_ptr<LocalWavesPreset> localPreset_;
    std::vector<std::shared_ptr<LocalWavesPreset>> localPresets_;
    float maxWindForce_ = 0.0f;
    std::shared_ptr<EqualizerPreset> defaultEqualizer_;
};

