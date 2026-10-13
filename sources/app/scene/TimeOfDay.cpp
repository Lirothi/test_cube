#include "app/scene/TimeOfDay.h"

#include <algorithm>
#include <cmath>

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

const char* BodyName(TimeOfDay::Body body)
{
    return body == TimeOfDay::Body::Sun ? "sun" : body == TimeOfDay::Body::Moon ? "moon" : "nobody";
}
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
    moon.SetColor(Math::float3(1.0f, 1.0f, 1.0f));
    moon.SetUseSunTemperature(true);
    moon.SetSunTemperatureK(settings_.moonTemperatureK);
    const double lux = settings_.moonIlluminanceLux * astro::MoonPhaseBrightness(phaseAngleDeg) * distanceScale *
                       settings_.nightBrightness;
    moon.SetSunIlluminanceLux(static_cast<float>(lux));
    moon.SetLightSourceAngleDegrees(settings_.moonAngleDeg);
    return moon;
}

void TimeOfDay::Tick(float deltaSeconds, DirectionalLight& sceneLight, DirectionalLight& skySun)
{
    if (!settings_.present) { return; }
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
        astro::SimpleMoonDirection(moonPath, sunPath, clockHours_, s.moonPhase, s.moonOrbitOffsetDeg, moonLocal);
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
    sunToward_ = sunToward;
    moonToward_ = moonToward;

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
    light.SetDirection(-lightToward_);
    light.ContinueTransformVersion(sceneLight); // +1 only when the direction actually changed
    sceneLight = light;

    // The sky's sun: exact, every frame.
    DirectionalLight sky = sunTemplate_;
    sky.SetDirection(-sunToward);
    skySun = sky;

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
