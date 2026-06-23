#include "ocean/OceanSimulationInputs.h"

#include <algorithm>
#include <cmath>

namespace
{
    inline float InverseLerp(float a, float b, float value)
    {
        if (std::abs(b - a) < Math::EPS)
        {
            return 0.0f;
        }
        return Math::Saturate((value - a) / (b - a));
    }
}

FoamParams FoamParams::GetDefault()
{
    return FoamParams();
}

FoamParams FoamParams::Lerp(const FoamParams& lhs, const FoamParams& rhs, float t)
{
    FoamParams result;
    result.decayRate = Math::Lerp(lhs.decayRate, rhs.decayRate, t);
    result.coverage = Math::Lerp(lhs.coverage, rhs.coverage, t);
    result.density = Math::Lerp(lhs.density, rhs.density, t);
    result.sharpness = Math::Lerp(lhs.sharpness, rhs.sharpness, t);
    result.persistence = Math::Lerp(lhs.persistence, rhs.persistence, t);
    result.trail = Math::Lerp(lhs.trail, rhs.trail, t);
    result.trailTextureStrength = Math::Lerp(lhs.trailTextureStrength, rhs.trailTextureStrength, t);
    result.trailTextureSize = Math::float2(
        Math::Lerp(lhs.trailTextureSize.x, rhs.trailTextureSize.x, t),
        Math::Lerp(lhs.trailTextureSize.y, rhs.trailTextureSize.y, t));
    result.underwater = Math::Lerp(lhs.underwater, rhs.underwater, t);
    result.cascadesWeights = Math::float4(
        Math::Lerp(lhs.cascadesWeights.x, rhs.cascadesWeights.x, t),
        Math::Lerp(lhs.cascadesWeights.y, rhs.cascadesWeights.y, t),
        Math::Lerp(lhs.cascadesWeights.z, rhs.cascadesWeights.z, t),
        Math::Lerp(lhs.cascadesWeights.w, rhs.cascadesWeights.w, t));
    return result;
}

SpectrumParams SpectrumParams::Lerp(const SpectrumParams& lhs, const SpectrumParams& rhs, float t)
{
    SpectrumParams result;
    result.energySpectrum = (t < 0.5f) ? lhs.energySpectrum : rhs.energySpectrum;
    result.windSpeed = Math::Lerp(lhs.windSpeed, rhs.windSpeed, t);
    result.fetch = Math::Lerp(lhs.fetch, rhs.fetch, t);
    result.peaking = Math::Lerp(lhs.peaking, rhs.peaking, t);
    result.scale = Math::Lerp(lhs.scale, rhs.scale, t);
    result.cutoffWavelength = Math::Lerp(lhs.cutoffWavelength, rhs.cutoffWavelength, t);
    result.alignment = Math::Lerp(lhs.alignment, rhs.alignment, t);
    result.extraAlignment = Math::Lerp(lhs.extraAlignment, rhs.extraAlignment, t);
    return result;
}

SpectrumParams SpectrumParams::GetDefaultLocal()
{
    SpectrumParams s;
    s.energySpectrum = EnergySpectrumModel::PM;
    s.windSpeed = 5.0f;
    s.fetch = 100.0f;
    s.peaking = 3.3f;
    s.scale = 1.0f;
    s.cutoffWavelength = 0.01f;
    s.alignment = 1.0f;
    s.extraAlignment = 0.0f;
    return s;
}

SpectrumParams SpectrumParams::GetDefaultSwell()
{
    SpectrumParams s;
    s.energySpectrum = EnergySpectrumModel::JONSWAP;
    s.windSpeed = 10.0f;
    s.fetch = 100.0f;
    s.peaking = 10.0f;
    s.scale = 0.0f;
    s.cutoffWavelength = 1.0f;
    s.alignment = 1.0f;
    s.extraAlignment = 0.8f;
    return s;
}

EqualizerPreset::EqualizerPreset() = default;

EqualizerPreset::EqualizerPreset(std::vector<Filter> scaleFilters, std::vector<Filter> chopFilters)
    : scaleFilters_(std::move(scaleFilters))
    , chopFilters_(std::move(chopFilters))
{
}

Math::float2 EqualizerPreset::Sample(float normalizedU) const
{
    const float u = Math::Saturate(normalizedU);
    const float x = Math::Lerp(kXMin, kXMax, u);
    const float scale = EvaluateFilters(scaleFilters_, x);
    const float chop = EvaluateFilters(chopFilters_, x);
    return Math::float2(scale, chop);
}

void EqualizerPreset::SetScaleFilters(std::vector<Filter> filters)
{
    scaleFilters_ = std::move(filters);
}

void EqualizerPreset::SetChopFilters(std::vector<Filter> filters)
{
    chopFilters_ = std::move(filters);
}

std::shared_ptr<EqualizerPreset> EqualizerPreset::CreateDefault()
{
    return std::make_shared<EqualizerPreset>();
}

float EqualizerPreset::EvaluateFilters(const std::vector<Filter>& filters, float x)
{
    if (filters.empty())
    {
        return 1.0f;
    }

    float value = 1.0f;
    for (const Filter& filter : filters)
    {
        const float width = std::max(kMinWidth, filter.width);
        float arg = 0.0f;
        switch (filter.type)
        {
        case FilterType::Bell:
            arg = (x - filter.center);
            value += filter.value * std::exp(-arg * arg / (width * width));
            break;
        case FilterType::Highshelf:
            arg = std::min(x - filter.center, 0.0f);
            value += filter.value * std::exp(-arg * arg / (width * width));
            break;
        case FilterType::Lowshelf:
            arg = std::max(x - filter.center, 0.0f);
            value += filter.value * std::exp(-arg * arg / (width * width));
            break;
        default:
            break;
        }
    }

    return std::max(0.0f, value);
}

SwellPreset::SwellPreset()
    : spectrum_(SpectrumParams::GetDefaultSwell())
    , referenceWaveHeight_(0.0f)
{
}

LocalWavesPreset::LocalWavesPreset()
    : spectrum_(SpectrumParams::GetDefaultLocal())
    , foam_(FoamParams::GetDefault())
    , equalizer_(EqualizerPreset::CreateDefault())
    , referenceWaveHeight_(1.0f)
    , chop_(1.0f)
    , windForce_(0.0f)
{
}

OceanSimulationInputsProvider::OceanSimulationInputsProvider()
    : swell_(std::make_shared<SwellPreset>())
    , localPreset_(std::make_shared<LocalWavesPreset>())
    , defaultEqualizer_(EqualizerPreset::CreateDefault())
{
    UpdateMaxWindForce();
}

void OceanSimulationInputsProvider::SetDisplayWindForce(float value)
{
    displayWindForce01_ = Math::Saturate(value);
}

void OceanSimulationInputsProvider::SetSwellPreset(const std::shared_ptr<SwellPreset>& swell)
{
    swell_ = swell ? swell : std::make_shared<SwellPreset>();
}

void OceanSimulationInputsProvider::SetLocalWavesPreset(const std::shared_ptr<LocalWavesPreset>& preset)
{
    localPreset_ = preset ? preset : std::make_shared<LocalWavesPreset>();
}

void OceanSimulationInputsProvider::SetLocalWavesArray(const std::vector<std::shared_ptr<LocalWavesPreset>>& presets)
{
    localPresets_ = presets;
    UpdateMaxWindForce();
}

void OceanSimulationInputsProvider::SetDefaultEqualizer(const std::shared_ptr<EqualizerPreset>& preset)
{
    defaultEqualizer_ = preset ? preset : EqualizerPreset::CreateDefault();
}

void OceanSimulationInputsProvider::PopulateInputs(OceanSimulationInputs& target) const
{
    PopulateInputs(target, displayWindForce01_);
}

void OceanSimulationInputsProvider::PopulateInputs(OceanSimulationInputs& target, float windForce01) const
{
    target.timeScale = timeScale_;
    target.depth = depth_;

    float referenceWaveHeight = 0.0f;
    if (swell_)
    {
        target.swell = swell_->GetSpectrum();
        referenceWaveHeight += swell_->GetReferenceWaveHeight();
    }
    else
    {
        target.swell = SpectrumParams::GetDefaultSwell();
    }

    if (mode_ == InputsProviderMode::Fixed || localPresets_.size() < 2)
    {
        target.foamTrailUpdateTime = 0.0f;
        if (localPreset_)
        {
            ApplyPreset(target, *localPreset_);
            referenceWaveHeight += localPreset_->GetReferenceWaveHeight();
        }
    }
    else
    {
        target.foamTrailUpdateTime = 1.0f;
        const LerpVars lerp = GetLerpVars(windForce01, maxWindForce_, localPresets_);
        if (lerp.start && lerp.end)
        {
            ApplyPreset(target, *lerp.start, *lerp.end, lerp.t);
            referenceWaveHeight += Math::Lerp(lerp.start->GetReferenceWaveHeight(),
                lerp.end->GetReferenceWaveHeight(), lerp.t);
        }
    }

    target.referenceWaveHeight = referenceWaveHeight;
}

OceanSimulationInputsProvider::LerpVars OceanSimulationInputsProvider::GetLerpVars(float windForce01,
    float maxWindForce,
    const std::vector<std::shared_ptr<LocalWavesPreset>>& presets)
{
    LerpVars result;
    if (presets.size() < 2)
    {
        return result;
    }

    const float windForce = Math::Saturate(windForce01) * maxWindForce;
    size_t index = 0;
    for (; index < presets.size(); ++index)
    {
        if (presets[index] && windForce < presets[index]->GetWindForce())
        {
            break;
        }
    }

    if (index == 0)
    {
        return result;
    }

    if (index >= presets.size())
    {
        index = presets.size() - 1;
    }

    const LocalWavesPreset* prev = presets[index - 1].get();
    const LocalWavesPreset* next = presets[index].get();
    if (!prev || !next)
    {
        return result;
    }

    result.start = prev;
    result.end = next;
    result.t = InverseLerp(prev->GetWindForce(), next->GetWindForce(), windForce);
    return result;
}

void OceanSimulationInputsProvider::ApplyPreset(OceanSimulationInputs& target, const LocalWavesPreset& preset) const
{
    target.chop = preset.GetChop();
    target.local = preset.GetSpectrum();
    target.foam = preset.GetFoam();
    target.equalizerLerpValue = 0.0f;
    target.equalizerRamp0 = GetSafeEqualizer(preset.GetEqualizer());
    target.equalizerRamp1 = GetSafeEqualizer(nullptr);
}

void OceanSimulationInputsProvider::ApplyPreset(OceanSimulationInputs& target,
    const LocalWavesPreset& start,
    const LocalWavesPreset& end,
    float t) const
{
    target.chop = Math::Lerp(start.GetChop(), end.GetChop(), t);
    target.local = SpectrumParams::Lerp(start.GetSpectrum(), end.GetSpectrum(), t);
    target.foam = FoamParams::Lerp(start.GetFoam(), end.GetFoam(), t);
    target.equalizerLerpValue = t;
    target.equalizerRamp0 = GetSafeEqualizer(start.GetEqualizer());
    target.equalizerRamp1 = GetSafeEqualizer(end.GetEqualizer());
}

std::shared_ptr<EqualizerPreset> OceanSimulationInputsProvider::GetSafeEqualizer(const std::shared_ptr<EqualizerPreset>& preset) const
{
    if (preset)
    {
        return preset;
    }
    return defaultEqualizer_ ? defaultEqualizer_ : EqualizerPreset::CreateDefault();
}

void OceanSimulationInputsProvider::UpdateMaxWindForce()
{
    maxWindForce_ = 0.0f;
    for (const auto& preset : localPresets_)
    {
        if (preset)
        {
            maxWindForce_ = std::max(maxWindForce_, preset->GetWindForce());
        }
    }
}

