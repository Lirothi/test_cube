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

    // --- S8 filtering (UE names in brackets) ---------------------------------------------------
    // [r.Shadow.CSMReceiverBias, default 0.9] Multiplier on the transition-zone width at grazing
    // incidence: width *= lerp(this, 1, NoL). Lower = a much wider ramp when the light rakes the
    // surface, which is where self-shadowing is worst. 1 = no receiver bias at all.
    float csmReceiverBias = 0.9f;
    // [ULightComponent::ShadowSharpen, default 0, UI range 0..1] Narrows the shadow transition.
    // UE maps the artist value to the shader as `x * 7 + 1`, so 0 is a genuine no-op and 1 is 8x.
    // Exposed in ARTIST units; the mapping happens on the CPU exactly as UE does it.
    float shadowFilterSharpen = 0.0f;
    // [ApplyPCFOverBlurCorrection = Square(shadow)] PCF over-blurs the penumbra; UE squares the
    // result unconditionally for filtered shadows. A toggle here only because it is worth being able
    // to A/B a term that changes the whole penumbra profile. UE's behaviour is ON.
    bool pcfOverBlurCorrection = true;

    // --- Split distribution ------------------------------------------------------------------
    // OFF (default): the four `sliceDistances` above are authored by hand.
    // ON: only `maxDistance` is authored; the three intermediate splits come from UE's exponential
    // distribution. `sliceDistances` is NEVER written by this path -- switching the toggle back
    // restores the hand-authored numbers untouched, which is the whole point of keeping them apart.
    bool useUeSplitDistribution = false;
    // UE's UDirectionalLightComponent::CascadeDistributionExponent (default 3, clamped .1 .. 10).
    // Note: UE substitutes 4 when precomputed lighting is invalid, i.e. a fully dynamic scene --
    // see GetEffectiveCascadeDistributionExponent. We expose the authored value and let it be tuned.
    float cascadeDistributionExponent = 3.0f;

    // UE's FDirectionalLightSceneProxy::ComputeAccumulatedScale, transcribed. Fraction of the
    // (near -> maxDistance) range at which split `index` sits, for `count` cascades.
    static float UeAccumulatedScale(float exponent, int index, int count);

    // The four cascade FAR distances UE's distribution would produce. Pure function of
    // (zNear, maxDistance, exponent) -- computed for display even when the toggle is off, so the
    // developer window can show both schemes side by side.
    std::array<float, 4> ComputeUeSplitDistances(float zNear) const;

    std::array<float, 5> BuildSplitScheme(float zNear, float zFar) const;
};
