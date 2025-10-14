#pragma once

#include "core/math/Math.h"

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
    SpotLight();
    explicit SpotLight(const SpotLightDesc& desc);

    void SetDesc(const SpotLightDesc& d);
    void SetPosition(const Math::float3& position);
    void SetDirection(const Math::float3& direction);
    void SetRange(float range);
    void SetAngles(float inner, float outer);
    void SetColor(const Math::float3& color);
    void SetIntensity(float intensity);
    void SetShadowNormalBias(float bias);
    void SetShadowDepthBias(float bias);
    void SetNearPlane(float nearPlane);

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

    DebugConeParams GetDebugConeParams() const;

    void UpdateCachedData();

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

