#pragma once

#include "core/math/Math.h"

class DirectionalLight
{
public:
    DirectionalLight();

    const Math::float3& GetDirection() const;
    void SetDirection(const Math::float3& direction);

    const Math::float3& GetColor() const;
    void SetColor(const Math::float3& color);

    float GetExposure() const;
    void SetExposure(float exposure);

    float GetAmbient() const;
    void SetAmbient(float ambient);

private:
    Math::float3 direction_;
    Math::float3 color_;
    float exposure_;
    float ambient_;
};

