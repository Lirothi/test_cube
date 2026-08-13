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

    // The LEGACY surface's authored shore knobs. Ignored by the modern run-up surface.
    // Contact foam strength = foamParams2.y in the June-22 shader, which hardcoded 0.1.
    float shoreLegacyContactFoamStrength = 0.1f;
    // Nearshore damping, split from the June hardcode `saturate(depth * 0.15)` on the whole
    // displacement: separate vertical / XZ strengths (1 = fully still at the waterline) and the
    // water depth where the damping starts biting. Defaults reproduce the original curve.
    float shoreLegacyVerticalDampStrength = 1.0f;
    float shoreLegacyXzDampStrength = 1.0f;
    float shoreLegacyDampFadeDepth = 6.67f;
    // The legacy contact-foam TAIL (the ContactFoam.dds strip): texture scale in tiles per metre,
    // the depth-difference reach it survives to, its wind-drift speed, and the second-octave
    // de-tiling mix. Edge fade reuses shoreEdgeSoftDepth; albedo reuses the shore foam albedo
    // scale/scroll. Defaults (1 / 0.2 / 0 / 0) reproduce the June behaviour.
    float shoreLegacyTailTextureScale = 1.0f;
    float shoreLegacyTailDepth = 0.2f;
    float shoreLegacyTailScrollSpeed = 0.0f;
    float shoreLegacyTailDetile = 0.0f;
    float shoreLegacyTailEdgeFade = 0.1f;
    // Remap of the tail texel before the coverage math: contrast around mid-grey and a brightness
    // bias. They shape the dissipation length (how far bright tongues outrun dark ones): a texel
    // of brightness t survives to TailDepth / (1 - t), so pulling the top of the distribution
    // down gives the tail a finite dissipation depth. Defaults (1, 0) = identity = June.
    float shoreLegacyTailContrast = 1.0f;
    float shoreLegacyTailBias = 0.0f;

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
    // Swash: the horizontal shuttle of the splash-zone water with the arriving wave — a material
    // motion of the vertices (the wet edge rides along), not a mask. A LOW-WIND device: the
    // excursion peaks in the light-to-mid wind band (ramping in above the calm threshold, fading
    // out quadratically toward full wind), because a big sea's own run-up push already moves the
    // shoreline and stacking the shuttle on top overdrives the waves, while below the calm
    // threshold there is no surf at all. This slider scales it 0..1.
    float shoreSwashAmplitude = 0.85f;
    // Baseline (in shore-map texels, ~1 m each) of the centred difference that reads the beach
    // slope for the run-up gate. THE tooth-count control: a short baseline reads texel noise and
    // cuts many small teeth; a long one reads the beach's real shape and leaves a few wide bays.
    float shoreRunupSlopeSmoothing = 1.5f;
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

    // --- Caustics -----------------------------------------------------------------------------
    // Sampled in the deferred lighting pass (lighting_cs.hlsl) for every surface below the water
    // line, so they land on the lagoon floor, the submerged beach and any prop under water, and
    // they inherit the sun shadow for free. Off when there is no ocean in the level.
    bool  causticsEnabled = true;
    float causticsIntensity = 2.6f;     // gain added to the direct sun term at a cord
    float causticsScale = 6.0f;         // metres per tile; the pattern is ~16 cells wide, so this
                                        // sets the cell size (6 m -> ~37 cm cells)
    float causticsSpeed = 10.0f;        // flipbook frames per second (the loop is 16 frames)
    float causticsDepthFade = 14.0f;    // metres below the surface over which it fades to nothing
    float causticsSurfaceFade = 0.4f;   // metres of fade-in right under the surface (kills the waterline seam)
    float causticsUpFacing = 0.7f;      // 0 = ignore the normal, 1 = full N.up gate
    // The flipbook stores ray DENSITY, so there is one physically right value here: whatever
    // unfocused sunlight encodes to (tools/gen_caustics.py prints it). At that setting the dark
    // cells stay neutral and only the cords add light.
    float causticsBias = 0.186f;
    float causticsDispersion = 0.25f;   // chromatic split in texels (0 = monochrome, and 3x cheaper)
    float causticsLayerBlend = 0.6f;    // second de-tiling layer, min-combined (0 = single layer)
    Math::float4 causticsTint = Math::float4(0.85f, 1.0f, 0.95f, 1.0f);

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

namespace ocean
{
// "--ocean-wind=<0..1>": boot-time override applied AFTER the level's own wind sources (inline
// block and wind entity both), so a headless capture can be taken at a chosen sea state — the
// artifacts this exists to check only show up in a swell. Negative = no override. Diagnostic only;
// never written back to any config.
inline float g_windForceOverride = -1.0f;

// "--ocean-geomfade=<metres>": override shoreGeometryFadeDistance for a headless capture without
// touching the level file. Negative = no override. Diagnostic only.
inline float g_geometryFadeOverride = -1.0f;
}

OceanSimulationConfig CloneOceanSimulationConfig(const OceanSimulationConfig& config);
bool LoadOceanSimulationConfigFromFile(const std::wstring& path, OceanSimulationConfig& outConfig);
bool SaveOceanSimulationConfigToFile(const std::wstring& path, const OceanSimulationConfig& config);
