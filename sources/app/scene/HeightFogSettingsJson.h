#pragma once

#include "third_party/json/json.hpp"

#include "core/schema/KeyTable.h" // R9.2: the section's table of keys

#include "rendering/scene/SceneFrameData.h"

// P7: the ONE mapping between HeightFogSettings and JSON. The level reader, the editor's
// environment object and the editor's runtime-apply path all go through these, so a field added to
// the struct cannot end up silently missing from one of the three. Same shape and same reasoning as
// GtaoSettingsJson next door -- including living in its own header rather than in SceneFrameData.h,
// which most of the renderer includes and which should not drag the single-header JSON parser in
// front of every one of those translation units.
namespace HeightFogSettingsJson
{

// Architecture plan R9.2: the section's table of keys (core/schema/KeyTable.h); reading and writing walk it.
inline const schema::Table<HeightFogSettings>& Keys()
{
    using S = HeightFogSettings;
    static const schema::Table<S> table{ {
        schema::Field("enabled", &S::enabled),
        schema::Field("density", &S::density),
        schema::Field("heightFalloff", &S::heightFalloff),
        schema::Field("referenceHeight", &S::referenceHeight),
        schema::Field("startDistance", &S::startDistance),
        schema::Field("maxOpacity", &S::maxOpacity),
        schema::Field("sunScatterStrength", &S::sunScatterStrength),
        schema::Field("sunScatterExponent", &S::sunScatterExponent),
        schema::Field("sunScatterStartDistance", &S::sunScatterStartDistance),
        schema::Field("skyBlur", &S::skyBlur),
        schema::Field("nonDirectionalDistance", &S::nonDirectionalDistance),
        schema::Field("fullyDirectionalDistance", &S::fullyDirectionalDistance),
        schema::Field("inscatteringLuminance", &S::inscatteringLuminance),
        schema::Field("skyAmbientContribution", &S::skyAmbientContribution),
        schema::Field("sunScatterColor", &S::sunScatterColor),
        schema::Field("volumetric", &S::volumetric),
        schema::Field("volumetricDistance", &S::volumetricDistance),
        schema::Field("volumetricStartDistance", &S::volumetricStartDistance),
        schema::Field("volumetricNearFadeInDistance", &S::volumetricNearFadeInDistance),
        schema::Field("albedo", &S::albedo),
        schema::Field("extinctionScale", &S::extinctionScale),
        schema::Field("phaseG", &S::phaseG),
        schema::Field("sunScatter", &S::sunScatter),
        schema::Field("skyScatter", &S::skyScatter),
        schema::Field("historyWeight", &S::historyWeight),
        schema::Field("temporal", &S::temporal),
        schema::Field("jitter", &S::jitter),
        schema::Field("samplesPerCell", &S::samplesPerCell),
        schema::Field("conservativeDepth", &S::conservativeDepth),
        schema::Field("localLights", &S::localLights),
        schema::Field("localLightScatter", &S::localLightScatter),
        schema::Field("localSoftFading", &S::localSoftFading),
        schema::Field("localDistanceBias", &S::localDistanceBias),
    } };
    return table;
}

inline void ApplyOverrides(const nlohmann::json& j, HeightFogSettings& s)
{
    Keys().Read(j, s);
}

inline nlohmann::json ToJson(const HeightFogSettings& s)
{
    return Keys().Write(s);
}

} // namespace HeightFogSettingsJson
