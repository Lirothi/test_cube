#pragma once

#include <array>

struct CascadeShadowConfig
{
    float maxDistance = 300.0f;
    std::array<float, 4> sliceDistances = { 10.0f, 35.0f, 100.0f, 300.0f };
    float normalBiasInTexels = 1.0f;
    float depthBiasInTexels = 1.5f;
    // S2: padding on the fitted sphere radius, absorbing the texel-snap shift. In CASCADE TEXELS,
    // not world units — the snap (std::floor in UpdateCascades) moves the centre by at most ONE
    // texel per axis, so a texel is the only unit in which one constant is right for every cascade.
    // 2 = one texel of slack over the worst case. As metres this was 2.0, i.e. ~15% of cascade 0's
    // radius thrown away to cover a 14 mm shift.
    float overlapInTexels = 2.0f;
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
