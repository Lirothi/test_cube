#include "OceanSimulationSettings.h"

#include <algorithm>
#include <array>

OceanSimulationSettings::OceanSimulationSettings()
    : resolution_(ResolutionValue::Eight)
    , cascadesNumber_(CascadesNumberValue::Four)
    , anisoLevel_(6)
    , simulateFoam_(false)
    , updateSpectrum_(false)
    , readbackCascades_(ReadbackCascadesMode::None)
    , samplingIterations_(3)
    , domainsMode_(CascadeDomainsMode::Auto)
    , simulationScale_(200.0f)
    , allowOverlap_(false)
    , minWavesInCascade_(6.0f)
    , manualScales_(Math::float4(0.0f, 0.0f, 0.0f, 0.0f))
{
}

Math::float4 OceanSimulationSettings::ComputeLengthScales() const
{
    Math::float4 lengthScales(0.0f, 0.0f, 0.0f, 0.0f);
    float* data = &lengthScales.x;

    if (domainsMode_ == CascadeDomainsMode::Auto)
    {
        if (simulationScale_ <= Math::EPS || GetResolution() == 0)
        {
            return lengthScales;
        }

        data[0] = simulationScale_;
        const float multiplier = kSmallestWaveMultiplierAuto * kMinWavesInCascadeAuto / static_cast<float>(GetResolution());
        for (int i = 1; i < 4; ++i)
        {
            data[i] = data[i - 1] * multiplier;
        }
    }
    else
    {
        data[0] = manualScales_.x;
        data[1] = manualScales_.y;
        data[2] = manualScales_.z;
        data[3] = manualScales_.w;
    }

    return lengthScales;
}

void OceanSimulationSettings::CalculateCascadeDomains(Math::float4& cutoffsLow, Math::float4& cutoffsHigh) const
{
    Math::float4 lengthScales = ComputeLengthScales();

    if (domainsMode_ == CascadeDomainsMode::Auto)
    {
        CalculateCascadeDomainsManual(lengthScales, false, kMinWavesInCascadeAuto, cutoffsLow, cutoffsHigh);
    }
    else
    {
        CalculateCascadeDomainsManual(lengthScales, allowOverlap_, minWavesInCascade_, cutoffsLow, cutoffsHigh);
    }
}

void OceanSimulationSettings::CalculateCascadeDomainsManual(const Math::float4& lengthScales,
    bool allowOverlap,
    float minWavesInCascade,
    Math::float4& cutoffsLow,
    Math::float4& cutoffsHigh) const
{
    std::array<float, 4> lows{};
    std::array<float, 4> highs{};

    const float* scale = &lengthScales.x;
    const uint32_t resolution = GetResolution();
    const uint32_t cascadeCount = GetCascadeCount();

    for (uint32_t i = 0; i < 4; ++i)
    {
        const float length = scale[i];
        if (length <= Math::EPS)
        {
            lows[i] = 0.0f;
            highs[i] = 0.0f;
            continue;
        }

        lows[i] = 2.0f * Math::PI / length * minWavesInCascade;
        highs[i] = 2.0f * Math::PI * static_cast<float>(resolution) / length / kSmallestWaveMultiplierAuto;
    }

    if (cascadeCount > 0)
    {
        highs[cascadeCount - 1] *= kSmallestWaveMultiplierAuto * 0.5f;
    }

    for (uint32_t i = cascadeCount; i < 4; ++i)
    {
        lows[i] = 0.0f;
        highs[i] = 0.0f;
    }

    if (!allowOverlap)
    {
        lows[0] = std::max(lows[0], 0.0f);
        lows[1] = std::max(lows[1], highs[0]);
        lows[2] = std::max(lows[2], highs[1]);
        lows[3] = std::max(lows[3], highs[2]);
    }

    cutoffsLow = Math::float4(lows[0], lows[1], lows[2], lows[3]);
    cutoffsHigh = Math::float4(highs[0], highs[1], highs[2], highs[3]);
}
