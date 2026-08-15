#pragma once

#include <algorithm>
#include <cmath>

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
    float compensationEv = 0.0f;
    // Safety net for the adapted value, in EV100. NOTE the span below is 22 stops, which is wide
    // enough that it clamps essentially nothing -- it is "off", not "tuned". Narrowing it is how a
    // scene stops the camera from opening all the way up in a dark frame. It is deliberately NOT
    // the mechanism for keeping different lighting conditions apart; see decision 5a of the plan.
    float minEv100 = -6.0f;
    float maxEv100 = 16.0f;
    // Histogram percentiles used to derive the metered luminance. Clipping the tails is what keeps
    // a sun glint or a patch of sky from dragging the whole frame, without hard-coding what water
    // or sky look like.
    float lowPercentile = 0.02f;
    float highPercentile = 0.95f;
    // Adaptation rates in stops/second, separately controllable because the eye darkens much
    // faster than it brightens and a single rate always looks wrong in one direction.
    float speedUp = 3.0f;
    float speedDown = 1.0f;
    // Used when autoExposure is false.
    float manualEv100 = 10.0f;
};

// ---- EV100 conventions (plan section 6.2), documented once, here ----
//
// EV100 is the exposure value referenced to ISO 100. The persistent, serialised and UI-facing
// quantity is always EV100; shaders only ever see the linear multiplier derived from it. Higher
// EV100 = a more closed camera = a darker image.

// Scene luminance (cd/m^2) -> the EV100 that exposes it as middle grey.
// EV100 = log2(L * S / K) with S = 100 (ISO) and K = 12.5 (the reflected-light meter calibration
// constant used by Canon/Nikon/Sekonic), which reduces to log2(L * 8).
inline float Ev100FromLuminance(float luminance)
{
    return std::log2(std::max(luminance, 1e-6f) * 8.0f);
}

// EV100 -> the linear value scene colour is multiplied by before the tone curve.
// The 1.2 is the saturation-based speed constant from the same photometric derivation; it is the
// reason a "correctly" metered image lands slightly below 1.0 rather than exactly at it.
inline float ExposureMultiplierFromEv100(float ev100)
{
    return 1.0f / (1.2f * std::exp2(ev100));
}

// The multiplier a disabled camera must produce. Kept as a named constant so the "dormant means
// exactly 1.0" contract is greppable rather than a magic literal in three places.
inline constexpr float kIdentityExposureMultiplier = 1.0f;

} // namespace render
