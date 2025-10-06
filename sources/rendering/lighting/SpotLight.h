#pragma once

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
    void SetDesc(const SpotLightDesc& d) { desc_ = d; }
    const SpotLightDesc& GetDesc() const { return desc_; }

private:
    SpotLightDesc desc_{};
};

