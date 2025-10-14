#include "rendering/lighting/SpotLight.h"

#include <algorithm>
#include <cmath>
#include <cstring>

SpotLight::SpotLight()
{
    UpdateCachedData();
}

SpotLight::SpotLight(const SpotLightDesc& desc)
{
    SetDesc(desc);
}

void SpotLight::SetDesc(const SpotLightDesc& d)
{
    if (std::memcmp(&desc_, &d, sizeof(SpotLightDesc)) == 0)
    {
        UpdateCachedData();
        return;
    }
    desc_ = d;
    dirty_ = true;
    UpdateCachedData();
}

void SpotLight::SetPosition(const Math::float3& position)
{
    if (desc_.position.x == position.x && desc_.position.y == position.y && desc_.position.z == position.z)
    {
        return;
    }
    desc_.position = position;
    dirty_ = true;
    UpdateCachedData();
}

void SpotLight::SetDirection(const Math::float3& direction)
{
    if (desc_.direction.x == direction.x && desc_.direction.y == direction.y && desc_.direction.z == direction.z)
    {
        return;
    }
    desc_.direction = direction;
    dirty_ = true;
    UpdateCachedData();
}

void SpotLight::SetRange(float range)
{
    if (desc_.range == range)
    {
        return;
    }
    desc_.range = range;
    dirty_ = true;
    UpdateCachedData();
}

void SpotLight::SetAngles(float inner, float outer)
{
    if (desc_.innerAngle == inner && desc_.outerAngle == outer)
    {
        return;
    }
    desc_.innerAngle = inner;
    desc_.outerAngle = outer;
    dirty_ = true;
    UpdateCachedData();
}

void SpotLight::SetColor(const Math::float3& color)
{
    desc_.color = color;
}

void SpotLight::SetIntensity(float intensity)
{
    desc_.intensity = intensity;
}

void SpotLight::SetShadowNormalBias(float bias)
{
    desc_.shadowNormalBias = bias;
}

void SpotLight::SetShadowDepthBias(float bias)
{
    desc_.shadowDepthBias = bias;
}

void SpotLight::SetNearPlane(float nearPlane)
{
    if (desc_.nearPlane == nearPlane)
    {
        return;
    }
    desc_.nearPlane = nearPlane;
    dirty_ = true;
    UpdateCachedData();
}

SpotLight::DebugConeParams SpotLight::GetDebugConeParams() const
{
    DebugConeParams params{};
    params.apex = desc_.position;
    params.direction = direction_;
    params.height = std::max(desc_.range, 0.0f);
    if (params.height > 0.0f)
    {
        params.radius = params.height * std::tan(outerAngle_);
    }
    return params;
}

void SpotLight::UpdateCachedData()
{
    if (!dirty_)
    {
        return;
    }

    dirty_ = false;

    Math::float3 dir = desc_.direction;
    if (dir.Length() <= Math::EPS)
    {
        dir = Math::float3(0.0f, -1.0f, 0.0f);
    }
    direction_ = dir.Normalized();

    float inner = std::max(0.0f, desc_.innerAngle);
    float outer = std::max(inner + Math::EPS, desc_.outerAngle);
    constexpr float kMaxAngle = DirectX::XM_PIDIV2 - 1e-4f;
    inner = std::min(inner, kMaxAngle - Math::EPS);
    outer = std::clamp(outer, inner + Math::EPS, kMaxAngle);
    innerAngle_ = inner;
    outerAngle_ = outer;

    cosInner_ = std::cos(innerAngle_);
    cosOuter_ = std::cos(outerAngle_);
    const float denom = std::max(1e-4f, cosInner_ - cosOuter_);
    invAngleRange_ = 1.0f / denom;

    const Math::float3 up = std::abs(direction_.y) > 0.99f
        ? Math::float3(0.0f, 0.0f, 1.0f)
        : Math::float3(0.0f, 1.0f, 0.0f);
    view_ = Math::mat4::LookAtLH(desc_.position, desc_.position + direction_, up);

    const float fov = outer * 2.0f;
    const float aspect = 1.0f;
    const float nearPlane = std::max(desc_.nearPlane, 0.01f);
    const float farPlane = std::max(desc_.range, nearPlane + 0.1f);
    proj_ = Math::mat4::PerspectiveFovLH(fov, aspect, nearPlane, farPlane);
}
