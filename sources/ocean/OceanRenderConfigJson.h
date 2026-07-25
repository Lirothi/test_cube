#pragma once

#include <algorithm>
#include <utility>

#include "ocean/OceanSimulationConfig.h"

#pragma warning(push)
#pragma warning(disable: 26819)
#include "third_party/json/json.hpp"
#pragma warning(pop)

namespace OceanRenderConfigJson
{
    inline float ReadFloat(const nlohmann::json& object, const char* key, float fallback)
    {
        const auto it = object.find(key);
        return it != object.end() && it->is_number() ? it->get<float>() : fallback;
    }

    inline Math::float4 ReadFloat4(
        const nlohmann::json& object,
        const char* key,
        const Math::float4& fallback)
    {
        const auto it = object.find(key);
        if (it == object.end() || !it->is_array() || it->size() < 4 ||
            !(*it)[0].is_number() || !(*it)[1].is_number() ||
            !(*it)[2].is_number() || !(*it)[3].is_number())
        {
            return fallback;
        }
        return Math::float4(
            (*it)[0].get<float>(),
            (*it)[1].get<float>(),
            (*it)[2].get<float>(),
            (*it)[3].get<float>());
    }

    inline nlohmann::json WriteFloat4(const Math::float4& value)
    {
        return nlohmann::json::array({ value.x, value.y, value.z, value.w });
    }

    inline void ApplyOverrides(const nlohmann::json& object, OceanRenderConfig& render)
    {
        if (!object.is_object())
        {
            return;
        }

        render.deepScatterColor = ReadFloat4(object, "deepScatterColor", render.deepScatterColor);
        render.sssColor = ReadFloat4(object, "sssColor", render.sssColor);
        render.diffuseColor = ReadFloat4(object, "diffuseColor", render.diffuseColor);
        render.foamTint = ReadFloat4(object, "foamTint", render.foamTint);

        render.specularStrength = std::max(
            0.0f, ReadFloat(object, "specularStrength", render.specularStrength));
        render.roughnessScale = Math::Saturate(
            ReadFloat(object, "roughnessScale", render.roughnessScale));
        render.roughnessDistance = std::max(
            1.0f, ReadFloat(object, "roughnessDistance", render.roughnessDistance));
        render.horizonFogStrength = std::max(
            0.01f, ReadFloat(object, "horizonFogStrength", render.horizonFogStrength));

        render.surfaceRefractionStrength = std::max(
            0.0f, ReadFloat(object, "surfaceRefractionStrength", render.surfaceRefractionStrength));
        render.underwaterRefractionStrength = std::max(
            0.0f, ReadFloat(object, "underwaterRefractionStrength", render.underwaterRefractionStrength));
        render.absorptionDepthScale = std::max(
            1.0f, ReadFloat(object, "absorptionDepthScale", render.absorptionDepthScale));
        render.fogDensity = std::max(
            0.0f, ReadFloat(object, "fogDensity", render.fogDensity));

        render.sunScatterStrength = std::max(
            0.0f, ReadFloat(object, "sunScatterStrength", render.sunScatterStrength));
        render.skyScatterStrength = std::max(
            0.0f, ReadFloat(object, "skyScatterStrength", render.skyScatterStrength));
        render.scatterSpread = std::max(
            0.001f, ReadFloat(object, "scatterSpread", render.scatterSpread));
        render.viewAlignmentStrength = Math::Saturate(
            ReadFloat(object, "viewAlignmentStrength", render.viewAlignmentStrength));

        render.sssHeightBias = ReadFloat(object, "sssHeightBias", render.sssHeightBias);
        render.sssFadeDistance = std::max(
            0.0f, ReadFloat(object, "sssFadeDistance", render.sssFadeDistance));
        render.horizonFogDistanceScale = std::max(
            0.0f, ReadFloat(object, "horizonFogDistanceScale", render.horizonFogDistanceScale));
        render.reflectionNormalStrength = Math::Saturate(
            ReadFloat(object, "reflectionNormalStrength", render.reflectionNormalStrength));
        render.cascadeFadeScale = std::max(
            0.0f, ReadFloat(object, "cascadeFadeScale", render.cascadeFadeScale));
        render.minMeshScale = std::max(
            0.001f, ReadFloat(object, "minMeshScale", render.minMeshScale));
        render.detailNormalMipBias = std::clamp(
            ReadFloat(object, "detailNormalMipBias", render.detailNormalMipBias), -4.0f, 4.0f);
        render.macroNormalMipBiasDlss = std::clamp(
            ReadFloat(object, "macroNormalMipBiasDlss", render.macroNormalMipBiasDlss), -4.0f, 4.0f);
        render.macroNormalMipBiasNative = std::clamp(
            ReadFloat(object, "macroNormalMipBiasNative", render.macroNormalMipBiasNative), -4.0f, 4.0f);

        render.shoreVerticalFadeDepth = std::max(
            0.01f, ReadFloat(object, "shoreVerticalFadeDepth", render.shoreVerticalFadeDepth));
        render.shoreHorizontalMin = Math::Saturate(
            ReadFloat(object, "shoreHorizontalMin", render.shoreHorizontalMin));
        render.shoreHorizontalFadeDepth = std::max(
            0.01f, ReadFloat(object, "shoreHorizontalFadeDepth", render.shoreHorizontalFadeDepth));
        render.shoreNormalFadeDepth = std::max(
            0.01f, ReadFloat(object, "shoreNormalFadeDepth", render.shoreNormalFadeDepth));
        render.shoreNormalMinWeights = ReadFloat4(
            object, "shoreNormalMinWeights", render.shoreNormalMinWeights);
        render.shoreNormalMinWeights.x = Math::Saturate(render.shoreNormalMinWeights.x);
        render.shoreNormalMinWeights.y = Math::Saturate(render.shoreNormalMinWeights.y);
        render.shoreNormalMinWeights.z = Math::Saturate(render.shoreNormalMinWeights.z);
        render.shoreNormalMinWeights.w = Math::Saturate(render.shoreNormalMinWeights.w);
        render.shoreRunupDepth = std::max(
            0.01f, ReadFloat(object, "shoreRunupDepth", render.shoreRunupDepth));
        render.shoreRunupStrength = std::max(
            0.0f, ReadFloat(object, "shoreRunupStrength", render.shoreRunupStrength));
        render.shoreRunupMaxWave = std::max(
            0.0f, ReadFloat(object, "shoreRunupMaxWave", render.shoreRunupMaxWave));
        render.shoreRunupSlopeStartDegrees = std::clamp(
            ReadFloat(object, "shoreRunupSlopeStartDegrees", render.shoreRunupSlopeStartDegrees),
            0.0f, 89.0f);
        render.shoreRunupSlopeEndDegrees = std::clamp(
            ReadFloat(object, "shoreRunupSlopeEndDegrees", render.shoreRunupSlopeEndDegrees),
            0.0f, 89.0f);
        render.shoreBottomClearance = std::max(
            0.0f, ReadFloat(object, "shoreBottomClearance", render.shoreBottomClearance));
        render.shoreEdgeSoftDepth = std::max(
            0.0f, ReadFloat(object, "shoreEdgeSoftDepth", render.shoreEdgeSoftDepth));
        render.shoreGeometryEdgeRefractionFadeDepth = std::max(
            0.0f,
            ReadFloat(
                object,
                "shoreGeometryEdgeRefractionFadeDepth",
                render.shoreGeometryEdgeRefractionFadeDepth));
        render.shoreGeometryFadeDistance = std::max(
            1.0f,
            ReadFloat(
                object,
                "shoreGeometryFadeDistance",
                render.shoreGeometryFadeDistance));
        const float legacyContactFoamWidth = std::max(
            0.0f,
            ReadFloat(
                object,
                "shoreContactFoamWidth",
                render.shoreContactFoamMainWidth));
        const float legacyCoreFraction = Math::Saturate(
            ReadFloat(object, "shoreContactFoamCoreFraction", 1.0f));
        render.shoreContactFoamMainWidth = std::max(
            0.0f,
            ReadFloat(
                object,
                "shoreContactFoamMainWidth",
                legacyContactFoamWidth * legacyCoreFraction));
        const float legacyBreakupLength = std::max(
            0.0f,
            ReadFloat(
                object,
                "shoreContactFoamMainFadeLength",
                render.shoreContactFoamBreakupLength));
        render.shoreContactFoamBreakupLength = std::max(
            0.0f,
            ReadFloat(
                object,
                "shoreContactFoamBreakupLength",
                legacyBreakupLength));
        render.shoreContactFoamBreakupLengthVariation = std::max(
            0.0f,
            ReadFloat(
                object,
                "shoreContactFoamBreakupLengthVariation",
                render.shoreContactFoamBreakupLengthVariation));
        render.shoreContactFoamBreakupVariationScale = std::max(
            0.001f,
            ReadFloat(
                object,
                "shoreContactFoamBreakupVariationScale",
                render.shoreContactFoamBreakupVariationScale));
        render.shoreContactFoamDepthWarpStrength = std::max(
            0.0f,
            ReadFloat(
                object,
                "shoreContactFoamDepthWarpStrength",
                render.shoreContactFoamDepthWarpStrength));
        render.shoreContactFoamDepthWarpRange = std::max(
            0.0f,
            ReadFloat(
                object,
                "shoreContactFoamDepthWarpRange",
                render.shoreContactFoamDepthWarpRange));
        render.shoreContactFoamDepthWarpScale = std::max(
            0.001f,
            ReadFloat(
                object,
                "shoreContactFoamDepthWarpScale",
                render.shoreContactFoamDepthWarpScale));
        const float legacyContactFoamSpeed = std::max(
            0.0f, ReadFloat(
                object,
                "shoreContactFoamSpeed",
                render.shoreContactFoamPatternScrollSpeed));
        const float legacyPatternScrollSpeed = std::max(
            0.0f, ReadFloat(
                object,
                "shoreContactFoamTailScrollSpeed",
                legacyContactFoamSpeed));
        render.shoreContactFoamPatternScrollSpeed = std::max(
            0.0f,
            ReadFloat(
                object,
                "shoreContactFoamPatternScrollSpeed",
                legacyPatternScrollSpeed));
        const float legacyContactFoamScale = std::max(
            0.001f, ReadFloat(
                object,
                "shoreContactFoamScale",
                render.shoreContactFoamAlbedoScale));
        render.shoreContactFoamAlbedoScale = std::max(
            0.001f, ReadFloat(
                object,
                "shoreContactFoamAlbedoScale",
                legacyContactFoamScale));
        render.shoreContactFoamAlbedoScrollSpeed = std::max(
            0.0f, ReadFloat(
                object,
                "shoreContactFoamAlbedoScrollSpeed",
                render.shoreContactFoamAlbedoScrollSpeed));
        const float legacyContactFoamOpacity = Math::Saturate(
            ReadFloat(object, "contactFoamStrength", render.shoreContactFoamOpacity * 0.125f) * 8.0f);
        render.shoreContactFoamOpacity = Math::Saturate(
            ReadFloat(object, "shoreContactFoamOpacity", legacyContactFoamOpacity));
        const float legacyPatternScale = std::max(
            0.001f,
            ReadFloat(
                object,
                "shoreContactFoamTailScale",
                render.shoreContactFoamPatternScale));
        render.shoreContactFoamPatternScale = std::max(
            0.001f,
            ReadFloat(
                object,
                "shoreContactFoamPatternScale",
                legacyPatternScale));
        const float legacyPatternDensity = Math::Saturate(
            ReadFloat(
                object,
                "shoreContactFoamTailDensity",
                render.shoreContactFoamPatternDensity));
        render.shoreContactFoamPatternDensity = Math::Saturate(
            ReadFloat(
                object,
                "shoreContactFoamPatternDensity",
                legacyPatternDensity));

        render.windSpeed = std::max(
            0.0f, ReadFloat(object, "windSpeed", render.windSpeed));
        render.wavesScale = std::max(
            0.0f, ReadFloat(object, "wavesScale", render.wavesScale));
        render.windAlignment = Math::Saturate(
            ReadFloat(object, "windAlignment", render.windAlignment));
        render.windUvWarpStrength = std::max(
            0.0f, ReadFloat(object, "windUvWarpStrength", render.windUvWarpStrength));

        render.foamNormalStrength = Math::Saturate(
            ReadFloat(object, "foamNormalStrength", render.foamNormalStrength));
        render.underwaterFoamParallax = std::max(
            0.0f, ReadFloat(object, "underwaterFoamParallax", render.underwaterFoamParallax));

        render.absorptionGradientType = Math::Saturate(
            ReadFloat(object, "absorptionGradientType", render.absorptionGradientType));
        const auto colorsIt = object.find("absorptionColors");
        if (colorsIt != object.end() && colorsIt->is_array())
        {
            std::vector<Math::float4> colors;
            colors.reserve(std::min<size_t>(colorsIt->size(), 8u));
            for (const nlohmann::json& entry : *colorsIt)
            {
                if (colors.size() >= 8u)
                {
                    break;
                }
                if (!entry.is_array() || entry.size() < 4 ||
                    !entry[0].is_number() || !entry[1].is_number() ||
                    !entry[2].is_number() || !entry[3].is_number())
                {
                    continue;
                }
                colors.emplace_back(
                    entry[0].get<float>(),
                    entry[1].get<float>(),
                    entry[2].get<float>(),
                    Math::Saturate(entry[3].get<float>()));
            }
            if (!colors.empty())
            {
                std::sort(colors.begin(), colors.end(), [](const Math::float4& lhs, const Math::float4& rhs)
                {
                    return lhs.w < rhs.w;
                });
                render.absorptionColors = std::move(colors);
            }
        }
    }

    inline nlohmann::json ToJson(const OceanRenderConfig& render)
    {
        nlohmann::json out;
        out["deepScatterColor"] = WriteFloat4(render.deepScatterColor);
        out["sssColor"] = WriteFloat4(render.sssColor);
        out["diffuseColor"] = WriteFloat4(render.diffuseColor);
        out["foamTint"] = WriteFloat4(render.foamTint);

        out["specularStrength"] = render.specularStrength;
        out["roughnessScale"] = render.roughnessScale;
        out["roughnessDistance"] = render.roughnessDistance;
        out["horizonFogStrength"] = render.horizonFogStrength;

        out["surfaceRefractionStrength"] = render.surfaceRefractionStrength;
        out["underwaterRefractionStrength"] = render.underwaterRefractionStrength;
        out["absorptionDepthScale"] = render.absorptionDepthScale;
        out["fogDensity"] = render.fogDensity;

        out["sunScatterStrength"] = render.sunScatterStrength;
        out["skyScatterStrength"] = render.skyScatterStrength;
        out["scatterSpread"] = render.scatterSpread;
        out["viewAlignmentStrength"] = render.viewAlignmentStrength;

        out["sssHeightBias"] = render.sssHeightBias;
        out["sssFadeDistance"] = render.sssFadeDistance;
        out["horizonFogDistanceScale"] = render.horizonFogDistanceScale;
        out["reflectionNormalStrength"] = render.reflectionNormalStrength;
        out["cascadeFadeScale"] = render.cascadeFadeScale;
        out["minMeshScale"] = render.minMeshScale;
        out["detailNormalMipBias"] = render.detailNormalMipBias;
        out["macroNormalMipBiasDlss"] = render.macroNormalMipBiasDlss;
        out["macroNormalMipBiasNative"] = render.macroNormalMipBiasNative;

        out["shoreVerticalFadeDepth"] = render.shoreVerticalFadeDepth;
        out["shoreHorizontalMin"] = render.shoreHorizontalMin;
        out["shoreHorizontalFadeDepth"] = render.shoreHorizontalFadeDepth;
        out["shoreNormalFadeDepth"] = render.shoreNormalFadeDepth;
        out["shoreNormalMinWeights"] = WriteFloat4(render.shoreNormalMinWeights);
        out["shoreRunupDepth"] = render.shoreRunupDepth;
        out["shoreRunupStrength"] = render.shoreRunupStrength;
        out["shoreRunupMaxWave"] = render.shoreRunupMaxWave;
        out["shoreRunupSlopeStartDegrees"] = render.shoreRunupSlopeStartDegrees;
        out["shoreRunupSlopeEndDegrees"] = render.shoreRunupSlopeEndDegrees;
        out["shoreBottomClearance"] = render.shoreBottomClearance;
        out["shoreEdgeSoftDepth"] = render.shoreEdgeSoftDepth;
        out["shoreGeometryEdgeRefractionFadeDepth"] =
            render.shoreGeometryEdgeRefractionFadeDepth;
        out["shoreGeometryFadeDistance"] = render.shoreGeometryFadeDistance;
        out["shoreContactFoamMainWidth"] = render.shoreContactFoamMainWidth;
        out["shoreContactFoamBreakupLength"] = render.shoreContactFoamBreakupLength;
        out["shoreContactFoamBreakupLengthVariation"] =
            render.shoreContactFoamBreakupLengthVariation;
        out["shoreContactFoamBreakupVariationScale"] =
            render.shoreContactFoamBreakupVariationScale;
        out["shoreContactFoamDepthWarpStrength"] =
            render.shoreContactFoamDepthWarpStrength;
        out["shoreContactFoamDepthWarpRange"] =
            render.shoreContactFoamDepthWarpRange;
        out["shoreContactFoamDepthWarpScale"] =
            render.shoreContactFoamDepthWarpScale;
        out["shoreContactFoamPatternScrollSpeed"] =
            render.shoreContactFoamPatternScrollSpeed;
        out["shoreContactFoamAlbedoScale"] = render.shoreContactFoamAlbedoScale;
        out["shoreContactFoamAlbedoScrollSpeed"] = render.shoreContactFoamAlbedoScrollSpeed;
        out["shoreContactFoamOpacity"] = render.shoreContactFoamOpacity;
        out["shoreContactFoamPatternScale"] = render.shoreContactFoamPatternScale;
        out["shoreContactFoamPatternDensity"] = render.shoreContactFoamPatternDensity;

        out["windSpeed"] = render.windSpeed;
        out["wavesScale"] = render.wavesScale;
        out["windAlignment"] = render.windAlignment;
        out["windUvWarpStrength"] = render.windUvWarpStrength;

        out["foamNormalStrength"] = render.foamNormalStrength;
        out["underwaterFoamParallax"] = render.underwaterFoamParallax;
        out["absorptionGradientType"] = render.absorptionGradientType;

        nlohmann::json colors = nlohmann::json::array();
        const size_t count = std::min<size_t>(render.absorptionColors.size(), 8u);
        for (size_t index = 0; index < count; ++index)
        {
            colors.push_back(WriteFloat4(render.absorptionColors[index]));
        }
        out["absorptionColors"] = std::move(colors);
        return out;
    }
}
