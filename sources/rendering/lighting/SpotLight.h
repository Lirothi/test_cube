#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/Math.h"

struct SpotLightDesc {
    Math::float3 position = Math::float3(0.0f, 0.0f, 0.0f);
    float        range = 10.0f;
    Math::float3 direction = Math::float3(0.0f, -1.0f, 0.0f);
    float        innerAngle = DirectX::XMConvertToRadians(15.0f);
    Math::float3 color = Math::float3(1.0f, 1.0f, 1.0f);
    float        outerAngle = DirectX::XMConvertToRadians(25.0f);
    float        intensity = 5.0f;
    float        shadowNormalBias = 0.01f;
    float        shadowDepthBias = 0.001f;
    float        nearPlane = 0.1f;
};

class SpotLight {
public:
    SpotLight() { UpdateCachedData(); }
    explicit SpotLight(const SpotLightDesc& desc) { SetDesc(desc); }

    void SetDesc(const SpotLightDesc& d)
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

    void SetPosition(const Math::float3& position)
    {
        if (desc_.position.x == position.x && desc_.position.y == position.y && desc_.position.z == position.z)
        {
            return;
        }
        desc_.position = position;
        dirty_ = true;
        UpdateCachedData();
    }

    void SetDirection(const Math::float3& direction)
    {
        if (desc_.direction.x == direction.x && desc_.direction.y == direction.y && desc_.direction.z == direction.z)
        {
            return;
        }
        desc_.direction = direction;
        dirty_ = true;
        UpdateCachedData();
    }

    void SetRange(float range)
    {
        if (desc_.range == range)
        {
            return;
        }
        desc_.range = range;
        dirty_ = true;
        UpdateCachedData();
    }

    void SetAngles(float inner, float outer)
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

    void SetColor(const Math::float3& color)
    {
        desc_.color = color;
    }

    void SetIntensity(float intensity)
    {
        desc_.intensity = intensity;
    }

    void SetShadowNormalBias(float bias)
    {
        desc_.shadowNormalBias = bias;
    }

    void SetShadowDepthBias(float bias)
    {
        desc_.shadowDepthBias = bias;
    }

    void SetNearPlane(float nearPlane)
    {
        if (desc_.nearPlane == nearPlane)
        {
            return;
        }
        desc_.nearPlane = nearPlane;
        dirty_ = true;
        UpdateCachedData();
    }

    const SpotLightDesc& GetDesc() const { return desc_; }

    const Math::float3& GetDirection() const { return direction_; }
    float GetInnerAngle() const { return innerAngle_; }
    float GetOuterAngle() const { return outerAngle_; }
    float GetCosInner() const { return cosInner_; }
    float GetCosOuter() const { return cosOuter_; }
    float GetInvAngleRange() const { return invAngleRange_; }
    float GetShadowNormalBias() const { return desc_.shadowNormalBias; }
    float GetShadowDepthBias() const { return desc_.shadowDepthBias; }

    const Math::mat4& GetViewMatrix() const { return view_; }
    const Math::mat4& GetProjMatrix() const { return proj_; }
    Math::mat4 GetViewProjMatrix() const { return view_ * proj_; }

    struct DebugConeParams
    {
        Math::float3 apex;
        Math::float3 direction;
        float        height = 0.0f;
        float        radius = 0.0f;
    };

    DebugConeParams GetDebugConeParams() const
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

    void UpdateCachedData()
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

private:
    SpotLightDesc desc_{};
    Math::float3 direction_ = Math::float3(0.0f, -1.0f, 0.0f);
    float        innerAngle_ = 0.0f;
    float        outerAngle_ = 0.0f;
    float        cosInner_ = 0.0f;
    float        cosOuter_ = 0.0f;
    float        invAngleRange_ = 0.0f;
    Math::mat4   view_ = Math::mat4::Identity();
    Math::mat4   proj_ = Math::mat4::Identity();
    bool         dirty_ = true;
};

