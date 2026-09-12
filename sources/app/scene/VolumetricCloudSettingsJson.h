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
    s.extinctionScale = std::clamp(j.value("extinctionScale", s.extinctionScale), 0.0f, 1.0f);
    s.albedo = std::clamp(j.value("albedo", s.albedo), 0.0f, 1.0f);
    s.detailStrength = std::clamp(j.value("detailStrength", s.detailStrength), 0.0f, 1.0f);
    s.baseTileKm = std::clamp(j.value("baseTileKm", s.baseTileKm), 0.1f, 100.0f);
    s.detailTileKm = std::clamp(j.value("detailTileKm", s.detailTileKm), 0.01f, 10.0f);
    s.weatherTileKm = std::clamp(j.value("weatherTileKm", s.weatherTileKm), 1.0f, 1000.0f);
    s.windKmH = std::clamp(j.value("windKmH", s.windKmH), 0.0f, 500.0f);
    s.seed = j.value("seed", s.seed);
    s.phaseG = std::clamp(j.value("phaseG", s.phaseG), -0.99f, 0.99f);
    s.phaseG2 = std::clamp(j.value("phaseG2", s.phaseG2), -0.99f, 0.99f);
    s.phaseBlend = std::clamp(j.value("phaseBlend", s.phaseBlend), 0.0f, 1.0f);
    s.msContribution = std::clamp(j.value("msContribution", s.msContribution), 0.0f, 1.0f);
    s.msOcclusion = std::clamp(j.value("msOcclusion", s.msOcclusion), 0.0f, 1.0f);
    s.msEccentricity = std::clamp(j.value("msEccentricity", s.msEccentricity), 0.0f, 1.0f);
    s.skyLightBottomOcclusion = std::clamp(j.value("skyLightBottomOcclusion", s.skyLightBottomOcclusion), 0.0f, 1.0f);
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
    j["extinctionScale"] = s.extinctionScale;
    j["albedo"] = s.albedo;
    j["detailStrength"] = s.detailStrength;
    j["baseTileKm"] = s.baseTileKm;
    j["detailTileKm"] = s.detailTileKm;
    j["weatherTileKm"] = s.weatherTileKm;
    j["windKmH"] = s.windKmH;
    j["seed"] = s.seed;
    j["phaseG"] = s.phaseG;
    j["phaseG2"] = s.phaseG2;
    j["phaseBlend"] = s.phaseBlend;
    j["msContribution"] = s.msContribution;
    j["msOcclusion"] = s.msOcclusion;
    j["msEccentricity"] = s.msEccentricity;
    j["skyLightBottomOcclusion"] = s.skyLightBottomOcclusion;
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
    j["shadowSnapKm"] = s.shadowSnapKm;
    j["shadowDepthBiasKm"] = s.shadowDepthBiasKm;
    j["shadowMapSampleCount"] = s.shadowMapSampleCount;
    return j;
}

} // namespace VolumetricCloudSettingsJson
