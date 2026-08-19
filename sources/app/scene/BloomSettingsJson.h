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
    s.method = j.value("method", s.method);
    s.convBlades = j.value("convBlades", s.convBlades);
    s.convBladeRotation = j.value("convBladeRotation", s.convBladeRotation);
    s.convKernelRadius = j.value("convKernelRadius", s.convKernelRadius);
    s.convSpokeStrength = j.value("convSpokeStrength", s.convSpokeStrength);
    s.convSpokeLength = j.value("convSpokeLength", s.convSpokeLength);
    s.convSpokeWidth = j.value("convSpokeWidth", s.convSpokeWidth);
    s.convAnamorphic = j.value("convAnamorphic", s.convAnamorphic);
    s.convAnamorphicLength = j.value("convAnamorphicLength", s.convAnamorphicLength);
    s.convChroma = j.value("convChroma", s.convChroma);
    s.convGhosts = j.value("convGhosts", s.convGhosts);
    s.convGhostSpacing = j.value("convGhostSpacing", s.convGhostSpacing);
    s.convGhostBokeh = j.value("convGhostBokeh", s.convGhostBokeh);
    s.convGhostIntensity = j.value("convGhostIntensity", s.convGhostIntensity);
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
    j["method"] = s.method;
    j["convBlades"] = s.convBlades;
    j["convBladeRotation"] = s.convBladeRotation;
    j["convKernelRadius"] = s.convKernelRadius;
    j["convSpokeStrength"] = s.convSpokeStrength;
    j["convSpokeLength"] = s.convSpokeLength;
    j["convSpokeWidth"] = s.convSpokeWidth;
    j["convAnamorphic"] = s.convAnamorphic;
    j["convAnamorphicLength"] = s.convAnamorphicLength;
    j["convChroma"] = s.convChroma;
    j["convGhosts"] = s.convGhosts;
    j["convGhostSpacing"] = s.convGhostSpacing;
    j["convGhostBokeh"] = s.convGhostBokeh;
    j["convGhostIntensity"] = s.convGhostIntensity;
    return j;
}

} // namespace BloomSettingsJson
