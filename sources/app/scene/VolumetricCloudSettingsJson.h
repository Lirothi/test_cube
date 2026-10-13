#pragma once

#include "third_party/json/json.hpp"

#include "rendering/lighting/VolumetricCloudSettings.h"

#include <algorithm>

// Plan C1: the ONE mapping between the LEVEL-AUTHORED half of VolumetricCloudSettings and JSON,
// for the same reason SkyAtmosphereSettingsJson exists: the level reader, the editor's runtime
// apply and the serializer read one field list. `debugView` is session state and stays out.
namespace VolumetricCloudSettingsJson
{

inline void ApplyOverrides(const nlohmann::json& j, VolumetricCloudSettings& s)
{
    // The section existing is what gives the level clouds (as adding the actor is in UE);
    // `enabled` switches them off without losing the tuning.
    s.enabled = j.value("enabled", true);
    s.layerBottomKm = std::clamp(j.value("layerBottomKm", s.layerBottomKm), 0.1f, 20.0f);
    s.layerHeightKm = std::clamp(j.value("layerHeightKm", s.layerHeightKm), 0.1f, 20.0f);
    s.coverage = std::clamp(j.value("coverage", s.coverage), 0.0f, 1.0f);
    s.cloudType = std::clamp(j.value("cloudType", s.cloudType), -1.0f, 1.0f);
    s.formationChangeSpeed = std::clamp(j.value("formationChangeSpeed", s.formationChangeSpeed), 0.0f, 100.0f);
    s.macroVariation = std::clamp(j.value("macroVariation", s.macroVariation), 0.0f, 1.0f);
    s.macroTileKm = std::clamp(j.value("macroTileKm", s.macroTileKm), 1.0f, 1000.0f);
    s.overcastFrom = std::clamp(j.value("overcastFrom", s.overcastFrom), 0.0f, 0.99f);
    s.overcastEvenness = std::clamp(j.value("overcastEvenness", s.overcastEvenness), 0.0f, 1.0f);
    s.formationRoughness = std::clamp(j.value("formationRoughness", s.formationRoughness), 0.3f, 0.9f);
    s.floorVariationClear = std::clamp(j.value("floorVariationClear", s.floorVariationClear), 0.0f, 2.0f);
    s.floorVariationCloudy = std::clamp(j.value("floorVariationCloudy", s.floorVariationCloudy), 0.0f, 2.0f);
    s.erosionStrength = std::clamp(j.value("erosionStrength", s.erosionStrength), 0.0f, 4.0f);
    s.erosionPower = std::clamp(j.value("erosionPower", s.erosionPower), 0.25f, 8.0f);
    s.erosionTileKm = std::clamp(j.value("erosionTileKm", s.erosionTileKm), 0.1f, 20.0f);
    s.highFrequencyStrength = std::clamp(j.value("highFrequencyStrength", s.highFrequencyStrength), 0.0f, 2.0f);
    s.highFrequencyDistanceKm = std::clamp(j.value("highFrequencyDistanceKm", s.highFrequencyDistanceKm), 0.1f, 20.0f);
    s.profileHeight = std::clamp(j.value("profileHeight", s.profileHeight), 0.2f, 2.0f);
    s.profileDome = std::clamp(j.value("profileDome", s.profileDome), 0.0f, 1.0f);
    s.profileTopSoftness = std::clamp(j.value("profileTopSoftness", s.profileTopSoftness), 0.01f, 1.0f);
    s.profileBaseSoftness = std::clamp(j.value("profileBaseSoftness", s.profileBaseSoftness), 0.0f, 1.0f);
    s.profileSmallDensity = std::clamp(j.value("profileSmallDensity", s.profileSmallDensity), 0.0f, 1.0f);
    s.profileBodyDensity = std::clamp(j.value("profileBodyDensity", s.profileBodyDensity), 0.0f, 1.0f);
    s.profileBaseErosion = std::clamp(j.value("profileBaseErosion", s.profileBaseErosion), 0.0f, 1.0f);
    s.profileErosionRise = std::clamp(j.value("profileErosionRise", s.profileErosionRise), 0.05f, 1.0f);
    s.profileHeavyErosionCut = std::clamp(j.value("profileHeavyErosionCut", s.profileHeavyErosionCut), 0.0f, 1.0f);
    s.profileHeavyThinning = std::clamp(j.value("profileHeavyThinning", s.profileHeavyThinning), 0.0f, 1.0f);
    // Up to 20: the profile's density is applied cubed, so the scale is ~8x a real cloud's (UDS 10).
    s.extinctionScale = std::clamp(j.value("extinctionScale", s.extinctionScale), 0.0f, 20.0f);
    s.albedo = std::clamp(j.value("albedo", s.albedo), 0.0f, 1.0f);
    s.weatherTileKm = std::clamp(j.value("weatherTileKm", s.weatherTileKm), 1.0f, 1000.0f);
    s.windKmH = std::clamp(j.value("windKmH", s.windKmH), 0.0f, 500.0f);
    s.seed = j.value("seed", s.seed);
    s.phaseG = std::clamp(j.value("phaseG", s.phaseG), -0.99f, 0.99f);
    s.phaseG2 = std::clamp(j.value("phaseG2", s.phaseG2), -0.99f, 0.99f);
    s.phaseBlend = std::clamp(j.value("phaseBlend", s.phaseBlend), 0.0f, 1.0f);
    s.msContribution = std::clamp(j.value("msContribution", s.msContribution), 0.0f, 1.0f);
    s.msOcclusion = std::clamp(j.value("msOcclusion", s.msOcclusion), 0.0f, 1.0f);
    s.msEccentricity = std::clamp(j.value("msEccentricity", s.msEccentricity), 0.0f, 1.0f);
    s.msOctaves = std::clamp(j.value("msOctaves", s.msOctaves), 0u, 2u);
    s.skyLightBottomOcclusion = std::clamp(j.value("skyLightBottomOcclusion", s.skyLightBottomOcclusion), 0.0f, 1.0f);
    s.skyLightIntensity = std::clamp(j.value("skyLightIntensity", s.skyLightIntensity), 0.0f, 10.0f);
    s.diffuseLight = std::clamp(j.value("diffuseLight", s.diffuseLight), 0.0f, 10.0f);
    s.viewSampleCountMax = std::clamp(j.value("viewSampleCountMax", s.viewSampleCountMax), 4u, 768u);
    s.viewSampleCountMin = std::clamp(j.value("viewSampleCountMin", s.viewSampleCountMin), 1u, 64u);
    s.distanceToSampleCountMaxKm = std::clamp(j.value("distanceToSampleCountMaxKm", s.distanceToSampleCountMaxKm), 0.1f, 1000.0f);
    s.shadowSampleCount = std::clamp(j.value("shadowSampleCount", s.shadowSampleCount), 1u, 80u);
    s.shadowTracingDistanceKm = std::clamp(j.value("shadowTracingDistanceKm", s.shadowTracingDistanceKm), 0.1f, 100.0f);
    s.stopTracingTransmittance = std::clamp(j.value("stopTracingTransmittance", s.stopTracingTransmittance), 0.0f, 0.5f);
    s.tracingStartMaxDistanceKm = std::clamp(j.value("tracingStartMaxDistanceKm", s.tracingStartMaxDistanceKm), 1.0f, 10000.0f);
    s.tracingMaxDistanceKm = std::clamp(j.value("tracingMaxDistanceKm", s.tracingMaxDistanceKm), 1.0f, 1000.0f);
    s.temporal = j.value("temporal", s.temporal);
    s.historyWeight = std::clamp(j.value("historyWeight", s.historyWeight), 0.0f, 0.99f);
    s.shadowMap = j.value("shadowMap", s.shadowMap);
    s.shadowExtentKm = std::clamp(j.value("shadowExtentKm", s.shadowExtentKm), 1.0f, 500.0f);
    s.shadowStrength = std::clamp(j.value("shadowStrength", s.shadowStrength), 0.0f, 1.0f);
    s.oceanBodyShadow = std::clamp(j.value("oceanBodyShadow", s.oceanBodyShadow), 0.0f, 1.0f);
    s.shadowSnapKm = std::clamp(j.value("shadowSnapKm", s.shadowSnapKm), 0.01f, 100.0f);
    s.shadowDepthBiasKm = std::clamp(j.value("shadowDepthBiasKm", s.shadowDepthBiasKm), -5.0f, 5.0f);
    s.shadowMapSampleCount = std::clamp(j.value("shadowMapSampleCount", s.shadowMapSampleCount), 4u, 128u);
}

inline nlohmann::json ToJson(const VolumetricCloudSettings& s)
{
    nlohmann::json j = nlohmann::json::object();
    j["enabled"] = s.enabled;
    j["layerBottomKm"] = s.layerBottomKm;
    j["layerHeightKm"] = s.layerHeightKm;
    j["coverage"] = s.coverage;
    j["cloudType"] = s.cloudType;
    j["formationChangeSpeed"] = s.formationChangeSpeed;
    j["macroVariation"] = s.macroVariation;
    j["macroTileKm"] = s.macroTileKm;
    j["overcastFrom"] = s.overcastFrom;
    j["overcastEvenness"] = s.overcastEvenness;
    j["formationRoughness"] = s.formationRoughness;
    j["floorVariationClear"] = s.floorVariationClear;
    j["floorVariationCloudy"] = s.floorVariationCloudy;
    j["erosionStrength"] = s.erosionStrength;
    j["erosionPower"] = s.erosionPower;
    j["erosionTileKm"] = s.erosionTileKm;
    j["highFrequencyStrength"] = s.highFrequencyStrength;
    j["highFrequencyDistanceKm"] = s.highFrequencyDistanceKm;
    j["profileHeight"] = s.profileHeight;
    j["profileDome"] = s.profileDome;
    j["profileTopSoftness"] = s.profileTopSoftness;
    j["profileBaseSoftness"] = s.profileBaseSoftness;
    j["profileSmallDensity"] = s.profileSmallDensity;
    j["profileBodyDensity"] = s.profileBodyDensity;
    j["profileBaseErosion"] = s.profileBaseErosion;
    j["profileErosionRise"] = s.profileErosionRise;
    j["profileHeavyErosionCut"] = s.profileHeavyErosionCut;
    j["profileHeavyThinning"] = s.profileHeavyThinning;
    j["extinctionScale"] = s.extinctionScale;
    j["albedo"] = s.albedo;
    j["weatherTileKm"] = s.weatherTileKm;
    j["windKmH"] = s.windKmH;
    j["seed"] = s.seed;
    j["phaseG"] = s.phaseG;
    j["phaseG2"] = s.phaseG2;
    j["phaseBlend"] = s.phaseBlend;
    j["msContribution"] = s.msContribution;
    j["msOcclusion"] = s.msOcclusion;
    j["msEccentricity"] = s.msEccentricity;
    j["msOctaves"] = s.msOctaves;
    j["skyLightBottomOcclusion"] = s.skyLightBottomOcclusion;
    j["skyLightIntensity"] = s.skyLightIntensity;
    j["diffuseLight"] = s.diffuseLight;
    j["viewSampleCountMax"] = s.viewSampleCountMax;
    j["viewSampleCountMin"] = s.viewSampleCountMin;
    j["distanceToSampleCountMaxKm"] = s.distanceToSampleCountMaxKm;
    j["shadowSampleCount"] = s.shadowSampleCount;
    j["shadowTracingDistanceKm"] = s.shadowTracingDistanceKm;
    j["stopTracingTransmittance"] = s.stopTracingTransmittance;
    j["tracingStartMaxDistanceKm"] = s.tracingStartMaxDistanceKm;
    j["tracingMaxDistanceKm"] = s.tracingMaxDistanceKm;
    j["temporal"] = s.temporal;
    j["historyWeight"] = s.historyWeight;
    j["shadowMap"] = s.shadowMap;
    j["shadowExtentKm"] = s.shadowExtentKm;
    j["shadowStrength"] = s.shadowStrength;
    j["oceanBodyShadow"] = s.oceanBodyShadow;
    j["shadowSnapKm"] = s.shadowSnapKm;
    j["shadowDepthBiasKm"] = s.shadowDepthBiasKm;
    j["shadowMapSampleCount"] = s.shadowMapSampleCount;
    return j;
}

} // namespace VolumetricCloudSettingsJson
