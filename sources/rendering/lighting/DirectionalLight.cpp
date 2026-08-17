#include "rendering/lighting/DirectionalLight.h"

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

void DirectionalLight::MigrateLegacyExposure(float legacyExposure)
{
    // Fold the multiplier into the intensity. Every shader that lit with `sunColor * exposure` now
    // lights with `sunColor * sunIntensity` for an identical product, and the opaque fill term
    // (`ambient * lightRgb`) picks the factor up through the colour -- so `ambient_` is left alone
    // on purpose. Scaling it here too would square the factor on shaded surfaces.
    sunIntensity_ = legacyExposure;

    // Seed the fill colour with a daylight sky at the sun's own luminance. Seeding the SUN colour
    // instead (the first attempt) made unticking the tint a no-op, which is a switch that does
    // nothing -- and a control that does nothing is worse than one that does too much. Matching
    // luminance keeps it a pure hue change, so it cannot be misread as a brightness bug.
    ambientColor_ = DefaultSkyFillColor(GetEffectiveColor());

    // Retired. Left at 1.0 so a consumer that still multiplies by it is a no-op, not a regression.
    exposure_ = 1.0f;
}
