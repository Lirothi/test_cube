#pragma once

#include "third_party/json/json.hpp"

#include "core/schema/KeyTable.h" // R9.2: the section's table of keys

#include "rendering/lighting/VolumetricCloudSettings.h"

#include <algorithm>

// Plan C1: the ONE mapping between the LEVEL-AUTHORED half of VolumetricCloudSettings and JSON,
// for the same reason SkyAtmosphereSettingsJson exists: the level reader, the editor's runtime
// apply and the serializer read one field list. `debugView` is session state and stays out.
namespace VolumetricCloudSettingsJson
{

// Architecture plan R9.2: the section's table of keys (core/schema/KeyTable.h); reading and writing walk it.
inline const schema::Table<VolumetricCloudSettings>& Keys()
{
    using S = VolumetricCloudSettings;
    static const schema::Table<S> table{ {
        // A level that has the section has clouds unless it says otherwise: `enabled` reads true when absent, not the
        // struct's default (off) -- it switches them off without losing the tuning.
        schema::Custom<S>("enabled",
            [](const nlohmann::json& j, S& s) { s.enabled = jsonfield::Value(j, "enabled", true); },
            [](nlohmann::json& j, const S& s) { j["enabled"] = s.enabled; }),
        schema::Field("layerBottomKm", &S::layerBottomKm).Clamp(0.1f, 20.0f),
        schema::Field("layerHeightKm", &S::layerHeightKm).Clamp(0.1f, 20.0f),
        schema::Field("coverage", &S::coverage).Clamp(0.0f, 1.0f),
        schema::Field("cloudType", &S::cloudType).Clamp(-1.0f, 1.0f),
        schema::Field("formationChangeSpeed", &S::formationChangeSpeed).Clamp(0.0f, 100.0f),
        schema::Field("macroVariation", &S::macroVariation).Clamp(0.0f, 1.0f),
        schema::Field("macroTileKm", &S::macroTileKm).Clamp(1.0f, 1000.0f),
        schema::Field("overcastFrom", &S::overcastFrom).Clamp(0.0f, 0.99f),
        schema::Field("overcastEvenness", &S::overcastEvenness).Clamp(0.0f, 1.0f),
        schema::Field("formationRoughness", &S::formationRoughness).Clamp(0.3f, 0.9f),
        schema::Field("floorVariationClear", &S::floorVariationClear).Clamp(0.0f, 2.0f),
        schema::Field("floorVariationCloudy", &S::floorVariationCloudy).Clamp(0.0f, 2.0f),
        schema::Field("erosionStrength", &S::erosionStrength).Clamp(0.0f, 4.0f),
        schema::Field("erosionPower", &S::erosionPower).Clamp(0.25f, 8.0f),
        schema::Field("erosionTileKm", &S::erosionTileKm).Clamp(0.1f, 20.0f),
        schema::Field("highFrequencyStrength", &S::highFrequencyStrength).Clamp(0.0f, 2.0f),
        schema::Field("highFrequencyDistanceKm", &S::highFrequencyDistanceKm).Clamp(0.1f, 20.0f),
        schema::Field("profileHeight", &S::profileHeight).Clamp(0.2f, 2.0f),
        schema::Field("profileDome", &S::profileDome).Clamp(0.0f, 1.0f),
        schema::Field("profileTopSoftness", &S::profileTopSoftness).Clamp(0.01f, 1.0f),
        schema::Field("profileBaseSoftness", &S::profileBaseSoftness).Clamp(0.0f, 1.0f),
        schema::Field("profileSmallDensity", &S::profileSmallDensity).Clamp(0.0f, 1.0f),
        schema::Field("profileBodyDensity", &S::profileBodyDensity).Clamp(0.0f, 1.0f),
        schema::Field("profileBaseErosion", &S::profileBaseErosion).Clamp(0.0f, 1.0f),
        schema::Field("profileErosionRise", &S::profileErosionRise).Clamp(0.05f, 1.0f),
        schema::Field("profileHeavyErosionCut", &S::profileHeavyErosionCut).Clamp(0.0f, 1.0f),
        schema::Field("profileHeavyThinning", &S::profileHeavyThinning).Clamp(0.0f, 1.0f),
        schema::Field("extinctionScale", &S::extinctionScale).Clamp(0.0f, 20.0f),
        schema::Field("albedo", &S::albedo).Clamp(0.0f, 1.0f),
        schema::Field("weatherTileKm", &S::weatherTileKm).Clamp(1.0f, 1000.0f),
        schema::Field("windKmH", &S::windKmH).Clamp(0.0f, 500.0f),
        schema::Field("seed", &S::seed),
        schema::Field("phaseG", &S::phaseG).Clamp(-0.99f, 0.99f),
        schema::Field("phaseG2", &S::phaseG2).Clamp(-0.99f, 0.99f),
        schema::Field("phaseBlend", &S::phaseBlend).Clamp(0.0f, 1.0f),
        schema::Field("msContribution", &S::msContribution).Clamp(0.0f, 1.0f),
        schema::Field("msOcclusion", &S::msOcclusion).Clamp(0.0f, 1.0f),
        schema::Field("msEccentricity", &S::msEccentricity).Clamp(0.0f, 1.0f),
        schema::Field("msOctaves", &S::msOctaves).Clamp(0.0f, 2.0f),
        schema::Field("skyLightBottomOcclusion", &S::skyLightBottomOcclusion).Clamp(0.0f, 1.0f),
        schema::Field("skyLightIntensity", &S::skyLightIntensity).Clamp(0.0f, 10.0f),
        schema::Field("diffuseLight", &S::diffuseLight).Clamp(0.0f, 10.0f),
        schema::Field("reflectionHorizonFadeDeg", &S::reflectionHorizonFadeDeg).Clamp(0.0f, 45.0f),
        schema::Field("viewSampleCountMax", &S::viewSampleCountMax).Clamp(4.0f, 768.0f),
        schema::Field("viewSampleCountMin", &S::viewSampleCountMin).Clamp(1.0f, 768.0f),
        schema::Field("distanceToSampleCountMaxKm", &S::distanceToSampleCountMaxKm).Clamp(0.1f, 1000.0f),
        schema::Field("shadowSampleCount", &S::shadowSampleCount).Clamp(1.0f, 80.0f),
        schema::Field("shadowTracingDistanceKm", &S::shadowTracingDistanceKm).Clamp(0.1f, 100.0f),
        schema::Field("stopTracingTransmittance", &S::stopTracingTransmittance).Clamp(0.0f, 0.5f),
        schema::Field("tracingStartMaxDistanceKm", &S::tracingStartMaxDistanceKm).Clamp(1.0f, 10000.0f),
        schema::Field("tracingMaxDistanceKm", &S::tracingMaxDistanceKm).Clamp(1.0f, 1000.0f),
        schema::Field("temporal", &S::temporal),
        schema::Field("historyWeight", &S::historyWeight).Clamp(0.0f, 0.99f),
        schema::Field("shadowMap", &S::shadowMap),
        schema::Field("shadowExtentKm", &S::shadowExtentKm).Clamp(1.0f, 500.0f),
        schema::Field("shadowStrength", &S::shadowStrength).Clamp(0.0f, 1.0f),
        schema::Field("oceanBodyShadow", &S::oceanBodyShadow).Clamp(0.0f, 1.0f),
        schema::Field("shadowHorizonFadeDeg", &S::shadowHorizonFadeDeg).Clamp(0.0f, 45.0f),
        schema::Field("shadowSnapKm", &S::shadowSnapKm).Clamp(0.01f, 100.0f),
        schema::Field("shadowDepthBiasKm", &S::shadowDepthBiasKm).Clamp(-5.0f, 5.0f),
        schema::Field("shadowMapSampleCount", &S::shadowMapSampleCount).Clamp(4.0f, 128.0f),
    } };
    return table;
}

inline void ApplyOverrides(const nlohmann::json& j, VolumetricCloudSettings& s)
{
    Keys().Read(j, s);
}

inline nlohmann::json ToJson(const VolumetricCloudSettings& s)
{
    return Keys().Write(s);
}

} // namespace VolumetricCloudSettingsJson
