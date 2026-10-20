#pragma once

#include <string>
#include <string_view>

#include "core/math/Math.h"
#include "rendering/lighting/DirectionalLight.h"
#include "rendering/lighting/TimeOfDaySettings.h"

// Time-of-day plan T2: the entity that owns the scene's directional light (docs/time_of_day_night_sky_plan.md).
//
// Every frame it runs the clock, places the sun and the moon (rendering/lighting/Astronomy.h) and writes
// two lights:
//   * the SCENE light -- the one DirectionalLight every surface, shadow, fog and cloud pass lights with --
//     handed to whichever body reaches the ground: the sun while any of its disc is above the horizon,
//     else the moon, else nobody (it stays on the last body, under the ground, and lights nothing; the
//     shadow passes sleep, FrameDecisions::sunShadowsOff). It turns only past `lightUpdateThresholdDeg`;
//   * the SKY's sun -- always the sun, at its exact direction. The sky's LUTs, the sky pixel, its capture
//     and the aerial perspective take this one (SceneFrameData::skyLight), so the twilight glow outlives
//     the hand-over: at the switch the sun's direct light is already 0 and the moon's 0.3 lux arrive under
//     a sky still delivering hundreds. The moon's own disc and its light in the sky are N1's.
//
// The change of hands happens where the outgoing body's direct light is exactly zero (its disc wholly
// below the sea-level horizon -- the same disc fraction Scene::PrepareViews applies), so it costs no
// visible step; a small hysteresis keeps a frozen clock sitting on the edge from flipping it.
class TimeOfDay
{
public:
    enum class Body : unsigned char { Sun, Moon, Nobody };

    // Level load: the section and its sun. Forgets the previous level's clock and light.
    void Reset(const TimeOfDaySettings& settings, const DirectionalLight& sun);
    // An edit of the section (the editor's Inspector, an MCP setEnvironment): the new settings, but a
    // RUNNING clock keeps its place unless the edit moved the authored time, date or phase -- dragging
    // the sun's lux while the day plays must not throw the day back to where it was authored.
    void Apply(const TimeOfDaySettings& settings, const DirectionalLight& sun);
    bool Present() const { return settings_.present; }

    // The level-authored state; --set and (T3) the editor write it. A changed `timeHours` is picked up
    // on the next Tick.
    TimeOfDaySettings& SettingsRef() { return settings_; }
    const TimeOfDaySettings& Settings() const { return settings_; }
    DirectionalLight& SunTemplateRef() { return sunTemplate_; }

    // Session-only clock rate multiplier (--time-speed); not part of the level.
    void SetSpeed(float speed) { speed_ = speed; }

    // One frame. `sceneLight` is Scene's directional light, `skySun` the sky's sun.
    void Tick(float deltaSeconds, DirectionalLight& sceneLight, DirectionalLight& skySun);

    // What lights the ground now (Nobody = neither body is up), and where the bodies stand -- the
    // Inspector's read-out of the running clock.
    Body LitBy() const { return litBy_; }
    float SunElevationDeg() const { return sunElevationDeg_; }
    float MoonElevationDeg() const { return moonElevationDeg_; }
    float MoonPhaseAngleDeg() const { return moonPhaseAngleDeg_; }
    // Unit world directions TO the bodies, exact (the scene light may lag by the update threshold);
    // the viewport marks them when the Time of Day object is selected.
    const Math::float3& SunToward() const { return sunToward_; }
    const Math::float3& MoonToward() const { return moonToward_; }
    // N1: the moon as the sky sees it (the atmosphere's second light and the disc), this frame.
    const SkyMoonFrame& GetSkyMoon() const { return skyMoon_; }
    // N2: the night sky's layers this frame (its twinkle clock is the scene's, set by Scene).
    const NightSkyFrame& GetNightSky() const { return nightSky_; }
    // N4: what the water's glints of the scene light are scaled by -- 1 for the sun, the moon's phase law
    // to moonGlintPhaseFalloff for the moon (OceanRenderable::GetSpecularParams).
    float GlintScale() const { return glintScale_; }
    // The twilight (TimeOfDaySettings::sunSkyStartDeg ..): the share of the sun's light the sky gets this
    // frame, and what the frame's atmosphere multiplies its Rayleigh scattering by (Scene::PrepareViews).
    float SunSkyShare() const { return sunSkyShare_; }
    const float* RayleighTint() const { return rayleighTint_; }
    const float* OzoneTint() const { return ozoneTint_; }
    // What the frame's sky luminance scale is multiplied by (TimeOfDaySettings::daySkyBrightness at the day blend).
    float SkyLuminanceScale() const { return skyLuminanceScale_; }
    // Stops added to the frame camera's compensation (TimeOfDaySettings::nightExposureEv by the night's share).
    float NightExposureEv() const { return nightExposureEv_; }
    // The scene light's share through the hand-over cross-fade (TimeOfDaySettings::lightHandOverDeg), 0..1.
    float HandOverFade() const { return handOverFade_; }

private:
    void AdvanceClock(double deltaSeconds);
    DirectionalLight MoonLight(double phaseAngleDeg, double distanceScale) const;

    TimeOfDaySettings settings_{};
    DirectionalLight sunTemplate_{};
    float speed_ = 1.0f;
    // The clock in double, so an animated day does not accumulate float rounding; re-read from the
    // settings whenever someone else wrote `timeHours`.
    double clockHours_ = 12.0;
    float clockWritten_ = -1.0f;
    Body lightBody_ = Body::Sun;   // whose light the scene light carries (never Nobody)
    Body litBy_ = Body::Sun;       // who actually lights the ground (for the log)
    bool lightPlaced_ = false;
    Math::float3 lightToward_{0.0f, 1.0f, 0.0f}; // where the scene light was last turned to
    bool logged_ = false;
    // The authored clock the running one started from (Apply compares against it).
    float authoredTime_ = 12.0f;
    int authoredYear_ = 0, authoredMonth_ = 0, authoredDay_ = 0;
    float authoredMoonPhase_ = 0.5f;
    float sunElevationDeg_ = 0.0f;
    float moonElevationDeg_ = 0.0f;
    float moonPhaseAngleDeg_ = 0.0f;
    Math::float3 sunToward_{0.0f, 1.0f, 0.0f};
    Math::float3 moonToward_{0.0f, -1.0f, 0.0f};
    SkyMoonFrame skyMoon_{};
    NightSkyFrame nightSky_{};
    float glintScale_ = 1.0f;
    float sunSkyShare_ = 1.0f;
    float rayleighTint_[3] = {1.0f, 1.0f, 1.0f};
    float ozoneTint_[3] = {1.0f, 1.0f, 1.0f};
    float skyLuminanceScale_ = 1.0f;
    float nightExposureEv_ = 0.0f;
    float handOverFade_ = 1.0f;
};

// T4: what the editor's `setTime` / `setDate` accept (the command bar, MCP), resolved against the
// section's settings:
//   * "18:30", "6:15", "18", "18.5"                      -- a clock time;
//   * "sunrise"/"dawn", "sunset"/"dusk", "noon", "midnight" (and рассвет/восход, закат, полдень,
//     полночь) -- with real positions the date's own sunrise / sunset (the sun's centre on the
//     horizon) and the sun's highest point, on the simple path dawn / dusk / the path's top;
//   * "+2h", "-30m", "+1:30", "+1.5"                     -- a shift from `settings.timeHours`.
// False with a sentence that says what IS accepted.
bool ResolveTimeOfDaySpec(std::string_view spec, const TimeOfDaySettings& settings, float& outHours,
                          std::string& outError);
// "YYYY-MM-DD".
bool ParseTimeOfDayDate(std::string_view spec, int& year, int& month, int& day, std::string& outError);
// "HH:MM", the minute rounded.
std::string FormatTimeOfDay(float hours);
