#pragma once

#include "third_party/json/json.hpp"

#include "app/scene/SceneFrameData.h"

// P7: the ONE mapping between AtmosphereSettings and JSON. The level reader, the editor's
// environment object and the editor's runtime-apply path all go through these, so a field added to
// the struct cannot end up silently missing from one of the three. Same shape and same reasoning as
// GtaoSettingsJson next door -- including living in its own header rather than in SceneFrameData.h,
// which most of the renderer includes and which should not drag the single-header JSON parser in
// front of every one of those translation units.
namespace AtmosphereSettingsJson
{

inline void ApplyOverrides(const nlohmann::json& j, AtmosphereSettings& s)
{
    if (!j.is_object())
    {
        return;
    }
    s.enabled = j.value("enabled", s.enabled);
    s.density = j.value("density", s.density);
    s.heightFalloff = j.value("heightFalloff", s.heightFalloff);
    s.referenceHeight = j.value("referenceHeight", s.referenceHeight);
    s.startDistance = j.value("startDistance", s.startDistance);
    s.maxOpacity = j.value("maxOpacity", s.maxOpacity);
    s.sunScatterStrength = j.value("sunScatterStrength", s.sunScatterStrength);
    s.sunScatterExponent = j.value("sunScatterExponent", s.sunScatterExponent);
    s.sunScatterStartDistance = j.value("sunScatterStartDistance", s.sunScatterStartDistance);
}

inline nlohmann::json ToJson(const AtmosphereSettings& s)
{
    nlohmann::json j;
    j["enabled"] = s.enabled;
    j["density"] = s.density;
    j["heightFalloff"] = s.heightFalloff;
    j["referenceHeight"] = s.referenceHeight;
    j["startDistance"] = s.startDistance;
    j["maxOpacity"] = s.maxOpacity;
    j["sunScatterStrength"] = s.sunScatterStrength;
    j["sunScatterExponent"] = s.sunScatterExponent;
    j["sunScatterStartDistance"] = s.sunScatterStartDistance;
    return j;
}

} // namespace AtmosphereSettingsJson
