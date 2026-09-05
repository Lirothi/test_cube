#pragma once

#include <string>

#include "app/scene/SceneRenderConfig.h"

class Renderer;
class Scene;
struct SceneRenderSettings;

// One entry per persisted control in Developer Controls. Individual reset buttons route through
// GraphicsSettingsManager so they use the same canonical defaults as ResetAll and the JSON file.
enum class GraphicsControl
{
    // Contiguous per tab: ResetControl dispatches its Apply group by range (GraphicsSettings.cpp).
    AsyncCompute,                 // Frame
    VisibilityChunkMask,          // Visibility ...
    OcclusionMethod,
    OcclusionQueryLatency,
    OcclusionIndirectQueries,
    IndirectGBuffer,
    GbufferHzb,
    DlssEnabled,                  // AA / Scale ...
    DlssMode,
    Fxaa,
    RenderScale,
    SsrTechnique,                 // Reflections ...
    UeSsrQuality,
    UeSsrSteps,
    UeSsrRays,
    UeSsrGlossyRays,
    UeSsrUseSurfaceRoughness,
    UeSsrRoughnessOverride,
    UeSsrIntensity,
    UeSsrMaxRoughness,
    ReflectionTemporal,
    ReflectionTemporalBlend,
    ReflectionTemporalStillInertia,
    ReflectionResolution,
    ReflectionGlossyScale,
    SunMetalSpecInfluence,
    SunAngularSize,
    OceanReflectionResolution,
    ReflectionSource,
    RtAlphaMode,
    RtAlphaMissKeep,
    RtWindBlas,
    RtWindBlasRadius,
    FogGridPixels,                // Fog
    FogGridZ,
    LodEnabled,                   // LOD ...
    LodBound0,
    LodBound1,
    LodBound2,
    LodFadeBand,
    ChunkLodDistance,
    ChunkLodFactor,
    ShadowMode,
    CsmMaxDistance,
    CsmAutoSplits,
    CsmDistributionExponent,
    CsmSplitDistances,
    CsmOverlap,
    CsmZPadding,
    CsmCasterReach,
    CsmScissorOptim,
    CsmScissorPad,
    CsmAccurateCasterCull,
    CsmHzbCull,
    CsmPancake,
    CsmPancakeSlack,
    CsmDepthBias,
    CsmSlopeScale,
    CsmMaxSlope,
    CsmNormalBias,
    CsmBlendFraction,
    CsmDistanceFade,
    CsmFilter,
    CsmSharpen,
    CsmReceiverBias,
    CsmOverBlur,
    ContactEnabled,
    ContactLocalMode,
    ContactTemporal,
    ContactLengthWorldSpace,
    ContactLength,
    ContactIntensity,
    ContactSteps,
    ContactThickness,
    ContactNormalOffset,
    ContactGrazingFade,
    ContactMinDistance,
    ContactMaxDistance,
    ContactFadeBand,
    GiIndirectShadows,
    ShadowLodBias,
    ShadowLodBiasNearTier,
    ShadowLodTierStride,
    VsmRefDistance,
    VsmRequestDownscale,
    VsmLru,
    VsmClipmapBaseExtent,
    VsmClipmapBlend,
    VsmClipmapBlendWidth,
    VsmSmrtEnabled,
    VsmSmrtRays,
    VsmSmrtSamples,
    VsmSmrtTemporal,
    VsmSmrtAdaptive,
    VsmSmrtMargin,
    VsmSmrtSunAngle,
    VsmSmrtTexelDither,
    VsmSmrtRayLength,
    VsmDepthBias,
    VsmDepthBiasDecay,
    VsmDepthBiasFloor,
    VsmNormalBias,
    VsmLocalLateralBias,
    VsmLocalDepthPush,
    VsmResidentOnly,
    VsmSingleDraw,
    VsmHzbCull,
    VsmPageCaching,
    VsmWindAnimateMaxLevel,
};

// Persists project-wide graphics quality. Scene-authored look settings (GTAO, atmosphere, bloom,
// exposure and colour grading) stay in the level. The CSM fit has no level-serialization path, so
// it is a project default here and is re-applied whenever a level is loaded.
class GraphicsSettingsManager
{
public:
    static constexpr const char* kPath = "graphics_settings.json";

    void Initialize(Renderer& renderer, Scene& scene, SceneRenderSettings& settings);
    void ApplySceneSettings(Scene& scene) const;

    void ObserveUiEdit(Renderer& renderer, Scene& scene,
                       const SceneRenderSettings& settings,
                       bool graphicsSettingsDirty,
                       float deltaTime);
    void Flush(Renderer& renderer, const Scene& scene, const SceneRenderSettings& settings);

    bool SaveCurrent(Renderer& renderer, const Scene& scene, const SceneRenderSettings& settings);
    bool Reload(Renderer& renderer, Scene& scene, SceneRenderSettings& settings);
    bool ResetAll(Renderer& renderer, Scene& scene, SceneRenderSettings& settings);
    bool ResetUpscale(Renderer& renderer, const Scene& scene, SceneRenderSettings& settings);
    bool ResetVisibility(Renderer& renderer, const Scene& scene, const SceneRenderSettings& settings);
    bool ResetReflections(Renderer& renderer, const Scene& scene, SceneRenderSettings& settings);
    bool ResetFog(Renderer& renderer, const Scene& scene, const SceneRenderSettings& settings);
    bool ResetLod(Renderer& renderer, const Scene& scene, const SceneRenderSettings& settings);
    bool ResetCsm(Renderer& renderer, Scene& scene, const SceneRenderSettings& settings);
    bool ResetContactShadows(Renderer& renderer, const Scene& scene,
                             const SceneRenderSettings& settings);
    bool ResetVsm(Renderer& renderer, const Scene& scene, const SceneRenderSettings& settings);
    bool ResetControl(GraphicsControl control, Renderer& renderer, Scene& scene,
                      SceneRenderSettings& settings);

    const std::string& Status() const { return status_; }

private:
    bool initialized_ = false;
    bool savePending_ = false;
    float saveDelaySeconds_ = 0.0f;
    std::string status_;
    CascadeShadowConfig projectCsm_{};
};
