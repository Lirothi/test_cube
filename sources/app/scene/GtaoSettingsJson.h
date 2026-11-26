#pragma once

#include "third_party/json/json.hpp"

#include "core/schema/KeyTable.h"
#include "rendering/scene/SceneFrameData.h"

// P6B: the ONE mapping between GtaoSettings and JSON. The level reader, the editor's environment
// object and the editor's runtime-apply path all go through these, so a field added to the struct
// cannot end up silently missing from one of the three.
//
// Deliberately its own header rather than living in SceneFrameData.h: that one is included by most
// of the renderer, and pulling nlohmann/json into it puts the whole single-header JSON parser in
// front of every one of those translation units. Same reason PhotographicSettingsJson.h exists
// separately from PhotographicSettings.h.
//
// Architecture plan R9.2: the mapping is the section's table of keys (core/schema/KeyTable.h); reading and writing
// walk it.
namespace GtaoSettingsJson
{

inline const schema::Table<GtaoSettings>& Keys()
{
    using S = GtaoSettings;
    static const schema::Table<S> table{ {
        schema::Field("enabled", &S::enabled),
        schema::Field("worldRadius", &S::worldRadius),
        schema::Field("thickness", &S::thickness),
        schema::Field("intensity", &S::intensity),
        schema::Field("strength", &S::strength),
        schema::Field("fadeStart", &S::fadeStart),
        schema::Field("fadeEnd", &S::fadeEnd),
        schema::Field("numAngles", &S::numAngles),
        schema::Field("numSteps", &S::numSteps),
        schema::Field("denoise", &S::denoise),
        schema::Field("temporal", &S::temporal),
        schema::Field("filterRadius", &S::filterRadius),
        schema::Field("filterPlaneTolerance", &S::filterPlaneTolerance),
        schema::Field("temporalBlendWeight", &S::temporalBlendWeight),
        schema::Field("temporalClampRange", &S::temporalClampRange),
        schema::Field("upsampleTolerance", &S::upsampleTolerance),
        schema::Field("useGBufferNormal", &S::useGBufferNormal),
        schema::Field("useHzb", &S::useHzb),
        schema::Field("hzbMipBias", &S::hzbMipBias),
        schema::Field("skyRadius", &S::skyRadius),
        schema::Field("skyMipBias", &S::skyMipBias),
        schema::Field("skyIntensity", &S::skyIntensity),
    } };
    return table;
}

inline void ApplyOverrides(const nlohmann::json& j, GtaoSettings& s)
{
    Keys().Read(j, s);
}

inline nlohmann::json ToJson(const GtaoSettings& s)
{
    return Keys().Write(s);
}

} // namespace GtaoSettingsJson
