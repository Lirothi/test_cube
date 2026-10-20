#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "core/math/Math.h"

class GBufferRenderable;

// Time-of-day plan L: lamps that come on at night (docs/time_of_day_night_sky_plan.md).
//
// A point or spot light, or a mesh's emissive (a bulb, a lit window), carries `"onAt": "night"` (or "day"; absent =
// always). It is switched when the SUN crosses `TimeOfDaySettings::lampSunElevationDeg` -- 0 by default, the moment
// Ultra Dynamic Sky fires its Sunset / Sunrise events -- and comes on the way UDS's Light Day/Night Toggle brings a
// light up: after a delay (up to `lampRandomDelaySec`), over `lampTurnOnSec` along t^`lampTurnOnExponent`, with a
// flicker of `lampTurnOnFlicker` while it warms; it goes out at once. A clock set by hand (a level load, the time
// slider, setTime, --set tod.time) switches everything instantly, as UDS's Instant Time of Day Change does.
//
// THE DELAY IS A SMOOTH NOISE OF THE POSITION (10 m cells), not a random number per light: a street's lamps still
// come on one by one, but a light and the glowing bulb beside it -- two items the level does not link -- get the
// same delay and warm up together. Ours, not UDS's (whose component sits on one light and drives listed materials).
//
// A NIGHT lamp is night light: Scene multiplies its level by TimeOfDaySettings::nightBrightness, as the moon's and
// the night sky's are -- coming in with the night (nightBrightness^NightShare, the sun -6 -> -12), since the dusk is
// lit by the sun's unmultiplied sky. Without it, at UDS's moon (x11364: ~3600 lux of moonlight) a 4000-lm lamp's
// ~35 lux was 1 % of what lit the sand and showed nothing (measured on wind_test, 2026-10-20).
//
// Brightness, never `enabled`: an enable rebuilds the light list behind a GPU wait (EnvironmentRuntime::RebuildLights),
// a brightness is a number the frame already uploads. A light at 0 is skipped by the shading (its range is zeroed,
// as an occluded one's) and by the shadow-slot choice.
enum class LampSwitch : std::uint8_t
{
    Always = 0,
    Night,
    Day,
};

// "night" / "day" (case-insensitive); anything else is Always.
LampSwitch LampSwitchFromName(std::string_view name);
// "always" / "night" / "day".
const char* LampSwitchName(LampSwitch lampSwitch);

class NightLamps
{
public:
    struct Params
    {
        float sunElevationDeg = 0.0f;
        float randomDelaySec = 1.0f;
        float turnOnSec = 1.5f;
        float turnOnFlicker = 0.2f;
        float turnOnExponent = 0.6f;
    };

    // One frame: whether the sun is under the switch elevation now, and whether the clock was set by hand this frame
    // (then a switch is instant). Returns true on the frame the night switches (the first frame included).
    bool Tick(double deltaSeconds, float sunElevationDeg, bool clockSetByHand, const Params& params);
    bool Night() const { return night_; }

    // What an item's light (a lamp's flux, a mesh's emissive) is multiplied by now, 0..1; Always = 1.
    float Level(LampSwitch lampSwitch, const Math::float3& position) const;

    // The meshes whose emissive switches (GBufferRenderable::SetLampSwitch registers them), so the frame walks a
    // list of the few instead of the scene.
    static void Register(GBufferRenderable* object);
    static void Unregister(GBufferRenderable* object);
    static const std::vector<GBufferRenderable*>& Registered();

private:
    Params params_{};
    double clock_ = 0.0;          // real seconds since the first Tick
    double switchedAt_ = -1.0e30; // when the night last switched by itself; far past = settled
    bool night_ = false;
    bool started_ = false;
};
