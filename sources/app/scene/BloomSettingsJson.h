#pragma once

#include "third_party/json/json.hpp"

#include "app/scene/SceneFrameData.h"

// P8: the ONE mapping between BloomSettings and JSON. The level reader, the editor's environment
// object and the editor's runtime-apply path all go through these, so a field added to the struct
// cannot end up silently missing from one of the three. Same shape and same reasoning as
// AtmosphereSettingsJson and GtaoSettingsJson next door -- including living in its own header
// rather than in SceneFrameData.h, which most of the renderer includes and which should not drag
// the single-header JSON parser in front of every one of those translation units.
namespace BloomSettingsJson
{

inline void ApplyOverrides(const nlohmann::json& j, BloomSettings& s)
{
    if (!j.is_object())
    {
        return;
    }
    s.enabled = j.value("enabled", s.enabled);
    s.intensity = j.value("intensity", s.intensity);
    s.threshold = j.value("threshold", s.threshold);
    s.softKnee = j.value("softKnee", s.softKnee);
    s.radius = j.value("radius", s.radius);
    s.fireflyClamp = j.value("fireflyClamp", s.fireflyClamp);
}

inline nlohmann::json ToJson(const BloomSettings& s)
{
    nlohmann::json j;
    j["enabled"] = s.enabled;
    j["intensity"] = s.intensity;
    j["threshold"] = s.threshold;
    j["softKnee"] = s.softKnee;
    j["radius"] = s.radius;
    j["fireflyClamp"] = s.fireflyClamp;
    return j;
}

} // namespace BloomSettingsJson
