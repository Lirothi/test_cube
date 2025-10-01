#include "ocean/OceanSpectrum.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr float kPi = Math::PI;
    constexpr float kGravity = 9.81f;
    constexpr float kSigmaOverRho = 0.074e-3f;

    float DonelanBannerBeta(float x)
    {
        const float ax = std::abs(x);
        if (ax < 0.95f)
        {
            return 2.61f * std::pow(ax, 1.3f);
        }
        if (ax < 1.6f)
        {
            return 2.28f * std::pow(ax, -1.3f);
        }
        const float p = -0.4f + 0.8393f * std::exp(-0.567f * std::log(ax * ax));
        return std::pow(10.0f, p);
    }

    float DonelanBanner(float theta, float omega, float peakOmega)
    {
        const float beta = DonelanBannerBeta(omega / peakOmega);
        const float sech = 1.0f / std::cosh(beta * theta);
        return beta / 2.0f / std::tanh(beta * kPi) * sech * sech;
    }

    float NormalisationFactor(float s)
    {
        const float s2 = s * s;
        const float s3 = s2 * s;
        const float s4 = s3 * s;
        if (s < 5.0f)
        {
            return -0.000564f * s4 + 0.00776f * s3 - 0.044f * s2 + 0.192f * s + 0.163f;
        }
        return -4.80e-08f * s4 + 1.07e-05f * s3 - 9.53e-04f * s2 + 5.90e-02f * s + 3.93e-01f;
    }

    float SpreadPowerHasselman(float omega, float peakOmega, float u)
    {
        const float ratio = std::abs(omega / peakOmega);
        if (omega > peakOmega)
        {
            return 9.77f * std::pow(ratio, -2.33f - 1.45f * (u * peakOmega / kGravity - 1.17f));
        }
        return 6.97f * std::pow(ratio, 4.06f);
    }

    float Cosine2s(float theta, float s)
    {
        return NormalisationFactor(s) * std::pow(std::abs(std::cos(0.5f * theta)), 2.0f * s);
    }

    float DirectionSpectrum(float theta, float omega, float peakOmega, const SpectrumParams& pars)
    {
        const float spreadPower = SpreadPowerHasselman(omega, peakOmega, pars.windSpeed)
            + Math::Lerp(16.0f * std::tanh(std::min(omega / peakOmega / 10.0f, 20.0f)), 25.0f,
                pars.extraAlignment) * pars.extraAlignment * pars.extraAlignment;
        const float spread = Cosine2s(theta, spreadPower);
        return Math::Lerp(0.5f / kPi, spread, pars.alignment);
    }

    float PiersonMoskowitzPeakOmega(float u)
    {
        const float nu = 0.13f;
        return 2.0f * kPi * nu * kGravity / std::max(u, Math::EPS);
    }

    float PiersonMoskowitz(float omega, float peakOmega)
    {
        const float invOmega = 1.0f / std::max(omega, Math::EPS);
        const float peakOverOmega = peakOmega / std::max(omega, Math::EPS);
        return 8.1e-3f * kGravity * kGravity * invOmega * invOmega * invOmega * invOmega * invOmega
            * std::exp(-1.25f * std::pow(peakOverOmega, 4.0f));
    }

    float JonswapAlpha(float chi)
    {
        return 0.076f * std::pow(chi, -0.22f);
    }

    float JonswapPeakOmega(float chi, float u)
    {
        const float nu = 3.5f * std::pow(chi, -0.33f);
        return 2.0f * kPi * nu * kGravity / std::max(u, Math::EPS);
    }

    float JONSWAP(float omega, float peakOmega, float chi, float gamma)
    {
        float sigma = (omega <= peakOmega) ? 0.07f : 0.09f;
        const float exponent = -(omega - peakOmega) * (omega - peakOmega)
            / (2.0f * sigma * sigma * peakOmega * peakOmega);
        const float r = std::exp(exponent);
        const float invOmega = 1.0f / std::max(omega, Math::EPS);
        const float peakOverOmega = peakOmega / std::max(omega, Math::EPS);
        const float gammaClamped = std::max(std::abs(gamma), Math::EPS);
        return JonswapAlpha(chi) * kGravity * kGravity
            * std::pow(invOmega, 5.0f)
            * std::exp(-1.25f * std::pow(peakOverOmega, 4.0f))
            * std::pow(gammaClamped, r) * 3.3f / gammaClamped;
    }

    float TMACorrection(float omega, float depth)
    {
        const float omegaH = omega * std::sqrt(depth / kGravity);
        if (omegaH <= 1.0f)
        {
            return 0.5f * omegaH * omegaH;
        }
        if (omegaH < 2.0f)
        {
            const float diff = 2.0f - omegaH;
            return 1.0f - 0.5f * diff * diff;
        }
        return 1.0f;
    }
}

namespace OceanSpectrum
{
    float Frequency(float k, float depth)
    {
        const float kh = std::min(k * depth, 10.0f);
        const float tanhKh = std::tanh(kh);
        return std::sqrt(std::max((kGravity * k + kSigmaOverRho * k * k * k) * tanhKh, 0.0f));
    }

    float FrequencyDerivative(float k, float depth)
    {
        const float kh = std::min(k * depth, 10.0f);
        const float tanhKh = std::tanh(kh);
        const float coshKh = std::cosh(kh);
        const float freq = Frequency(k, depth);
        if (freq < Math::EPS)
        {
            return 0.0f;
        }

        const float term1 = depth * (kSigmaOverRho * k * k * k + kGravity * k)
            / (coshKh * coshKh);
        const float term2 = (kGravity + 3.0f * kSigmaOverRho * k * k) * tanhKh;
        return 0.5f * (term1 + term2) / freq;
    }

    float FullSpectrum(float omega, float theta, const SpectrumParams& params, float depth)
    {
        if (params.windSpeed < Math::EPS)
        {
            return 0.0f;
        }

        const float chiNumerator = kGravity * params.fetch * 1000.0f;
        float chi = std::abs(chiNumerator / std::max(params.windSpeed * params.windSpeed, Math::EPS));
        chi = std::min(1.0e4f, chi);

        float peakOmega = 1.0f;
        float energySpectrum = 1.0f;
        switch (params.energySpectrum)
        {
        case SpectrumParams::EnergySpectrumModel::PM:
            peakOmega = PiersonMoskowitzPeakOmega(params.windSpeed);
            energySpectrum = PiersonMoskowitz(omega, peakOmega);
            break;
        case SpectrumParams::EnergySpectrumModel::JONSWAP:
            peakOmega = JonswapPeakOmega(chi, params.windSpeed);
            energySpectrum = JONSWAP(omega, peakOmega, chi, params.peaking);
            break;
        case SpectrumParams::EnergySpectrumModel::TMA:
            peakOmega = JonswapPeakOmega(chi, params.windSpeed);
            energySpectrum = JONSWAP(omega, peakOmega, chi, params.peaking) * TMACorrection(omega, depth);
            break;
        default:
            break;
        }

        const float spread = DirectionSpectrum(theta, omega, peakOmega, params);
        return energySpectrum * spread;
    }

    float ShortWavesFade(float kLength, float fadeLength)
    {
        if (fadeLength <= Math::EPS)
        {
            return 1.0f;
        }
        return std::exp(-fadeLength * fadeLength * kLength * kLength);
    }
}

