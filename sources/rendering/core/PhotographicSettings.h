#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Photographic camera settings (docs/photographic_rendering_improvement_plan.md, section 6.1).
//
// This is the CAMERA, not a light. Sun intensity, sky intensity and camera exposure are three
// separate settings by decision 3 of that plan; the legacy `exposure` field on the directional
// light is a camera substitute that P4 is going to take apart. Nothing here touches a light.
//
// Ownership mirrors DirectionalLight: the Scene holds one instance, the level's "cameraExposure"
// section serialises it, and the renderer reads it when the metering passes land in P2. A level
// without the section keeps the default below, which is `enabled = false` -- i.e. exactly the
// pre-plan image.
namespace render
{

struct CameraExposureSettings
{
    // Master switch. While false the linear exposure multiplier is exactly 1.0 and no metering
    // work is scheduled, so the frame is bit-for-bit what it was before this struct existed.
    bool  enabled = false;
    // false = hold `manualEv100`; true = meter the scene and adapt towards it.
    bool  autoExposure = true;
    // Artistic offset applied on top of the metered result, in stops.
    // -0.15 pairs with the default Filmic curve + "Warm sand" grade (see ColorPipelineSettings).
    // Compensation belongs to the CURVE, not to the grade: Filmic runs about 0.4 EV darker than the
    // legacy fit, so a value tuned against one stacks with the other.
    float compensationEv = -0.15f;
    // Safety net for the adapted value, in EV100. NOTE the span below is 22 stops, which is wide
    // enough that it clamps essentially nothing -- it is "off", not "tuned". Narrowing it is how a
    // scene stops the camera from opening all the way up in a dark frame. It is deliberately NOT
    // the mechanism for keeping different lighting conditions apart; see decision 5a of the plan.
    float minEv100 = -6.0f;
    float maxEv100 = 16.0f;
    // Histogram percentiles used to derive the metered luminance. Clipping the tails is what keeps
    // a sun glint or a patch of sky from dragging the whole frame, without hard-coding what water
    // or sky look like.
    //
    // The high percentile is the GLINT knob, and 0.80 is measured, not guessed. A sun-glint field
    // covers 10-20% of the frame looking into the sun over water, so at 0.95 most of it still
    // counts as scene brightness and the camera crushes the shaded island: median 0.157 -> 0.114 on
    // the sun_glint view. At 0.80 the specular field is discarded and the median lands at 0.149,
    // i.e. the crush is gone. Narkowicz recommends discarding 2-20% of the brightest samples; this
    // sits at the top of that range because our content is water.
    // The low percentile is the SHADOW knob. 0.02 was badly wrong -- it let essentially all the
    // shade into the meter, so a shaded frame dragged the camera wide open (the palm grove went
    // from median 0.063 with the camera off to 0.343, its sub-2% shadows collapsing 22% -> 0.01%).
    //
    // 0.65 was then ALSO wrong, and instructively so: it was measured before the metering weight
    // mask existed, when the low percentile was the only tool available against a bright region
    // dragging the frame. Once the mask took that job, 0.65 was doing the work twice and the result
    // was too dark looking into the sun -- found by flying the scene, which two static captures had
    // not shown. Measured again WITH the mask on (strength 0.7, sky bias 0.6), median / sub-2%
    // shadows, sun-facing | grove:
    //   0.02 -> 0.309/0.02 | 0.331/0.02     0.30 -> 0.221/1.39 | 0.233/0.05
    //   0.15 -> 0.262/0.26 | 0.276/0.03     0.50 -> 0.174/2.62 | 0.203/0.07
    //                                       0.65 -> 0.134/3.36 | 0.178/0.33
    // The reference photograph is 0.196 / 3.42. 0.15 is the value the scene was flown at and
    // preferred; 0.30 lands nearest the reference on both views if a little more contrast is
    // wanted. Raise it toward 0.5 only if shaded interiors read as over-exposed AND the mask is
    // already doing its job -- the two knobs solve different problems and stacking them
    // over-corrects, which is exactly the mistake recorded above.
    float lowPercentile = 0.15f;
    float highPercentile = 0.80f;
    // Metering weight mask (centre-weighted metering). Every histogram sample is scaled by a
    // weight derived from its screen position, which is what a camera's centre-weighted meter does
    // and what UE implements with a mask texture. This is the principled answer to "the sun/sky
    // drags the whole frame": rather than discarding bright samples everywhere (the percentile
    // knobs), it de-weights the part of the frame that is not the subject.
    //
    // strength 0 = uniform (every sample counts equally, i.e. the mask is off).
    // strength 1 = full falloff between the two radii below.
    // Radii are fractions of the half-diagonal, so 1.0 is the frame corner.
    // A floor of 0.05 is applied in the shader (as UE does) so the edges still contribute a little
    // rather than dropping out entirely — a subject that fills the border should still be metered.
    // Defaults are the configuration that was actually measured, not rounded guesses. Sweeping
    // strength at sky bias 0.6 (median / sub-2% shadows, grove | sun-facing glint view):
    //   0.00 -> 0.1804/0.29 | 0.1093/4.03      0.70 -> 0.1783/0.33 | 0.1340/3.36
    //   0.35 -> 0.1796/0.30 | 0.1180/3.75      1.00 -> 0.1770/0.37 | 0.1571/2.93
    // The mask barely touches the grove -- correctly, because shade fills that whole frame and
    // there is no bright region to de-weight; that case is the low percentile's job. On the
    // sun-facing view it is worth +0.29 stops of median at 0.7, i.e. the shaded island stops being
    // crushed when you turn into the sun. Two different problems, two different knobs.
    float meterMaskStrength = 0.7f;
    float meterMaskInnerRadius = 0.35f;
    float meterMaskOuterRadius = 1.0f;
    // Extra de-weighting applied to the TOP of the frame, 0 = none. The sky is almost always the
    // brightest thing in an exterior shot and almost never the subject, so a purely radial mask
    // still lets it dominate when the camera tilts up. Scales the weight linearly from 1 at the
    // horizon-ish midline to (1 - this) at the top edge.
    float meterMaskSkyBias = 0.6f;

    // Adaptation rates in stops/second, separately controllable because the eye darkens much
    // faster than it brightens and a single rate always looks wrong in one direction.
    float speedUp = 3.0f;
    float speedDown = 1.0f;
    // Distance, in stops, at which adaptation switches from linear to exponential. Far from the
    // target it runs at a constant stops/second so a big transition is time-bounded; inside this
    // distance it eases in, so the last fraction of a stop does not arrive at full speed and then
    // stop dead. Matches UE's `r.EyeAdaptation.ExponentialTransitionDistance`, default 1.5.
    float adaptationStartDistance = 1.5f;
    // Weight multiplier for the darkest histogram bucket. 1 = counts normally. Lower it when a
    // scene has large regions of pure black (letterboxing, an unlit interior) that would otherwise
    // drag the meter. UE exposes the same knob as `r.EyeAdaptation.BlackHistogramBucketInfluence`.
    float blackBucketInfluence = 1.0f;
    // Used when autoExposure is false.
    //
    // NOTE the default is 0, not the 10 a photometric pipeline would use. **This renderer's HDR is
    // not in cd/m^2.** Scene-referred linear values here sit around 0.1-3 for a lit daylight
    // surface, not the thousands real luminance would give, so EV100 is relative to the engine's
    // arbitrary linear scale. Measured on wind_test: EV 0 lands just under the authored look and
    // auto-exposure settles near -0.3; the photometric default of 10 renders a black screen
    // (multiplier 1/(1.2*2^10) = 0.0008). Anything that later claims real-world units -- P4's
    // optional lux-backed sun UI in particular -- has to establish a scene-to-luminance scale
    // first, and this comment is the reason that is not free.
    float manualEv100 = 0.0f;
};

// Display transform (plan section 6.1 / step P3).
enum class ToneCurve : std::uint32_t
{
    // Narkowicz ACES fit + pow(1/2.2). What shipped before P3, and **the default again** — see the
    // note below. Selecting it is bit-identical to the pre-P3 image.
    LegacyAces = 0,
    // AgX + the real sRGB transfer function. Correct, and kept, but NOT the default.
    //
    // Measured on wind_test and rejected as the default for two reasons. (1) AgX's whole value is
    // graceful behaviour at clipping and out-of-gamut, and **we clip 0.000% of pixels** (P0
    // measurements) -- it is protecting us from a problem we do not have, and charging contrast for
    // it: p99/p02 spread 10.9 against the legacy fit's 18.1 on the same frame. Correcting the
    // exposure under-shoot makes it brighter but FLATTER still (7.7), because the content moves
    // into the gentlest part of a 16.5-stop sigmoid. (2) Unreal, which is the look being targeted,
    // does not use AgX anywhere -- it ships an ACES-derived filmic curve with Slope/Toe/Shoulder/
    // BlackClip/WhiteClip plus colour grading. The legacy fit here is an approximation of that same
    // ACES curve, which is why it reads closer to the target.
    //
    // Revisit when P2B and P3B have pushed the image to actually use the top of the range and we
    // start clipping (the reference photograph clips 0.125%); at that point AgX's highlight
    // behaviour stops being theoretical.
    AgX = 1,
    // P3C: Unreal's parameterised film curve, with their five artist controls. The tonal response
    // is faithful; the ACES glow module, red modifier and AP1 working space are NOT included,
    // because their headers are not in the reference drop (see film_curve.hlsli).
    Filmic = 2,
};

struct ColorPipelineSettings
{
    // Default set chosen by the user after flying the scene, and measured: on `overview` it lands
    // median 0.2016 against the reference photograph's 0.1964. The grade below is the "Warm sand"
    // preset -- Vivid with the midtones opened up so lit sand and foliage keep detail -- and it
    // needs the -0.15 EV compensation in CameraExposureSettings, because the gamma lift makes it
    // brighter than Vivid at the same setting.
    ToneCurve toneCurve = ToneCurve::Filmic;

    // AgX "look", an ASC-CDL style grade applied inside the log domain. (1, 1, 1) is neutral and
    // is the default, because the plan's P11 wants grading to happen after the renderer's
    // responsibilities are stable, not baked into the transform now. The reference "punchy" look
    // is roughly slope 1.0 / power 1.35 / saturation 1.4 if a starting point is wanted.
    float agxSlope = 1.0f;
    float agxPower = 1.0f;
    float agxSaturation = 1.0f;

    // P3C colour grade, applied in scene-referred linear BEFORE the tone curve -- the same place
    // Unreal applies it, baked into its ColorGradingLUT alongside the film curve.
    //
    // This is where the look actually comes from. Unreal's film curve on its own is not what makes
    // their images punchy; saturation and contrast here are. Every value below is neutral, so a
    // level that grades nothing renders exactly as if this did not exist.
    //
    // Scalars for now. Unreal additionally exposes each of these per-channel AND separately for
    // shadows / midtones / highlights; if the global set proves insufficient, that split is the
    // next step rather than pushing these harder.
    float gradeSaturation = 1.30f;
    float gradeContrast = 1.15f;
    float gradeGamma = 1.10f;
    float gradeGain = 1.0f;
    float gradeOffset = 0.0f;

    // P3C film curve controls, Unreal's own defaults. Only used by ToneCurve::Filmic. The curve is
    // solved so that 0.18 in gives 0.18 out no matter how these are set, which is what keeps it
    // from fighting the exposure solve -- that targets the same 0.18.
    float filmSlope = 0.88f;
    float filmToe = 0.55f;
    float filmShoulder = 0.26f;
    float filmBlackClip = 0.0f;
    float filmWhiteClip = 0.04f;
};

// ---- EV100 conventions (plan section 6.2), documented once, here ----
//
// EV100 is the exposure value referenced to ISO 100. The persistent, serialised and UI-facing
// quantity is always EV100; shaders only ever see the linear multiplier derived from it. Higher
// EV100 = a more closed camera = a darker image.

// S/K from the photometric derivation: S = 100 (ISO) and K = 12.5 (the reflected-light meter
// calibration constant used by Canon/Nikon/Sekonic).
inline constexpr float kEv100LuminanceScale = 8.0f; // = S / K

// The exposure target. 0.18 is the photographic middle grey and what every reference
// implementation aims at -- UE computes `TargetExposure = TargetAverageLuminance / 0.18` directly.
//
// This constant used to be implicit and WRONG. The multiplier below was `1/(1.2 * 2^EV)`, the
// saturation-based formulation; composed with Ev100FromLuminance it mapped the metered luminance to
// L/(9.6L) = 0.104, i.e. the whole renderer sat **0.79 stops under-exposed** against the convention
// every artist and every reference assumes. Measured: forcing +0.79 EV of compensation put the
// frame's mean and median essentially onto the reference image's. Keep this explicit and visible.
inline constexpr float kMiddleGrey = 0.18f;

// Scene luminance -> the EV100 that exposes it as middle grey. EV100 = log2(L * S / K).
inline float Ev100FromLuminance(float luminance)
{
    return std::log2(std::max(luminance, 1e-6f) * kEv100LuminanceScale);
}

// EV100 -> the linear value scene colour is multiplied by before the tone curve.
// Inverting the above gives L = 2^EV100 / (S/K); we want L * m = kMiddleGrey, hence
// m = kMiddleGrey * (S/K) / 2^EV100. Mirrored in tonemap_cs.hlsl -- the two must not drift.
inline float ExposureMultiplierFromEv100(float ev100)
{
    return (kMiddleGrey * kEv100LuminanceScale) / std::exp2(ev100);
}

// The multiplier a disabled camera must produce. Kept as a named constant so the "dormant means
// exactly 1.0" contract is greppable rather than a magic literal in three places.
inline constexpr float kIdentityExposureMultiplier = 1.0f;

} // namespace render
