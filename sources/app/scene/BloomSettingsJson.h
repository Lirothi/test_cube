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
    // P8C-2: the aperture-kernel fields (convKernelRadius, the spoke set, convChroma, the
    // convAnamorphic squeeze, convGhostSpacing) are GONE, not renamed -- a level that still
    // carries them keeps rendering, the keys are simply ignored. P8C-2d retired
    // `convBladeRotation` the same way, for being inert against the noise floor.
    s.convSize = j.value("convSize", s.convSize);
    s.convPercent = j.value("convPercent", s.convPercent);
    s.convBlades = j.value("convBlades", s.convBlades);
    s.convAnamorphicIntensity = j.value("convAnamorphicIntensity", s.convAnamorphicIntensity);
    s.convAnamorphicLength = j.value("convAnamorphicLength", s.convAnamorphicLength);
    s.convAnamorphicWidth = j.value("convAnamorphicWidth", s.convAnamorphicWidth);
    s.convAnamorphicThreshold = j.value("convAnamorphicThreshold", s.convAnamorphicThreshold);
    s.convAnamorphicNarrow = j.value("convAnamorphicNarrow", s.convAnamorphicNarrow);
    s.convAnamorphicChroma = j.value("convAnamorphicChroma", s.convAnamorphicChroma);
    if (j.contains("convAnamorphicTint") && j["convAnamorphicTint"].is_array() &&
        j["convAnamorphicTint"].size() == 3)
    {
        for (int i = 0; i < 3; ++i)
        {
            s.convAnamorphicTint[i] = j["convAnamorphicTint"][i].get<float>();
        }
    }
    s.convGhosts = j.value("convGhosts", s.convGhosts);
    s.convGhostBokeh = j.value("convGhostBokeh", s.convGhostBokeh);
    s.convGhostIntensity = j.value("convGhostIntensity", s.convGhostIntensity);
    s.convGhostThreshold = j.value("convGhostThreshold", s.convGhostThreshold);
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
    j["convSize"] = s.convSize;
    j["convPercent"] = s.convPercent;
    j["convBlades"] = s.convBlades;
    j["convAnamorphicIntensity"] = s.convAnamorphicIntensity;
    j["convAnamorphicLength"] = s.convAnamorphicLength;
    j["convAnamorphicWidth"] = s.convAnamorphicWidth;
    j["convAnamorphicThreshold"] = s.convAnamorphicThreshold;
    j["convAnamorphicNarrow"] = s.convAnamorphicNarrow;
    j["convAnamorphicChroma"] = s.convAnamorphicChroma;
    j["convAnamorphicTint"] = { s.convAnamorphicTint[0], s.convAnamorphicTint[1],
                                s.convAnamorphicTint[2] };
    j["convGhosts"] = s.convGhosts;
    j["convGhostBokeh"] = s.convGhostBokeh;
    j["convGhostIntensity"] = s.convGhostIntensity;
    j["convGhostThreshold"] = s.convGhostThreshold;
    return j;
}

} // namespace BloomSettingsJson
