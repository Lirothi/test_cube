#pragma once

#include "third_party/json/json.hpp"

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
//     "fog": { "dayDensity": 1, "duskDensity": 1, "nightDensity": 1, "dayFullDeg": 25, "duskDeg": 5, "nightFullDeg": -8 }
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

inline void ApplyOverrides(const nlohmann::json& j, TimeOfDaySettings& s, DirectionalLight& sun)
{
    using DirectionalLightJson::detail::Number;
    using DirectionalLightJson::detail::Flag;
    s.present = true;
    s.timeHours = Number(j, "time", s.timeHours);
    s.timeHours = s.timeHours - 24.0f * std::floor(s.timeHours / 24.0f);
    s.animate = Flag(j, "animate", s.animate);
    s.dayLengthMin = std::clamp(Number(j, "dayLengthMin", s.dayLengthMin), 0.01f, 10000.0f);
    s.nightLengthMin = std::clamp(Number(j, "nightLengthMin", s.nightLengthMin), 0.01f, 10000.0f);
    s.timeScale = std::clamp(Number(j, "timeScale", s.timeScale), 0.0f, 1.0e6f);
    s.realPositions = Flag(j, "realPositions", s.realPositions);
    s.dawnHours = std::clamp(Number(j, "dawn", s.dawnHours), 0.0f, 24.0f);
    s.duskHours = std::clamp(Number(j, "dusk", s.duskHours), 0.0f, 24.0f);
    s.pathYawDeg = Number(j, "pathYawDeg", s.pathYawDeg);
    s.pathTiltDeg = std::clamp(Number(j, "pathTiltDeg", s.pathTiltDeg), -89.0f, 89.0f);
    s.heightOffset = std::clamp(Number(j, "heightOffset", s.heightOffset), -1.0f, 1.0f);
    s.extendTwilight = std::clamp(Number(j, "extendTwilight", s.extendTwilight), 0.0f, 1.0f);
    // Any numbers, rounded: an MCP setEnvironment writes [2026.0, 6.0, 21.0] as readily as integers,
    // and a date that silently stopped being read would be a control that lies.
    if (const auto it = j.find("date"); it != j.end() && it->is_array() && it->size() >= 3 &&
        (*it)[0].is_number() && (*it)[1].is_number() && (*it)[2].is_number())
    {
        s.year = std::clamp(static_cast<int>(std::lround((*it)[0].get<double>())), 1, 9999);
        s.month = std::clamp(static_cast<int>(std::lround((*it)[1].get<double>())), 1, 12);
        s.day = std::clamp(static_cast<int>(std::lround((*it)[2].get<double>())), 1, 31);
    }
    s.latitudeDeg = std::clamp(Number(j, "latitude", s.latitudeDeg), -90.0f, 90.0f);
    s.longitudeDeg = std::clamp(Number(j, "longitude", s.longitudeDeg), -180.0f, 180.0f);
    s.utcOffsetHours = std::clamp(Number(j, "utcOffset", s.utcOffsetHours), -14.0f, 14.0f);
    s.northYawDeg = Number(j, "northYawDeg", s.northYawDeg);
    s.nightBrightness = std::clamp(Number(j, "nightBrightness", s.nightBrightness), 0.0f, 100000.0f);
    s.daySkyBrightness = std::clamp(Number(j, "daySkyBrightness", s.daySkyBrightness), 0.0f, 10.0f);
    s.nightExposureEv = std::clamp(Number(j, "nightExposureEv", s.nightExposureEv), -10.0f, 10.0f);
    s.nightCameraEv100 = std::clamp(Number(j, "nightCameraEv100", s.nightCameraEv100), -20.0f, 30.0f);
    s.nightCameraFollow = std::clamp(Number(j, "nightCameraFollow", s.nightCameraFollow), 0.0f, 1.0f);
    s.lightUpdateThresholdDeg = std::clamp(Number(j, "lightUpdateThresholdDeg", s.lightUpdateThresholdDeg), 0.0f, 5.0f);

    if (const auto it = j.find("sun"); it != j.end() && it->is_object())
    {
        ApplySun(*it, sun);
    }
    if (const auto it = j.find("moon"); it != j.end() && it->is_object())
    {
        const nlohmann::json& m = *it;
        s.moonEnabled = Flag(m, "enabled", s.moonEnabled);
        s.moonIlluminanceLux = std::clamp(Number(m, "illuminanceLux", s.moonIlluminanceLux), 0.0f, 1000.0f);
        s.moonNewBrightness = std::clamp(Number(m, "newMoonBrightness", s.moonNewBrightness), 0.0f, 1.0f);
        if (const auto lc = m.find("lightColor"); lc != m.end() && lc->is_array() && lc->size() >= 3)
        {
            for (int c = 0; c < 3; ++c)
            {
                s.moonLightColor[c] = (*lc)[c].is_number() ? std::max((*lc)[c].get<float>(), 0.0f) : s.moonLightColor[c];
            }
        }
        if (const auto dc = m.find("discColor"); dc != m.end() && dc->is_array() && dc->size() >= 3)
        {
            for (int c = 0; c < 3; ++c)
            {
                s.moonDiscColor[c] = (*dc)[c].is_number() ? std::max((*dc)[c].get<float>(), 0.0f) : s.moonDiscColor[c];
            }
        }
        if (const auto st = m.find("lightShaftBloomTint"); st != m.end() && st->is_array() && st->size() >= 3)
        {
            for (int c = 0; c < 3; ++c)
            {
                s.moonLightShaftTint[c] = (*st)[c].is_number() ? std::max((*st)[c].get<float>(), 0.0f) : s.moonLightShaftTint[c];
            }
        }
        s.moonAngleDeg = std::clamp(Number(m, "lightSourceAngle", s.moonAngleDeg), 0.0f, 20.0f);
        s.moonSoftAngleDeg = std::clamp(Number(m, "lightSourceSoftAngle", s.moonSoftAngleDeg), 0.0f, 20.0f);
        s.moonGlow = std::clamp(Number(m, "glow", s.moonGlow), 0.0f, 10.0f);
        s.moonGlowRadiusDeg = std::clamp(Number(m, "glowRadiusDeg", s.moonGlowRadiusDeg), 0.1f, 90.0f);
        s.moonDiscAdaptation = std::clamp(Number(m, "discAdaptation", s.moonDiscAdaptation), 0.0f, 1.0f);
        // The moon's own UE bloom properties, named as the sun's are (DirectionalLightJson).
        s.moonLightShaftBloomScale = std::clamp(Number(m, "lightShaftBloomScale", s.moonLightShaftBloomScale), 0.0f, 10.0f);
        s.moonLightShaftBloomThreshold = std::clamp(Number(m, "lightShaftBloomThreshold", s.moonLightShaftBloomThreshold), 0.0f, 50.0f);
        s.moonLightShaftBloomMaxBrightness =
            std::clamp(Number(m, "lightShaftBloomMaxBrightness", s.moonLightShaftBloomMaxBrightness), 0.0f, 1000.0f);
        s.moonGlintPhaseFalloff = std::clamp(Number(m, "glintPhaseFalloff", s.moonGlintPhaseFalloff), 0.0f, 1.0f);
        s.moonEarthshine = std::clamp(Number(m, "earthshine", s.moonEarthshine), 0.0f, 100.0f);
        s.moonPathYawDeg = Number(m, "pathYawDeg", s.moonPathYawDeg);
        s.moonPathTiltDeg = std::clamp(Number(m, "pathTiltDeg", s.moonPathTiltDeg), -89.0f, 89.0f);
        s.moonOrbitOffsetDeg = Number(m, "orbitOffsetDeg", s.moonOrbitOffsetDeg);
        s.moonPhase = Number(m, "phase", s.moonPhase);
        s.moonPhase = s.moonPhase - std::floor(s.moonPhase);
        s.moonPhaseAuto = Flag(m, "phaseAuto", s.moonPhaseAuto);
    }
    // N2: the night sky's layers.
    if (const auto it = j.find("nightSky"); it != j.end() && it->is_object())
    {
        const nlohmann::json& n = *it;
        s.nightSkyEnabled = Flag(n, "enabled", s.nightSkyEnabled);
        s.airglowZenith = std::clamp(Number(n, "airglowZenith", s.airglowZenith), 0.0f, 1.0f);
        if (const auto ac = n.find("airglowColor"); ac != n.end() && ac->is_array() && ac->size() >= 3)
        {
            for (int c = 0; c < 3; ++c)
            {
                s.airglowColor[c] = (*ac)[c].is_number() ? std::max((*ac)[c].get<float>(), 0.0f) : s.airglowColor[c];
            }
        }
        s.starsBrightness = std::clamp(Number(n, "stars", s.starsBrightness), 0.0f, 1000.0f);
        s.milkyWayBrightness = std::clamp(Number(n, "milkyWay", s.milkyWayBrightness), 0.0f, 1000.0f);
        s.starTwinkle = std::clamp(Number(n, "twinkle", s.starTwinkle), 0.0f, 1.0f);
        s.siderealAtMidnightHours = Number(n, "siderealAtMidnight", s.siderealAtMidnightHours);
    }
    // Plan L: the night lamps' switch.
    if (const auto it = j.find("lamps"); it != j.end() && it->is_object())
    {
        const nlohmann::json& l = *it;
        s.lampSunElevationDeg = std::clamp(Number(l, "sunElevationDeg", s.lampSunElevationDeg), -90.0f, 90.0f);
        s.lampRandomDelaySec = std::clamp(Number(l, "randomDelaySec", s.lampRandomDelaySec), 0.0f, 600.0f);
        s.lampTurnOnSec = std::clamp(Number(l, "turnOnSec", s.lampTurnOnSec), 0.0f, 600.0f);
        s.lampTurnOnFlicker = std::clamp(Number(l, "turnOnFlicker", s.lampTurnOnFlicker), 0.0f, 1.0f);
        s.lampTurnOnExponent = std::clamp(Number(l, "turnOnExponent", s.lampTurnOnExponent), 0.01f, 10.0f);
        s.lampDayBrightness = std::clamp(Number(l, "dayBrightness", s.lampDayBrightness), 0.0f, 100000.0f);
    }
    // The height fog's density by the sun's height (multipliers on heightFog.density).
    if (const auto it = j.find("fog"); it != j.end() && it->is_object())
    {
        const nlohmann::json& f = *it;
        s.fogDayDensity = std::clamp(Number(f, "dayDensity", s.fogDayDensity), 0.0f, 100.0f);
        s.fogDuskDensity = std::clamp(Number(f, "duskDensity", s.fogDuskDensity), 0.0f, 100.0f);
        s.fogNightDensity = std::clamp(Number(f, "nightDensity", s.fogNightDensity), 0.0f, 100.0f);
        s.fogDayFullDeg = std::clamp(Number(f, "dayFullDeg", s.fogDayFullDeg), -90.0f, 90.0f);
        s.fogDuskDeg = std::clamp(Number(f, "duskDeg", s.fogDuskDeg), -90.0f, 90.0f);
        s.fogNightFullDeg = std::clamp(Number(f, "nightFullDeg", s.fogNightFullDeg), -90.0f, 90.0f);
    }
    // The twilight as Ultra Dynamic Sky draws it: the sun leaving the atmosphere, the night Rayleigh.
    if (const auto it = j.find("twilight"); it != j.end() && it->is_object())
    {
        const nlohmann::json& w = *it;
        s.sunSkyStartDeg = std::clamp(Number(w, "sunSkyStartDeg", s.sunSkyStartDeg), -90.0f, 90.0f);
        s.sunSkyFullDeg = std::clamp(Number(w, "sunSkyFullDeg", s.sunSkyFullDeg), -90.0f, 90.0f);
        if (const auto rt = w.find("rayleighNightTint"); rt != w.end() && rt->is_array() && rt->size() >= 3)
        {
            for (int c = 0; c < 3; ++c)
            {
                s.rayleighNightTint[c] = (*rt)[c].is_number() ? std::clamp((*rt)[c].get<float>(), 0.0f, 10.0f)
                                                              : s.rayleighNightTint[c];
            }
        }
        s.rayleighDayStartDeg = std::clamp(Number(w, "rayleighDayStartDeg", s.rayleighDayStartDeg), -90.0f, 90.0f);
        s.rayleighDayFullDeg = std::clamp(Number(w, "rayleighDayFullDeg", s.rayleighDayFullDeg), -90.0f, 90.0f);
        const auto tint3 = [&w](const char* key, float* out) {
            if (const auto t = w.find(key); t != w.end() && t->is_array() && t->size() >= 3)
            {
                for (int c = 0; c < 3; ++c)
                {
                    out[c] = (*t)[c].is_number() ? std::clamp((*t)[c].get<float>(), 0.0f, 20.0f) : out[c];
                }
            }
        };
        tint3("ozoneNightTint", s.ozoneNightTint);
        tint3("ozoneSunriseTint", s.ozoneSunriseTint);
        s.ozoneBlendDeg = std::clamp(Number(w, "ozoneBlendDeg", s.ozoneBlendDeg), 0.001f, 90.0f);
        s.lightHandOverDeg = std::clamp(Number(w, "lightHandOverDeg", s.lightHandOverDeg), 0.0f, 20.0f);
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
    nlohmann::json j = nlohmann::json::object();
    j["time"] = s.timeHours;
    j["animate"] = s.animate;
    j["dayLengthMin"] = s.dayLengthMin;
    j["nightLengthMin"] = s.nightLengthMin;
    j["timeScale"] = s.timeScale;
    j["realPositions"] = s.realPositions;
    j["dawn"] = s.dawnHours;
    j["dusk"] = s.duskHours;
    j["pathYawDeg"] = s.pathYawDeg;
    j["pathTiltDeg"] = s.pathTiltDeg;
    j["heightOffset"] = s.heightOffset;
    j["extendTwilight"] = s.extendTwilight;
    j["date"] = nlohmann::json::array({ s.year, s.month, s.day });
    j["latitude"] = s.latitudeDeg;
    j["longitude"] = s.longitudeDeg;
    j["utcOffset"] = s.utcOffsetHours;
    j["northYawDeg"] = s.northYawDeg;
    j["nightBrightness"] = s.nightBrightness;
    j["daySkyBrightness"] = s.daySkyBrightness;
    j["nightExposureEv"] = s.nightExposureEv;
    j["nightCameraEv100"] = s.nightCameraEv100;
    j["nightCameraFollow"] = s.nightCameraFollow;
    j["lightUpdateThresholdDeg"] = s.lightUpdateThresholdDeg;
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
        { "lightShaftGlareSeed", sun.GetLightShaftGlareSeed() },
        { "lightSourceAngle", sun.GetLightSourceAngleDegrees() },
        { "lightSourceSoftAngle", sun.GetLightSourceSoftAngleDegrees() },
    };
    j["moon"] = {
        { "enabled", s.moonEnabled },
        { "illuminanceLux", s.moonIlluminanceLux },
        { "newMoonBrightness", s.moonNewBrightness },
        { "lightColor", { s.moonLightColor[0], s.moonLightColor[1], s.moonLightColor[2] } },
        { "discColor", { s.moonDiscColor[0], s.moonDiscColor[1], s.moonDiscColor[2] } },
        { "glow", s.moonGlow },
        { "glowRadiusDeg", s.moonGlowRadiusDeg },
        { "lightShaftBloomScale", s.moonLightShaftBloomScale },
        { "lightShaftBloomThreshold", s.moonLightShaftBloomThreshold },
        { "lightShaftBloomMaxBrightness", s.moonLightShaftBloomMaxBrightness },
        { "lightShaftBloomTint", { s.moonLightShaftTint[0], s.moonLightShaftTint[1], s.moonLightShaftTint[2] } },
        { "lightSourceAngle", s.moonAngleDeg },
        { "lightSourceSoftAngle", s.moonSoftAngleDeg },
        { "discAdaptation", s.moonDiscAdaptation },
        { "glintPhaseFalloff", s.moonGlintPhaseFalloff },
        { "earthshine", s.moonEarthshine },
        { "pathYawDeg", s.moonPathYawDeg },
        { "pathTiltDeg", s.moonPathTiltDeg },
        { "orbitOffsetDeg", s.moonOrbitOffsetDeg },
        { "phase", s.moonPhase },
        { "phaseAuto", s.moonPhaseAuto },
    };
    j["nightSky"] = {
        { "enabled", s.nightSkyEnabled },
        { "airglowZenith", s.airglowZenith },
        { "airglowColor", { s.airglowColor[0], s.airglowColor[1], s.airglowColor[2] } },
        { "stars", s.starsBrightness },
        { "milkyWay", s.milkyWayBrightness },
        { "twinkle", s.starTwinkle },
        { "siderealAtMidnight", s.siderealAtMidnightHours },
    };
    j["twilight"] = {
        { "sunSkyStartDeg", s.sunSkyStartDeg },
        { "sunSkyFullDeg", s.sunSkyFullDeg },
        { "rayleighNightTint", { s.rayleighNightTint[0], s.rayleighNightTint[1], s.rayleighNightTint[2] } },
        { "rayleighDayStartDeg", s.rayleighDayStartDeg },
        { "rayleighDayFullDeg", s.rayleighDayFullDeg },
        { "ozoneNightTint", { s.ozoneNightTint[0], s.ozoneNightTint[1], s.ozoneNightTint[2] } },
        { "ozoneSunriseTint", { s.ozoneSunriseTint[0], s.ozoneSunriseTint[1], s.ozoneSunriseTint[2] } },
        { "ozoneBlendDeg", s.ozoneBlendDeg },
        { "lightHandOverDeg", s.lightHandOverDeg },
    };
    j["lamps"] = {
        { "sunElevationDeg", s.lampSunElevationDeg },
        { "randomDelaySec", s.lampRandomDelaySec },
        { "turnOnSec", s.lampTurnOnSec },
        { "turnOnFlicker", s.lampTurnOnFlicker },
        { "turnOnExponent", s.lampTurnOnExponent },
        { "dayBrightness", s.lampDayBrightness },
    };
    j["fog"] = {
        { "dayDensity", s.fogDayDensity },
        { "duskDensity", s.fogDuskDensity },
        { "nightDensity", s.fogNightDensity },
        { "dayFullDeg", s.fogDayFullDeg },
        { "duskDeg", s.fogDuskDeg },
        { "nightFullDeg", s.fogNightFullDeg },
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
