#pragma once

#include <algorithm>

#include "third_party/json/json.hpp"

#include "core/schema/KeyTable.h" // R9.2: the section's table of keys

#include "rendering/scene/SceneFrameData.h"

// P8: the ONE mapping between BloomSettings and JSON. The level reader, the editor's environment
// object and the editor's runtime-apply path all go through these, so a field added to the struct
// cannot end up silently missing from one of the three. Same shape and same reasoning as
// HeightFogSettingsJson and GtaoSettingsJson next door -- including living in its own header
// rather than in SceneFrameData.h, which most of the renderer includes and which should not drag
// the single-header JSON parser in front of every one of those translation units.
namespace BloomSettingsJson
{

// Architecture plan R9.2: the section's table of keys (core/schema/KeyTable.h); reading and writing walk it.
inline const schema::Table<BloomSettings>& Keys()
{
    using S = BloomSettings;
    static const schema::Table<S> table{ {
        schema::Field("convKernelTint", &S::convKernelTint),
        schema::Field("convAnamorphicTint", &S::convAnamorphicTint),
        schema::Field("enabled", &S::enabled),
        schema::Field("intensity", &S::intensity),
        schema::Field("threshold", &S::threshold),
        schema::Field("softKnee", &S::softKnee),
        schema::Field("radius", &S::radius),
        schema::Field("fireflyClamp", &S::fireflyClamp),
        schema::Field("method", &S::method),
        schema::Field("convKernel", &S::convKernel),
        schema::Field("convPreFilterMin", &S::convPreFilterMin),
        schema::Field("convPreFilterMax", &S::convPreFilterMax),
        schema::Field("convPreFilterMult", &S::convPreFilterMult),
        schema::Field("convSize", &S::convSize),
        schema::Field("convPercent", &S::convPercent),
        schema::Field("convBeforeUpscale", &S::convBeforeUpscale),
        schema::Field("convTemporalWeight", &S::convTemporalWeight).Clamp(0.01f, 1.0f),
        schema::Field("convSunKernelScale", &S::convSunKernelScale).Clamp(0.0f, 10.0f),
        schema::Field("convSunGhostScale", &S::convSunGhostScale).Clamp(0.0f, 10.0f),
        schema::Field("convSunSplitRadiusDeg", &S::convSunSplitRadiusDeg).Clamp(0.1f, 45.0f),
        schema::Field("convBlades", &S::convBlades),
        schema::Field("convAnamorphicIntensity", &S::convAnamorphicIntensity),
        schema::Field("convAnamorphicLength", &S::convAnamorphicLength),
        schema::Field("convAnamorphicWidth", &S::convAnamorphicWidth),
        schema::Field("convAnamorphicThreshold", &S::convAnamorphicThreshold),
        schema::Field("convAnamorphicChroma", &S::convAnamorphicChroma),
        schema::Field("convGhosts", &S::convGhosts),
        schema::Field("convGhostBokeh", &S::convGhostBokeh),
        schema::Field("convGhostIntensity", &S::convGhostIntensity),
        schema::Field("convGhostThreshold", &S::convGhostThreshold),
        schema::Field("sunGlareIntensity", &S::sunGlareIntensity).AtLeast(0.0f),
        schema::Field("sunGlareRadiusDeg", &S::sunGlareRadiusDeg).AtLeast(0.05f),
        schema::Field("sunGlareVeil", &S::sunGlareVeil).AtLeast(0.0f),
        schema::Field("sunGlareVeilRadiusDeg", &S::sunGlareVeilRadiusDeg).AtLeast(0.1f),
        schema::Field("sunGlareSmoothingSec", &S::sunGlareSmoothingSec).Clamp(0.0f, 2.0f),
        schema::Field("sunGlareLowSunDeg", &S::sunGlareLowSunDeg).Clamp(0.0f, 90.0f),
        schema::Field("sunGlareLowSunFloor", &S::sunGlareLowSunFloor).Clamp(0.0f, 1.0f),
        schema::Field("sunRaysIntensity", &S::sunRaysIntensity).AtLeast(0.0f),
        schema::Field("sunRaysCount", &S::sunRaysCount).Clamp(16.0f, 2000.0f),
        schema::Field("sunRaysBundles", &S::sunRaysBundles).Clamp(1.0f, 128.0f),
        schema::Field("sunRaysBundleWidthDeg", &S::sunRaysBundleWidthDeg).Clamp(0.1f, 360.0f),
        schema::Field("sunRaysBundleStrength", &S::sunRaysBundleStrength).Clamp(0.0f, 1.0f),
        schema::Field("sunRaysLengthDeg", &S::sunRaysLengthDeg).AtLeast(0.1f),
        schema::Field("sunRaysWidthDeg", &S::sunRaysWidthDeg).Clamp(0.005f, 2.0f),
        schema::Field("sunRaysHalo", &S::sunRaysHalo).AtLeast(0.0f),
        schema::Field("sunRaysHaloRadiusDeg", &S::sunRaysHaloRadiusDeg).AtLeast(0.1f),
        schema::Field("sunRaysTexture", &S::sunRaysTexture),
        schema::Field("sunRaysTextureSizeDeg", &S::sunRaysTextureSizeDeg).AtLeast(0.5f),
        schema::Field("sunRaysSpikes", &S::sunRaysSpikes).AtLeast(0.0f),
        schema::Field("sunRaysSpikeCount", &S::sunRaysSpikeCount).Clamp(2.0f, 32.0f),
        schema::Field("sunRaysSpikeWidthDeg", &S::sunRaysSpikeWidthDeg).Clamp(0.005f, 2.0f),
        schema::Field("sunRaysSpikeLengthDeg", &S::sunRaysSpikeLengthDeg).Clamp(0.1f, 60.0f),
        schema::Field("sunRaysRotationDeg", &S::sunRaysRotationDeg),
        schema::Field("sunRaysVisibleExponent", &S::sunRaysVisibleExponent).Clamp(0.0f, 1.0f),
    } };
    return table;
}

inline void ApplyOverrides(const nlohmann::json& j, BloomSettings& s)
{
    Keys().Read(j, s);
}

inline nlohmann::json ToJson(const BloomSettings& s)
{
    return Keys().Write(s);
}

} // namespace BloomSettingsJson
