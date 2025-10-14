#pragma once

#include <array>

struct CascadeShadowConfig
{
    float maxDistance = 300.0f;
    std::array<float, 4> sliceDistances = { 10.0f, 30.0f, 100.0f, 300.0f };
    float normalBiasInTexels = 0.75f;
    float depthBiasInTexels = 2.0f;
    float overlap = 2.0f;
    float forwardOffset = 1.0f;
    float stabilizationStepFraction = 0.1f;
    float zPadding = 25.0f;

    std::array<float, 5> BuildSplitScheme(float zNear, float zFar) const;
};
