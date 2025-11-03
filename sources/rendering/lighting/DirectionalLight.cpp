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

