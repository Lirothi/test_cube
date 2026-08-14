#pragma once

#if WITH_EDITOR

#include <algorithm>
#include <cmath>

#include "core/math/Math.h"

namespace EditorLightDirection
{
    inline Math::float3 NormalizedRay(const Math::float3& direction,
        const Math::float3& fallback = Math::float3(-1.0f, -1.0f, -1.0f))
    {
        const Math::float3 normalized = direction.Normalized();
        if (normalized.Length() >= Math::EPS)
        {
            return normalized;
        }
        return fallback.Normalized();
    }

    inline void SourceAngles(const Math::float3& rayDirection,
        float& azimuthDegrees,
        float& elevationDegrees)
    {
        const Math::float3 ray = NormalizedRay(rayDirection);
        const Math::float3 sourceDirection(-ray.x, -ray.y, -ray.z);
        azimuthDegrees = std::atan2(sourceDirection.x, sourceDirection.z) *
            (180.0f / Math::PI);
        elevationDegrees = std::asin(std::clamp(sourceDirection.y, -1.0f, 1.0f)) *
            (180.0f / Math::PI);
    }

    inline Math::float3 RayFromSourceAngles(float azimuthDegrees, float elevationDegrees)
    {
        const float azimuth = azimuthDegrees * (Math::PI / 180.0f);
        const float elevation = elevationDegrees * (Math::PI / 180.0f);
        const float cosElevation = std::cos(elevation);
        const Math::float3 sourceDirection(
            std::sin(azimuth) * cosElevation,
            std::sin(elevation),
            std::cos(azimuth) * cosElevation);
        return Math::float3(
            -sourceDirection.x, -sourceDirection.y, -sourceDirection.z).Normalized();
    }
}

#endif // WITH_EDITOR
