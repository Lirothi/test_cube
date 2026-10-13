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
    if (lightBody == Body::Moon)
    {
        // The ground light in the moon's LIGHT colour (moonLightColor, unit luminance: the lux stay); the
        // sky frame below keeps the body's own colour, so the disc and the night sky do not turn blue.
        const float* c = s.moonLightColor;
        const float luma = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
        light.SetUseSunTemperature(false);
        light.SetColor(luma > 1e-6f ? Math::float3(c[0] / luma, c[1] / luma, c[2] / luma) : Math::float3(1.0f, 1.0f, 1.0f));
    }
    light.SetDirection(-lightToward_);
    light.ContinueTransformVersion(sceneLight); // +1 only when the direction actually changed
    sceneLight = light;

    // The sky's sun: exact, every frame.
    DirectionalLight sky = sunTemplate_;
    sky.SetDirection(-sunToward);
    skySun = sky;

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
        skyMoon_.illuminance[0] = lit.x; skyMoon_.illuminance[1] = lit.y; skyMoon_.illuminance[2] = lit.z;
        skyMoon_.fullIlluminance[0] = fullLit.x; skyMoon_.fullIlluminance[1] = fullLit.y; skyMoon_.fullIlluminance[2] = fullLit.z;
        skyMoon_.halfApexRad = now.GetSunHalfApexRadians();
        skyMoon_.phaseAngleRad = static_cast<float>(alpha);
        skyMoon_.litScale = lommelSeeliger > 1.0e-6
            ? static_cast<float>(std::clamp(astro::MoonPhaseBrightness(phaseAngleDeg) / lommelSeeliger, 0.0, 10.0))
            : 0.0f;
        skyMoon_.earthshine = static_cast<float>(std::max(0.0f, s.moonEarthshine) * kEarthshineAtNewMoon * 0.5 * (1.0 - std::cos(alpha)));
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
        const float bright = std::max(0.0f, s.nightBrightness);
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
