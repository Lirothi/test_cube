#include "app/scene/TimeOfDay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "core/logging/Log.h"
#include "rendering/lighting/Astronomy.h"

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kSynodicMonthDays = 29.530588853;
constexpr double kMoonMeanDistanceKm = 384400.0;
// How far past the switching edge a body must be to TAKE the light: a frozen clock sitting exactly on
// the edge must not hand it back and forth (each hand-over drops the shadow caches).
constexpr double kHandOverHysteresisDeg = 0.05;
// N2: above this sun elevation the night sky's layers are not drawn (they cannot show; see Tick).
constexpr double kNightSkySunCutoffDeg = 10.0;

double ElevationDeg(const Math::float3& toward)
{
    return std::asin(std::clamp(static_cast<double>(toward.y), -1.0, 1.0)) * 180.0 / kPi;
}

Math::float3 ToWorld(const double local[3], double northYawDeg)
{
    double world[3];
    astro::LocalToWorld(local, northYawDeg, world);
    return Math::float3(static_cast<float>(world[0]), static_cast<float>(world[1]), static_cast<float>(world[2]))
        .Normalized();
}

int DaysInMonth(int year, int month)
{
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29 : kDays[std::clamp(month, 1, 12) - 1];
}

// The disc-integrated brightness of a Lommel-Seeliger sphere at phase angle `alpha` (radians),
// relative to full: 1 - sin(a/2) tan(a/2) ln(cot(a/4)). The drawn disc is shaded by that law (the
// real full moon shows no limb darkening, which Lambert would give it); dividing the phase law the
// light follows by this makes the disc's integral agree with the light.
double LommelSeeligerPhase(double alpha)
{
    if (alpha <= 1.0e-6) { return 1.0; }
    if (alpha >= kPi - 1.0e-6) { return 0.0; }
    return std::max(0.0, 1.0 - std::sin(0.5 * alpha) * std::tan(0.5 * alpha) * std::log(1.0 / std::tan(0.25 * alpha)));
}

// The ashen light: Earth's illuminance on the moon's night side against the sun's on its day side,
// about 1.2e-4 at full Earth (new moon) -- ~15 lux of earthlight against 127 klx of sunlight, the
// same albedo either way -- falling with the Earth's phase as seen from the moon, (1 - cos a) / 2.
constexpr double kEarthshineAtNewMoon = 1.2e-4;

const char* BodyName(TimeOfDay::Body body)
{
    return body == TimeOfDay::Body::Sun ? "sun" : body == TimeOfDay::Body::Moon ? "moon" : "nobody";
}

// The twilight's ramps (TimeOfDaySettings::sunSkyStartDeg ..): 0 at and below `startDeg` of sun elevation,
// 1 at and above `fullDeg`, linear between; a range of (almost) nothing is a step at `startDeg`.
float TwilightRamp(double elevationDeg, float startDeg, float fullDeg)
{
    const double span = static_cast<double>(fullDeg) - static_cast<double>(startDeg);
    if (span < 1.0e-3) { return elevationDeg >= startDeg ? 1.0f : 0.0f; }
    return static_cast<float>(std::clamp((elevationDeg - startDeg) / span, 0.0, 1.0));
}
// The twilight's atmosphere blends (night Rayleigh, ozone) are held to steps of 1/64: every new value
// rebuilds the atmosphere's LUTs and restarts the volumetric fog's history (a MAJOR distant-light revision),
// and a running clock would otherwise do both every frame of the hour it takes. A step moves the blue
// scattering by under 1 %.
constexpr float kTwilightBlendSteps = 64.0f;
}

void TimeOfDay::Reset(const TimeOfDaySettings& settings, const DirectionalLight& sun)
{
    settings_ = settings;
    sunTemplate_ = sun;
    clockHours_ = settings.timeHours;
    clockWritten_ = settings.timeHours;
    lightBody_ = Body::Sun;
    litBy_ = Body::Sun;
    lightPlaced_ = false;
    logged_ = false;
    clockSetPending_ = true;
    sunElevationKnown_ = false;
    authoredTime_ = settings.timeHours;
    authoredYear_ = settings.year;
    authoredMonth_ = settings.month;
    authoredDay_ = settings.day;
    authoredMoonPhase_ = settings.moonPhase;
    if (settings_.present)
    {
        LOG_INFO(logging::LogCategory::Scene,
                 "time of day: {} positions, {:.2f} h, {}{}", settings_.realPositions ? "real" : "simple",
                 settings_.timeHours, settings_.animate ? "animated" : "still",
                 settings_.realPositions ? "" : (settings_.moonPhaseAuto ? ", moon phase auto" : ", moon phase fixed"));
    }
}

void TimeOfDay::Apply(const TimeOfDaySettings& settings, const DirectionalLight& sun)
{
    if (!settings_.present || !settings.present)
    {
        Reset(settings, sun);
        return;
    }
    const TimeOfDaySettings running = settings_;
    const bool sameClock = settings.timeHours == authoredTime_ && settings.year == authoredYear_ &&
                           settings.month == authoredMonth_ && settings.day == authoredDay_;
    const bool samePhase = settings.moonPhase == authoredMoonPhase_;
    // A moved clock (a still one shows what is authored, so any edit of it is a move too) switches at once.
    if (!sameClock || !settings.animate || settings.realPositions != settings_.realPositions) { clockSetPending_ = true; }
    settings_ = settings;
    sunTemplate_ = sun;
    // Still: what is on screen is what is authored. Running: keep its place unless the edit moved it.
    if (settings.animate && sameClock)
    {
        settings_.timeHours = running.timeHours;
        settings_.year = running.year;
        settings_.month = running.month;
        settings_.day = running.day;
    }
    if (settings.animate && samePhase && settings.moonPhaseAuto)
    {
        settings_.moonPhase = running.moonPhase;
    }
    authoredTime_ = settings.timeHours;
    authoredYear_ = settings.year;
    authoredMonth_ = settings.month;
    authoredDay_ = settings.day;
    authoredMoonPhase_ = settings.moonPhase;
}

void TimeOfDay::AdvanceClock(double deltaSeconds)
{
    TimeOfDaySettings& s = settings_;
    // Someone else moved the clock (--set, the editor): theirs is the time now.
    if (s.timeHours != clockWritten_)
    {
        clockHours_ = s.timeHours;
        clockSetByHand_ = true;
    }
    if (s.animate && deltaSeconds > 0.0 && speed_ > 0.0f)
    {
        double advanceHours = 0.0;
        if (s.realPositions)
        {
            advanceHours = deltaSeconds * speed_ * s.timeScale / 3600.0;
        }
        else
        {
            // UDS's split: the day half (dawn -> dusk) lasts dayLengthMin real minutes, the night half
            // nightLengthMin. A frame that crosses dawn or dusk advances at the rate it started with.
            double dayHours = static_cast<double>(s.duskHours) - s.dawnHours;
            if (dayHours <= 0.0) { dayHours += 24.0; }
            dayHours = std::clamp(dayHours, 0.01, 23.99);
            double sinceDawn = std::fmod(clockHours_ - s.dawnHours, 24.0);
            if (sinceDawn < 0.0) { sinceDawn += 24.0; }
            const bool day = sinceDawn < dayHours;
            const double rate = day ? dayHours / (60.0 * std::max(s.dayLengthMin, 0.01f))
                                    : (24.0 - dayHours) / (60.0 * std::max(s.nightLengthMin, 0.01f));
            advanceHours = deltaSeconds * speed_ * rate;
            if (s.moonPhaseAuto)
            {
                const double phase = s.moonPhase + advanceHours / (24.0 * kSynodicMonthDays);
                s.moonPhase = static_cast<float>(phase - std::floor(phase));
            }
        }
        clockHours_ += advanceHours;
        while (clockHours_ >= 24.0)
        {
            clockHours_ -= 24.0;
            if (s.realPositions && ++s.day > DaysInMonth(s.year, s.month))
            {
                s.day = 1;
                if (++s.month > 12) { s.month = 1; ++s.year; }
            }
        }
    }
    s.timeHours = static_cast<float>(clockHours_);
    clockWritten_ = s.timeHours;
}

DirectionalLight TimeOfDay::MoonLight(double phaseAngleDeg, double distanceScale) const
{
    // The sun's scene-wide fields (ambient fraction, sky fill, ground albedo, shafts) stay, so the fill
    // cannot jump when the light changes hands; the moon brings its brightness, colour and size.
    DirectionalLight moon = sunTemplate_;
    // White at unit luminance: each of its hues is set where it is used (the ground's and the sky's
    // moonLightColor, the disc's moonDiscColor -- Tick).
    moon.SetColor(Math::float3(1.0f, 1.0f, 1.0f));
    moon.SetUseSunTemperature(false);
    // The phase law, lifted off zero by moonNewBrightness (UDS's New Moon Light Brightness): the LIGHT only --
    // the drawn disc keeps the bare law (litScale), dark at new moon as UDS's is.
    const double floor = std::clamp(static_cast<double>(settings_.moonNewBrightness), 0.0, 1.0);
    // Night Brightness by the night's share (nightGain_, Tick): a physical moon at dusk and by day -- or in full from the
    // sunset with Ultra Dynamic Sky's moon (moonGain_).
    const double lux = settings_.moonIlluminanceLux * (floor + (1.0 - floor) * astro::MoonPhaseBrightness(phaseAngleDeg)) *
                       distanceScale * moonGain_;
    moon.SetSunIlluminanceLux(static_cast<float>(lux));
    moon.SetLightSourceAngleDegrees(settings_.moonAngleDeg);
    moon.SetLightSourceSoftAngleDegrees(settings_.moonSoftAngleDeg);
    return moon;
}

void TimeOfDay::Tick(float deltaSeconds, DirectionalLight& sceneLight, DirectionalLight& skySun)
{
    if (!settings_.present) { return; }
    clockSetByHand_ = clockSetPending_;
    clockSetPending_ = false;
    AdvanceClock(deltaSeconds);
    const TimeOfDaySettings& s = settings_;

    // Where the bodies are.
    double sunLocal[3] = {0.0, 1.0, 0.0};
    double moonLocal[3] = {0.0, -1.0, 0.0};
    double phaseAngleDeg = 0.0;
    double distanceScale = 1.0;
    if (s.realPositions)
    {
        const double jd = astro::JulianDayUtc(s.year, s.month, s.day, clockHours_ - s.utcOffsetHours);
        astro::HorizontalToLocal(astro::SunPosition(s.latitudeDeg, s.longitudeDeg, jd).position, sunLocal);
        const astro::MoonState moon = astro::MoonPosition(s.latitudeDeg, s.longitudeDeg, jd);
        astro::HorizontalToLocal(moon.position, moonLocal);
        phaseAngleDeg = moon.phaseAngleDeg;
        distanceScale = moon.distanceKm > 1.0 ? (kMoonMeanDistanceKm / moon.distanceKm) * (kMoonMeanDistanceKm / moon.distanceKm)
                                              : 1.0;
    }
    else
    {
        astro::SimplePath sunPath;
        sunPath.riseHours = s.dawnHours;
        sunPath.setHours = s.duskHours;
        sunPath.yawDeg = s.pathYawDeg;
        sunPath.tiltDeg = s.pathTiltDeg;
        sunPath.heightOffset = s.heightOffset;
        sunPath.flatten = s.extendTwilight;
        astro::SimpleDirection(sunPath, clockHours_, sunLocal);
        astro::SimplePath moonPath = sunPath;
        moonPath.yawDeg = s.moonPathYawDeg;
        moonPath.tiltDeg = s.moonPathTiltDeg;
        // THE PHASE CHANGES THE MOON'S FACE, NOT ITS PLACE (owner, 2026-10-13: "when the phase changes the
        // moon must not roll across the sky"), as Ultra Dynamic Sky's simple moon does: it rides opposite
        // the sun, turned along its circle by the orbit offset, whatever the phase -- which then sets only
        // the disc's lit shape and the light's brightness. (Placed by the phase, as the real moon is, a
        // first quarter stood 90 degrees from the sun and every phase change slid it across the sky.)
        // Real positions keep the real place and the real phase, both from the date.
        astro::SimpleMoonDirection(moonPath, sunPath, clockHours_, 0.5, s.moonOrbitOffsetDeg, moonLocal);
        // Phase cycle 0 new .. 0.5 full .. 1 new -> the sun-moon-earth angle, 180 at new, 0 at full.
        phaseAngleDeg = 180.0 * std::fabs(1.0 - 2.0 * s.moonPhase);
    }
    const Math::float3 sunToward = ToWorld(sunLocal, s.northYawDeg);
    const Math::float3 moonToward = ToWorld(moonLocal, s.northYawDeg);
    const double sunElevation = ElevationDeg(sunToward);
    const double moonElevation = ElevationDeg(moonToward);
    sunElevationDeg_ = static_cast<float>(sunElevation);
    moonElevationDeg_ = static_cast<float>(moonElevation);
    moonPhaseAngleDeg_ = static_cast<float>(phaseAngleDeg);
    // Plan L: sunrise and sunset as events (UDS's Sunrise / Sunset dispatchers): the sun's centre crossing the
    // horizon, once per crossing -- a transition, so it needs no per-frame gate.
    if (sunElevationKnown_ && (lastSunElevationDeg_ < 0.0f) != (sunElevationDeg_ < 0.0f))
    {
        const int minutes = static_cast<int>(std::floor(std::fmod(clockHours_, 24.0) * 60.0 + 0.5)) % (24 * 60);
        if (s.realPositions)
        {
            LOG_INFO(logging::LogCategory::Scene, "{} at {:02}:{:02} on {:04}-{:02}-{:02}{}",
                     sunElevationDeg_ < 0.0f ? "sunset" : "sunrise", minutes / 60, minutes % 60, s.year, s.month, s.day,
                     clockSetByHand_ ? " (clock set)" : "");
        }
        else
        {
            LOG_INFO(logging::LogCategory::Scene, "{} at {:02}:{:02}{}", sunElevationDeg_ < 0.0f ? "sunset" : "sunrise",
                     minutes / 60, minutes % 60, clockSetByHand_ ? " (clock set)" : "");
        }
    }
    lastSunElevationDeg_ = sunElevationDeg_;
    sunElevationKnown_ = true;
    if (clockSetByHand_) { clockSetUnread_ = true; }
    sunToward_ = sunToward;
    moonToward_ = moonToward;

    // The night's share and the owner's night exposure (TimeOfDaySettings::nightExposureEv): none above -6 degrees,
    // all of it from -12. Before the lights below: the night's light is scaled by it.
    {
        const float x = std::clamp(static_cast<float>((-6.0 - sunElevation) / 6.0), 0.0f, 1.0f);
        nightShare_ = x * x * (3.0f - 2.0f * x);
        nightExposureEv_ = s.nightExposureEv * nightShare_;
    }
    // ONE night gain for everything that lights the night -- the moon (the ground's light, the atmosphere's second
    // light, the disc), the night sky (stars, airglow, Milky Way), the night lamps (Scene) AND the sun's twilight sky:
    // nightBrightness^NightShare. The moon and the night sky took the whole multiplier at any hour, so at dusk a
    // x11364 moon (~3600 lux) outshone the physical twilight -- the rising full moon at 19:30-19:54 lit the clouds
    // and the sand as a second sun under a giant aureole, with the night's stars over a blue-hour sky (owner,
    // 2026-10-31: "very pretty but not realistic"). One gain on all of them keeps every ratio physical at dusk and
    // makes Night Brightness what it is at night, an exposure shift; with the sun under -12 degrees nothing changes.
    nightGain_ = std::pow(std::max(s.nightBrightness, 1.0e-6f), nightShare_);
    // ...or ULTRA DYNAMIC SKY'S MOON (TimeOfDaySettings::udsMoon): the moon and the night sky in full from the sunset, linear
    // from +1.15 degrees to 0 as UDS's moon light (0.065 lux at +0.65, 0.15 at 0); the sun's twilight sky without it.
    moonGain_ = nightGain_;
    sunSkyGain_ = nightGain_;
    if (s.udsMoon)
    {
        const float t = std::clamp(static_cast<float>((1.15 - sunElevation) / 1.15), 0.0f, 1.0f);
        moonGain_ = 1.0f + (std::max(s.nightBrightness, 1.0f) - 1.0f) * t;
        sunSkyGain_ = 1.0f;
    }

    // The night's light on a level surface, physical lux (before Night Brightness), as the EV100 a meter reads off middle
    // grey under it and against the full moon's at 45 degrees -- what the steady night camera follows through the exposure
    // compensation curve (Scene). Held at the full moon's EV whatever the moon did, a quarter moon 22 degrees up (14x less
    // light) came out nearly black and a moonless night wholly black, p95 0 of 255 (owner, 2026-10-31: "at night with a
    // partial moon it is terribly dark").
    {
        constexpr double kRefMoonElevationDeg = 45.0; // wind_test's full moon at 01:00 on 2026-09-27 stands at 44.6
        const double moonFloor = std::clamp(static_cast<double>(s.moonNewBrightness), 0.0, 1.0);
        const double moonLux = s.moonEnabled
            ? std::max(static_cast<double>(s.moonIlluminanceLux), 0.0) *
              (moonFloor + (1.0 - moonFloor) * astro::MoonPhaseBrightness(phaseAngleDeg)) * distanceScale *
              std::max(std::sin(moonElevation * kPi / 180.0), 0.0)
            : 0.0;
        // A clear sky's twilight on a level surface: log10 lux 0.53 with the sun 6 degrees down, falling 0.43 a degree --
        // 3.4 / 0.5 / 0.08 / 0.009 lux at -6 / -8 / -10 / -12 (the published clear-sky twilight tables). Held at the -6
        // degree value above it, where the night camera is not on.
        const double depression = std::max(-6.0 - sunElevation, 0.0);
        const double twilightLux = std::pow(10.0, 0.53 - 0.43 * depression);
        // The night sky: the airglow (zenith luminance x pi, x2 for its brightening toward the horizon), the stars and the
        // Milky Way (~2e-4 / 1e-4 lux at their unit brightness): ~0.001 lux at the defaults, a moonless clear night's.
        const double nightSkyLux = s.nightSkyEnabled
            ? 2.0 * kPi * std::max(static_cast<double>(s.airglowZenith), 0.0) +
              2.0e-4 * std::max(static_cast<double>(s.starsBrightness), 0.0) +
              1.0e-4 * std::max(static_cast<double>(s.milkyWayBrightness), 0.0)
            : 0.0;
        const double refLux = std::max(static_cast<double>(s.moonIlluminanceLux), 0.0) *
                              std::sin(kRefMoonElevationDeg * kPi / 180.0) + nightSkyLux;
        // Ultra Dynamic Sky's sky material (skyUdsMaterial, uds_sky.hlsli) adds its Night Sky Glow to the sky -- a light of
        // its own, never scaled by Night Brightness, which the sky light's capture takes twice over. On a level surface:
        // the glow's lerp(1.3, 0.6, cos) x cos over the hemisphere = 2.618 x its luminance, x2, x the Overall Intensity.
        // Left out, a moonless night under it read 7.9 stops under the full moon where the frame was 3.6 under, and the
        // steady camera opened the difference: a new moon came out a dim day, brighter than the full moon (2026-11-04,
        // the owner's view at 21:54 on Sep 11: median 206 of 255; the full moon at that hour 45).
        double glowLux = 0.0;
        if (s.skyUdsMaterial)
        {
            float overall = 1.0f, midpointPerLux = 1.0f, glowPerLux[3]{}, lit = 1.0f, extraContrast = 0.0f;
            UdsSkyAt(sunElevation, overall, midpointPerLux, glowPerLux, lit, extraContrast);
            const double luma = 0.2126 * glowPerLux[0] + 0.7152 * glowPerLux[1] + 0.0722 * glowPerLux[2];
            glowLux = 2.618 * 2.0 * overall * luma * sunTemplate_.GetOuterSpaceLuminance();
        }
        // Everything as RENDERED, over the night's gain: the camera (Scene) multiplies the night gain back in, so each
        // light counts with the gain it is drawn with -- the moon and the night sky moonGain_, the sun's twilight
        // sunSkyGain_ (both the night gain unless udsMoon), the glow none. Without UDS this is the physical lux as before.
        const double lux = (moonGain_ * (moonLux + nightSkyLux) + sunSkyGain_ * twilightLux + glowLux) /
                           std::max(static_cast<double>(nightGain_), 1.0e-9);
        nightLightEv_ = (refLux > 0.0 && lux > 0.0) ? static_cast<float>(std::log2(lux / refLux)) : 0.0f;
        // Middle grey (0.18) under E lux is 0.18 E / pi cd/m2, EV100 log2(L) + 3 (the meter's form): the full moon at
        // 45 degrees -3.27, a quarter moon 22 degrees up -6.9, a moonless night -11.
        const auto meterEv = [](double e) { return static_cast<float>(std::log2(std::max(e, 1.0e-9) * 0.18 / kPi) + 3.0); };
        nightEv100_ = meterEv(lux);
        nightRefEv100_ = meterEv(refLux);
    }

    // Who reaches the ground: a body lights while any of its disc is above the horizon -- exactly where
    // Scene::PrepareViews' disc fraction (and so its light) is non-zero. The body in charge keeps it to
    // that very edge, so its light has reached 0 when it lets go; a body taking over must be a
    // hysteresis past the edge.
    const double sunRadius = 0.5 * sunTemplate_.GetLightSourceAngleDegrees();
    const double moonRadius = 0.5 * s.moonAngleDeg;
    const bool sunLights = sunElevation > -sunRadius + (lightBody_ == Body::Sun ? 0.0 : kHandOverHysteresisDeg);
    const bool moonLights = s.moonEnabled &&
        moonElevation > -moonRadius + (lightBody_ == Body::Moon ? 0.0 : kHandOverHysteresisDeg);
    const Body litBy = sunLights ? Body::Sun : moonLights ? Body::Moon : Body::Nobody;
    // Nobody up: the light stays with its body, under the ground and lighting nothing.
    const Body lightBody = litBy == Body::Nobody ? lightBody_ : litBy;

    // The scene light turns only past the threshold (every turn re-renders the sun's shadows) or when it
    // changes hands.
    const Math::float3 toward = lightBody == Body::Sun ? sunToward : moonToward;
    const float thresholdCos = static_cast<float>(std::cos(std::max(s.lightUpdateThresholdDeg, 0.0f) * kPi / 180.0));
    if (!lightPlaced_ || lightBody != lightBody_ || toward.Dot(lightToward_) < thresholdCos)
    {
        lightToward_ = toward;
        lightPlaced_ = true;
    }
    DirectionalLight light = lightBody == Body::Sun ? sunTemplate_ : MoonLight(phaseAngleDeg, distanceScale);
    if (lightBody == Body::Moon)
    {
        // The ground light in the moon's LIGHT colour (moonLightColor, unit luminance: the lux stay); the
        // sky frame below keeps the body's own colour, so the disc and the night sky do not turn blue.
        const float* c = s.moonLightColor;
        const float luma = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
        light.SetUseSunTemperature(false);
        light.SetColor(luma > 1e-6f ? Math::float3(c[0] / luma, c[1] / luma, c[2] / luma) : Math::float3(1.0f, 1.0f, 1.0f));
    }
    // The hand-over cross-fade (TimeOfDaySettings::lightHandOverDeg), by the sun's elevation from the edge it
    // changes hands at: the moon is out by then, the sun comes in from 0 -- one light, never a jump.
    {
        const double span = std::max(static_cast<double>(s.lightHandOverDeg), 0.0);
        const double fromEdge = sunElevation + sunRadius; // > 0: the sun's disc is (partly) up
        double fade = 1.0;
        if (span > 1.0e-3)
        {
            const double x = std::clamp((lightBody == Body::Sun ? fromEdge : -fromEdge) / span, 0.0, 1.0);
            fade = x * x * (3.0 - 2.0 * x);
        }
        handOverFade_ = static_cast<float>(fade);
        if (fade < 1.0) { light.SetSunIlluminanceLux(static_cast<float>(light.GetSunIlluminanceLux() * fade)); }
    }
    light.SetDirection(-lightToward_);
    light.ContinueTransformVersion(sceneLight); // +1 only when the direction actually changed
    sceneLight = light;

    // The sky's sun: exact, every frame -- and leaving the atmosphere as Ultra Dynamic Sky's does, between
    // sunSkyStartDeg and sunSkyFullDeg (the twilight's glow; the disc is under the horizon by then).
    DirectionalLight sky = sunTemplate_;
    sky.SetDirection(-sunToward);
    sunSkyShare_ = TwilightRamp(sunElevation, s.sunSkyStartDeg, s.sunSkyFullDeg);
    // The twilight is night light too (nightGain_, above): x1 until the sun is 6 degrees down.
    if (sunSkyShare_ * sunSkyGain_ != 1.0f) { sky.SetSunIlluminanceLux(sky.GetSunIlluminanceLux() * sunSkyShare_ * sunSkyGain_); }
    skySun = sky;
    // UDS's night Rayleigh: the tint while the sun is down, easing out to none by rayleighDayFullDeg.
    {
        const float t = TwilightRamp(sunElevation, s.rayleighDayStartDeg, s.rayleighDayFullDeg);
        const float day = std::round((1.0f - (1.0f - t) * (1.0f - t)) * kTwilightBlendSteps) / kTwilightBlendSteps;
        for (int c = 0; c < 3; ++c)
        {
            rayleighTint_[c] = std::max(s.rayleighNightTint[c], 0.0f) * (1.0f - day) + day;
        }
        // The day sky's brightness comes in with UDS's day sky, on the same (stepped) blend.
        skyLuminanceScale_ = 1.0f + (std::max(s.daySkyBrightness, 0.0f) - 1.0f) * day;
    }
    // The night's aerosol (TimeOfDaySettings::mieNightScale) by the night's share, stepped like the tints so the sky's
    // LUTs rebuild at most 64 times across the dusk.
    {
        const float night = std::round(nightShare_ * kTwilightBlendSteps) / kTwilightBlendSteps;
        mieScale_ = 1.0f + (std::max(s.mieNightScale, 0.0f) - 1.0f) * night;
    }
    // The height fog by the sun's height (TimeOfDaySettings::fogDayDensity ..): day over fogDayFullDeg, dusk at
    // fogDuskDeg, night under fogNightFullDeg, smoothstep between. Not stepped like the atmosphere's tints: the fog
    // rebuilds no LUT, and its volume's history follows a medium that changes this slowly.
    fogDensityScale_ = FogDensityScaleAt(s, sunElevation);
    // Ultra Dynamic Sky's fog colour by the sun's height (TimeOfDaySettings::fogUdsColor): its floor per lux of ITS sun,
    // times ours (the sun's outer-space luminance, whoever lights the ground: UDS keys it on its sun alone).
    fogSkyContribution_ = 1.0f;
    for (int c = 0; c < 3; ++c) { fogSunScatterTint_[c] = 1.0f; fogInscatterFloor_[c] = 0.0f; }
    if (s.fogUdsColor)
    {
        float floorPerLux[3]{};
        UdsFogColorAt(sunElevation, fogSkyContribution_, fogSunScatterTint_, floorPerLux);
        const float sunLuminance = sunTemplate_.GetOuterSpaceLuminance();
        for (int c = 0; c < 3; ++c) { fogInscatterFloor_[c] = floorPerLux[c] * sunLuminance; }
    }
    // Ultra Dynamic Sky's sky material over the atmosphere (TimeOfDaySettings::skyUdsMaterial, uds_sky.hlsli): its numbers
    // by the sun's height, its absolute ones per lux of its sun x ours; the SKY's sun and the moon's direction.
    udsSky_ = UdsSkyFrameData{};
    if (s.skyUdsMaterial)
    {
        float overall = 1.0f, midpointPerLux = 1.0f, glowPerLux[3]{}, lit = 1.0f, extraContrast = 0.0f;
        UdsSkyAt(sunElevation, overall, midpointPerLux, glowPerLux, lit, extraContrast);
        const float k = sunTemplate_.GetOuterSpaceLuminance();
        udsSky_.params[0] = overall;
        udsSky_.params[1] = lit;
        udsSky_.params[2] = midpointPerLux * k;
        udsSky_.params[3] = extraContrast;
        for (int c = 0; c < 3; ++c) { udsSky_.glow[c] = glowPerLux[c] * k; }
        udsSky_.glow[3] = 1.0f;
        udsSky_.sun[0] = sunToward.x; udsSky_.sun[1] = sunToward.y; udsSky_.sun[2] = sunToward.z;
        udsSky_.sun[3] = std::clamp(-15.0f * static_cast<float>(std::sin(sunElevation * kPi / 180.0)), 0.0f, 1.0f);
        udsSky_.moon[0] = moonToward.x; udsSky_.moon[1] = moonToward.y; udsSky_.moon[2] = moonToward.z;
        udsSky_.moon[3] = s.moonEnabled ? 1.0f : 0.0f;
    }
    // UDS's twilight ozone: night -> sunrise (at 0) -> the authored ozone, across +-ozoneBlendDeg, in the same
    // 1/64 steps (of each half).
    {
        const float w = std::max(s.ozoneBlendDeg, 1.0e-3f);
        const float u = std::round(static_cast<float>(std::clamp(sunElevation / w, -1.0, 1.0)) * kTwilightBlendSteps) /
                        kTwilightBlendSteps;
        for (int c = 0; c < 3; ++c)
        {
            const float night = std::max(s.ozoneNightTint[c], 0.0f), sunrise = std::max(s.ozoneSunriseTint[c], 0.0f);
            ozoneTint_[c] = u < 0.0f ? night + (sunrise - night) * (1.0f + u) : sunrise + (1.0f - sunrise) * u;
        }
    }

    // N1: the moon in the sky -- the atmosphere's second light and the disc -- whoever lights the ground.
    skyMoon_ = SkyMoonFrame{};
    if (s.moonEnabled)
    {
        const DirectionalLight now = MoonLight(phaseAngleDeg, distanceScale);
        const DirectionalLight full = MoonLight(0.0, distanceScale);
        const Math::float3 lit = now.GetOuterSpaceIlluminance();
        const Math::float3 fullLit = full.GetOuterSpaceIlluminance();
        const double alpha = phaseAngleDeg * kPi / 180.0;
        const double lommelSeeliger = LommelSeeligerPhase(alpha);
        skyMoon_.present = true;
        skyMoon_.toward[0] = moonToward.x; skyMoon_.toward[1] = moonToward.y; skyMoon_.toward[2] = moonToward.z;
        // The moon lights the ATMOSPHERE in its light colour too (moonLightColor at the light's luminance), as
        // Ultra Dynamic Sky's moon -- a directional light of that colour -- lights its SkyAtmosphere. It used to
        // keep a cream temperature so the disc would not turn blue; the disc and its glow have their own
        // colour since N4 (moonDiscColor, below), and UDS's night Rayleigh (twilight.rayleighNightTint)
        // scatters less blue -- under the cream light the moonlit sky went grey (b/r 2.1 -> 1.35 at the
        // probe), under UDS's light it keeps its hue, as UDS's does.
        {
            const float* lc = s.moonLightColor;
            const float lightLuma = 0.2126f * lc[0] + 0.7152f * lc[1] + 0.0722f * lc[2];
            const float litLuma = 0.2126f * lit.x + 0.7152f * lit.y + 0.0722f * lit.z;
            const float litRgb[3] = {lit.x, lit.y, lit.z};
            for (int c = 0; c < 3; ++c)
            {
                skyMoon_.illuminance[c] = lightLuma > 1e-6f ? litLuma * std::max(lc[c], 0.0f) / lightLuma : litRgb[c];
            }
        }
        // The full disc's reference in the DISC's colour (moonDiscColor at the full moon's luminance): only the
        // hue is the artist's, the disc stays exactly as bright as the light it sheds.
        {
            const float* dc = s.moonDiscColor;
            const float discLuma = 0.2126f * dc[0] + 0.7152f * dc[1] + 0.0722f * dc[2];
            const float fullLuma = 0.2126f * fullLit.x + 0.7152f * fullLit.y + 0.0722f * fullLit.z;
            const float fullRgb[3] = {fullLit.x, fullLit.y, fullLit.z};
            for (int c = 0; c < 3; ++c)
            {
                skyMoon_.fullIlluminance[c] = discLuma > 1e-6f ? fullLuma * dc[c] / discLuma : fullRgb[c];
            }
        }
        skyMoon_.halfApexRad = now.GetSunHalfApexRadians();
        skyMoon_.phaseAngleRad = static_cast<float>(alpha);
        skyMoon_.litScale = lommelSeeliger > 1.0e-6
            ? static_cast<float>(std::clamp(astro::MoonPhaseBrightness(phaseAngleDeg) / lommelSeeliger, 0.0, 10.0))
            : 0.0f;
        skyMoon_.earthshine = static_cast<float>(std::max(0.0f, s.moonEarthshine) * kEarthshineAtNewMoon * 0.5 * (1.0 - std::cos(alpha)));
        skyMoon_.lightsGround = lightBody == Body::Moon;
        skyMoon_.discAdaptation = std::clamp(s.moonDiscAdaptation, 0.0f, 1.0f);
        skyMoon_.glow = static_cast<float>(std::max(s.moonGlow, 0.0f) * astro::MoonPhaseBrightness(phaseAngleDeg));
        skyMoon_.glowRadiusRad = static_cast<float>(std::clamp(s.moonGlowRadiusDeg, 0.1f, 90.0f) * kPi / 180.0);
        // The bright limb (SkyMoonFrame::limbAngle): toward the real sun in Real mode; in Simple mode toward
        // a virtual one on the moon's own circle, where the phase would have put the sun had it placed the
        // moon (theta + 2 pi phase -- SimpleMoonDirection's phaseCycle 0.5 - phase), so a waxing and a
        // waning moon light opposite limbs. In the sky's local frame (x, z, y of the world) and the disc
        // basis skybox.hlsl builds: u = up projected, r = f x u.
        {
            double litWorld[3] = {sunToward.x, sunToward.y, sunToward.z};
            if (!s.realPositions)
            {
                astro::SimplePath sunPath;
                sunPath.riseHours = s.dawnHours; sunPath.setHours = s.duskHours; sunPath.yawDeg = s.pathYawDeg;
                sunPath.tiltDeg = s.pathTiltDeg; sunPath.heightOffset = s.heightOffset; sunPath.flatten = s.extendTwilight;
                astro::SimplePath moonPath = sunPath;
                moonPath.yawDeg = s.moonPathYawDeg;
                moonPath.tiltDeg = s.moonPathTiltDeg;
                double virtualLocal[3];
                astro::SimpleMoonDirection(moonPath, sunPath, clockHours_, 0.5 - s.moonPhase, s.moonOrbitOffsetDeg, virtualLocal);
                const Math::float3 virtualWorld = ToWorld(virtualLocal, s.northYawDeg);
                litWorld[0] = virtualWorld.x; litWorld[1] = virtualWorld.y; litWorld[2] = virtualWorld.z;
            }
            const double f[3] = {moonToward.x, moonToward.z, moonToward.y};
            const double sun[3] = {litWorld[0], litWorld[2], litWorld[1]};
            double upRef[3] = {0.0, 0.0, 1.0};
            if (std::fabs(f[2]) >= 0.999) { upRef[0] = 1.0; upRef[2] = 0.0; }
            const double ud = upRef[0] * f[0] + upRef[1] * f[1] + upRef[2] * f[2];
            double u[3] = {upRef[0] - ud * f[0], upRef[1] - ud * f[1], upRef[2] - ud * f[2]};
            const double ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
            for (double& c : u) { c /= std::max(ul, 1.0e-12); }
            const double r[3] = {f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0]};
            const double sd = sun[0] * f[0] + sun[1] * f[1] + sun[2] * f[2];
            const double t[3] = {sun[0] - sd * f[0], sun[1] - sd * f[1], sun[2] - sd * f[2]};
            const double tu = t[0] * u[0] + t[1] * u[1] + t[2] * u[2];
            const double tr = t[0] * r[0] + t[1] * r[1] + t[2] * r[2];
            // Almost exactly behind the viewer the direction is undefined: the lit side then turns down.
            skyMoon_.limbAngle = (tu * tu + tr * tr) > 1.0e-10 ? static_cast<float>(std::atan2(tu, tr)) : static_cast<float>(-0.5 * kPi);
        }
        skyMoon_.lightShaftBloomScale = std::max(s.moonLightShaftBloomScale, 0.0f);
        skyMoon_.lightShaftBloomThreshold = std::max(s.moonLightShaftBloomThreshold, 0.0f);
        skyMoon_.lightShaftBloomMaxBrightness = std::max(s.moonLightShaftBloomMaxBrightness, 0.0f);
        // The shafts are the moon's LIGHT scattered by the air, so they start in the light's colour
        // (moonLightColor at unit luminance, the cold light on the ground) and the owner's tint multiplies
        // that. Seeded by the drawn disc alone they were the disc's cream: "the rays from the moon must not
        // be yellow" (owner, 2026-10-13); a neutral grey over dark green crowns still read olive.
        {
            const float* lc = s.moonLightColor;
            const float lightLuma = 0.2126f * lc[0] + 0.7152f * lc[1] + 0.0722f * lc[2];
            for (int c = 0; c < 3; ++c)
            {
                const float hue = lightLuma > 1e-6f ? lc[c] / lightLuma : 1.0f;
                skyMoon_.lightShaftTint[c] = std::max(s.moonLightShaftTint[c], 0.0f) * hue;
            }
        }
    }

    // N2: the night sky's layers. Off while the sun stands above kNightSkySunCutoffDeg: the sky is then
    // thousands of cd/m2 and the brightest star a few hundredths of it in its pixel -- invisible, and the
    // pass skips the work. At the cutoff the sky is still ~3000 cd/m2, so nothing on screen steps.
    nightSky_ = NightSkyFrame{};
    if (s.nightSkyEnabled && sunElevation < kNightSkySunCutoffDeg)
    {
        const double siderealDeg = s.realPositions
            ? astro::LocalSiderealDeg(s.longitudeDeg,
                                      astro::JulianDayUtc(s.year, s.month, s.day, clockHours_ - s.utcOffsetHours))
            : 15.0 * (clockHours_ + static_cast<double>(s.siderealAtMidnightHours));
        // local (east, up, north) = M * equatorial, M orthonormal; world = the north yaw of local. So
        // equatorial = M^T * yaw^-1 * world.
        double m[9];
        astro::CelestialToLocal(s.latitudeDeg, siderealDeg, m);
        const double c = std::cos(static_cast<double>(s.northYawDeg) * kPi / 180.0);
        const double sn = std::sin(static_cast<double>(s.northYawDeg) * kPi / 180.0);
        // yaw^-1 rows: local = (c x - s z, y, s x + c z) of the world (LocalToWorld's inverse).
        const double yawInv[9] = {c, 0.0, -sn, 0.0, 1.0, 0.0, sn, 0.0, c};
        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 3; ++col)
            {
                double sum = 0.0;
                for (int k = 0; k < 3; ++k) { sum += m[k * 3 + row] * yawInv[k * 3 + col]; }
                nightSky_.worldToEquatorial[row * 3 + col] = static_cast<float>(sum);
            }
        }
        const float bright = moonGain_;
        const float* ac = s.airglowColor;
        const float luma = 0.2126f * ac[0] + 0.7152f * ac[1] + 0.0722f * ac[2];
        for (int k = 0; k < 3; ++k)
        {
            nightSky_.airglow[k] = std::max(0.0f, s.airglowZenith) * bright * (luma > 1e-6f ? ac[k] / luma : 1.0f);
        }
        nightSky_.stars = std::max(0.0f, s.starsBrightness) * bright;
        nightSky_.milkyWay = std::max(0.0f, s.milkyWayBrightness) * bright;
        nightSky_.twinkle = std::clamp(s.starTwinkle, 0.0f, 1.0f);
        nightSky_.present = nightSky_.stars > 0.0f || nightSky_.milkyWay > 0.0f ||
                            nightSky_.airglow[0] + nightSky_.airglow[1] + nightSky_.airglow[2] > 0.0f;
    }

    // N4: the moon's glints on the water, faded again by the phase law (moonGlintPhaseFalloff).
    glintScale_ = lightBody == Body::Moon
        ? static_cast<float>(std::pow(std::max(astro::MoonPhaseBrightness(phaseAngleDeg), 1.0e-6),
                                      std::clamp(static_cast<double>(s.moonGlintPhaseFalloff), 0.0, 1.0)))
        : 1.0f;

    if (!logged_ || litBy != litBy_)
    {
        const int minutes = static_cast<int>(std::floor(clockHours_ * 60.0 + 0.5)) % (24 * 60);
        LOG_INFO(logging::LogCategory::Scene,
                 "time of day: {:02}:{:02}, sun {:.2f} deg, moon {:.2f} deg phase angle {:.0f} -> directional light: {}",
                 minutes / 60, minutes % 60, sunElevation, moonElevation, phaseAngleDeg, BodyName(litBy));
        logged_ = true;
    }
    litBy_ = litBy;
    lightBody_ = lightBody;
}

namespace
{
// The sun's geometric altitude at a local clock time on the section's date and place.
double RealSunAltitudeDeg(const TimeOfDaySettings& s, double hours)
{
    const double jd = astro::JulianDayUtc(s.year, s.month, s.day, hours - s.utcOffsetHours);
    return astro::SunPosition(s.latitudeDeg, s.longitudeDeg, jd).position.altitudeDeg;
}

// The minute-resolution crossing of the horizon on the date, refined linearly: rising or setting.
bool RealHorizonCrossing(const TimeOfDaySettings& s, bool rising, double& outHours)
{
    double previous = RealSunAltitudeDeg(s, 0.0);
    for (int minute = 1; minute <= 24 * 60; ++minute)
    {
        const double hours = minute / 60.0;
        const double now = RealSunAltitudeDeg(s, hours);
        const bool crossed = rising ? (previous < 0.0 && now >= 0.0) : (previous >= 0.0 && now < 0.0);
        if (crossed)
        {
            const double t = previous / (previous - now);
            outHours = hours - (1.0 - t) / 60.0;
            return true;
        }
        previous = now;
    }
    return false;
}

std::string Lower(std::string_view text)
{
    std::string out;
    for (const char ch : text)
    {
        if (ch == ' ' || ch == '\t') { continue; }
        out.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch);
    }
    return out;
}

bool StartsWith(const std::string& text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

// "H", "H.5", "H:MM" -> hours. `allowUnits`: a trailing h/ч (hours) or m/min/м/мин (minutes).
bool ParseClock(std::string text, bool allowUnits, double& outHours)
{
    double unit = 1.0;
    if (allowUnits)
    {
        for (const std::string_view suffix : { "min", "мин", "m", "м" })
        {
            if (text.size() > suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0)
            {
                text.resize(text.size() - suffix.size());
                unit = 1.0 / 60.0;
                break;
            }
        }
        if (unit == 1.0)
        {
            for (const std::string_view suffix : { "h", "ч" })
            {
                if (text.size() > suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0)
                {
                    text.resize(text.size() - suffix.size());
                    break;
                }
            }
        }
    }
    if (text.empty()) { return false; }
    const std::size_t colon = text.find(':');
    char* end = nullptr;
    if (colon == std::string::npos)
    {
        const double value = std::strtod(text.c_str(), &end);
        if (end != text.c_str() + text.size()) { return false; }
        outHours = value * unit;
        return true;
    }
    if (unit != 1.0) { return false; } // "1:30m" means nothing
    const std::string h = text.substr(0, colon);
    const std::string m = text.substr(colon + 1);
    if (h.empty() || m.size() != 2) { return false; }
    const long hours = std::strtol(h.c_str(), &end, 10);
    if (end != h.c_str() + h.size()) { return false; }
    const long minutes = std::strtol(m.c_str(), &end, 10);
    if (end != m.c_str() + m.size() || minutes < 0 || minutes > 59 || hours < 0) { return false; }
    outHours = static_cast<double>(hours) + static_cast<double>(minutes) / 60.0;
    return true;
}
}

namespace
{
// ULTRA DYNAMIC SKY'S FOG COLOUR (TimeOfDaySettings::fogUdsColor), read off its actor in the owner's UE 5.8 (2026-11-04,
// scratchpad ue_uds_sweep.py: its 'Time of Day' every 3 minutes over the sunset, 15 over the day and the night; the
// construction script replaces its components on every set, so they were fetched again each time). UDS's simple sun
// path is symmetric (60 degrees at noon, rise 6:00, set 18:00), so one curve in the sun's elevation serves dawn and dusk.
// Its HeightFogContribution STEPS at the horizon (0.84 at +0.65 degrees, 1.21 at 0): its day and night curves meet there.
struct UdsKey1 { float elevationDeg, value; };
struct UdsKey3 { float elevationDeg, rgb[3]; };
constexpr UdsKey1 kUdsFogSkyContribution[] = {
    {-90.0f, 1.65f}, {-28.76f, 1.65f}, {-25.66f, 1.735f}, {-22.52f, 1.907f}, {-19.36f, 2.025f}, {-16.16f, 2.083f},
    {-12.95f, 2.076f}, {-11.02f, 2.037f}, {-9.08f, 1.973f}, {-7.14f, 1.881f}, {-5.19f, 1.761f}, {-3.25f, 1.614f},
    {-2.60f, 1.558f}, {-1.95f, 1.454f}, {-1.30f, 1.271f}, {-0.65f, 1.304f}, {0.0f, 1.206f}, {0.001f, 0.71f},
    {0.65f, 0.841f}, {1.30f, 0.976f}, {1.95f, 1.103f}, {2.60f, 1.221f}, {3.25f, 1.331f}, {3.90f, 1.432f},
    {4.54f, 1.525f}, {5.19f, 1.610f}, {5.84f, 1.686f}, {6.49f, 1.754f}, {7.14f, 1.813f}, {7.79f, 1.864f},
    {8.43f, 1.907f}, {9.08f, 1.942f}, {9.73f, 1.969f}, {10.37f, 1.987f}, {11.02f, 1.997f}, {11.66f, 2.0f}, {90.0f, 2.0f}};
// Its DirectionalInscatteringLuminance (UE multiplies it by the sun's luminance, FogRendering.cpp:436).
constexpr UdsKey3 kUdsFogSunScatter[] = {
    {0.0f, {0.0f, 0.0f, 0.0f}}, {0.65f, {0.00355f, 0.00048f, 0.00021f}}, {1.30f, {0.01174f, 0.00178f, 0.00049f}},
    {1.95f, {0.0263f, 0.00529f, 0.0008f}}, {2.60f, {0.04664f, 0.01056f, 0.00114f}}, {3.25f, {0.07277f, 0.01757f, 0.00152f}},
    {3.90f, {0.09734f, 0.02853f, 0.00325f}}, {4.54f, {0.1252f, 0.04197f, 0.00545f}}, {5.19f, {0.1564f, 0.0579f, 0.00815f}},
    {5.84f, {0.1937f, 0.07904f, 0.0151f}}, {6.49f, {0.235f, 0.1032f, 0.02337f}}, {7.14f, {0.2802f, 0.1305f, 0.03294f}},
    {7.79f, {0.3293f, 0.1608f, 0.04381f}}, {8.43f, {0.3822f, 0.1941f, 0.05597f}}, {9.08f, {0.439f, 0.2304f, 0.06943f}},
    {9.73f, {0.4934f, 0.2671f, 0.09329f}}, {10.37f, {0.5507f, 0.3064f, 0.1196f}}, {11.02f, {0.611f, 0.3482f, 0.1484f}},
    {11.66f, {0.667f, 0.3885f, 0.1777f}}, {12.31f, {0.6946f, 0.4125f, 0.2f}}, {12.95f, {0.7221f, 0.4365f, 0.2224f}},
    {16.16f, {0.7535f, 0.4885f, 0.2944f}}, {19.36f, {0.7846f, 0.5402f, 0.3659f}}, {22.52f, {0.7853f, 0.5597f, 0.402f}},
    {25.66f, {0.7859f, 0.579f, 0.4378f}}, {28.76f, {0.7866f, 0.5981f, 0.4732f}}, {31.82f, {0.7872f, 0.6169f, 0.5081f}},
    {34.82f, {0.7874f, 0.6235f, 0.5204f}}, {37.76f, {0.7877f, 0.63f, 0.5324f}}, {40.63f, {0.7879f, 0.6363f, 0.5441f}},
    {43.40f, {0.7881f, 0.6424f, 0.5554f}}, {90.0f, {0.7881f, 0.6424f, 0.5554f}}};
// Its FogInscatteringLuminance, cd/m2 under its 5-lux sun (TimeOfDay divides by 5: per lux).
constexpr UdsKey3 kUdsFogFloor[] = {
    {-90.0f, {0.00045f, 0.00068f, 0.00122f}}, {-28.76f, {0.00045f, 0.00068f, 0.00122f}},
    {-25.66f, {0.00049f, 0.00074f, 0.00133f}}, {-22.52f, {0.00058f, 0.00089f, 0.00159f}},
    {-19.36f, {0.00068f, 0.00104f, 0.00187f}}, {-16.16f, {0.00078f, 0.00119f, 0.00215f}},
    {-12.95f, {0.00089f, 0.00135f, 0.00244f}}, {-9.73f, {0.001f, 0.00152f, 0.00273f}}, {-6.49f, {0.00111f, 0.00168f, 0.00303f}},
    {-3.25f, {0.00122f, 0.00185f, 0.00334f}}, {-2.60f, {0.00124f, 0.00189f, 0.0034f}}, {-1.95f, {0.00117f, 0.00179f, 0.00322f}},
    {-1.30f, {0.00085f, 0.0013f, 0.00233f}}, {-0.65f, {0.00056f, 0.00085f, 0.00153f}}, {0.0f, {0.00031f, 0.00047f, 0.00084f}},
    {0.65f, {0.00012f, 0.00019f, 0.00033f}}, {1.30f, {0.00007f, 0.00011f, 0.0002f}}, {1.95f, {0.00004f, 0.00006f, 0.00011f}},
    {2.60f, {0.00001f, 0.00002f, 0.00004f}}, {3.25f, {0.0f, 0.0f, 0.00001f}}, {3.90f, {0.0f, 0.0f, 0.0f}}, {90.0f, {0.0f, 0.0f, 0.0f}}};
constexpr float kUdsSunLux = 5.0f; // UDS's Sun Light Intensity

// ITS SKY MATERIAL's parameters (TimeOfDaySettings::skyUdsMaterial, uds_sky.hlsli), read off the sky sphere's material
// instance the same way (scratchpad ue_uds_skysweep.py: every 3 minutes over the sunset, 30 over the day and night).
// Overall Intensity steps at the horizon as its HeightFogContribution does (0.40 at +0.65 degrees, 0.69 at 0); its
// twilight peak (1.25 at -2.6) is what keeps UDS's dusk sky bright. Contrast Midpoint and Night Sky Glow Color are
// under its 5-lux sun (TimeOfDay divides by 5: per lux of ours). Lit_Intensity 0.8 and Extra_Contrast 0.1 held still.
constexpr UdsKey1 kUdsSkyOverall[] = {
    {-90.0f, 0.45f}, {-31.82f, 0.45f}, {-25.66f, 0.4891f}, {-19.35f, 0.6862f}, {-12.95f, 0.8959f}, {-11.66f, 0.9391f},
    {-10.37f, 0.9826f}, {-9.08f, 1.0264f}, {-7.79f, 1.0705f}, {-6.49f, 1.1149f}, {-5.19f, 1.1594f}, {-3.90f, 1.2042f},
    {-3.25f, 1.2267f}, {-2.60f, 1.2492f}, {-1.95f, 1.2245f}, {-1.30f, 1.0966f}, {-0.65f, 0.9265f}, {0.0f, 0.6895f},
    {0.65f, 0.3955f}, {1.30f, 0.3875f}, {1.95f, 0.3815f}, {2.60f, 0.3774f}, {3.25f, 0.3753f}, {3.90f, 0.375f}, {90.0f, 0.375f}};
constexpr UdsKey1 kUdsSkyMidpoint[] = {
    {-90.0f, 0.020513f}, {-60.0f, 0.020513f}, {-53.14f, 0.019743f}, {-48.59f, 0.019213f}, {-43.40f, 0.018445f},
    {-37.76f, 0.017486f}, {-31.82f, 0.016411f}, {-25.66f, 0.015163f}, {-19.35f, 0.013609f}, {-12.95f, 0.011605f},
    {-9.73f, 0.010389f}, {-6.49f, 0.008499f}, {-3.25f, 0.006146f}, {-2.60f, 0.005851f}, {-1.95f, 0.005888f},
    {-1.30f, 0.006802f}, {-0.65f, 0.010517f}, {0.0f, 0.021585f}, {0.65f, 0.047313f}, {1.95f, 0.090485f}, {3.25f, 0.13936f},
    {4.54f, 0.19009f}, {6.49f, 0.2662f}, {9.73f, 0.36541f}, {12.95f, 0.43484f}, {19.35f, 0.52903f}, {25.66f, 0.60288f},
    {37.76f, 0.70992f}, {48.59f, 0.78782f}, {60.0f, 0.85288f}, {90.0f, 0.85288f}};
constexpr UdsKey3 kUdsSkyGlow[] = {
    {-90.0f, {0.001117f, 0.0017f, 0.003062f}}, {-2.60f, {0.001117f, 0.0017f, 0.003062f}},
    {-1.95f, {0.001071f, 0.001631f, 0.002938f}}, {-1.30f, {0.000836f, 0.001273f, 0.002293f}},
    {-0.65f, {0.00063f, 0.000959f, 0.001727f}}, {0.0f, {0.000453f, 0.00069f, 0.001242f}},
    {0.65f, {0.000305f, 0.000464f, 0.000836f}}, {1.30f, {0.000186f, 0.000284f, 0.000511f}},
    {1.95f, {0.000097f, 0.000147f, 0.000265f}}, {2.60f, {0.000036f, 0.000055f, 0.000099f}},
    {3.25f, {0.000005f, 0.000007f, 0.000013f}}, {3.90f, {0.0f, 0.0f, 0.0f}}, {90.0f, {0.0f, 0.0f, 0.0f}}};
constexpr float kUdsSkyLit = 0.8f;
constexpr float kUdsSkyExtraContrast = 0.1f;

template <typename Key, size_t N, typename Get>
void UdsLookup(const Key (&keys)[N], float e, Get&& get)
{
    if (e <= keys[0].elevationDeg) { get(keys[0], keys[0], 0.0f); return; }
    for (size_t i = 1; i < N; ++i)
    {
        if (e <= keys[i].elevationDeg)
        {
            const float span = keys[i].elevationDeg - keys[i - 1].elevationDeg;
            get(keys[i - 1], keys[i], span > 0.0f ? (e - keys[i - 1].elevationDeg) / span : 1.0f);
            return;
        }
    }
    get(keys[N - 1], keys[N - 1], 0.0f);
}
}

void UdsFogColorAt(double sunElevationDeg, float& skyContribution, float sunScatter[3], float floorPerLux[3])
{
    const float e = static_cast<float>(sunElevationDeg);
    UdsLookup(kUdsFogSkyContribution, e, [&](const UdsKey1& a, const UdsKey1& b, float t) {
        skyContribution = a.value + (b.value - a.value) * t; });
    // Under the horizon its directional term is 0 (the first key).
    UdsLookup(kUdsFogSunScatter, e, [&](const UdsKey3& a, const UdsKey3& b, float t) {
        for (int c = 0; c < 3; ++c) { sunScatter[c] = a.rgb[c] + (b.rgb[c] - a.rgb[c]) * t; } });
    UdsLookup(kUdsFogFloor, e, [&](const UdsKey3& a, const UdsKey3& b, float t) {
        for (int c = 0; c < 3; ++c) { floorPerLux[c] = (a.rgb[c] + (b.rgb[c] - a.rgb[c]) * t) / kUdsSunLux; } });
}

void UdsSkyAt(double sunElevationDeg, float& overall, float& midpointPerLux, float glowPerLux[3], float& lit, float& extraContrast)
{
    const float e = static_cast<float>(sunElevationDeg);
    lit = kUdsSkyLit;
    extraContrast = kUdsSkyExtraContrast;
    UdsLookup(kUdsSkyOverall, e, [&](const UdsKey1& a, const UdsKey1& b, float t) { overall = a.value + (b.value - a.value) * t; });
    UdsLookup(kUdsSkyMidpoint, e, [&](const UdsKey1& a, const UdsKey1& b, float t) {
        midpointPerLux = (a.value + (b.value - a.value) * t) / kUdsSunLux; });
    UdsLookup(kUdsSkyGlow, e, [&](const UdsKey3& a, const UdsKey3& b, float t) {
        for (int c = 0; c < 3; ++c) { glowPerLux[c] = (a.rgb[c] + (b.rgb[c] - a.rgb[c]) * t) / kUdsSunLux; } });
}

float FogDensityScaleAt(const TimeOfDaySettings& s, double sunElevationDeg)
{
    const auto smooth = [](float x) { x = std::clamp(x, 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); };
    const float day = std::max(s.fogDayDensity, 0.0f), dusk = std::max(s.fogDuskDensity, 0.0f);
    const float night = std::max(s.fogNightDensity, 0.0f);
    if (sunElevationDeg >= s.fogDuskDeg)
    {
        return dusk + (day - dusk) * smooth(TwilightRamp(sunElevationDeg, s.fogDuskDeg, s.fogDayFullDeg));
    }
    return night + (dusk - night) * smooth(TwilightRamp(sunElevationDeg, s.fogNightFullDeg, s.fogDuskDeg));
}

float SunElevationAtHours(const TimeOfDaySettings& s, double hours)
{
    // The same two paths Tick takes for the sun (real positions on the date and place, or the simple path).
    double local[3] = {0.0, 1.0, 0.0};
    if (s.realPositions)
    {
        const double jd = astro::JulianDayUtc(s.year, s.month, s.day, hours - s.utcOffsetHours);
        astro::HorizontalToLocal(astro::SunPosition(s.latitudeDeg, s.longitudeDeg, jd).position, local);
    }
    else
    {
        astro::SimplePath path;
        path.riseHours = s.dawnHours;
        path.setHours = s.duskHours;
        path.yawDeg = s.pathYawDeg;
        path.tiltDeg = s.pathTiltDeg;
        path.heightOffset = s.heightOffset;
        path.flatten = s.extendTwilight;
        astro::SimpleDirection(path, hours, local);
    }
    return static_cast<float>(ElevationDeg(ToWorld(local, s.northYawDeg)));
}

bool ResolveTimeOfDaySpec(std::string_view spec, const TimeOfDaySettings& s, float& outHours, std::string& outError)
{
    const std::string text = Lower(spec);
    const auto wrap = [](double h) { h = std::fmod(h, 24.0); return static_cast<float>(h < 0.0 ? h + 24.0 : h); };
    const auto oneOf = [&text](std::initializer_list<std::string_view> words) {
        return std::find(words.begin(), words.end(), text) != words.end();
    };
    const bool sunrise = oneOf({ "sunrise", "dawn", "рассвет", "восход" });
    const bool sunset = oneOf({ "sunset", "dusk", "закат" });
    if (sunrise || sunset)
    {
        if (!s.realPositions)
        {
            outHours = wrap(sunrise ? s.dawnHours : s.duskHours);
            return true;
        }
        double hours = 0.0;
        if (!RealHorizonCrossing(s, sunrise, hours))
        {
            outError = std::string("the sun does not ") + (sunrise ? "rise" : "set") + " on " +
                       std::to_string(s.year) + "-" + std::to_string(s.month) + "-" + std::to_string(s.day) +
                       " at latitude " + std::to_string(static_cast<int>(std::lround(s.latitudeDeg)));
            return false;
        }
        outHours = wrap(hours);
        return true;
    }
    if (oneOf({ "noon", "midday", "полдень" }))
    {
        if (!s.realPositions)
        {
            double day = static_cast<double>(s.duskHours) - s.dawnHours;
            if (day <= 0.0) { day += 24.0; }
            outHours = wrap(s.dawnHours + 0.5 * day);
            return true;
        }
        int best = 0;
        double bestAltitude = -1.0e9;
        for (int minute = 0; minute < 24 * 60; ++minute)
        {
            const double altitude = RealSunAltitudeDeg(s, minute / 60.0);
            if (altitude > bestAltitude) { bestAltitude = altitude; best = minute; }
        }
        outHours = wrap(best / 60.0);
        return true;
    }
    if (oneOf({ "midnight", "полночь" }))
    {
        outHours = 0.0f;
        return true;
    }
    double hours = 0.0;
    if (StartsWith(text, "+") || StartsWith(text, "-"))
    {
        if (ParseClock(text.substr(1), true, hours))
        {
            outHours = wrap(s.timeHours + (text[0] == '-' ? -hours : hours));
            return true;
        }
    }
    else if (ParseClock(text, false, hours) && hours >= 0.0 && hours <= 24.0)
    {
        outHours = wrap(hours);
        return true;
    }
    outError = "'" + std::string(spec) + "' is not a time: give HH:MM (\"18:30\"), sunrise / sunset / noon / "
               "midnight, or a shift like +2h, -30m, +1:30";
    return false;
}

bool ParseTimeOfDayDate(std::string_view spec, int& year, int& month, int& day, std::string& outError)
{
    const std::string text = Lower(spec);
    int y = 0, m = 0, d = 0;
    char tail = 0;
    if (sscanf_s(text.c_str(), "%d-%d-%d%c", &y, &m, &d, &tail, 1) != 3 || y < 1 || y > 9999 || m < 1 || m > 12 ||
        d < 1 || d > 31)
    {
        outError = "'" + std::string(spec) + "' is not a date: give YYYY-MM-DD (\"2026-06-21\")";
        return false;
    }
    year = y;
    month = m;
    day = d;
    return true;
}

std::string FormatTimeOfDay(float hours)
{
    const int minutes = static_cast<int>(std::lround(static_cast<double>(hours) * 60.0)) % (24 * 60);
    char text[8];
    std::snprintf(text, sizeof(text), "%02d:%02d", minutes / 60, minutes % 60);
    return text;
}
