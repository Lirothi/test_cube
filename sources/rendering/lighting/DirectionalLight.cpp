#include "rendering/lighting/DirectionalLight.h"

#include <algorithm>
#include <cmath>

DirectionalLight::DirectionalLight()
    : direction_(0.0f)
    , color_(0.0f)
    , exposure_(1.0f)
    , ambient_(0.05f)
{
}

const Math::float3& DirectionalLight::GetDirection() const
{
    return direction_;
}

void DirectionalLight::SetDirection(const Math::float3& direction)
{
    direction_ = direction;
    ++transformVersion_; // Step 11: the sun direction drives the CSM projection
}

const Math::float3& DirectionalLight::GetColor() const
{
    return color_;
}

void DirectionalLight::SetColor(const Math::float3& color)
{
    color_ = color;
}

float DirectionalLight::GetExposure() const
{
    return exposure_;
}

void DirectionalLight::SetExposure(float exposure)
{
    exposure_ = exposure;
}

float DirectionalLight::GetAmbient() const
{
    return ambient_;
}

void DirectionalLight::SetAmbient(float ambient)
{
    ambient_ = ambient;
}

Math::float3 DirectionalLight::GetTemperatureRgb() const
{
    if (!useSunTemperature_)
    {
        return Math::float3(1.0f, 1.0f, 1.0f);
    }

    // Planckian locus in CIE 1960 UCS, transcribed from UE's MaterialExpressionBlackBody. The fit is
    // quoted as valid over roughly 1000..15000 K; clamp rather than extrapolate, because outside it
    // the rational functions bend back on themselves and produce colours that are not on the locus
    // at all.
    const float t = std::clamp(sunTemperatureK_, 1000.0f, 15000.0f);
    const float t2 = t * t;

    const float u = (0.860117757f + 1.54118254e-4f * t + 1.28641212e-7f * t2) /
                    (1.0f + 8.42420235e-4f * t + 7.08145163e-7f * t2);
    const float v = (0.317398726f + 4.22806245e-5f * t + 4.20481691e-8f * t2) /
                    (1.0f - 2.89741816e-5f * t + 1.61456053e-7f * t2);

    const float denom = 2.0f * u - 8.0f * v + 4.0f;
    if (std::fabs(denom) < 1e-6f)
    {
        return Math::float3(1.0f, 1.0f, 1.0f);
    }
    const float x = 3.0f * u / denom;
    const float y = 2.0f * v / denom;
    if (y < 1e-6f)
    {
        return Math::float3(1.0f, 1.0f, 1.0f);
    }
    const float z = 1.0f - x - y;

    // Y = 1; the absolute level is discarded below anyway.
    const float X = x / y;
    const float Z = z / y;

    // XYZ -> linear sRGB (Rec. 709 primaries, D65 white).
    Math::float3 rgb(
         3.2404542f * X - 1.5371385f * 1.0f - 0.4985314f * Z,
        -0.9692660f * X + 1.8760108f * 1.0f + 0.0415560f * Z,
         0.0556434f * X - 0.2040259f * 1.0f + 1.0572252f * Z);
    // The transform leaves the sRGB gamut for saturated temperatures; clamping is what UE does too.
    rgb.x = std::max(0.0f, rgb.x);
    rgb.y = std::max(0.0f, rgb.y);
    rgb.z = std::max(0.0f, rgb.z);

    // Unit luminance: the dial must change hue only. Without this, 3000K would arrive roughly a
    // stop darker than 6500K purely from where the locus sits in the gamut, and every temperature
    // change would silently need an intensity change to compensate.
    const float luma = 0.2126f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z;
    if (luma < 1e-6f)
    {
        return Math::float3(1.0f, 1.0f, 1.0f);
    }
    return Math::float3(rgb.x / luma, rgb.y / luma, rgb.z / luma);
}

void DirectionalLight::MigrateLegacyExposure(float legacyExposure)
{
    // Fold the multiplier into the intensity. Every shader that lit with `sunColor * exposure` now
    // lights with `sunColor * sunIlluminanceLux` for an identical product, and the opaque fill term
    // (`ambient * lightRgb`) picks the factor up through the colour -- so `ambient_` is left alone
    // on purpose. Scaling it here too would square the factor on shaded surfaces.
    sunIlluminanceLux_ = legacyExposure;

    // Retired. Left at 1.0 so a consumer that still multiplies by it is a no-op, not a regression.
    exposure_ = 1.0f;
}

void DirectionalLight::MigrateLegacySunIntensity(float legacyIntensity)
{
    // Deliberately an assignment and nothing else. P16.2 changed what the number MEANS, not what it
    // IS: the engine's linear light unit was already lux (see GetSunIlluminanceLux), so a level that
    // authored `sunIntensity 2` had authored two lux, and rewriting it as anything else would move
    // its pixels for the sake of a prettier readout.
    sunIlluminanceLux_ = legacyIntensity;
}
