#pragma once

#include <array>

struct CascadeShadowConfig
{
    float maxDistance = 300.0f;
    std::array<float, 4> sliceDistances = { 15.0f, 40.0f, 100.0f, 300.0f };
    float normalBiasInTexels = 0.75f;
    float depthBiasInTexels = 2.0f;
    float overlap = 2.0f;
    float zPadding = 25.0f;
    // Step 2b: how far (world units) a caster may extend TOWARD the light beyond a slice.
    // The light ortho near plane is pulled back by this so such casters still render and
    // cast, instead of being clipped. Bounded well under maxDistance (do NOT set near the
    // light eye); acne-safe because the world-space depth bias is range-independent. The
    // fully robust alternative is depth-clamp pancaking (DepthClipEnable=FALSE on the
    // shadow PSO) — deferred (material/PSO change).
    float casterReachWS = 150.0f;

    std::array<float, 5> BuildSplitScheme(float zNear, float zFar) const;
};
