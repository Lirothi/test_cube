#include "rendering/lighting/SpotLight.h"
#include "rendering/shadows/ShadowSettings.h" // plan S4: render::SpotShadowNear

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
    SyncToRender(); // a new light is not the render half's yet: it starts with its cache built
}

// Game thread plan G3: the setters write the GAME's copy only; the cached cone, matrices and version are the render
// half's and are rebuilt from the copy SyncToRender takes.
void SpotLight::SetDesc(const SpotLightDesc& d)
{
    TC_CHECK_GAME("SpotLight::SetDesc");
    std::memcpy(&gameDesc_, &d, sizeof(SpotLightDesc)); // the padding too: SyncToRender compares bytes
}

void SpotLight::SetPosition(const Math::float3& position)
{
    gameDesc_.position = position;
}

void SpotLight::SetDirection(const Math::float3& direction)
{
    gameDesc_.direction = direction;
}

void SpotLight::SetRange(float range)
{
    gameDesc_.range = range;
}

void SpotLight::SetAngles(float inner, float outer)
{
    gameDesc_.innerAngle = inner;
    gameDesc_.outerAngle = outer;
}

void SpotLight::SetColor(const Math::float3& color)
{
    gameDesc_.color = color;
}

void SpotLight::SetIntensity(float intensity)
{
    gameDesc_.luminousFluxLm = intensity; // P16.5: the argument is LUMENS now
}

void SpotLight::SetShadowNormalBias(float bias)
{
    gameDesc_.shadowNormalBias = bias;
}

void SpotLight::SetShadowDepthBias(float bias)
{
    gameDesc_.shadowDepthBias = bias;
}

void SpotLight::SetNearPlane(float nearPlane)
{
    gameDesc_.nearPlane = nearPlane;
}

void SpotLight::SyncToRender()
{
    // A byte-for-byte copy, so an unchanged desc compares equal here every frame (the version, which the shadow caches
    // key on, moves only on a real change -- as each setter's own compare had it).
    if (std::memcmp(&desc_, &gameDesc_, sizeof(SpotLightDesc)) != 0)
    {
        std::memcpy(&desc_, &gameDesc_, sizeof(SpotLightDesc));
        dirty_ = true;
    }
    lampLevel_ = gameLampLevel_;
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
    // Plan S4: the automatic near follows the depth mode (render::SpotShadowNear), which a shadow-mode switch or
    // shadow.spotLinearDepth changes under an unchanged desc.
    if (!dirty_ && render::SpotShadowNear(desc_.nearPlane) == shadowNear_)
    {
        return;
    }

    dirty_ = false;
    ++transformVersion_; // Step 11: monotonic signal that this light's shadow transform changed

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
    const float nearPlane = render::SpotShadowNear(desc_.nearPlane);
    const float farPlane = std::max(desc_.range, nearPlane + 0.1f);
    proj_ = Math::mat4::PerspectiveFovLH(fov, aspect, nearPlane, farPlane);
    shadowNear_ = nearPlane;
    shadowFar_ = farPlane;

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

    if (coneBounds_.IsValid() && !bounds.Intersects(coneBounds_))
    {
        return false;
    }

    //if (coneObb_.IsValid() && !coneObb_.Intersects(bounds))
    //{
    //    return false;
    //}

    const Math::float3 apex = desc_.position;
    const Math::float3 dir = direction_;

    const Math::float3 minPt = bounds.GetMin();
    const Math::float3 maxPt = bounds.GetMax();
    if (apex.x >= minPt.x && apex.x <= maxPt.x &&
        apex.y >= minPt.y && apex.y <= maxPt.y &&
        apex.z >= minPt.z && apex.z <= maxPt.z)
    {
        return true;
    }

    const Math::float3 coneEnd = apex + dir * range;
    if (bounds.IntersectsSegment(apex, coneEnd))
    {
        return true;
    }

    const Math::float3 center = bounds.GetCenter();
    const float radius = bounds.GetRadius();
    if (SphereIntersectsCone(center, radius))
    {
        return true;
    }

    return false;
}
