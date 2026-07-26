#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/math/Math.h"
#include "ocean/OceanSimulationInputs.h"
#include "ocean/OceanSimulationSettings.h"

struct OceanRenderConfig
{
    Math::float4 deepScatterColor = Math::float4(0.0f, 0.012745098f, 0.04019608f, 1.0f);
    Math::float4 sssColor = Math::float4(0.13333334f, 0.9411765f, 0.6039216f, 1.0f);
    Math::float4 diffuseColor = Math::float4(0.0f, 0.025490196f, 0.02745098f, 1.0f);
    Math::float4 foamTint = Math::float4(1.0f, 1.0f, 1.0f, 1.0f);

    float specularStrength = 1.1f;
    float roughnessScale = 0.7f;
    float roughnessDistance = 150.0f;
    float horizonFogStrength = 0.55f;

    float surfaceRefractionStrength = 0.35f;
    float underwaterRefractionStrength = 0.75f;
    float absorptionDepthScale = 10.0f;
    float fogDensity = 0.1f;

    float sunScatterStrength = 0.35f;
    float skyScatterStrength = 0.35f;
    float scatterSpread = 0.2f;
    float viewAlignmentStrength = 0.8f;

    float sssHeightBias = 0.0f;
    float sssFadeDistance = 6.0f;
    float horizonFogDistanceScale = 2.5f;
    float reflectionNormalStrength = 0.05f;
    float cascadeFadeScale = 20.0f;
    float minMeshScale = 15.0f;
    float detailNormalMipBias = 0.0f;
    float macroNormalMipBiasDlss = 0.0f;
    float macroNormalMipBiasNative = 1.0f;

    float shoreVerticalFadeDepth = 1.25f;
    float shoreHorizontalMin = 0.65f;
    float shoreHorizontalFadeDepth = 2.0f;
    float shoreNormalFadeDepth = 2.0f;
    Math::float4 shoreNormalMinWeights = Math::float4(0.25f, 0.45f, 0.75f, 1.0f);
    float shoreRunupDepth = 2.0f;
    float shoreRunupStrength = 1.5f;
    float shoreRunupMaxWave = 0.8f;
    float shoreRunupSlopeStartDegrees = 25.0f;
    float shoreRunupSlopeEndDegrees = 55.0f;
    float shoreBottomClearance = 0.05f;
    float shoreEdgeSoftDepth = 0.03f;
    float shoreGeometryEdgeRefractionFadeDepth = 0.1f;
    float shoreGeometryFadeDistance = 150.0f;
    float shoreContactFoamMainWidth = 0.1225f;
    float shoreContactFoamBreakupLength = 0.05f;
    float shoreContactFoamBreakupLengthVariation = 0.05f;
    float shoreContactFoamBreakupVariationScale = 0.04f;
    float shoreContactFoamDepthWarpStrength = 0.03f;
    float shoreContactFoamDepthWarpRange = 0.25f;
    float shoreContactFoamDepthWarpScale = 0.098f;
    float shoreContactFoamPatternScrollSpeed = 0.2f;
    float shoreContactFoamAlbedoScale = 0.15f;
    float shoreContactFoamAlbedoScrollSpeed = 0.2f;
    float shoreContactFoamOpacity = 0.8f;
    float shoreContactFoamCalmAmount = 0.1f;
    float shoreContactFoamFullWindForce = 0.6f;
    float shoreContactFoamNormalStrength = 1.0f;
    float shoreContactFoamPatternScale = 0.28f;
    float shoreContactFoamPatternDensity = 0.55f;

    float windSpeed = 12.0f;
    float wavesScale = 1.0f;
    float windAlignment = 0.5f;
    float windUvWarpStrength = 0.2f;

    float foamNormalStrength = 0.6f;
    float underwaterFoamParallax = 1.6f;

    float absorptionGradientType = 0.0f;
    std::vector<Math::float4> absorptionColors = {
        Math::float4(0.0f, 0.041025557f, 0.094412796f, 0.0f),
        Math::float4(0.0f, 0.17351386f, 0.43203577f, 0.2f),
        Math::float4(0.16198544f, 0.68352747f, 0.79865986f, 0.66608685f),
        Math::float4(1.0f, 1.0f, 1.0f, 1.0f)
    };
};

struct OceanSimulationConfig
{
    OceanSimulationSettings settings;
    OceanRenderConfig render;

    float localWindDirectionDegrees = 0.0f;
    float swellDirectionDegrees = 0.0f;
    float windForce01 = 0.0f;

    OceanSimulationInputsProvider::InputsProviderMode inputMode = OceanSimulationInputsProvider::InputsProviderMode::Fixed;
    float timeScale = 1.0f;
    float depth = 1000.0f;

    std::shared_ptr<EqualizerPreset> defaultEqualizer = EqualizerPreset::CreateDefault();
    std::shared_ptr<SwellPreset> swellPreset = std::make_shared<SwellPreset>();
    std::shared_ptr<LocalWavesPreset> localPreset = std::make_shared<LocalWavesPreset>();
    std::vector<std::shared_ptr<LocalWavesPreset>> localPresets;
    size_t localPresetIndex = 0;
};

OceanSimulationConfig CloneOceanSimulationConfig(const OceanSimulationConfig& config);
bool LoadOceanSimulationConfigFromFile(const std::wstring& path, OceanSimulationConfig& outConfig);
bool SaveOceanSimulationConfigToFile(const std::wstring& path, const OceanSimulationConfig& config);
