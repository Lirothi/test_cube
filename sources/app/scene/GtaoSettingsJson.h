#pragma once

#include "third_party/json/json.hpp"

#include "app/scene/SceneFrameData.h"

// P6B: the ONE mapping between GtaoSettings and JSON. The level reader, the editor's environment
// object and the editor's runtime-apply path all go through these, so a field added to the struct
// cannot end up silently missing from one of the three.
//
// Deliberately its own header rather than living in SceneFrameData.h: that one is included by most
// of the renderer, and pulling nlohmann/json into it puts the whole single-header JSON parser in
// front of every one of those translation units. Same reason PhotographicSettingsJson.h exists
// separately from PhotographicSettings.h.
namespace GtaoSettingsJson
{

inline void ApplyOverrides(const nlohmann::json& j, GtaoSettings& s)
{
    if (!j.is_object())
    {
        return;
    }
    s.enabled = j.value("enabled", s.enabled);
    s.worldRadius = j.value("worldRadius", s.worldRadius);
    s.thickness = j.value("thickness", s.thickness);
    s.intensity = j.value("intensity", s.intensity);
    s.strength = j.value("strength", s.strength);
    s.fadeStart = j.value("fadeStart", s.fadeStart);
    s.fadeEnd = j.value("fadeEnd", s.fadeEnd);
    s.numAngles = j.value("numAngles", s.numAngles);
    s.numSteps = j.value("numSteps", s.numSteps);
    s.denoise = j.value("denoise", s.denoise);
    s.temporal = j.value("temporal", s.temporal);
    s.filterRadius = j.value("filterRadius", s.filterRadius);
    s.filterPlaneTolerance = j.value("filterPlaneTolerance", s.filterPlaneTolerance);
    s.temporalBlendWeight = j.value("temporalBlendWeight", s.temporalBlendWeight);
    s.temporalClampRange = j.value("temporalClampRange", s.temporalClampRange);
    s.upsampleTolerance = j.value("upsampleTolerance", s.upsampleTolerance);
    s.useGBufferNormal = j.value("useGBufferNormal", s.useGBufferNormal);
    s.useHzb = j.value("useHzb", s.useHzb);
    s.hzbMipBias = j.value("hzbMipBias", s.hzbMipBias);
}

inline nlohmann::json ToJson(const GtaoSettings& s)
{
    nlohmann::json j;
    j["enabled"] = s.enabled;
    j["worldRadius"] = s.worldRadius;
    j["thickness"] = s.thickness;
    j["intensity"] = s.intensity;
    j["strength"] = s.strength;
    j["fadeStart"] = s.fadeStart;
    j["fadeEnd"] = s.fadeEnd;
    j["numAngles"] = s.numAngles;
    j["numSteps"] = s.numSteps;
    j["denoise"] = s.denoise;
    j["temporal"] = s.temporal;
    j["filterRadius"] = s.filterRadius;
    j["filterPlaneTolerance"] = s.filterPlaneTolerance;
    j["temporalBlendWeight"] = s.temporalBlendWeight;
    j["temporalClampRange"] = s.temporalClampRange;
    j["upsampleTolerance"] = s.upsampleTolerance;
    j["useGBufferNormal"] = s.useGBufferNormal;
    j["useHzb"] = s.useHzb;
    j["hzbMipBias"] = s.hzbMipBias;
    return j;
}

} // namespace GtaoSettingsJson
