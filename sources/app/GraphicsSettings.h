#pragma once

#include <map>
#include <string>
#include <vector>

#include "rendering/scene/SceneRenderConfig.h"

class Renderer;
class Scene;
struct SceneRenderSettings;

// One entry per persisted control in Developer Controls. Individual reset buttons route through
// GraphicsSettingsManager so they use the same canonical defaults as ResetAll and the JSON file.
enum class GraphicsControl
{
    // Contiguous per tab: ResetControl dispatches its Apply group by range (GraphicsSettings.cpp).
    AsyncCompute,                 // Frame ...
    VSync,
    MaxFps,
    MouseSmoothing,
    RenderThread,                 // game thread plan G8: frame.renderThread
    OneFrameThreadLag,            // ... and frame.oneFrameThreadLag (UE r.OneFrameThreadLag)
    RhiThread,                    // ... and frame.rhiThread (G10b, UE r.RHIThread.Enable)
    VisibilityChunkMask,          // Visibility ...
    OcclusionMethod,
    OcclusionQueryLatency,
    OcclusionIndirectQueries,
    IndirectGBuffer,
    GbufferHzb,
    RasterBindless,               // texture streaming A6
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
    WaterReflectionTemporal,
    WaterReflectionTemporalBlend,
    WaterReflectionTemporalStillInertia,
    WaterReflectionSurfaceNormal,
    WaterReflectionNormalCascades,
    WaterReflectionNormalFilter,
    WaterReflectionPrefilter,
    ReflectionResolution,
    ReflectionGlossyScale,
    SunMetalSpecInfluence,
    OceanReflectionResolution,
    ReflectionSource,
    RtAlphaMode,
    RtAlphaMissKeep,
    RtWindBlas,
    RtWindBlasRadius,
    FogGridPixels,                // Fog
    FogGridZ,
    CloudShadowUpdateFrames,      // Sky ...
    CloudShadowBilinearWeather,
    CloudShadowDetailMean,
    CloudTraceMode,
    CloudCaptureTexels,
    LodEnabled,                   // LOD ...
    LodUeScreenSize,              // UE's per-mesh screen sizes (render::g_lodUeScreenSize) ...
    LodPixelError,
    LodDistanceScale,
    LodBound0,                    // ... or the global distance/radius bounds
    LodBound1,
    LodBound2,
    LodTemporalLag,
    ChunkLodDistance,
    ChunkLodFactor,
    ShadowMode,
    LocalShadowWindRange,         // the atlas lamps' sway range (render::g_localShadowWindRange)
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
    CsmAtlasRes,
    // S15 (SDSM). In the CSM group on purpose: they tune the same atlas and the same filter, and
    // the CSM tab's Reset button is what a user reaches for after moving them.
    SdsmPartitions,
    SdsmBorder,
    SdsmDilation,
    SdsmMinScale,
    SdsmZMargin,
    SdsmStability,
    SdsmCacheFrames,
    SdsmCacheDilation,
    SdsmTexelSnap,
    SdsmSizeHysteresis,
    SdsmEvsm,
    SdsmEvsmPos,
    SdsmEvsmNeg,
    SdsmBlur,
    SdsmBlurMax,
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
    VsmClipmapFirstLevel,
    VsmResolutionLodBiasDirectional,
    VsmSmrtEnabled,
    VsmSmrtRays,
    VsmSmrtSamples,
    VsmLocalSmrtRays,
    VsmLocalSmrtSamples,
    VsmSmrtTemporal,
    VsmSmrtAdaptive,
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
    TessyEnabled,                 // Tessy: the master switch (every Tessy path)
    TessyVsm,                     // Tessy N3: the VSM page render draws clusters
    TessyRaster,                  // Tessy N6: ...through the mesh shader (1) or the VS (0)
    TessyAtlas,                   // Tessy N4: the local lights' atlas views draw clusters
    TessyGBuffer,                 // Tessy N5: the camera's G-buffer draws clusters
    TessySdsm,                    // Tessy: the SDSM sun partitions draw clusters
    TessyLod,                     // Tessy N7: the cluster DAG's cut
    TessyLodPixelError,           // Tessy N7: the camera's DAG cut threshold (pixels)
    VsmWindRange,
    StreamingEnabled,             // Streaming (texture streaming A3)
    StreamingPoolSizeMB,
    StreamingMipBias,
    StreamingBoost,
    StreamingHiddenScale,
    StreamingMipFade,
    StreamingPerTextureBias,
    StreamingAnisotropy,          // the material samplers' anisotropy (render::g_textureAnisotropy)
    GiEnabled,                    // GI (the DDGI probe volume, SceneRenderSettings::gi*)
    GiSpacing,
    GiHysteresis,
    GiIntensity,
    GiFar,                        // the far cascade (SceneRenderSettings::giFarCascade)
    GiFarSpacing,
    GiDivisor,                    // the trace's price (SceneRenderSettings::giUpdateDivisor ... giSleepPeriod)
    GiFarDivisor,
    GiRays,
    GiFixedRayPeriod,
    GiSleepPeriod,
    GiRayDistance,
    GiInfiniteBounce,
    GiBounceVisibility,
    GiSkyRay,
    GiLights,
    SkyVisEnabled,                // the RT sky visibility (SceneRenderSettings::skyVis*), on the GI tab
    SkyVisRays,
    SkyVisHistory,
    SkyVisMaxDistance,
    SkyVisDiffuse,
    SkyVisSpecular,
    SkyVisFilter,
    SkyVisFogRays,                // ...and the volumetric fog's (SceneRenderSettings::skyVisFogRays)
    SkyVisFilterRadius,           // ...its spatial step's reach (SceneRenderSettings::skyVisFilterRadius)
    SkyVisSeed,                   // ...and the seed of a texel without history (SceneRenderSettings::skyVisSeed)
    SkyVisFallback,               // ...without the trace, from above (SceneRenderSettings::skyVisFallback)
    SkyVisFallbackResolution,     // ...its map's texels a side
    SkyVisFallbackTexel,          // ...and the world size of one (m)
    SkyVisFallbackFade,           // ...and its edge's fade (share of the radius it traces in full)
};

// The scalability table's `--set` name a control writes (data/config/scalability.json's knobs), or nullptr. Developer
// Controls marks a control a graphics level manages, and one changed by hand over it (docs/scalability_plan.md §3.3).
const char* ScalabilityKnobOf(GraphicsControl control);

// Persists project-wide graphics quality. Scene-authored look settings (GTAO, height fog, bloom,
// exposure and colour grading) stay in the level. The CSM fit has no level-serialization path, so
// it is a project default here and is re-applied whenever a level is loaded.
//
// Where (docs/scalability_plan.md §3.3, §6.4; since 2026-11-16): user_settings.json's "graphics" section holds only what
// differs from the build's defaults (config/graphics_defaults.json in the packages, a dev build's
// tools/dist/graphics_defaults.json); a Reset goes back to the graphics levels and the defaults. The old
// graphics_settings.json (a dev build's full snapshot in the repo root) is read once to move it there and never written.
class GraphicsSettingsManager
{
public:
    static constexpr const char* kPath = "user_settings.json";
    static constexpr const char* kLegacyPath = "graphics_settings.json";

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
    bool ResetStreaming(Renderer& renderer, const Scene& scene, const SceneRenderSettings& settings);
    bool ResetGi(Renderer& renderer, const Scene& scene, SceneRenderSettings& settings);
    bool ResetControl(GraphicsControl control, Renderer& renderer, Scene& scene,
                      SceneRenderSettings& settings);
    // The Reset button of the control whose setting this is (a persisted row's --set name, or a control's scalability
    // knob: dlss.mode, shadow.mode, ...): --graphics-reset-at-dump=<setting>. False when no control has it.
    bool ResetSetting(const std::string& setting, Renderer& renderer, Scene& scene, SceneRenderSettings& settings);

    const std::string& Status() const { return status_; }

    // --settings-dump's graphics lines (architecture plan R2.n's gate): the file Save would write, whole (not only what
    // differs from the defaults), as `<prefix><json path> <json value>` lines.
    void DescribeFile(Renderer& renderer, const Scene& scene, const SceneRenderSettings& settings, const char* prefix,
                      std::vector<std::string>& lines) const;

private:
    // After a Reset applied the defaults: the graphics levels take their knobs back, then the save.
    bool FinishReset(const std::map<std::string, float>& managedBefore, Renderer& renderer, const Scene& scene,
                     const SceneRenderSettings& settings);

    bool initialized_ = false;
    bool savePending_ = false;
    float saveDelaySeconds_ = 0.0f;
    std::string status_;
    CascadeShadowConfig projectCsm_{};
};
