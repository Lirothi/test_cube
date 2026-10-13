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
//     "moon": { "enabled": true, "illuminanceLux": 0.32, "temperatureK": 4300, "lightSourceAngle": 0.52,
//               "pathYawDeg": 180, "pathTiltDeg": 35, "orbitOffsetDeg": 0, "phase": 0.5, "phaseAuto": true }
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
    if (const auto it = j.find("date"); it != j.end() && it->is_array() && it->size() >= 3 &&
        (*it)[0].is_number_integer() && (*it)[1].is_number_integer() && (*it)[2].is_number_integer())
    {
        s.year = std::clamp((*it)[0].get<int>(), 1, 9999);
        s.month = std::clamp((*it)[1].get<int>(), 1, 12);
        s.day = std::clamp((*it)[2].get<int>(), 1, 31);
    }
    s.latitudeDeg = std::clamp(Number(j, "latitude", s.latitudeDeg), -90.0f, 90.0f);
    s.longitudeDeg = std::clamp(Number(j, "longitude", s.longitudeDeg), -180.0f, 180.0f);
    s.utcOffsetHours = std::clamp(Number(j, "utcOffset", s.utcOffsetHours), -14.0f, 14.0f);
    s.northYawDeg = Number(j, "northYawDeg", s.northYawDeg);
    s.nightBrightness = std::clamp(Number(j, "nightBrightness", s.nightBrightness), 0.0f, 1000.0f);
    s.lightUpdateThresholdDeg = std::clamp(Number(j, "lightUpdateThresholdDeg", s.lightUpdateThresholdDeg), 0.0f, 5.0f);

    if (const auto it = j.find("sun"); it != j.end() && it->is_object())
    {
        DirectionalLightJson::ApplyOverrides(*it, sun);
        // The section's own default is the time of day's sun, not the P16.2 field's 100 klx.
        if (!it->contains("sunIlluminanceLux") && !it->contains("sunIntensity") && !it->contains("exposure"))
        {
            sun.SetSunIlluminanceLux(DefaultSun().GetSunIlluminanceLux());
        }
    }
    if (const auto it = j.find("moon"); it != j.end() && it->is_object())
    {
        const nlohmann::json& m = *it;
        s.moonEnabled = Flag(m, "enabled", s.moonEnabled);
        s.moonIlluminanceLux = std::clamp(Number(m, "illuminanceLux", s.moonIlluminanceLux), 0.0f, 1000.0f);
        s.moonTemperatureK = std::clamp(Number(m, "temperatureK", s.moonTemperatureK), 1000.0f, 20000.0f);
        s.moonAngleDeg = std::clamp(Number(m, "lightSourceAngle", s.moonAngleDeg), 0.0f, 20.0f);
        s.moonPathYawDeg = Number(m, "pathYawDeg", s.moonPathYawDeg);
        s.moonPathTiltDeg = std::clamp(Number(m, "pathTiltDeg", s.moonPathTiltDeg), -89.0f, 89.0f);
        s.moonOrbitOffsetDeg = Number(m, "orbitOffsetDeg", s.moonOrbitOffsetDeg);
        s.moonPhase = Number(m, "phase", s.moonPhase);
        s.moonPhase = s.moonPhase - std::floor(s.moonPhase);
        s.moonPhaseAuto = Flag(m, "phaseAuto", s.moonPhaseAuto);
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

} // namespace TimeOfDaySettingsJson
