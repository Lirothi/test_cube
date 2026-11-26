#pragma once

#include "third_party/json/json.hpp"
#include "core/schema/KeyTable.h" // R9.2: the section's table of keys

#include "app/scene/DirectionalLightJson.h"
#include "rendering/lighting/Astronomy.h"
#include "rendering/lighting/DirectionalLight.h"
#include "rendering/lighting/TimeOfDaySettings.h"

#include <algorithm>
#include <cmath>

// Time-of-day plan T2: the level's `timeOfDay` section. Its presence is what hands the directional
// light to the entity (as the other environment sections give a level clouds or a procedural sky):
//
//   "timeOfDay": {
//     "time": 17.5, "animate": false, "dayLengthMin": 30, "nightLengthMin": 15, "timeScale": 60,
//     "realPositions": false,
//     "dawn": 6, "dusk": 18, "pathYawDeg": 180, "pathTiltDeg": 30, "heightOffset": 0, "extendTwilight": 0,
//     "date": [2026, 6, 21], "latitude": -16.5, "longitude": -151.75, "utcOffset": -10, "northYawDeg": 0,
//     "nightBrightness": 1, "lightUpdateThresholdDeg": 0.05,
//     "sun":  { ...the keys of `directionalLight`, without "direction"... },
//     "moon": { "enabled": true, "illuminanceLux": 0.32, "lightColor": [0.446, 0.557, 0.865], "lightSourceAngle": 0.52,
//               "pathYawDeg": 180, "pathTiltDeg": 35, "orbitOffsetDeg": 0, "phase": 0.5, "phaseAuto": true },
//     "twilight": { "sunSkyStartDeg": -9.03, "sunSkyFullDeg": -6.49, "rayleighNightTint": [1.426, 0.848, 0.609],
//                   "rayleighDayStartDeg": -2.07, "rayleighDayFullDeg": 3.68, "ozoneNightTint": [6.154, 1.806, 1.428],
//                   "ozoneSunriseTint": [4.615, 1.557, 4.031], "ozoneBlendDeg": 1.95 },
//     "lamps": { "sunElevationDeg": 0, "randomDelaySec": 1, "turnOnSec": 1.5, "turnOnFlicker": 0.2, "turnOnExponent": 0.6 },
//     "fog": { "dayDensity": 1, "duskDensity": 1, "nightDensity": 1, "dayFullDeg": 25, "duskDeg": 5, "nightFullDeg": -8,
//              "udsColor": false }
//   }
//
// Every key is optional; an absent one keeps the struct default.
namespace TimeOfDaySettingsJson
{

// The sun a `timeOfDay` section starts from before its `sun` object is read: white, and 120 klx outside
// the atmosphere (the solar constant's 127 klx, rounded the way UE's sun presets are) -- a DirectionalLight
// out of its constructor is black.
inline DirectionalLight DefaultSun()
{
    DirectionalLight sun;
    sun.SetColor(Math::float3(1.0f, 1.0f, 1.0f));
    sun.SetSunIlluminanceLux(120000.0f);
    return sun;
}

// The `sun` object: the keys of `directionalLight`, whose own default intensity is the time of day's
// sun rather than the P16.2 field's 100 klx.
inline void ApplySun(const nlohmann::json& j, DirectionalLight& sun)
{
    DirectionalLightJson::ApplyOverrides(j, sun);
    if (!j.contains("sunIlluminanceLux") && !j.contains("sunIntensity") && !j.contains("exposure"))
    {
        sun.SetSunIlluminanceLux(DefaultSun().GetSunIlluminanceLux());
    }
}

namespace detail
{
// A colour of three (or more) numbers: each element a number or left as it was, then raised to 0 -- or clamped into
// [0, hi] when hi > 0. The moon's, the airglow's and the twilight's tints.
inline void Tint(const nlohmann::json& j, const char* key, float* out, float hi = 0.0f)
{
    if (const auto it = j.find(key); it != j.end() && it->is_array() && it->size() >= 3)
    {
        for (int c = 0; c < 3; ++c)
        {
            if (!(*it)[c].is_number()) { continue; }
            const float v = (*it)[c].get<float>();
            out[c] = hi > 0.0f ? std::clamp(v, 0.0f, hi) : std::max(v, 0.0f);
        }
    }
}
inline nlohmann::json Three(const float* v) { return nlohmann::json::array({ v[0], v[1], v[2] }); }
} // namespace detail

// Architecture plan R9.2: the section's table of keys (core/schema/KeyTable.h) -- every number and flag read the
// tolerant way (DirectionalLightJson's Number / Flag: a value of another type is skipped), the five nested objects as
// tables of their own. The `sun` object is a DirectionalLight's, read by ApplySun beside the table.
inline const schema::Table<TimeOfDaySettings>& Keys()
{
    using S = TimeOfDaySettings;
    using J = nlohmann::json;
    static const schema::Table<S> moon{ {
        schema::Field("enabled", &S::moonEnabled).Tolerant(),
        schema::Field("illuminanceLux", &S::moonIlluminanceLux).Tolerant().Clamp(0.0f, 1000.0f),
        schema::Field("newMoonBrightness", &S::moonNewBrightness).Tolerant().Clamp(0.0f, 1.0f),
        schema::Field("udsMoon", &S::udsMoon).Tolerant(),
        schema::Custom<S>("lightColor", [](const J& m, S& s) { detail::Tint(m, "lightColor", s.moonLightColor); },
                          [](J& m, const S& s) { m["lightColor"] = detail::Three(s.moonLightColor); }),
        schema::Custom<S>("discColor", [](const J& m, S& s) { detail::Tint(m, "discColor", s.moonDiscColor); },
                          [](J& m, const S& s) { m["discColor"] = detail::Three(s.moonDiscColor); }),
        schema::Custom<S>("lightShaftBloomTint", [](const J& m, S& s) { detail::Tint(m, "lightShaftBloomTint", s.moonLightShaftTint); },
                          [](J& m, const S& s) { m["lightShaftBloomTint"] = detail::Three(s.moonLightShaftTint); }),
        schema::Field("lightSourceAngle", &S::moonAngleDeg).Tolerant().Clamp(0.0f, 20.0f),
        schema::Field("lightSourceSoftAngle", &S::moonSoftAngleDeg).Tolerant().Clamp(0.0f, 20.0f),
        schema::Field("glow", &S::moonGlow).Tolerant().Clamp(0.0f, 10.0f),
        schema::Field("glowRadiusDeg", &S::moonGlowRadiusDeg).Tolerant().Clamp(0.1f, 90.0f),
        schema::Field("discAdaptation", &S::moonDiscAdaptation).Tolerant().Clamp(0.0f, 1.0f),
        // The moon's own UE bloom properties, named as the sun's are (DirectionalLightJson).
        schema::Field("lightShaftBloomScale", &S::moonLightShaftBloomScale).Tolerant().Clamp(0.0f, 10.0f),
        schema::Field("lightShaftBloomThreshold", &S::moonLightShaftBloomThreshold).Tolerant().Clamp(0.0f, 50.0f),
        schema::Field("lightShaftBloomMaxBrightness", &S::moonLightShaftBloomMaxBrightness).Tolerant().Clamp(0.0f, 1000.0f),
        schema::Field("glintPhaseFalloff", &S::moonGlintPhaseFalloff).Tolerant().Clamp(0.0f, 1.0f),
        schema::Field("earthshine", &S::moonEarthshine).Tolerant().Clamp(0.0f, 100.0f),
        schema::Field("pathYawDeg", &S::moonPathYawDeg).Tolerant(),
        schema::Field("pathTiltDeg", &S::moonPathTiltDeg).Tolerant().Clamp(-89.0f, 89.0f),
        schema::Field("orbitOffsetDeg", &S::moonOrbitOffsetDeg).Tolerant(),
        schema::Custom<S>("phase",
            [](const J& m, S& s) {
                s.moonPhase = DirectionalLightJson::detail::Number(m, "phase", s.moonPhase);
                s.moonPhase = s.moonPhase - std::floor(s.moonPhase);
            },
            [](J& m, const S& s) { m["phase"] = s.moonPhase; }),
        schema::Field("phaseAuto", &S::moonPhaseAuto).Tolerant(),
    } };
    // N2: the night sky's layers.
    static const schema::Table<S> nightSky{ {
        schema::Field("enabled", &S::nightSkyEnabled).Tolerant(),
        schema::Field("airglowZenith", &S::airglowZenith).Tolerant().Clamp(0.0f, 1.0f),
        schema::Custom<S>("airglowColor", [](const J& n, S& s) { detail::Tint(n, "airglowColor", s.airglowColor); },
                          [](J& n, const S& s) { n["airglowColor"] = detail::Three(s.airglowColor); }),
        schema::Field("stars", &S::starsBrightness).Tolerant().Clamp(0.0f, 1000.0f),
        schema::Field("milkyWay", &S::milkyWayBrightness).Tolerant().Clamp(0.0f, 1000.0f),
        schema::Field("twinkle", &S::starTwinkle).Tolerant().Clamp(0.0f, 1.0f),
        schema::Field("siderealAtMidnight", &S::siderealAtMidnightHours).Tolerant(),
    } };
    // Plan L: the night lamps' switch.
    static const schema::Table<S> lamps{ {
        schema::Field("sunElevationDeg", &S::lampSunElevationDeg).Tolerant().Clamp(-90.0f, 90.0f),
        schema::Field("randomDelaySec", &S::lampRandomDelaySec).Tolerant().Clamp(0.0f, 600.0f),
        schema::Field("turnOnSec", &S::lampTurnOnSec).Tolerant().Clamp(0.0f, 600.0f),
        schema::Field("turnOnFlicker", &S::lampTurnOnFlicker).Tolerant().Clamp(0.0f, 1.0f),
        schema::Field("turnOnExponent", &S::lampTurnOnExponent).Tolerant().Clamp(0.01f, 10.0f),
        schema::Field("dayBrightness", &S::lampDayBrightness).Tolerant().Clamp(0.0f, 100000.0f),
    } };
    // The height fog's density by the sun's height (multipliers on heightFog.density).
    static const schema::Table<S> fog{ {
        schema::Field("dayDensity", &S::fogDayDensity).Tolerant().Clamp(0.0f, 100.0f),
        schema::Field("duskDensity", &S::fogDuskDensity).Tolerant().Clamp(0.0f, 100.0f),
        schema::Field("nightDensity", &S::fogNightDensity).Tolerant().Clamp(0.0f, 100.0f),
        schema::Field("dayFullDeg", &S::fogDayFullDeg).Tolerant().Clamp(-90.0f, 90.0f),
        schema::Field("duskDeg", &S::fogDuskDeg).Tolerant().Clamp(-90.0f, 90.0f),
        schema::Field("nightFullDeg", &S::fogNightFullDeg).Tolerant().Clamp(-90.0f, 90.0f),
        schema::Field("udsColor", &S::fogUdsColor).Tolerant(),
    } };
    // The twilight as Ultra Dynamic Sky draws it: the sun leaving the atmosphere, the night Rayleigh.
    static const schema::Table<S> twilight{ {
        schema::Field("sunSkyStartDeg", &S::sunSkyStartDeg).Tolerant().Clamp(-90.0f, 90.0f),
        schema::Field("sunSkyFullDeg", &S::sunSkyFullDeg).Tolerant().Clamp(-90.0f, 90.0f),
        schema::Custom<S>("rayleighNightTint", [](const J& w, S& s) { detail::Tint(w, "rayleighNightTint", s.rayleighNightTint, 10.0f); },
                          [](J& w, const S& s) { w["rayleighNightTint"] = detail::Three(s.rayleighNightTint); }),
        schema::Field("rayleighDayStartDeg", &S::rayleighDayStartDeg).Tolerant().Clamp(-90.0f, 90.0f),
        schema::Field("rayleighDayFullDeg", &S::rayleighDayFullDeg).Tolerant().Clamp(-90.0f, 90.0f),
        schema::Custom<S>("ozoneNightTint", [](const J& w, S& s) { detail::Tint(w, "ozoneNightTint", s.ozoneNightTint, 20.0f); },
                          [](J& w, const S& s) { w["ozoneNightTint"] = detail::Three(s.ozoneNightTint); }),
        schema::Custom<S>("ozoneSunriseTint", [](const J& w, S& s) { detail::Tint(w, "ozoneSunriseTint", s.ozoneSunriseTint, 20.0f); },
                          [](J& w, const S& s) { w["ozoneSunriseTint"] = detail::Three(s.ozoneSunriseTint); }),
        schema::Field("ozoneBlendDeg", &S::ozoneBlendDeg).Tolerant().Clamp(0.001f, 90.0f),
        schema::Field("mieNightScale", &S::mieNightScale).Tolerant().Clamp(0.0f, 10.0f),
        schema::Field("udsSkyMaterial", &S::skyUdsMaterial).Tolerant(),
        schema::Field("lightHandOverDeg", &S::lightHandOverDeg).Tolerant().Clamp(0.0f, 20.0f),
    } };
    static const schema::Table<S> table{ {
        schema::Custom<S>("time",
            [](const J& j, S& s) {
                s.timeHours = DirectionalLightJson::detail::Number(j, "time", s.timeHours);
                s.timeHours = s.timeHours - 24.0f * std::floor(s.timeHours / 24.0f);
            },
            [](J& j, const S& s) { j["time"] = s.timeHours; }),
        schema::Field("animate", &S::animate).Tolerant(),
        schema::Field("dayLengthMin", &S::dayLengthMin).Tolerant().Clamp(0.01f, 10000.0f),
        schema::Field("nightLengthMin", &S::nightLengthMin).Tolerant().Clamp(0.01f, 10000.0f),
        schema::Field("timeScale", &S::timeScale).Tolerant().Clamp(0.0f, 1.0e6f),
        schema::Field("realPositions", &S::realPositions).Tolerant(),
        schema::Field("dawn", &S::dawnHours).Tolerant().Clamp(0.0f, 24.0f),
        schema::Field("dusk", &S::duskHours).Tolerant().Clamp(0.0f, 24.0f),
        schema::Field("pathYawDeg", &S::pathYawDeg).Tolerant(),
        schema::Field("pathTiltDeg", &S::pathTiltDeg).Tolerant().Clamp(-89.0f, 89.0f),
        schema::Field("heightOffset", &S::heightOffset).Tolerant().Clamp(-1.0f, 1.0f),
        schema::Field("extendTwilight", &S::extendTwilight).Tolerant().Clamp(0.0f, 1.0f),
        // Any numbers, rounded: an MCP setEnvironment writes [2026.0, 6.0, 21.0] as readily as integers,
        // and a date that silently stopped being read would be a control that lies.
        schema::Custom<S>("date",
            [](const J& j, S& s) {
                if (const auto it = j.find("date"); it != j.end() && it->is_array() && it->size() >= 3 &&
                    (*it)[0].is_number() && (*it)[1].is_number() && (*it)[2].is_number())
                {
                    s.year = std::clamp(static_cast<int>(std::lround((*it)[0].get<double>())), 1, 9999);
                    s.month = std::clamp(static_cast<int>(std::lround((*it)[1].get<double>())), 1, 12);
                    s.day = std::clamp(static_cast<int>(std::lround((*it)[2].get<double>())), 1, 31);
                }
            },
            [](J& j, const S& s) { j["date"] = J::array({ s.year, s.month, s.day }); }),
        schema::Field("latitude", &S::latitudeDeg).Tolerant().Clamp(-90.0f, 90.0f),
        schema::Field("longitude", &S::longitudeDeg).Tolerant().Clamp(-180.0f, 180.0f),
        schema::Field("utcOffset", &S::utcOffsetHours).Tolerant().Clamp(-14.0f, 14.0f),
        schema::Field("northYawDeg", &S::northYawDeg).Tolerant(),
        schema::Field("nightBrightness", &S::nightBrightness).Tolerant().Clamp(0.0f, 100000.0f),
        schema::Field("daySkyBrightness", &S::daySkyBrightness).Tolerant().Clamp(0.0f, 10.0f),
        schema::Field("nightExposureEv", &S::nightExposureEv).Tolerant().Clamp(-10.0f, 10.0f),
        schema::Field("nightCameraEv100", &S::nightCameraEv100).Tolerant().Clamp(-20.0f, 30.0f),
        schema::Field("nightCameraFollow", &S::nightCameraFollow).Tolerant().Clamp(0.0f, 1.0f),
        schema::Field("nightCameraOpenFrom", &S::nightCameraOpenFrom).Tolerant().Clamp(0.0f, 1.0f),
        schema::Field("nightCameraOpenFull", &S::nightCameraOpenFull).Tolerant().Clamp(0.0f, 1.0f),
        schema::Field("nightCameraIndoorLift", &S::nightCameraIndoorLift).Tolerant().Clamp(0.0f, 30.0f),
        schema::Field("lightUpdateThresholdDeg", &S::lightUpdateThresholdDeg).Tolerant().Clamp(0.0f, 5.0f),
        schema::Nested<S>("moon", moon),
        schema::Nested<S>("nightSky", nightSky),
        schema::Nested<S>("lamps", lamps),
        schema::Nested<S>("fog", fog),
        schema::Nested<S>("twilight", twilight),
    } };
    return table;
}

inline void ApplyOverrides(const nlohmann::json& j, TimeOfDaySettings& s, DirectionalLight& sun)
{
    s.present = true;
    Keys().Read(j, s);
    if (const auto it = j.find("sun"); it != j.end() && it->is_object())
    {
        ApplySun(*it, sun);
    }
}

// T3: a level written before the time of day carries its sun as `directionalLight`. This builds the
// `timeOfDay` section that lights it the same: every light property moves across (the intensity chain
// -- `sunIlluminanceLux`, else P4's `sunIntensity`, else the old `exposure`, each an assignment --
// folded into an explicit `sunIlluminanceLux`; F8's retired `ambientColor` / `ambientTintedBySun`,
// which nothing reads, left behind), the clock is still, and the simple path's time and yaw are SOLVED
// so the sun stands exactly where it was authored (astro::SolveSimpleTime; the tilt gives way for a
// sun above 60 degrees). Exactly two places call this, as with `atmosphere` -> `heightFog`: the level
// reader and the editor document's section table. The file takes the new section when it is saved.
inline nlohmann::json FromDirectionalLight(const nlohmann::json& dl)
{
    nlohmann::json sun = dl.is_object() ? dl : nlohmann::json::object();
    DirectionalLight parsed;
    DirectionalLightJson::ApplyOverrides(sun, parsed);
    for (const char* key : {"direction", "sunIntensity", "exposure", "ambientColor", "ambientTintedBySun"})
    {
        sun.erase(key);
    }
    sun["sunIlluminanceLux"] = parsed.GetSunIlluminanceLux();

    const Math::float3 d = DirectionalLightJson::Direction(dl, Math::float3(0.0f, -1.0f, 0.0f));
    const double toward[3] = {-d.x, -d.y, -d.z};
    astro::SimplePath path;
    path.riseHours = TimeOfDaySettings{}.dawnHours;
    path.setHours = TimeOfDaySettings{}.duskHours;
    path.tiltDeg = TimeOfDaySettings{}.pathTiltDeg;
    const double hours = astro::SolveSimpleTime(toward, path);

    nlohmann::json t = nlohmann::json::object();
    t["time"] = hours;
    t["animate"] = false;
    t["pathYawDeg"] = path.yawDeg;
    t["pathTiltDeg"] = path.tiltDeg;
    t["sun"] = sun;
    return t;
}

// Every key the section has, at the values these settings hold (the `sun` object in the keys of
// `directionalLight`). The editor's setEnvironment can only reach a key that is in the document, so
// the document carries all of them (T4) -- see WithAllKeys.
inline nlohmann::json ToJson(const TimeOfDaySettings& s, const DirectionalLight& sun)
{
    const auto f3 = [](const Math::float3& v) { return nlohmann::json::array({ v.x, v.y, v.z }); };
    nlohmann::json j = Keys().Write(s);
    j["sun"] = {
        { "enabled", true },
        { "color", f3(sun.GetColor()) },
        { "ambient", sun.GetAmbient() },
        { "sunIlluminanceLux", sun.GetSunIlluminanceLux() },
        { "skyFillIntensity", sun.GetSkyFillIntensity() },
        { "groundAlbedo", f3(sun.GetGroundAlbedo()) },
        { "useSunTemperature", sun.GetUseSunTemperature() },
        { "sunTemperatureK", sun.GetSunTemperatureK() },
        { "lightShaftsEnabled", sun.GetLightShaftsEnabled() },
        { "lightShaftBloomScale", sun.GetLightShaftBloomScale() },
        { "lightShaftBloomThreshold", sun.GetLightShaftBloomThreshold() },
        { "lightShaftBloomMaxBrightness", sun.GetLightShaftBloomMaxBrightness() },
        { "lightShaftBloomTint", f3(sun.GetLightShaftBloomTint()) },
        { "lightShaftOcclusionDepthRange", sun.GetLightShaftOcclusionDepthRange() },
        { "lightShaftSmoothing", sun.GetLightShaftSmoothingSec() },
        { "lightShaftLength", sun.GetLightShaftLength() },
        { "lightSourceAngle", sun.GetLightSourceAngleDegrees() },
        { "lightSourceSoftAngle", sun.GetLightSourceSoftAngleDegrees() },
    };
    return j;
}

// The section with every key present (T4): what is written stays as written, a missing key is added
// at the value the section ALREADY MEANS -- so nothing on screen changes, and setEnvironment can now
// name it. The sun's light is read with `enabled` set aside: a switched-off sun parses black, and its
// colour must not be filled in as black for the day it is switched back on.
inline nlohmann::json WithAllKeys(const nlohmann::json& section)
{
    nlohmann::json out = section.is_object() ? section : nlohmann::json::object();
    // Retired keys go (TimeOfDaySettings: the moon's temperature, 2026-10-15), so a saved level stops
    // carrying a number nothing reads.
    if (const auto moon = out.find("moon"); moon != out.end() && moon->is_object()) { moon->erase("temperatureK"); }
    TimeOfDaySettings s{};
    DirectionalLight sun = DefaultSun();
    ApplyOverrides(out, s, sun);
    if (const auto it = out.find("sun"); it != out.end() && it->is_object())
    {
        nlohmann::json lit = *it;
        lit.erase("enabled");
        sun = DefaultSun();
        ApplySun(lit, sun);
    }
    const nlohmann::json full = ToJson(s, sun);
    for (const auto& [key, value] : full.items())
    {
        const auto have = out.find(key);
        if (have == out.end())
        {
            out[key] = value;
        }
        else if (value.is_object() && have->is_object())
        {
            for (const auto& [subKey, subValue] : value.items())
            {
                if (!have->contains(subKey)) { (*have)[subKey] = subValue; }
            }
        }
    }
    return out;
}

} // namespace TimeOfDaySettingsJson
