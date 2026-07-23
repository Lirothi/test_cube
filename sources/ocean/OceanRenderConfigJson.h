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
        render.contactFoamStrength = std::max(
            0.0f, ReadFloat(object, "contactFoamStrength", render.contactFoamStrength));
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

        out["windSpeed"] = render.windSpeed;
        out["wavesScale"] = render.wavesScale;
        out["windAlignment"] = render.windAlignment;
        out["windUvWarpStrength"] = render.windUvWarpStrength;

        out["foamNormalStrength"] = render.foamNormalStrength;
        out["contactFoamStrength"] = render.contactFoamStrength;
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
