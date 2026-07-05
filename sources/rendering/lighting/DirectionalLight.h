#pragma once

#include <cstdint>

#include "core/math/Math.h"

class DirectionalLight
{
public:
    DirectionalLight();

    const Math::float3& GetDirection() const;
    void SetDirection(const Math::float3& direction);

    // Rung 1 (Step 11) foundation: monotonic version bumped when the sun direction changes (it
    // drives the CSM projection); a directional-shadow cache compares it. No consumer yet.
    std::uint32_t GetTransformVersion() const { return transformVersion_; }

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
    std::uint32_t transformVersion_ = 0; // Step 11: bumped on SetDirection
};

