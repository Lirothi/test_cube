#pragma once

#include "ocean/OceanSimulationInputs.h"

namespace OceanSpectrum
{
    float Frequency(float k, float depth);
    float FrequencyDerivative(float k, float depth);
    float FullSpectrum(float omega, float theta, const SpectrumParams& params, float depth);
    float ShortWavesFade(float kLength, float fadeLength);
}

