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

    Math::float3 up = std::abs(direction_.y) > 0.99f
        ? Math::float3(0.0f, 0.0f, 1.0f)
        : Math::float3(0.0f, 1.0f, 0.0f);
    view_ = Math::mat4::LookAtLH(desc_.position, desc_.position + direction_, up);

    const float fov = outer * 2.0f;
    const float aspect = 1.0f;
    const float nearPlane = std::max(desc_.nearPlane, 0.01f);
    const float farPlane = std::max(desc_.range, nearPlane + 0.1f);
    proj_ = Math::mat4::PerspectiveFovLH(fov, aspect, nearPlane, farPlane);

    coneBounds_ = AABB::Empty();
    coneObb_.Reset();
    const Math::float3 apex = desc_.position;
    coneBounds_.Expand(apex);

    const float range = std::max(desc_.range, 0.0f);
    if (range <= Math::EPS)
    {
        return;
    }

    const Math::float3 baseCenter = apex + direction_ * range;
    coneBounds_.Expand(baseCenter);
    Math::float3 obbCenter = apex + direction_ * (range * 0.5f);

    const float radius = range * std::tan(outerAngle_);
    Math::float3 upCandidate = (std::abs(direction_.y) > 0.99f)
        ? Math::float3(1.0f, 0.0f, 0.0f)
        : Math::float3(0.0f, 1.0f, 0.0f);
    Math::float3 right = direction_.Cross(upCandidate);
    if (right.Length() <= Math::EPS)
    {
        upCandidate = Math::float3(0.0f, 0.0f, 1.0f);
        right = direction_.Cross(upCandidate);
    }
    right = right.Normalized();
    up = right.Cross(direction_).Normalized();

    if (radius <= Math::EPS)
    {
        return;
    }

    Math::float3 axes[3] = { right, up, direction_ };
    coneObb_.Set(obbCenter, axes, Math::float3(radius, radius, range * 0.5f));
    if (!coneObb_.IsValid())
    {
        return;
    }

    const float xExtent = radius * std::sqrt(right.x * right.x + up.x * up.x);
    const float yExtent = radius * std::sqrt(right.y * right.y + up.y * up.y);
    const float zExtent = radius * std::sqrt(right.z * right.z + up.z * up.z);

    const Math::float3 minPt(
        std::min(apex.x, baseCenter.x - xExtent),
        std::min(apex.y, baseCenter.y - yExtent),
        std::min(apex.z, baseCenter.z - zExtent));
    const Math::float3 maxPt(
        std::max(apex.x, baseCenter.x + xExtent),
        std::max(apex.y, baseCenter.y + yExtent),
        std::max(apex.z, baseCenter.z + zExtent));

    coneBounds_.Expand(minPt);
    coneBounds_.Expand(maxPt);
}

bool SpotLight::PointInsideCone(const Math::float3& point) const
{
    const Math::float3 apex = desc_.position;
    const Math::float3 dir = direction_;
    const float range = std::max(desc_.range, 0.0f);

    Math::float3 v = point - apex;
    const float distAlong = v.Dot(dir);
    if (distAlong < 0.0f || distAlong > range)
    {
        return false;
    }

    const float lenSq = v.Dot(v);
    if (lenSq <= Math::EPS)
    {
        return true;
    }

    const float len = std::sqrt(lenSq);
    const float cosAngle = distAlong / len;
    return cosAngle >= cosOuter_;
}

bool SpotLight::SphereIntersectsCone(const Math::float3& center, float radius) const
{
    const Math::float3 apex = desc_.position;
    const Math::float3 dir = direction_;
    const float range = std::max(desc_.range, 0.0f);
    if (range <= Math::EPS)
    {
        return false;
    }

    if (radius <= 0.0f)
    {
        return PointInsideCone(center);
    }

    Math::float3 v = center - apex;
    const float axisDist = v.Dot(dir);
    if (axisDist + radius < 0.0f || axisDist - radius > range)
    {
        return false;
    }

    const float distSq = v.Dot(v);
    if (distSq <= Math::EPS)
    {
        return true;
    }

    const float dist = std::sqrt(distSq);
    if (axisDist >= 0.0f && axisDist <= range)
    {
        const float cosAngle = axisDist / dist;
        if (cosAngle >= cosOuter_)
        {
            return true;
        }
    }

    const float outerAngle = outerAngle_;
    const float tanOuter = std::tan(outerAngle);
    const float radialSq = std::max(0.0f, distSq - axisDist * axisDist);
    if (axisDist >= 0.0f && axisDist <= range)
    {
        const float coneRadiusAtAxis = axisDist * tanOuter + radius;
        if (radialSq <= coneRadiusAtAxis * coneRadiusAtAxis)
        {
            return true;
        }
    }

    if (axisDist < 0.0f && axisDist + radius >= 0.0f)
    {
        if (std::sqrt(radialSq) <= radius)
        {
            return true;
        }
    }

    const Math::float3 farCenter = apex + dir * range;
    const float farRadius = range * tanOuter;
    const float distToFar = (center - farCenter).Length();
    if (distToFar <= radius + farRadius)
    {
        return true;
    }

    return false;
}

bool SpotLight::AABBIntersectsCone(const AABB& bounds) const
{
    if (!bounds.IsValid())
    {
        return false;
    }

    const float range = std::max(desc_.range, 0.0f);
    if (range <= Math::EPS)
    {
        return false;
    }

    //if (coneBounds_.IsValid() && !bounds.Intersects(coneBounds_))
    //{
    //    return false;
    //}

    if (coneObb_.IsValid() && !coneObb_.Intersects(bounds))
    {
        return false;
    }

    //const Math::float3 apex = desc_.position;
    //const Math::float3 dir = direction_;

    //const Math::float3 minPt = bounds.GetMin();
    //const Math::float3 maxPt = bounds.GetMax();
    //if (apex.x >= minPt.x && apex.x <= maxPt.x &&
    //    apex.y >= minPt.y && apex.y <= maxPt.y &&
    //    apex.z >= minPt.z && apex.z <= maxPt.z)
    //{
    //    return true;
    //}

    //const Math::float3 coneEnd = apex + dir * range;
    //if (bounds.IntersectsSegment(apex, coneEnd))
    //{
    //    return true;
    //}

    //const Math::float3 center = bounds.GetCenter();
    //const float radius = bounds.GetRadius();
    //if (SphereIntersectsCone(center, radius))
    //{
    //    return true;
    //}

    return true;
}
