#pragma once

#include <array>
#include <cstdint>

struct CascadeShadowConfig
{
    float maxDistance = 300.0f;
    std::array<float, 4> sliceDistances = { 10.0f, 35.0f, 100.0f, 300.0f };
    // [r.Shadow.CSMDepthBias = 10] UE's value, CONVERTED, not copied. Their bias is
    //     DepthBias = CSMDepthBias / zRange * WorldSpaceTexelScale,  WorldSpaceTexelScale = radius / res
    // and our texel unit is the FULL texel, unitsPerTexel = 2*radius / tileRes -- exactly twice
    // theirs. So their 10 is our 5, and everything we ran before (2.0, then 1.5, then 1.15) was
    // under a QUARTER of Unreal's shadow-depth budget. That shortfall is the whole reason this
    // engine needed a receiver normal offset that UE has no equivalent for; with the budget right,
    // the normal offset is gone.
    // The same number also sets the penumbra width: UE derive TransitionSize from this very
    // expression (ComputeTransitionSize), and `transitionScale = 1/depthBiasNDC` in the shader is
    // that reciprocal -- so ramp and bias now agree with UE simultaneously, by construction.
    // 1.0 + normalBias 0.5 is a MEASURED optimum, not a transcription, and it beats UE's own
    // defaults on both halves of the goal. Grid on wind_test (floor: acne 0.007 %, lift +-0.004):
    //     depth 5.0, no normal (= UE)   acne 0.0387 %   peter-panning +3.130
    //     depth 1.5, normal 1.0         acne 0.0000 %                 +1.026
    //     depth 1.0, normal 0.5         acne 0.0000 %                 +0.238   <-- here
    //     depth 0.5, normal 1.0         acne 0.0121 %                 -0.979
    //     depth 0.5, normal 0.5         acne 5.02   %                 -3.524
    // i.e. zero acne at THIRTEEN TIMES less peter-panning than Unreal ships. The two knobs are
    // not interchangeable: depth bias moves the stored depth (peter-panning), the normal offset
    // moves the sample point (nearly free), so the cheap direction is to spend on the second and
    // keep the first only as high as the acne cliff demands.
    // [r.Shadow.CSMDepthBias = 10] would be 5.0 here -- their bias scales by radius/resolution and
    // our texel is 2*radius/resolution, exactly twice theirs. They ship 10 to cover every scene
    // blind; we measured this one.
    float depthBiasInTexels = 1.0f;
    // Receiver offset along its own normal, in cascade texels. UE's legacy CSM has NO equivalent
    // -- and their defaults visibly acne and peter-pan, so this is one place not to copy them.
    // It attacks acne from the RECEIVER side: sliding the sample point out of the surface costs
    // no depth push at all, so it buys acne protection without the peter-panning that raising
    // `depthBiasInTexels` would. Rides `cascadeTexelWS`, so it scales with each cascade.
    float normalBiasInTexels = 0.5f;
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
    // Filter kernel:
    //   0 = the original 3x3 hardware-PCF box with its per-cascade radius shrink (A/B arm, fallback)
    //   1 = soft-occlusion ramp + 4x4 tent / 4 gathers   (UE Manual3x3PCF, their SHADOW_QUALITY 3)
    //   2 = soft-occlusion ramp + 6x6 tent / 9 gathers   (UE Manual5x5PCF, their SHADOW_QUALITY 4-5)
    // `r.ShadowQuality` defaults to 5 in Unreal and `ManualPCF` then selects Manual5x5PCF, so a
    // stock UE runs the 6x6 kernel -- which is why its CSM reads softer than a 4x4 at the same
    // resolution. Was a process global in InstanceTypes.h; it is a per-scene QUALITY setting tuned
    // together with the three knobs below, and being apart from them was pure accretion.
    std::uint32_t filterMode = 1;
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

    // --- S6: depth bias moved to the depth PASS (UE names in brackets) -------------------------
    // The bias lives in the depth PASS, as UE's does, and the sampler compares the raw depth. There
    // is no switch back: the slope term needs the CASTER's normal, which the lighting pass does not
    // have, and at UE's budget (see depthBiasInTexels) a sampler-side bias would be over-biased by
    // construction -- a toggle here would be a control that lies. Git has the other arrangement.
    // [r.Shadow.CSMSlopeScaleDepthBias = 3] Multiplies the constant NDC bias: UE's
    // ShaderSlopeDepthBias = DepthBias * SlopeScaleDepthBias. 0 = constant bias only.
    float slopeScale = 2.0f;
    // [r.Shadow.ShadowMaxSlopeScaleDepthBias = 1] Clamp on tan(angle between surface and light).
    // Mandatory: as NoL -> 0 the required bias goes to infinity. NOTE 1.0, not 3.0 -- an earlier
    // revision of the plan had 3.0, i.e. three times UE's clamp.
    float maxSlope = 1.0f;
    // S7 -- PANCAKING [UE: FShadowDepthPassUniformParameters::bClampToNearPlane].
    // A caster in front of the cascade's projection near plane is pressed ONTO it instead of being
    // clipped. That is what lets the near plane be fitted tight (`nearProjLS = minZ` in
    // Scene::UpdateCascades) instead of pulled back by casterReachWS -- cascade 0's D16 depth range
    // drops from ~200 m to a few tens, i.e. the quantisation step from millimetres to fractions.
    // Casters are kept by the SEPARATE, wide culling near plane; see the block comment there.
    // Side effect, the same one UE document: a triangle with some vertices clamped and some not is
    // deformed. Only bites on casters straddling the projection near plane.
    bool pancakeCasters = true;
    // Metres of slack between the slice and the PROJECTION near plane. 0 = fitted tight (all the D16
    // precision, and every caster in front of the slice gets pancaked). Raising it trades precision
    // back for a lower clamp rate, and at `slack == casterReachWS` the projection near equals the
    // culling near and the step degenerates to its pre-S7 baseline exactly -- which is what makes
    // this the A/B lever for pancaking inside ONE binary.
    // Why one might want it: a triangle with SOME vertices clamped and some not is deformed, so a
    // caster straddling the near plane can shadow itself wrongly across that triangle. Slack pushes
    // the plane out of the geometry instead.
    // 40 m, NOT 0, and that is a measured retreat from "maximum precision".
    //
    // At slack 0 the near plane is fitted tight to the slice, so a caster taller than the slice
    // STRADDLES it: some of a triangle's vertices are clamped and some are not, the triangle is
    // deformed, and since every cascade fits its own near plane the cascades disagree -- which shows
    // up as GAPS IN THE SHADOW at cascade boundaries. Measured on a 100-unit pole (wind_test, ocean
    // off): slack 0 loses 2.38 % of the shadow; slack 2 already removes 99 % of that, and 40 removes
    // it entirely. The price is only the depth range S7 bought: cascade 0 goes 35.38 m -> 75.38 m,
    // i.e. a D16 step of 0.540 mm -> 1.150 mm -- still 2.5x better than the 2.829 mm before S7, and
    // S7's precision was measured to buy no visible improvement anyway (see the plan's S7 section).
    //
    // CONTENT-DEPENDENT: 40 clears the tallest caster in this project's scenes. A taller one needs
    // more, and the symptom is unmistakable -- the shadow breaks at a cascade boundary.
    float pancakeSlackWS = 40.0f;

    // --- S11: view-cone scissor [r.Shadow.CSMScissorOptim, default false] ----------------------
    // The cascade tile is a square around the slice's bounding SPHERE, but the camera only sees a
    // pyramid inside it. UE scissor the depth pass to the projection of that pyramid (extended to
    // the tile border, so it is a cone, not a pyramid -- see Scene::ComputeCascadeScissor). Pure
    // rasterisation saving; the sampled result is identical for every receiver the camera can see.
    // OFF by default, as in UE, and that is a safeguard, not a formality: the rect is derived from
    // the CAMERA frustum, and glass.hlsl shades reflected/refracted receivers that may lie outside
    // it -- those would read the undrawn part of the tile as LIT (S5 clear = 1.0).
    bool scissorOptim = false;
    // OURS (UE pad nothing). Texels added on every side of the rect: the receiver normal offset
    // (0.5 texel) plus the 6x6 tent's half-width (3 texels) can reach past the cone's edge at the
    // screen border, and a tap that lands on a scissored-out texel reads LIT. Cheap insurance.
    float scissorPadTexels = 4.0f;

    // --- S10: cascade cross-fade + distance fade ------------------------------------------------
    // [UDirectionalLightComponent::CascadeTransitionFraction = 0.1, clamped to 0.3] Fraction of a
    // cascade's OWN SLICE LENGTH over which it cross-fades into the next one. Was a hardcoded
    // `kCsmBlendFraction` in csm_sample.hlsli measured off the ABSOLUTE split distance instead --
    // on c2 (35..100 m) that made the band 10 m where UE's is 6.5.
    float blendFraction = 0.1f;
    // The last cascade has no coarser neighbour, so UE move its fade plane INWARD by the same
    // extension and let the shadow fade to "lit" (DirectionalLightComponent.cpp:936-941). Without
    // it the shadows end in a hard terminator line at `maxDistance`. 0 restores that line.
    float distanceFadeFraction = 0.1f;

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
