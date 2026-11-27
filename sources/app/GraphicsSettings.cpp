#include "app/GraphicsSettings.h"
#include "app/settings/Scalability.h" // the managed knobs are saved with their own values (CaptureScalabilityOriginals)
#include "app/settings/UserSettings.h" // user_settings.json's graphics section
#include "core/logging/Log.h"
#include "core/vfs/Vfs.h"

#include <type_traits>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>

#include "rendering/scene/SceneFrameData.h"
#include "app/scene/Scene.h"
#include "rendering/scene/SkyOcclusionMap.h" // skyVis.fallbackResolution / fallbackTexel ranges
#include "rendering/core/Renderer.h"
#include "rendering/streaming/StreamingSettings.h" // A3: streaming.* globals
#include "rendering/core/VisibilityStats.h"
#include "rendering/debug/LodDebugView.h"
#include "rendering/lighting/VolumetricCloudSettings.h" // render::g_cloudShadowUpdateFrames_game
#include "rendering/meshes/LodSelect.h"
#include "rendering/shadows/ShadowSettings.h"
#include "rendering/core/RasterSettings.h" // A6: rasterBindless
#include "rendering/shadows/VirtualShadowMap.h"
#include "rendering/shadows/TessySettings.h" // Tessy N3: tessy::g_vsm_game
#include "rendering/visibility/OcclusionHistory.h"
#include "core/threading/RenderThread.h" // G8: threading::g_renderThreadEnabled
#include "third_party/json/json.hpp"
#include "app/settings/SettingContext.h"    // R2.n: the graphics file through the registry
#include "core/settings/SettingsRegistry.h"
#include <array>
#include <cstring>

namespace
{
    using json = nlohmann::json;


    const char* DlssModeName(sl::DLSSMode mode)
    {
        switch (mode)
        {
        case sl::DLSSMode::eOff:              return "off";
        case sl::DLSSMode::eMaxPerformance:   return "maxPerformance";
        case sl::DLSSMode::eBalanced:         return "balanced";
        case sl::DLSSMode::eMaxQuality:       return "maxQuality";
        case sl::DLSSMode::eUltraPerformance: return "ultraPerformance";
        case sl::DLSSMode::eUltraQuality:     return "ultraQuality";
        case sl::DLSSMode::eDLAA:             return "dlaa";
        default:                              return "balanced";
        }
    }

    sl::DLSSMode ParseDlssMode(const json& value, sl::DLSSMode fallback)
    {
        if (!value.is_string()) { return fallback; }
        const std::string name = value.get<std::string>();
        if (name == "off")              { return sl::DLSSMode::eOff; }
        if (name == "maxPerformance")   { return sl::DLSSMode::eMaxPerformance; }
        if (name == "balanced")         { return sl::DLSSMode::eBalanced; }
        if (name == "maxQuality")       { return sl::DLSSMode::eMaxQuality; }
        if (name == "ultraPerformance") { return sl::DLSSMode::eUltraPerformance; }
        if (name == "ultraQuality")     { return sl::DLSSMode::eUltraQuality; }
        if (name == "dlaa")             { return sl::DLSSMode::eDLAA; }
        return fallback;
    }

    const char* ReflectionSourceName(ReflectionSource source)
    {
        switch (source)
        {
        case ReflectionSource::None:    return "none";
        case ReflectionSource::SkyOnly: return "skyOnly";
        case ReflectionSource::SSR:     return "ssr";
        case ReflectionSource::RT:      return "rt";
        default:                        return "rt";
        }
    }

    ReflectionSource ParseReflectionSource(const json& value, ReflectionSource fallback)
    {
        if (!value.is_string()) { return fallback; }
        const std::string name = value.get<std::string>();
        if (name == "none")    { return ReflectionSource::None; }
        if (name == "skyOnly") { return ReflectionSource::SkyOnly; }
        if (name == "ssr")     { return ReflectionSource::SSR; }
        if (name == "rt")      { return ReflectionSource::RT; }
        return fallback;
    }

    const char* SsrTechniqueName(SsrTechnique technique)
    {
        return technique == SsrTechnique::UeHzb ? "ueHzb" : "logMarch";
    }

    SsrTechnique ParseSsrTechnique(const json& value, SsrTechnique fallback)
    {
        if (!value.is_string()) { return fallback; }
        const std::string name = value.get<std::string>();
        if (name == "logMarch") { return SsrTechnique::LogMarch; }
        if (name == "ueHzb")    { return SsrTechnique::UeHzb; }
        return fallback;
    }

    const char* ShadowModeName(render::ShadowMode mode)
    {
        switch (mode)
        {
        case render::ShadowMode::Legacy: return "legacy";
        case render::ShadowMode::Sdsm:   return "sdsm";
        default:                         return "vsm";
        }
    }

    render::ShadowMode ParseShadowMode(const json& value, render::ShadowMode fallback)
    {
        if (!value.is_string()) { return fallback; }
        const std::string name = value.get<std::string>();
        if (name == "legacy") { return render::ShadowMode::Legacy; }
        if (name == "vsm")    { return render::ShadowMode::VSM; }
        if (name == "sdsm")   { return render::ShadowMode::Sdsm; }
        return fallback;
    }

    template <typename T>
    void Read(const json& object, const char* key, T& out)
    {
        if (!object.is_object()) { return; }
        const auto it = object.find(key);
        if (it == object.end()) { return; }
        if constexpr (std::is_same_v<T, bool>)
        {
            if (it->is_boolean()) { out = it->get<bool>(); }
        }
        else if constexpr (std::is_integral_v<T>)
        {
            if (it->is_number_integer() || it->is_number_unsigned()) { out = it->get<T>(); }
        }
        else if constexpr (std::is_floating_point_v<T>)
        {
            if (it->is_number()) { out = it->get<T>(); }
        }
    }

    const json& Section(const json& root, const char* key)
    {
        static const json empty = json::object();
        if (!root.is_object()) { return empty; }
        const auto it = root.find(key);
        return it != root.end() && it->is_object() ? *it : empty;
    }

    // R2.n (architecture plan): the persisted graphics through the settings registry. One entry a knob: its path in
    // user_settings.json's graphics section (the file's format is what it was), its --set name (the row that writes and
    // reads it -- its clamp is the one, decision В11 (а)), the JSON kind, the Developer Controls control a single Reset
    // moves, the tab a tab's Reset moves. In the order the graphics are applied (the UE SSR preset after its fields:
    // ApplyEntries). Every row an entry names is SettingPersistent, and only those and the kept-in-code parts' rows
    // (PersistentFlagMismatches, --scalability-selftest). Kept in
    // code: the DLSS trio (the renderer's own calls, coupled), the shadow mode (a --shadow-mode boot override keeps the
    // live mode, the file's is remembered) and the CSM fit (a struct the scene owns and gets back at each level load).
    enum class PKind : std::uint8_t { Bool, Int, Uint, Float, ReflectionSource, SsrTechnique };
    enum class PTab : std::uint8_t { Frame, Visibility, Upscale, Reflections, Fog, Sky, Streaming, Gi, Lod, Contact, Sdsm, Vsm, Csm };
    constexpr GraphicsControl kNoControl = static_cast<GraphicsControl>(0x7FFFFFFF); // a tab's Reset alone moves it
    struct PEntry
    {
        const char* path;
        const char* setting;
        PKind kind;
        GraphicsControl control;
        PTab tab;
    };
    const PEntry kEntries[] = {
        { "performance.asyncCompute", "frame.asyncCompute", PKind::Bool, GraphicsControl::AsyncCompute, PTab::Frame }, // asyncCompute
        { "frame.vsync", "frame.vsync", PKind::Bool, GraphicsControl::VSync, PTab::Frame }, // vsync
        { "frame.maxFps", "frame.maxFps", PKind::Int, GraphicsControl::MaxFps, PTab::Frame }, // maxFps
        { "frame.mouseSmoothingMs", "camera.mouseSmoothing", PKind::Float, GraphicsControl::MouseSmoothing, PTab::Frame }, // mouseSmoothingMs
        { "frame.renderThread", "frame.renderThread", PKind::Bool, GraphicsControl::RenderThread, PTab::Frame }, // renderThread
        { "frame.oneFrameThreadLag", "frame.oneFrameThreadLag", PKind::Bool, GraphicsControl::OneFrameThreadLag, PTab::Frame }, // oneFrameThreadLag
        { "frame.rhiThread", "frame.rhiThread", PKind::Bool, GraphicsControl::RhiThread, PTab::Frame }, // rhiThread
        { "visibility.chunkMask", "vis.chunkMask", PKind::Bool, GraphicsControl::VisibilityChunkMask, PTab::Visibility }, // visibilityChunkMask
        { "visibility.occlusionMethod", "vis.method", PKind::Int, GraphicsControl::OcclusionMethod, PTab::Visibility }, // occlusionMethod
        { "visibility.queryLatency", "vis.queryLatency", PKind::Int, GraphicsControl::OcclusionQueryLatency, PTab::Visibility }, // occlusionQueryLatency
        { "visibility.queryGpuDrivenObjects", "vis.indirectQueries", PKind::Bool, GraphicsControl::OcclusionIndirectQueries, PTab::Visibility }, // occlusionIndirectQueries
        { "performance.gpuDrivenGBuffer", "gbuffer.indirect", PKind::Bool, GraphicsControl::IndirectGBuffer, PTab::Visibility }, // indirectGBuffer
        { "performance.gbufferHzbCull", "gbuffer.hzb", PKind::Bool, GraphicsControl::GbufferHzb, PTab::Visibility }, // gbufferHzbCull
        { "performance.rasterBindless", "raster.bindless", PKind::Bool, GraphicsControl::RasterBindless, PTab::Visibility }, // rasterBindless
        { "antiAliasing.fxaa", "aa.fxaa", PKind::Bool, GraphicsControl::Fxaa, PTab::Upscale }, // fxaa
        { "reflections.ssrTechnique", "ssr.technique", PKind::SsrTechnique, GraphicsControl::SsrTechnique, PTab::Reflections }, // ssrTechnique
        { "reflections.ueSsr.preset", "ssr.ueQuality", PKind::Uint, GraphicsControl::UeSsrQuality, PTab::Reflections }, // ssrUe.preset
        { "reflections.ueSsr.stepsPerRay", "ssr.ueSteps", PKind::Uint, GraphicsControl::UeSsrSteps, PTab::Reflections }, // ssrUe.numSteps
        { "reflections.ueSsr.raysPerPixel", "ssr.ueRays", PKind::Uint, GraphicsControl::UeSsrRays, PTab::Reflections }, // ssrUe.numRays
        { "reflections.ueSsr.glossyRays", "ssr.ueGlossy", PKind::Bool, GraphicsControl::UeSsrGlossyRays, PTab::Reflections }, // ssrUe.glossyRays
        { "reflections.ueSsr.useSurfaceRoughness", "ssr.ueUseSurfaceRoughness", PKind::Bool, GraphicsControl::UeSsrUseSurfaceRoughness, PTab::Reflections }, // ssrUe.useSurfaceRoughness
        { "reflections.ueSsr.roughnessOverride", "ssr.ueRoughnessOverride", PKind::Float, GraphicsControl::UeSsrRoughnessOverride, PTab::Reflections }, // ssrUe.roughnessOverride
        { "reflections.ueSsr.intensity", "ssr.ueIntensity", PKind::Float, GraphicsControl::UeSsrIntensity, PTab::Reflections }, // ssrUe.intensity
        { "reflections.ueSsr.maxRoughness", "ssr.ueMaxRoughness", PKind::Float, GraphicsControl::UeSsrMaxRoughness, PTab::Reflections }, // ssrUe.maxRoughness
        { "reflections.temporal", "ssr.temporal", PKind::Bool, GraphicsControl::ReflectionTemporal, PTab::Reflections }, // reflectionTemporal
        { "reflections.temporalBlend", "ssr.temporalBlend", PKind::Float, GraphicsControl::ReflectionTemporalBlend, PTab::Reflections }, // reflectionTemporalBlend
        { "reflections.temporalStillInertia", "ssr.temporalClampExpand", PKind::Float, GraphicsControl::ReflectionTemporalStillInertia, PTab::Reflections }, // reflectionTemporalStillInertia
        { "reflections.waterTemporal", "ssr.waterTemporal", PKind::Bool, GraphicsControl::WaterReflectionTemporal, PTab::Reflections }, // waterReflectionTemporal
        { "reflections.waterTemporalBlend", "ssr.waterTemporalBlend", PKind::Float, GraphicsControl::WaterReflectionTemporalBlend, PTab::Reflections }, // waterReflectionTemporalBlend
        { "reflections.waterTemporalStillInertia", "ssr.waterTemporalInertia", PKind::Float, GraphicsControl::WaterReflectionTemporalStillInertia, PTab::Reflections }, // waterReflectionTemporalStillInertia
        { "reflections.waterSurfaceNormal", "ssr.waterSurfaceNormal", PKind::Bool, GraphicsControl::WaterReflectionSurfaceNormal, PTab::Reflections }, // waterReflectionSurfaceNormal
        { "reflections.waterNormalCascades", "ssr.waterNormalCascades", PKind::Uint, GraphicsControl::WaterReflectionNormalCascades, PTab::Reflections }, // waterReflectionNormalCascades
        { "reflections.waterNormalFilter", "ocean.reflectionNormalFilter", PKind::Float, GraphicsControl::WaterReflectionNormalFilter, PTab::Reflections }, // waterReflectionNormalFilter
        { "reflections.waterPrefilter", "ocean.reflectionPrefilter", PKind::Float, GraphicsControl::WaterReflectionPrefilter, PTab::Reflections }, // waterReflectionPrefilter
        { "reflections.glossyBlur", "reflections.glossyBlur", PKind::Float, GraphicsControl::ReflectionGlossyScale, PTab::Reflections }, // reflectionGlossyScale
        { "reflections.sunMetalSpec", "light.sunMetalSpec", PKind::Float, GraphicsControl::SunMetalSpecInfluence, PTab::Reflections }, // sunMetalSpecInfluence
        { "reflections.source", "render.reflectionSource", PKind::ReflectionSource, GraphicsControl::ReflectionSource, PTab::Reflections }, // reflectionSource
        { "reflections.rtFoliageAlphaMode", "rt.alphaMode", PKind::Uint, GraphicsControl::RtAlphaMode, PTab::Reflections }, // rtAlphaMode
        { "reflections.rtFoliageFill", "rt.alphaMissKeep", PKind::Float, GraphicsControl::RtAlphaMissKeep, PTab::Reflections }, // rtAlphaMissKeep
        { "reflections.rtWindSway", "rt.windBlas", PKind::Bool, GraphicsControl::RtWindBlas, PTab::Reflections }, // rtWindBlas
        { "reflections.rtWindRadius", "rt.windRadius", PKind::Float, GraphicsControl::RtWindBlasRadius, PTab::Reflections }, // rtWindBlasRadius
        { "reflections.resolution", "reflections.resolution", PKind::Float, GraphicsControl::ReflectionResolution, PTab::Reflections }, // reflectionResolution
        { "reflections.oceanResolution", "reflections.oceanResolution", PKind::Float, GraphicsControl::OceanReflectionResolution, PTab::Reflections }, // oceanReflectionResolution
        { "performance.fogGridPixels", "fog.gridPixels", PKind::Int, GraphicsControl::FogGridPixels, PTab::Fog }, // fogGridPixels
        { "performance.fogGridZ", "fog.gridZ", PKind::Int, GraphicsControl::FogGridZ, PTab::Fog }, // fogGridZ
        { "cloudShadow.updateFrames", "cloudShadow.updateFrames", PKind::Uint, GraphicsControl::CloudShadowUpdateFrames, PTab::Sky }, // cloudShadowUpdateFrames
        { "cloudShadow.bilinearWeather", "cloudShadow.bilinearWeather", PKind::Bool, GraphicsControl::CloudShadowBilinearWeather, PTab::Sky }, // cloudShadowBilinearWeather
        { "cloudShadow.detailMean", "cloudShadow.detailMean", PKind::Bool, GraphicsControl::CloudShadowDetailMean, PTab::Sky }, // cloudShadowDetailMean
        { "cloudTrace.mode", "cloudTrace.mode", PKind::Uint, GraphicsControl::CloudTraceMode, PTab::Sky }, // cloudTraceMode
        { "cloudTrace.captureTexels", "cloud.captureTexels", PKind::Float, GraphicsControl::CloudCaptureTexels, PTab::Sky }, // cloudCaptureTexels
        { "streaming.enabled", "streaming.enabled", PKind::Bool, GraphicsControl::StreamingEnabled, PTab::Streaming }, // streamingEnabled
        { "streaming.poolSizeMB", "streaming.poolSizeMB", PKind::Int, GraphicsControl::StreamingPoolSizeMB, PTab::Streaming }, // streamingPoolSizeMB
        { "streaming.mipBias", "streaming.mipBias", PKind::Int, GraphicsControl::StreamingMipBias, PTab::Streaming }, // streamingMipBias
        { "streaming.boost", "streaming.boost", PKind::Float, GraphicsControl::StreamingBoost, PTab::Streaming }, // streamingBoost
        { "streaming.hiddenScale", "streaming.hiddenScale", PKind::Float, GraphicsControl::StreamingHiddenScale, PTab::Streaming }, // streamingHiddenScale
        { "streaming.mipFade", "streaming.mipFade", PKind::Bool, GraphicsControl::StreamingMipFade, PTab::Streaming }, // streamingMipFade
        { "streaming.perTextureBias", "streaming.perTextureBias", PKind::Bool, GraphicsControl::StreamingPerTextureBias, PTab::Streaming }, // streamingPerTextureBias
        { "streaming.anisotropy", "textures.anisotropy", PKind::Uint, GraphicsControl::StreamingAnisotropy, PTab::Streaming }, // textureAnisotropy
        { "gi.enabled", "gi.enabled", PKind::Bool, GraphicsControl::GiEnabled, PTab::Gi }, // giEnabled
        { "gi.spacing", "gi.spacing", PKind::Float, GraphicsControl::GiSpacing, PTab::Gi }, // giSpacing
        { "gi.hysteresis", "gi.hysteresis", PKind::Float, GraphicsControl::GiHysteresis, PTab::Gi }, // giHysteresis
        { "gi.intensity", "gi.intensity", PKind::Float, GraphicsControl::GiIntensity, PTab::Gi }, // giIntensity
        { "gi.farCascade", "gi.far", PKind::Bool, GraphicsControl::GiFar, PTab::Gi }, // giFar
        { "gi.farSpacing", "gi.farSpacing", PKind::Float, GraphicsControl::GiFarSpacing, PTab::Gi }, // giFarSpacing
        { "gi.divisor", "gi.divisor", PKind::Uint, GraphicsControl::GiDivisor, PTab::Gi }, // giDivisor
        { "gi.farDivisor", "gi.farDivisor", PKind::Uint, GraphicsControl::GiFarDivisor, PTab::Gi }, // giFarDivisor
        { "gi.rays", "gi.rays", PKind::Uint, GraphicsControl::GiRays, PTab::Gi }, // giRays
        { "gi.fixedRayPeriod", "gi.fixedRayPeriod", PKind::Uint, GraphicsControl::GiFixedRayPeriod, PTab::Gi }, // giFixedRayPeriod
        { "gi.sleepPeriod", "gi.sleepPeriod", PKind::Uint, GraphicsControl::GiSleepPeriod, PTab::Gi }, // giSleepPeriod
        { "gi.rayDistance", "gi.rayDistance", PKind::Float, GraphicsControl::GiRayDistance, PTab::Gi }, // giRayDistance
        { "gi.infiniteBounce", "gi.infiniteBounce", PKind::Bool, GraphicsControl::GiInfiniteBounce, PTab::Gi }, // giInfiniteBounce
        { "gi.bounceVisibility", "gi.bounceVisibility", PKind::Bool, GraphicsControl::GiBounceVisibility, PTab::Gi }, // giBounceVisibility
        { "gi.skyRay", "gi.skyRay", PKind::Bool, GraphicsControl::GiSkyRay, PTab::Gi }, // giSkyRay
        { "gi.lights", "gi.lights", PKind::Bool, GraphicsControl::GiLights, PTab::Gi }, // giLights
        { "skyVis.enabled", "skyVis.enabled", PKind::Bool, GraphicsControl::SkyVisEnabled, PTab::Gi }, // skyVisEnabled
        { "skyVis.rays", "skyVis.rays", PKind::Uint, GraphicsControl::SkyVisRays, PTab::Gi }, // skyVisRays
        { "skyVis.history", "skyVis.history", PKind::Float, GraphicsControl::SkyVisHistory, PTab::Gi }, // skyVisHistory
        { "skyVis.maxDistance", "skyVis.maxDistance", PKind::Float, GraphicsControl::SkyVisMaxDistance, PTab::Gi }, // skyVisMaxDistance
        { "skyVis.diffuse", "skyVis.diffuse", PKind::Float, GraphicsControl::SkyVisDiffuse, PTab::Gi }, // skyVisDiffuse
        { "skyVis.specular", "skyVis.specular", PKind::Float, GraphicsControl::SkyVisSpecular, PTab::Gi }, // skyVisSpecular
        { "skyVis.spatialFilter", "skyVis.filter", PKind::Bool, GraphicsControl::SkyVisFilter, PTab::Gi }, // skyVisFilter
        { "skyVis.fogRays", "skyVis.fogRays", PKind::Uint, GraphicsControl::SkyVisFogRays, PTab::Gi }, // skyVisFogRays
        { "skyVis.filterRadius", "skyVis.filterRadius", PKind::Uint, GraphicsControl::SkyVisFilterRadius, PTab::Gi }, // skyVisFilterRadius
        { "skyVis.seed", "skyVis.seed", PKind::Bool, GraphicsControl::SkyVisSeed, PTab::Gi }, // skyVisSeed
        { "skyVis.fallback", "skyVis.fallback", PKind::Bool, GraphicsControl::SkyVisFallback, PTab::Gi }, // skyVisFallback
        { "skyVis.fallbackResolution", "skyVis.fallbackResolution", PKind::Uint, GraphicsControl::SkyVisFallbackResolution, PTab::Gi }, // skyVisFallbackResolution
        { "skyVis.fallbackTexel", "skyVis.fallbackTexel", PKind::Float, GraphicsControl::SkyVisFallbackTexel, PTab::Gi }, // skyVisFallbackTexel
        { "skyVis.fallbackFade", "skyVis.fallbackFade", PKind::Float, GraphicsControl::SkyVisFallbackFade, PTab::Gi }, // skyVisFallbackFade
        { "lod.enabled", "lod.enabled", PKind::Bool, GraphicsControl::LodEnabled, PTab::Lod }, // lodEnabled
        { "lod.ueScreenSize", "lod.ue", PKind::Bool, GraphicsControl::LodUeScreenSize, PTab::Lod }, // lodUeScreenSize
        { "lod.uePixelError", "lod.pixelError", PKind::Float, GraphicsControl::LodPixelError, PTab::Lod }, // lodPixelError
        { "lod.distanceScale", "lod.distanceScale", PKind::Float, GraphicsControl::LodDistanceScale, PTab::Lod }, // lodDistanceScale
        { "lod.regularMeshRatios[0]", "lod.bound0", PKind::Float, GraphicsControl::LodBound0, PTab::Lod }, // lodBound0
        { "lod.regularMeshRatios[1]", "lod.bound1", PKind::Float, GraphicsControl::LodBound1, PTab::Lod }, // lodBound1
        { "lod.regularMeshRatios[2]", "lod.bound2", PKind::Float, GraphicsControl::LodBound2, PTab::Lod }, // lodBound2
        { "lod.temporalLag", "lod.temporalLag", PKind::Float, GraphicsControl::LodTemporalLag, PTab::Lod }, // lodTemporalLag
        { "lod.chunkDistance", "lod.chunkDist0", PKind::Float, GraphicsControl::ChunkLodDistance, PTab::Lod }, // chunkLodDistance
        { "lod.chunkFactor", "lod.chunkFactor", PKind::Float, GraphicsControl::ChunkLodFactor, PTab::Lod }, // chunkLodFactor
        { "shadows.contact.enabled", "contact.enabled", PKind::Bool, GraphicsControl::ContactEnabled, PTab::Contact }, // contactEnabled
        { "shadows.contact.localLightMode", "contact.localMode", PKind::Uint, GraphicsControl::ContactLocalMode, PTab::Contact }, // contactLocalMode
        { "shadows.contact.temporalDither", "contact.temporal", PKind::Bool, GraphicsControl::ContactTemporal, PTab::Contact }, // contactTemporalDither
        { "shadows.contact.lengthInWorldSpace", "contact.lengthInWorldSpace", PKind::Bool, GraphicsControl::ContactLengthWorldSpace, PTab::Contact }, // contactLengthInWorldSpace
        { "shadows.contact.length", "contact.length", PKind::Float, GraphicsControl::ContactLength, PTab::Contact }, // contactLength
        { "shadows.contact.intensity", "contact.intensity", PKind::Float, GraphicsControl::ContactIntensity, PTab::Contact }, // contactIntensity
        { "shadows.contact.steps", "contact.steps", PKind::Uint, GraphicsControl::ContactSteps, PTab::Contact }, // contactSteps
        { "shadows.contact.maxThickness", "contact.thickness", PKind::Float, GraphicsControl::ContactThickness, PTab::Contact }, // contactMaxThickness
        { "shadows.contact.normalOffset", "contact.normalOffset", PKind::Float, GraphicsControl::ContactNormalOffset, PTab::Contact }, // contactNormalOffset
        { "shadows.contact.grazingFade", "contact.grazingFade", PKind::Float, GraphicsControl::ContactGrazingFade, PTab::Contact }, // contactGrazingFade
        { "shadows.contact.minDistance", "contact.minDistance", PKind::Float, GraphicsControl::ContactMinDistance, PTab::Contact }, // contactMinDistance
        { "shadows.contact.maxDistance", "contact.maxDistance", PKind::Float, GraphicsControl::ContactMaxDistance, PTab::Contact }, // contactMaxDistance
        { "shadows.contact.fadeBand", "contact.fadeBand", PKind::Float, GraphicsControl::ContactFadeBand, PTab::Contact }, // contactFadeBand
        { "shadows.sdsm.partitions", "sdsm.partitions", PKind::Uint, GraphicsControl::SdsmPartitions, PTab::Sdsm }, // sdsmPartitions
        { "shadows.sdsm.borderTexels", "sdsm.borderTexels", PKind::Float, GraphicsControl::SdsmBorder, PTab::Sdsm }, // sdsmBorderTexels
        { "shadows.sdsm.dilation", "sdsm.dilation", PKind::Float, GraphicsControl::SdsmDilation, PTab::Sdsm }, // sdsmDilation
        { "shadows.sdsm.minScaleOverSphere", "sdsm.minScaleOverSphere", PKind::Float, GraphicsControl::SdsmMinScale, PTab::Sdsm }, // sdsmMinScaleOverSphere
        { "shadows.sdsm.zMargin", "sdsm.zMargin", PKind::Float, GraphicsControl::SdsmZMargin, PTab::Sdsm }, // sdsmZMargin
        { "shadows.sdsm.stabilityFrames", "sdsm.stabilityFrames", PKind::Float, GraphicsControl::SdsmStability, PTab::Sdsm }, // sdsmStabilityFrames
        { "shadows.sdsm.cacheFrames", "sdsm.cacheFrames", PKind::Uint, GraphicsControl::SdsmCacheFrames, PTab::Sdsm }, // sdsmCacheFrames
        { "shadows.sdsm.cacheDilation", "sdsm.cacheDilation", PKind::Float, GraphicsControl::SdsmCacheDilation, PTab::Sdsm }, // sdsmCacheDilation
        { "shadows.sdsm.texelSnap", "sdsm.texelSnap", PKind::Bool, GraphicsControl::SdsmTexelSnap, PTab::Sdsm }, // sdsmTexelSnap
        { "shadows.sdsm.sizeHysteresis", "sdsm.sizeHysteresis", PKind::Float, GraphicsControl::SdsmSizeHysteresis, PTab::Sdsm }, // sdsmSizeHysteresis
        { "shadows.sdsm.evsm", "sdsm.evsm", PKind::Bool, GraphicsControl::SdsmEvsm, PTab::Sdsm }, // sdsmEvsm
        { "shadows.sdsm.evsmPositiveExponent", "sdsm.evsmPos", PKind::Float, GraphicsControl::SdsmEvsmPos, PTab::Sdsm }, // sdsmEvsmPos
        { "shadows.sdsm.evsmNegativeExponent", "sdsm.evsmNeg", PKind::Float, GraphicsControl::SdsmEvsmNeg, PTab::Sdsm }, // sdsmEvsmNeg
        { "shadows.sdsm.evsmBlur", "sdsm.blur", PKind::Float, GraphicsControl::SdsmBlur, PTab::Sdsm }, // sdsmBlur
        { "shadows.sdsm.evsmBlurMaxTexels", "sdsm.blurMaxTexels", PKind::Float, GraphicsControl::SdsmBlurMax, PTab::Sdsm }, // sdsmBlurMaxTexels
        { "shadows.gpuInstancedCasters", "shadow.giIndirect", PKind::Bool, GraphicsControl::GiIndirectShadows, PTab::Vsm }, // giIndirectShadows
        { "shadows.localLightWindRange", "shadow.localWindRange", PKind::Float, GraphicsControl::LocalShadowWindRange, PTab::Vsm }, // localShadowWindRange
        { "shadows.localLightMaxResolution", "shadow.localMaxRes", PKind::Uint, kNoControl, PTab::Vsm }, // localShadowMaxRes
        { "shadows.vsm.shadowLodBias", "vsm.shadowLodBias", PKind::Int, GraphicsControl::ShadowLodBias, PTab::Vsm }, // shadowLodBias
        { "shadows.vsm.biasNearestTier", "vsm.shadowLodBiasNearTier", PKind::Bool, GraphicsControl::ShadowLodBiasNearTier, PTab::Vsm }, // shadowLodBiasNearTier
        { "shadows.vsm.shadowTiersPerLod", "vsm.shadowLodTierStride", PKind::Int, GraphicsControl::ShadowLodTierStride, PTab::Vsm }, // shadowLodTierStride
        { "shadows.vsm.lodRefDistance", "vsm.lodRefDistance", PKind::Float, GraphicsControl::VsmRefDistance, PTab::Vsm }, // vsmLodRefDistance
        { "shadows.vsm.requestDownscale", "vsm.requestDownscale", PKind::Uint, GraphicsControl::VsmRequestDownscale, PTab::Vsm }, // vsmRequestDownscale
        { "shadows.vsm.lruFrames", "vsm.lruFrames", PKind::Uint, GraphicsControl::VsmLru, PTab::Vsm }, // vsmLruFrames
        { "shadows.vsm.clipmapFirstLevel", "vsm.clipmapFirstLevel", PKind::Int, GraphicsControl::VsmClipmapFirstLevel, PTab::Vsm }, // vsmClipmapFirstLevel
        { "shadows.vsm.resolutionLodBiasDirectional", "vsm.resolutionLodBiasDirectional", PKind::Float, GraphicsControl::VsmResolutionLodBiasDirectional, PTab::Vsm }, // vsmResolutionLodBiasDirectional
        { "shadows.vsm.resolutionLodBiasLocal", "vsm.resolutionLodBiasLocal", PKind::Float, kNoControl, PTab::Vsm }, // vsmResolutionLodBiasLocal
        { "shadows.vsm.smrtRayCount", "vsm.smrtRayCount", PKind::Uint, GraphicsControl::VsmSmrtRays, PTab::Vsm }, // smrtRayCount
        { "shadows.vsm.smrtSamplesPerRay", "vsm.smrtSamplesPerRay", PKind::Uint, GraphicsControl::VsmSmrtSamples, PTab::Vsm }, // smrtSamplesPerRay
        { "shadows.vsm.localSmrtRayCount", "vsm.localSmrtRayCount", PKind::Uint, GraphicsControl::VsmLocalSmrtRays, PTab::Vsm }, // localSmrtRayCount
        { "shadows.vsm.localSmrtSamplesPerRay", "vsm.localSmrtSamplesPerRay", PKind::Uint, GraphicsControl::VsmLocalSmrtSamples, PTab::Vsm }, // localSmrtSamplesPerRay
        { "shadows.vsm.smrtTemporalDither", "vsm.smrtTemporalDither", PKind::Bool, GraphicsControl::VsmSmrtTemporal, PTab::Vsm }, // smrtTemporalDither
        { "shadows.vsm.smrtAdaptiveAfterRays", "vsm.smrtAdaptiveRayCount", PKind::Uint, GraphicsControl::VsmSmrtAdaptive, PTab::Vsm }, // smrtAdaptiveRayCount
        { "shadows.vsm.smrtSunAngleDeg", "vsm.smrtSourceAngleDeg", PKind::Float, GraphicsControl::VsmSmrtSunAngle, PTab::Vsm }, // smrtSunAngleDeg
        { "shadows.vsm.smrtTexelDither", "vsm.smrtTexelDitherScale", PKind::Float, GraphicsControl::VsmSmrtTexelDither, PTab::Vsm }, // smrtTexelDitherScale
        { "shadows.vsm.smrtRayLengthScale", "vsm.smrtRayLengthScale", PKind::Float, GraphicsControl::VsmSmrtRayLength, PTab::Vsm }, // smrtRayLengthScale
        { "shadows.vsm.clipmapDepthBias", "vsm.clipmapDepthBias", PKind::Float, GraphicsControl::VsmDepthBias, PTab::Vsm }, // vsmClipmapDepthBias
        { "shadows.vsm.depthBiasDecay", "vsm.clipmapDepthBiasDecay", PKind::Float, GraphicsControl::VsmDepthBiasDecay, PTab::Vsm }, // vsmClipmapDepthBiasDecay
        { "shadows.vsm.depthBiasFloorTexels", "vsm.clipmapDepthBiasFloor", PKind::Float, GraphicsControl::VsmDepthBiasFloor, PTab::Vsm }, // vsmClipmapDepthBiasFloorTexels
        { "shadows.vsm.clipmapNormalBias", "vsm.clipmapNormalBias", PKind::Float, GraphicsControl::VsmNormalBias, PTab::Vsm }, // vsmClipmapNormalBias
        { "shadows.vsm.localLateralBiasTexels", "vsm.localLateralTexels", PKind::Float, GraphicsControl::VsmLocalLateralBias, PTab::Vsm }, // vsmLocalLateralTexels
        { "shadows.vsm.localDepthPushTexels", "vsm.localDepthPushTexels", PKind::Float, GraphicsControl::VsmLocalDepthPush, PTab::Vsm }, // vsmLocalDepthPushTexels
        { "shadows.vsm.residentOnly", "vsm.residentOnly", PKind::Bool, GraphicsControl::VsmResidentOnly, PTab::Vsm }, // vsmResidentOnly
        { "shadows.vsm.singleDraw", "vsm.singleDraw", PKind::Bool, GraphicsControl::VsmSingleDraw, PTab::Vsm }, // vsmSingleDraw
        { "shadows.vsm.hzbCull", "vsm.hzbCull", PKind::Bool, GraphicsControl::VsmHzbCull, PTab::Vsm }, // vsmHzbCull
        { "shadows.vsm.pageCaching", "vsm.pageCaching", PKind::Bool, GraphicsControl::VsmPageCaching, PTab::Vsm }, // vsmPageCaching
        { "shadows.vsm.tessyEnabled", "tessy.enabled", PKind::Bool, GraphicsControl::TessyEnabled, PTab::Vsm }, // tessyEnabled
        { "shadows.vsm.tessy", "tessy.vsm", PKind::Bool, GraphicsControl::TessyVsm, PTab::Vsm }, // tessyVsm
        { "shadows.vsm.tessyRaster", "tessy.raster", PKind::Bool, GraphicsControl::TessyRaster, PTab::Vsm }, // tessyRaster
        { "shadows.vsm.tessyAtlas", "tessy.atlas", PKind::Bool, GraphicsControl::TessyAtlas, PTab::Vsm }, // tessyAtlas
        { "shadows.vsm.tessyGBuffer", "tessy.gbuffer", PKind::Bool, GraphicsControl::TessyGBuffer, PTab::Vsm }, // tessyGBuffer
        { "shadows.vsm.tessySdsm", "tessy.sdsm", PKind::Bool, GraphicsControl::TessySdsm, PTab::Vsm }, // tessySdsm
        { "shadows.vsm.tessyLod", "tessy.lod", PKind::Bool, GraphicsControl::TessyLod, PTab::Vsm }, // tessyLod
        { "shadows.vsm.tessyLodPixelError", "tessy.lodPixelError", PKind::Float, GraphicsControl::TessyLodPixelError, PTab::Vsm }, // tessyLodPixelError
        { "shadows.vsm.windRange", "vsm.windRange", PKind::Float, GraphicsControl::VsmWindRange, PTab::Vsm }, // vsmWindRange
        { "shadows.legacyCsm.atlasResolution", "csm.atlasRes", PKind::Uint, GraphicsControl::CsmAtlasRes, PTab::Csm }, // csmAtlasRes
    };
    constexpr std::size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

    struct GraphicsSettingsSnapshot;
    GraphicsSettingsSnapshot& CompiledDefaults();

    struct GraphicsSettingsSnapshot
    {
        std::array<float, kEntryCount> value{};
        std::array<bool, kEntryCount> has{}; // the file (or the live state) gave it; an entry without one is not applied
        bool dlssEnabled = false;
        sl::DLSSMode dlssMode = sl::DLSSMode::eBalanced;
        float renderScale = 1.0f;
        render::ShadowMode shadowMode = render::ShadowMode::VSM;
        CascadeShadowConfig csm{};
    };

    // The live values before the graphics file is applied (GraphicsSettingsManager::Initialize): what a knob the defaults
    // file does not hold goes back to, as the old snapshot struct's initializers did. Empty before then.
    GraphicsSettingsSnapshot& CompiledDefaults()
    {
        static GraphicsSettingsSnapshot compiled{};
        return compiled;
    }

    // The kept-in-code parts' clamps, as the load had them (the rest is the rows').
    void SanitizeKept(GraphicsSettingsSnapshot& s)
    {
        const auto finite = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
        (void)finite;
        const int dlss = static_cast<int>(s.dlssMode);
        if (dlss < static_cast<int>(sl::DLSSMode::eOff) || dlss > static_cast<int>(sl::DLSSMode::eDLAA))
        {
            s.dlssMode = sl::DLSSMode::eBalanced;
        }
        s.renderScale = std::clamp(finite(s.renderScale, 1.0f), 0.1f, 1.0f);
        s.csm.maxDistance = std::clamp(finite(s.csm.maxDistance, 300.0f), 20.0f, 1000.0f);
        for (float& split : s.csm.sliceDistances)
            split = std::clamp(finite(split, s.csm.maxDistance), 0.5f, 1000.0f);
        s.csm.cascadeDistributionExponent = std::clamp(
            finite(s.csm.cascadeDistributionExponent, 3.0f), 0.1f, 10.0f);
        s.csm.overlapInTexels = std::clamp(finite(s.csm.overlapInTexels, 2.0f), 0.0f, 8.0f);
        s.csm.zPadding = std::clamp(finite(s.csm.zPadding, 25.0f), 0.0f, 100.0f);
        s.csm.casterReachWS = std::clamp(finite(s.csm.casterReachWS, 150.0f), 0.0f, 400.0f);
        s.csm.scissorPadTexels = std::clamp(finite(s.csm.scissorPadTexels, 4.0f), 0.0f, 16.0f);
        s.csm.pancakeSlackWS = std::clamp(finite(s.csm.pancakeSlackWS, 40.0f), 0.0f, 160.0f);
        s.csm.depthBiasInTexels = std::clamp(finite(s.csm.depthBiasInTexels, 1.0f), 0.0f, 12.0f);
        s.csm.slopeScale = std::clamp(finite(s.csm.slopeScale, 2.0f), 0.0f, 8.0f);
        s.csm.maxSlope = std::clamp(finite(s.csm.maxSlope, 1.0f), 0.0f, 4.0f);
        s.csm.normalBiasInTexels = std::clamp(finite(s.csm.normalBiasInTexels, 0.5f), 0.0f, 4.0f);
        s.csm.blendFraction = std::clamp(finite(s.csm.blendFraction, 0.1f), 0.0f, 0.3f);
        s.csm.distanceFadeFraction = std::clamp(
            finite(s.csm.distanceFadeFraction, 0.1f), 0.0f, 0.3f);
        s.csm.filterMode = std::min(s.csm.filterMode, 2u);
        s.csm.shadowFilterSharpen = std::clamp(
            finite(s.csm.shadowFilterSharpen, 0.0f), 0.0f, 1.0f);
        s.csm.csmReceiverBias = std::clamp(finite(s.csm.csmReceiverBias, 0.9f), 0.0f, 1.0f);
    }

    settings::SettingContext Ctx(const Scene& scene, const SceneRenderSettings& settings)
    {
        return settings::SettingContext{ const_cast<Scene&>(scene), const_cast<SceneRenderSettings&>(settings) };
    }

    // The knobs the graphics levels manage are saved with the values they had before (docs/scalability_plan.md §3.3):
    // the graphics section is the person's own, and an autosave while Low is chosen must not make Low their own; a value
    // changed by hand over the level is an override, saved with the levels (Scalability::NoteManualValues).
    void CaptureScalabilityOriginals(GraphicsSettingsSnapshot& s)
    {
        const settings::Scalability& sc = settings::TheScalability();
        float v = 0.0f;
        for (std::size_t i = 0; i < kEntryCount; ++i)
        {
            if (std::strcmp(kEntries[i].setting, "ssr.ueQuality") == 0) { continue; } // below: the preset's fields with it
            if (sc.OriginalValue(kEntries[i].setting, v)) { s.value[i] = v; s.has[i] = true; }
        }
        if (!render::g_shadowModeFromCli_game && sc.OriginalValue("shadow.mode", v))
        {
            s.shadowMode = static_cast<render::ShadowMode>(static_cast<int>(std::lround(v)));
        }
        if (sc.OriginalValue("csm.filterMode", v)) { s.csm.filterMode = static_cast<decltype(s.csm.filterMode)>(std::lround(v)); }
        if (sc.OriginalValue("dlss.mode", v)) { s.dlssMode = static_cast<sl::DLSSMode>(static_cast<int>(std::lround(v))); }
        if (sc.OriginalValue("aa.renderScale", v)) { s.renderScale = v; }
        if (sc.OriginalValue("ssr.ueQuality", v))
        {
            UeSsrSettings ue{};
            ApplyUeSsrQualityPreset(ue, static_cast<UeSsrQualityPreset>(static_cast<std::uint32_t>(std::lround(v))));
            const std::pair<const char*, float> fields[] = {
                { "ssr.ueQuality", static_cast<float>(static_cast<std::uint32_t>(ue.preset)) },
                { "ssr.ueSteps", static_cast<float>(ue.numSteps) }, { "ssr.ueRays", static_cast<float>(ue.numRays) },
                { "ssr.ueGlossy", static_cast<float>(ue.glossyRays) },
                { "ssr.ueUseSurfaceRoughness", ue.useSurfaceRoughness ? 1.0f : 0.0f },
                { "ssr.ueRoughnessOverride", ue.roughnessOverride }, { "ssr.ueIntensity", ue.intensity },
                { "ssr.ueMaxRoughness", ue.maxRoughness } };
            for (std::size_t i = 0; i < kEntryCount; ++i)
            {
                for (const auto& f : fields)
                {
                    if (std::strcmp(kEntries[i].setting, f.first) == 0) { s.value[i] = f.second; s.has[i] = true; }
                }
            }
        }
    }

    // `managedAsOwn`: a knob the graphics levels manage is captured with its own value (what is saved), not what the
    // level made it -- a single control's Reset takes the live values instead, so it moves that control alone.
    GraphicsSettingsSnapshot Capture(const Renderer& renderer, const Scene& scene,
                                     const SceneRenderSettings& settings, bool managedAsOwn = true)
    {
        GraphicsSettingsSnapshot s{};
        settings::SettingContext ctx = Ctx(scene, settings);
        const settings::SettingsRegistry& registry = settings::Registry();
        for (std::size_t i = 0; i < kEntryCount; ++i)
        {
            float v = 0.0f;
            s.has[i] = registry.Read(ctx, kEntries[i].setting, v) && std::isfinite(v);
            s.value[i] = s.has[i] ? v : 0.0f;
        }
        s.dlssEnabled = renderer.IsDlssRequestedActive();
        s.dlssMode = renderer.GetDlssMode();
        s.renderScale = renderer.GetRenderResolutionScale();
        s.shadowMode = render::g_shadowModeFromCli_game ? render::g_shadowModePersisted_game : render::g_shadowMode_game;
        s.csm = scene.CascadeConfig();
        if (managedAsOwn) { CaptureScalabilityOriginals(s); }
        SanitizeKept(s);
        return s;
    }

    // One tab, or one control (`control` != kNoControl), through the rows -- then the tab's kept-in-code part.
    void ApplyEntries(const GraphicsSettingsSnapshot& s, PTab tab, GraphicsControl control, Renderer& renderer, Scene& scene,
                      SceneRenderSettings& settings)
    {
        settings::SettingContext ctx{ scene, settings };
        const settings::SettingsRegistry& registry = settings::Registry();
        if (tab == PTab::Vsm && control == kNoControl)
        {
            // A `--shadow-mode=` boot override keeps the mode it set; the file's value is remembered
            // so a save in that session writes the file's own mode back, not the override.
            render::g_shadowModePersisted_game = s.shadowMode;
            if (!render::g_shadowModeFromCli_game) { render::g_shadowMode_game = s.shadowMode; }
        }
        // The UE SSR preset after its fields: each field's row marks the preset Custom, the preset's row sets its fields
        // (a Custom one is the fields as they are -- not applied).
        const auto applyOne = [&](std::size_t i) {
            if (!registry.Apply(ctx, kEntries[i].setting, s.value[i]))
            {
                LOG_WARNING_ONCE(logging::LogCategory::App, "graphics settings: {} was not applied ({})", kEntries[i].setting,
                                 kEntries[i].path);
            }
        };
        std::size_t preset = kEntryCount;
        for (std::size_t i = 0; i < kEntryCount; ++i)
        {
            const bool wanted = control == kNoControl ? kEntries[i].tab == tab : kEntries[i].control == control;
            if (!wanted || !s.has[i]) { continue; }
            if (std::strcmp(kEntries[i].setting, "ssr.ueQuality") == 0) { preset = i; continue; }
            applyOne(i);
        }
        if (preset < kEntryCount &&
            std::lround(s.value[preset]) != static_cast<long>(static_cast<std::uint32_t>(UeSsrQualityPreset::Custom)))
        {
            applyOne(preset);
        }
        if (tab == PTab::Upscale && control == kNoControl)
        {
            renderer.SetDlssMode(s.dlssMode);
            renderer.SetDlssActive(s.dlssEnabled && s.dlssMode != sl::DLSSMode::eOff);
            if (!s.dlssEnabled || s.dlssMode == sl::DLSSMode::eOff)
            {
                renderer.SetRenderResolutionScale(s.renderScale);
            }
        }
        if (tab == PTab::Csm && control == kNoControl) { scene.CascadeConfig() = s.csm; }
    }

    void ApplyTab(const GraphicsSettingsSnapshot& s, PTab tab, Renderer& renderer, Scene& scene, SceneRenderSettings& settings)
    {
        ApplyEntries(s, tab, kNoControl, renderer, scene, settings);
    }

    void ApplyAll(const GraphicsSettingsSnapshot& s, Renderer& renderer, Scene& scene,
                  SceneRenderSettings& settings)
    {
        for (PTab tab : { PTab::Frame, PTab::Visibility, PTab::Upscale, PTab::Reflections, PTab::Fog, PTab::Sky, PTab::Streaming,
                          PTab::Gi, PTab::Lod, PTab::Contact, PTab::Sdsm, PTab::Vsm, PTab::Csm })
        {
            ApplyTab(s, tab, renderer, scene, settings);
        }
    }

    // A path "a.b.c" or "a.b[i]" in the graphics JSON.
    const json* FindPath(const json& root, const char* path)
    {
        const json* at = &root;
        std::string p = path;
        std::size_t pos = 0;
        while (pos <= p.size())
        {
            const std::size_t dot = p.find('.', pos);
            std::string key = p.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
            int index = -1;
            if (const std::size_t br = key.find('['); br != std::string::npos)
            {
                index = std::atoi(key.c_str() + br + 1);
                key.resize(br);
            }
            if (!at->is_object()) { return nullptr; }
            const auto it = at->find(key);
            if (it == at->end()) { return nullptr; }
            at = &*it;
            if (index >= 0)
            {
                if (!at->is_array() || static_cast<std::size_t>(index) >= at->size()) { return nullptr; }
                at = &(*at)[static_cast<std::size_t>(index)];
            }
            if (dot == std::string::npos) { break; }
            pos = dot + 1;
        }
        return at;
    }

    void PutPath(json& root, const char* path, json value)
    {
        json* at = &root;
        std::string p = path;
        std::size_t pos = 0;
        while (true)
        {
            const std::size_t dot = p.find('.', pos);
            std::string key = p.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
            int index = -1;
            if (const std::size_t br = key.find('['); br != std::string::npos)
            {
                index = std::atoi(key.c_str() + br + 1);
                key.resize(br);
            }
            json& next = (*at)[key];
            if (index >= 0)
            {
                if (!next.is_array()) { next = json::array(); }
                while (next.size() <= static_cast<std::size_t>(index)) { next.push_back(nullptr); }
                if (dot == std::string::npos) { next[static_cast<std::size_t>(index)] = std::move(value); return; }
                at = &next[static_cast<std::size_t>(index)];
            }
            else
            {
                if (dot == std::string::npos) { next = std::move(value); return; }
                at = &next;
            }
            pos = dot + 1;
        }
    }

    json ToJson(const GraphicsSettingsSnapshot& s)
    {
        json root = json::object();
        root["version"] = 1;
        root["scope"] = "Global runtime graphics quality. Scene look stays in level environment data.";
        for (std::size_t i = 0; i < kEntryCount; ++i)
        {
            if (!s.has[i]) { continue; }
            const float v = s.value[i];
            json value;
            switch (kEntries[i].kind)
            {
            case PKind::Bool: value = v != 0.0f; break;
            case PKind::Int: value = static_cast<int>(std::lround(v)); break;
            case PKind::Uint: value = static_cast<std::uint32_t>(std::max(0L, std::lround(v))); break;
            case PKind::Float: value = v; break;
            case PKind::ReflectionSource: value = ReflectionSourceName(static_cast<ReflectionSource>(std::lround(v))); break;
            case PKind::SsrTechnique: value = SsrTechniqueName(static_cast<SsrTechnique>(std::lround(v))); break;
            }
            PutPath(root, kEntries[i].path, std::move(value));
        }
        PutPath(root, "antiAliasing.dlssEnabled", s.dlssEnabled);
        PutPath(root, "antiAliasing.dlssMode", DlssModeName(s.dlssMode));
        PutPath(root, "antiAliasing.renderScale", s.renderScale);
        PutPath(root, "shadows.mode", ShadowModeName(s.shadowMode));
        json& csm = root["shadows"]["legacyCsm"];
        csm["maxDistance"] = s.csm.maxDistance;
        csm["autoSplits"] = s.csm.useUeSplitDistribution;
        csm["distributionExponent"] = s.csm.cascadeDistributionExponent;
        csm["splitDistances"] = s.csm.sliceDistances;
        csm["overlapTexels"] = s.csm.overlapInTexels;
        csm["zPadding"] = s.csm.zPadding;
        csm["casterReach"] = s.csm.casterReachWS;
        csm["scissorOptim"] = s.csm.scissorOptim;
        csm["scissorPadTexels"] = s.csm.scissorPadTexels;
        csm["accurateCasterCull"] = s.csm.accurateCasterCull;
        csm["hzbCull"] = s.csm.hzbCull;
        csm["pancakeCasters"] = s.csm.pancakeCasters;
        csm["pancakeSlack"] = s.csm.pancakeSlackWS;
        csm["depthBiasTexels"] = s.csm.depthBiasInTexels;
        csm["slopeScale"] = s.csm.slopeScale;
        csm["maxSlope"] = s.csm.maxSlope;
        csm["normalBiasTexels"] = s.csm.normalBiasInTexels;
        csm["blendFraction"] = s.csm.blendFraction;
        csm["distanceFadeFraction"] = s.csm.distanceFadeFraction;
        csm["filterMode"] = s.csm.filterMode;
        csm["filterSharpen"] = s.csm.shadowFilterSharpen;
        csm["receiverBias"] = s.csm.csmReceiverBias;
        csm["pcfOverBlurCorrection"] = s.csm.pcfOverBlurCorrection;
        return root;
    }

    GraphicsSettingsSnapshot FromJson(const json& root)
    {
        GraphicsSettingsSnapshot s = CompiledDefaults();
        for (std::size_t i = 0; i < kEntryCount; ++i)
        {
            const json* j = FindPath(root, kEntries[i].path);
            if (j == nullptr) { continue; }
            float v = 0.0f;
            bool ok = false;
            switch (kEntries[i].kind)
            {
            case PKind::Bool: ok = j->is_boolean(); if (ok) { v = j->get<bool>() ? 1.0f : 0.0f; } break;
            case PKind::Int:
            case PKind::Uint: ok = j->is_number_integer() || j->is_number_unsigned(); if (ok) { v = static_cast<float>(j->get<double>()); } break;
            case PKind::Float: ok = j->is_number(); if (ok) { v = j->get<float>(); } break;
            case PKind::ReflectionSource:
                ok = j->is_string();
                if (ok)
                {
                    const ReflectionSource r = ParseReflectionSource(*j, ReflectionSource{});
                    ok = j->get<std::string>() == ReflectionSourceName(r); // a name, not a word the parser fell back on
                    v = static_cast<float>(static_cast<int>(r));
                }
                break;
            case PKind::SsrTechnique:
                ok = j->is_string();
                if (ok)
                {
                    const SsrTechnique r = ParseSsrTechnique(*j, SsrTechnique{});
                    ok = j->get<std::string>() == SsrTechniqueName(r);
                    v = static_cast<float>(static_cast<int>(r));
                }
                break;
            }
            if (ok && std::isfinite(v)) { s.value[i] = v; s.has[i] = true; } // else the defaults' (a wrong type: §6)
        }
        const json& aa = Section(root, "antiAliasing");
        const json& shadows = Section(root, "shadows");
        const json& csm = Section(shadows, "legacyCsm");
        Read(aa, "dlssEnabled", s.dlssEnabled);
        if (const auto it = aa.find("dlssMode"); it != aa.end())
        {
            s.dlssMode = ParseDlssMode(*it, s.dlssMode);
        }
        Read(aa, "renderScale", s.renderScale);
        if (const auto it = shadows.find("mode"); it != shadows.end())
        {
            s.shadowMode = ParseShadowMode(*it, s.shadowMode);
        }
        Read(csm, "maxDistance", s.csm.maxDistance);
        Read(csm, "autoSplits", s.csm.useUeSplitDistribution);
        Read(csm, "distributionExponent", s.csm.cascadeDistributionExponent);
        const auto splits = csm.find("splitDistances");
        if (splits != csm.end() && splits->is_array() && splits->size() >= s.csm.sliceDistances.size())
        {
            for (size_t i = 0; i < s.csm.sliceDistances.size(); ++i)
            {
                if ((*splits)[i].is_number())
                {
                    s.csm.sliceDistances[i] = (*splits)[i].get<float>();
                }
            }
        }
        Read(csm, "overlapTexels", s.csm.overlapInTexels);
        Read(csm, "zPadding", s.csm.zPadding);
        Read(csm, "casterReach", s.csm.casterReachWS);
        Read(csm, "scissorOptim", s.csm.scissorOptim);
        Read(csm, "scissorPadTexels", s.csm.scissorPadTexels);
        Read(csm, "accurateCasterCull", s.csm.accurateCasterCull);
        Read(csm, "hzbCull", s.csm.hzbCull);
        Read(csm, "pancakeCasters", s.csm.pancakeCasters);
        Read(csm, "pancakeSlack", s.csm.pancakeSlackWS);
        Read(csm, "depthBiasTexels", s.csm.depthBiasInTexels);
        Read(csm, "slopeScale", s.csm.slopeScale);
        Read(csm, "maxSlope", s.csm.maxSlope);
        Read(csm, "normalBiasTexels", s.csm.normalBiasInTexels);
        Read(csm, "blendFraction", s.csm.blendFraction);
        Read(csm, "distanceFadeFraction", s.csm.distanceFadeFraction);
        Read(csm, "filterMode", s.csm.filterMode);
        Read(csm, "filterSharpen", s.csm.shadowFilterSharpen);
        Read(csm, "receiverBias", s.csm.csmReceiverBias);
        Read(csm, "pcfOverBlurCorrection", s.csm.pcfOverBlurCorrection);
        SanitizeKept(s);
        return s;
    }


    // The settings file this used to be (docs/scalability_plan.md §6.4): read ONCE, when user_settings.json has no graphics
    // section yet, to move it there, and never written again. A dev build's is the repo root's full snapshot, a packaged
    // build's <user>\settings\graphics_settings.json its differences from the defaults.
    std::filesystem::path LegacySettingsFile()
    {
        return vfs::WritablePath(vfs::Writable::Settings, GraphicsSettingsManager::kLegacyPath);
    }

    json ParseDefaults(const char* path)
    {
        std::string text;
        if (!vfs::ReadText(path, text)) { return json(); }
        const json parsed = json::parse(text, nullptr, false, /*ignore_comments=*/true);
        if (!parsed.is_object()) { return json(); }
        try
        {
            return ToJson(FromJson(parsed));
        }
        catch (const std::exception&)
        {
            return json();
        }
    }

    // The defaults the person's values are taken against (docs/distribution_build_plan.md §6, docs/scalability_plan.md
    // §3.3 layer 2) -- in EVERY build since 2026-11-16: the curated config/graphics_defaults.json the packages carry (also
    // what a dev build reads with the packages mounted), else a dev build's tools/dist/graphics_defaults.json, the same
    // file's source; the compiled values only when neither is there. A patch that fixes a default reaches everyone who
    // did not change that knob, and a developer's file no longer freezes all ~190 knobs.
    json DefaultsJson()
    {
        static const char* used = nullptr;
        const char* const candidates[] = { "config/graphics_defaults.json",
#if !TC_PACKAGED
                                           "tools/dist/graphics_defaults.json",
#endif
        };
        for (const char* path : candidates)
        {
            json j = ParseDefaults(path);
            if (j.is_object())
            {
                if (used != path)
                {
                    used = path;
                    LOG_INFO(logging::LogCategory::App, "graphics settings: the defaults are {}", path);
                }
                return j;
            }
        }
        LOG_WARNING_ONCE(logging::LogCategory::App, "graphics settings: no usable graphics defaults file -- the compiled defaults");
        return ToJson(GraphicsSettingsSnapshot{});
    }

    // What Reset goes back to: the defaults this build reads (above), never the compiled ones a build does not ship with.
    GraphicsSettingsSnapshot DefaultSnapshot() { return FromJson(DefaultsJson()); }

    // `full` less what equals `base`, through objects key by key (an array or a value is one): what a shipped build keeps
    // of the player's settings.
    json Difference(const json& base, const json& full)
    {
        json out = json::object();
        for (auto it = full.begin(); it != full.end(); ++it)
        {
            const auto b = base.find(it.key());
            if (b == base.end())
            {
                out[it.key()] = it.value();
            }
            else if (it.value().is_object() && b->is_object())
            {
                json d = Difference(*b, it.value());
                if (!d.empty()) { out[it.key()] = std::move(d); }
            }
            else if (it.value() != *b)
            {
                out[it.key()] = it.value();
            }
        }
        return out;
    }

    // The legacy file as a whole snapshot: a dev build's is a full one, a packaged build's only the differences.
    bool LoadLegacySnapshot(GraphicsSettingsSnapshot& out, std::string& error)
    {
        std::ifstream file(LegacySettingsFile(), std::ios::binary); // vfs-ok: the user's old settings file, read once to move it
        if (!file)
        {
            error = "file not found";
            return false;
        }
        json root = json::parse(file, nullptr, false, /*ignore_comments=*/true);
        if (!root.is_object())
        {
            error = "invalid JSON root";
            return false;
        }
#if TC_PACKAGED
        json merged = DefaultsJson();
        merged.merge_patch(root);
        root = std::move(merged);
#endif
        try
        {
            out = FromJson(root);
        }
        catch (const std::exception& e)
        {
            error = e.what();
            return false;
        }
        return true;
    }

    // The person's graphics: the defaults with user_settings.json's graphics section over them. The first time there is no
    // section, graphics_settings.json is moved into it (what it says less what the defaults say) -- the picture is the
    // same -- and is never written again. `found`: a section or a legacy file was there.
    bool LoadSnapshot(GraphicsSettingsSnapshot& out, bool& found, std::string& error)
    {
        found = false;
        const std::string text = settings::ReadUserSection("graphics");
        if (!text.empty())
        {
            found = true;
            const json diff = json::parse(text, nullptr, false);
            if (!diff.is_object())
            {
                error = "the graphics section of " + std::string(GraphicsSettingsManager::kPath) + " is not an object";
                return false;
            }
            json merged = DefaultsJson();
            merged.merge_patch(diff);
            try
            {
                out = FromJson(merged);
            }
            catch (const std::exception& e)
            {
                error = e.what();
                return false;
            }
            return true;
        }
        std::error_code ec;
        if (!std::filesystem::exists(LegacySettingsFile(), ec)) { return false; } // vfs-ok: the user's old settings file
        found = true;
        if (!LoadLegacySnapshot(out, error)) { return false; }
        const json diff = Difference(DefaultsJson(), ToJson(out));
        // The section must give back exactly what the old file said: the defaults with it over them, read as a snapshot.
        json back = DefaultsJson();
        back.merge_patch(diff);
        if (ToJson(FromJson(back)) != ToJson(out))
        {
            LOG_WARNING(logging::LogCategory::App, "graphics settings: moving {} would change a value -- left where it is",
                        vfs::Narrow(LegacySettingsFile().wstring()));
            return true;
        }
        std::string why;
        if (settings::WriteUserSection("graphics", diff.dump(), why))
        {
            LOG_INFO(logging::LogCategory::App, "graphics settings: {} moved into {} ({} section(s) differ from the defaults); "
                     "the old file is no longer read or written", vfs::Narrow(LegacySettingsFile().wstring()),
                     vfs::Narrow(settings::UserSettingsPath().wstring()), diff.size());
        }
        else
        {
            LOG_WARNING(logging::LogCategory::App, "graphics settings: {} could not be moved into {}: {} -- read from it this time",
                        vfs::Narrow(LegacySettingsFile().wstring()), vfs::Narrow(settings::UserSettingsPath().wstring()), why);
        }
        return true;
    }

    // What differs from the defaults, into user_settings.json's graphics section (its other sections kept; written whole
    // to a temp file and renamed over: a crash or a full disk mid-write leaves the old settings, not half a JSON).
    bool SaveSnapshot(const GraphicsSettingsSnapshot& snapshot, std::string& error)
    {
        const json content = Difference(DefaultsJson(), ToJson(snapshot));
        return settings::WriteUserSection("graphics", content.dump(), error);
    }
}

void GraphicsSettingsManager::Initialize(Renderer& renderer, Scene& scene,
                                         SceneRenderSettings& settings)
{
    initialized_ = true;
    {
        // The table's knobs as they are before the file; the kept-in-code parts as the old struct had them.
        GraphicsSettingsSnapshot compiled = Capture(renderer, scene, settings, /*managedAsOwn=*/false);
        const GraphicsSettingsSnapshot initializers{};
        compiled.dlssEnabled = initializers.dlssEnabled;
        compiled.dlssMode = initializers.dlssMode;
        compiled.renderScale = initializers.renderScale;
        compiled.shadowMode = initializers.shadowMode;
        compiled.csm = initializers.csm;
        CompiledDefaults() = compiled;
    }
    GraphicsSettingsSnapshot loaded{};
    std::string error;
    bool found = false;
    if (LoadSnapshot(loaded, found, error))
    {
        ApplyAll(loaded, renderer, scene, settings);
        status_ = std::string("Loaded ") + kPath;
    }
    else if (!found)
    {
        // Nothing saved yet: the defaults, and nothing written until something is changed.
        loaded = FromJson(DefaultsJson());
        ApplyAll(loaded, renderer, scene, settings);
        status_ = "Defaults (nothing changed yet)";
    }
    else
    {
        // A damaged section or file: a Warning and the defaults (§6); the next save writes a good one.
        LOG_WARNING(logging::LogCategory::App, "graphics settings: {} could not be read ({}) -- the defaults",
                    vfs::Narrow(settings::UserSettingsPath().wstring()), error);
        loaded = FromJson(DefaultsJson());
        ApplyAll(loaded, renderer, scene, settings);
        status_ = std::string("Could not load ") + kPath + ": " + error;
    }
    projectCsm_ = scene.CascadeConfig();
    savePending_ = false;
}

namespace
{
    void FlattenJson(const json& j, const std::string& path, std::vector<std::string>& lines)
    {
        if (j.is_object())
        {
            for (auto it = j.begin(); it != j.end(); ++it) { FlattenJson(it.value(), path.empty() ? it.key() : path + "." + it.key(), lines); }
            return;
        }
        if (j.is_array())
        {
            for (std::size_t i = 0; i < j.size(); ++i) { FlattenJson(j[i], path + "[" + std::to_string(i) + "]", lines); }
            return;
        }
        lines.push_back(path + " " + j.dump());
    }
}

void GraphicsSettingsManager::DescribeFile(Renderer& renderer, const Scene& scene, const SceneRenderSettings& settings,
                                           const char* prefix, std::vector<std::string>& lines) const
{
    std::vector<std::string> flat;
    FlattenJson(ToJson(Capture(renderer, scene, settings)), "", flat);
    for (const std::string& l : flat) { lines.push_back(prefix + l); }
}

void GraphicsSettingsManager::ApplySceneSettings(Scene& scene) const
{
    scene.CascadeConfig() = projectCsm_;
}

void GraphicsSettingsManager::ObserveUiEdit(Renderer& renderer, Scene& scene,
                                             const SceneRenderSettings& settings,
                                             bool graphicsSettingsDirty,
                                             float deltaTime)
{
    if (!initialized_) { return; }
    if (graphicsSettingsDirty)
    {
        savePending_ = true;
        projectCsm_ = scene.CascadeConfig();
        saveDelaySeconds_ = 0.4f;
        status_ = "Graphics settings changed - autosave pending";
    }

    if (!savePending_) { return; }
    saveDelaySeconds_ -= std::max(deltaTime, 0.0f);
    if (saveDelaySeconds_ <= 0.0f)
    {
        SaveCurrent(renderer, scene, settings);
    }
}

void GraphicsSettingsManager::Flush(Renderer& renderer, const Scene& scene,
                                    const SceneRenderSettings& settings)
{
    if (initialized_ && savePending_)
    {
        SaveCurrent(renderer, scene, settings);
    }
}

bool GraphicsSettingsManager::SaveCurrent(Renderer& renderer, const Scene& scene,
                                          const SceneRenderSettings& settings)
{
    std::string error;
    // A knob a managed graphics level sets, changed here by hand: an override over the level (Scalability.h), saved with
    // the levels -- the graphics section keeps the knob's own value (CaptureScalabilityOriginals).
    settings::Scalability& levels = settings::TheScalability();
    if (levels.NoteManualValues() && !levels.SaveUser(error))
    {
        status_ = std::string("Could not save the overrides: ") + error;
        return false;
    }
    if (!SaveSnapshot(Capture(renderer, scene, settings), error))
    {
        status_ = std::string("Could not save ") + kPath + ": " + error;
        return false;
    }
    projectCsm_ = scene.CascadeConfig();
    savePending_ = false;
    status_ = std::string("Saved ") + kPath;
    return true;
}

bool GraphicsSettingsManager::FinishReset(const std::map<std::string, float>& managedBefore, Renderer& renderer, const Scene& scene,
                                          const SceneRenderSettings& settings)
{
    // Reset goes back to the graphics levels (§3.3), not past them: a knob a managed level sets loses its override and
    // takes the level's value again; the rest has the build's defaults.
    settings::Scalability& levels = settings::TheScalability();
    levels.AfterReset(managedBefore);
    levels.Apply("reset");
    std::string why;
    if (levels.Managed() && !levels.SaveUser(why))
    {
        LOG_WARNING(logging::LogCategory::App, "graphics settings: the overrides were not saved after the reset: {}", why);
    }
    return SaveCurrent(renderer, scene, settings);
}

bool GraphicsSettingsManager::Reload(Renderer& renderer, Scene& scene,
                                     SceneRenderSettings& settings)
{
    GraphicsSettingsSnapshot loaded{};
    std::string error;
    bool found = false;
    if (!LoadSnapshot(loaded, found, error))
    {
        status_ = std::string("Could not reload ") + kPath + ": " + (found ? error : std::string("nothing saved"));
        return false;
    }
    ApplyAll(loaded, renderer, scene, settings);
    projectCsm_ = scene.CascadeConfig();
    savePending_ = false;
    status_ = std::string("Reloaded ") + kPath;
    return true;
}

bool GraphicsSettingsManager::ResetAll(Renderer& renderer, Scene& scene,
                                       SceneRenderSettings& settings)
{
    const std::map<std::string, float> managedBefore = settings::TheScalability().ManagedValues();
    const GraphicsSettingsSnapshot defaults = DefaultSnapshot();
    ApplyAll(defaults, renderer, scene, settings);
    return FinishReset(managedBefore, renderer, scene, settings);
}

std::vector<std::string> GraphicsPersistentFlagMismatches()
{
    // What the parts kept in code save (ToJson): the DLSS mode and render scale, the shadow mode, the CSM fit's fields.
    static constexpr const char* kKeptInCode[] = {
        "dlss.mode", "aa.renderScale", "shadow.mode", "csm.maxDistance", "csm.filterMode", "csm.sharpen", "csm.receiverBias",
        "csm.overBlurCorrect", "csm.slopeScale", "csm.maxSlope", "csm.depthBias", "csm.normalBias", "csm.blendFraction",
        "csm.distanceFade", "csm.scissorOptim", "csm.scissorPad", "csm.accurateCull", "csm.hzbCull",
    };
    const auto saved = [](const std::string& name) {
        for (const PEntry& e : kEntries)
        {
            if (name == e.setting) { return true; }
        }
        for (const char* kept : kKeptInCode)
        {
            if (name == kept) { return true; }
        }
        return false;
    };
    std::vector<std::string> out;
    const settings::SettingsRegistry& registry = settings::Registry();
    std::vector<std::string> rows;
    for (const settings::SettingDesc& d : registry.All())
    {
        rows.emplace_back(d.name);
        const bool persistent = (d.flags & settings::SettingPersistent) != 0u;
        if (persistent != saved(d.name))
        {
            out.push_back(std::string(d.name) + (persistent ? ": SettingPersistent, but the graphics file does not save it"
                                                            : ": the graphics file saves it, but the row is not SettingPersistent"));
        }
        if (persistent && (d.flags & settings::SettingTransient) != 0u)
        {
            out.push_back(std::string(d.name) + ": both SettingPersistent and SettingTransient");
        }
    }
    for (const settings::SettingFamily& f : registry.Families())
    {
        if ((f.flags & settings::SettingPersistent) != 0u)
        {
            out.push_back(std::string(f.prefix) + "*: a family is SettingPersistent, but the graphics file saves no family key");
        }
    }
    const auto isRow = [&](const std::string& name) { return std::find(rows.begin(), rows.end(), name) != rows.end(); };
    for (const PEntry& e : kEntries)
    {
        if (!isRow(e.setting)) { out.push_back(std::string(e.setting) + ": a graphics table entry names no row"); }
    }
    for (const char* kept : kKeptInCode)
    {
        if (!isRow(kept)) { out.push_back(std::string(kept) + ": kept in code, but no row has that name"); }
    }
    return out;
}

const char* ScalabilityKnobOf(GraphicsControl control)
{
    switch (control)
    {
    case GraphicsControl::DlssMode:                        return "dlss.mode";
    case GraphicsControl::Fxaa:                            return "aa.fxaa";
    case GraphicsControl::RenderScale:                     return "aa.renderScale";
    case GraphicsControl::UeSsrQuality:                    return "ssr.ueQuality";
    case GraphicsControl::WaterReflectionNormalCascades:   return "ssr.waterNormalCascades";
    case GraphicsControl::ReflectionResolution:            return "reflections.resolution";
    case GraphicsControl::OceanReflectionResolution:       return "reflections.oceanResolution";
    case GraphicsControl::ReflectionSource:                return "render.reflectionSource";
    case GraphicsControl::RtAlphaMode:                     return "rt.alphaMode";
    case GraphicsControl::FogGridPixels:                   return "fog.gridPixels";
    case GraphicsControl::FogGridZ:                        return "fog.gridZ";
    case GraphicsControl::CloudCaptureTexels:              return "cloud.captureTexels";
    case GraphicsControl::LodPixelError:                   return "lod.pixelError";
    case GraphicsControl::ChunkLodFactor:                  return "lod.chunkFactor";
    case GraphicsControl::ShadowMode:                      return "shadow.mode";
    case GraphicsControl::LocalShadowWindRange:            return "shadow.localWindRange";
    case GraphicsControl::CsmFilter:                       return "csm.filterMode";
    case GraphicsControl::CsmAtlasRes:                     return "csm.atlasRes";
    case GraphicsControl::SdsmPartitions:                  return "sdsm.partitions";
    case GraphicsControl::SdsmCacheFrames:                 return "sdsm.cacheFrames";
    case GraphicsControl::ContactEnabled:                  return "contact.enabled";
    case GraphicsControl::ContactSteps:                    return "contact.steps";
    case GraphicsControl::VsmResolutionLodBiasDirectional: return "vsm.resolutionLodBiasDirectional";
    case GraphicsControl::VsmSmrtRays:                     return "vsm.smrtRayCount";
    case GraphicsControl::VsmSmrtSamples:                  return "vsm.smrtSamplesPerRay";
    case GraphicsControl::VsmLocalSmrtRays:                return "vsm.localSmrtRayCount";
    case GraphicsControl::TessyLodPixelError:              return "tessy.lodPixelError";
    case GraphicsControl::VsmWindRange:                    return "vsm.windRange";
    case GraphicsControl::StreamingPoolSizeMB:             return "streaming.poolSizeMB";
    case GraphicsControl::StreamingMipBias:                return "streaming.mipBias";
    case GraphicsControl::StreamingAnisotropy:             return "textures.anisotropy";
    case GraphicsControl::GiEnabled:                       return "gi.enabled";
    case GraphicsControl::GiFar:                           return "gi.far";
    case GraphicsControl::GiDivisor:                       return "gi.divisor";
    case GraphicsControl::GiFarDivisor:                    return "gi.farDivisor";
    case GraphicsControl::GiRays:                          return "gi.rays";
    case GraphicsControl::GiRayDistance:                   return "gi.rayDistance";
    case GraphicsControl::SkyVisEnabled:                   return "skyVis.enabled";
    case GraphicsControl::SkyVisRays:                      return "skyVis.rays";
    case GraphicsControl::SkyVisFogRays:                   return "skyVis.fogRays";
    case GraphicsControl::SkyVisFilterRadius:              return "skyVis.filterRadius";
    default:                                               return nullptr;
    }
}

bool GraphicsSettingsManager::ResetSetting(const std::string& setting, Renderer& renderer, Scene& scene,
                                           SceneRenderSettings& settings)
{
    for (std::size_t i = 0; i < kEntryCount; ++i)
    {
        if (kEntries[i].control != kNoControl && setting == kEntries[i].setting)
        {
            return ResetControl(kEntries[i].control, renderer, scene, settings);
        }
    }
    for (int c = 0; c <= static_cast<int>(GraphicsControl::SkyVisFallbackFade); ++c)
    {
        const char* knob = ScalabilityKnobOf(static_cast<GraphicsControl>(c));
        if (knob != nullptr && setting == knob)
        {
            return ResetControl(static_cast<GraphicsControl>(c), renderer, scene, settings);
        }
    }
    LOG_ERROR(logging::LogCategory::App, "graphics settings: no control's Reset moves {}", setting);
    return false;
}

bool GraphicsSettingsManager::ResetControl(GraphicsControl control, Renderer& renderer,
                                           Scene& scene, SceneRenderSettings& settings)
{
    const std::map<std::string, float> managedBefore = settings::TheScalability().ManagedValues();
    // SMRT's on/off is its ray count (0 = off): the same knob as the rays' control.
    if (control == GraphicsControl::VsmSmrtEnabled) { control = GraphicsControl::VsmSmrtRays; }
    GraphicsSettingsSnapshot current = Capture(renderer, scene, settings, /*managedAsOwn=*/false);
    const GraphicsSettingsSnapshot defaults = DefaultSnapshot();
    // The kept-in-code controls (the DLSS trio, the shadow mode, the CSM fit): their field from the defaults.
    PTab tab = PTab::Vsm;
    bool found = false;
    switch (control)
    {
    case GraphicsControl::DlssEnabled:                    current.dlssEnabled = defaults.dlssEnabled; break;
    case GraphicsControl::DlssMode:                       current.dlssMode = defaults.dlssMode; break;
    case GraphicsControl::RenderScale:                    current.renderScale = defaults.renderScale; break;
    case GraphicsControl::ShadowMode:                     current.shadowMode = defaults.shadowMode; break;
    case GraphicsControl::CsmMaxDistance:                 current.csm.maxDistance = defaults.csm.maxDistance; break;
    case GraphicsControl::CsmAutoSplits:                  current.csm.useUeSplitDistribution = defaults.csm.useUeSplitDistribution; break;
    case GraphicsControl::CsmDistributionExponent:        current.csm.cascadeDistributionExponent = defaults.csm.cascadeDistributionExponent; break;
    case GraphicsControl::CsmSplitDistances:              current.csm.sliceDistances = defaults.csm.sliceDistances; break;
    case GraphicsControl::CsmOverlap:                     current.csm.overlapInTexels = defaults.csm.overlapInTexels; break;
    case GraphicsControl::CsmZPadding:                    current.csm.zPadding = defaults.csm.zPadding; break;
    case GraphicsControl::CsmCasterReach:                 current.csm.casterReachWS = defaults.csm.casterReachWS; break;
    case GraphicsControl::CsmScissorOptim:                current.csm.scissorOptim = defaults.csm.scissorOptim; break;
    case GraphicsControl::CsmScissorPad:                  current.csm.scissorPadTexels = defaults.csm.scissorPadTexels; break;
    case GraphicsControl::CsmAccurateCasterCull:          current.csm.accurateCasterCull = defaults.csm.accurateCasterCull; break;
    case GraphicsControl::CsmHzbCull:                     current.csm.hzbCull = defaults.csm.hzbCull; break;
    case GraphicsControl::CsmPancake:                     current.csm.pancakeCasters = defaults.csm.pancakeCasters; break;
    case GraphicsControl::CsmPancakeSlack:                current.csm.pancakeSlackWS = defaults.csm.pancakeSlackWS; break;
    case GraphicsControl::CsmDepthBias:                   current.csm.depthBiasInTexels = defaults.csm.depthBiasInTexels; break;
    case GraphicsControl::CsmSlopeScale:                  current.csm.slopeScale = defaults.csm.slopeScale; break;
    case GraphicsControl::CsmMaxSlope:                    current.csm.maxSlope = defaults.csm.maxSlope; break;
    case GraphicsControl::CsmNormalBias:                  current.csm.normalBiasInTexels = defaults.csm.normalBiasInTexels; break;
    case GraphicsControl::CsmBlendFraction:               current.csm.blendFraction = defaults.csm.blendFraction; break;
    case GraphicsControl::CsmDistanceFade:                current.csm.distanceFadeFraction = defaults.csm.distanceFadeFraction; break;
    case GraphicsControl::CsmFilter:                      current.csm.filterMode = defaults.csm.filterMode; break;
    case GraphicsControl::CsmSharpen:                     current.csm.shadowFilterSharpen = defaults.csm.shadowFilterSharpen; break;
    case GraphicsControl::CsmReceiverBias:                current.csm.csmReceiverBias = defaults.csm.csmReceiverBias; break;
    case GraphicsControl::CsmOverBlur:                    current.csm.pcfOverBlurCorrection = defaults.csm.pcfOverBlurCorrection; break;
    default: break;
    }
    if (control == GraphicsControl::DlssEnabled || control == GraphicsControl::DlssMode || control == GraphicsControl::RenderScale)
    {
        tab = PTab::Upscale;
        found = true;
    }
    else if (control == GraphicsControl::ShadowMode)
    {
        tab = PTab::Vsm;
        found = true;
    }
    else if (static_cast<unsigned>(control) >= static_cast<unsigned>(GraphicsControl::CsmMaxDistance) &&
             static_cast<unsigned>(control) <= static_cast<unsigned>(GraphicsControl::CsmReceiverBias))
    {
        tab = PTab::Csm;
        found = true;
    }
    // The table's: the control's knobs back to the defaults (UeSsrQuality: the preset and every field it sets), then the
    // whole tab applied with the live values for the rest (as a tab's Apply always did).
    for (std::size_t i = 0; i < kEntryCount; ++i)
    {
        const bool ueField = control == GraphicsControl::UeSsrQuality && std::strncmp(kEntries[i].setting, "ssr.ue", 6) == 0;
        if (kEntries[i].control != control && !ueField) { continue; }
        current.value[i] = defaults.value[i];
        current.has[i] = defaults.has[i];
        tab = kEntries[i].tab;
        found = true;
    }
    if (!found)
    {
        LOG_WARNING(logging::LogCategory::App, "graphics settings: Reset of control {} moved nothing", static_cast<int>(control));
        return false;
    }
    ApplyTab(current, tab, renderer, scene, settings);
    if (tab == PTab::Csm) { projectCsm_ = scene.CascadeConfig(); }
    return FinishReset(managedBefore, renderer, scene, settings);
}

bool GraphicsSettingsManager::ResetUpscale(Renderer& renderer, const Scene& scene,
                                           SceneRenderSettings& settings)
{
    const std::map<std::string, float> managedBefore = settings::TheScalability().ManagedValues();
    ApplyTab(DefaultSnapshot(), PTab::Upscale, renderer, const_cast<Scene&>(scene), const_cast<SceneRenderSettings&>(settings));
    return FinishReset(managedBefore, renderer, scene, settings);
}

bool GraphicsSettingsManager::ResetVisibility(Renderer& renderer, const Scene& scene,
                                              const SceneRenderSettings& settings)
{
    const std::map<std::string, float> managedBefore = settings::TheScalability().ManagedValues();
    ApplyTab(DefaultSnapshot(), PTab::Visibility, renderer, const_cast<Scene&>(scene), const_cast<SceneRenderSettings&>(settings));
    return FinishReset(managedBefore, renderer, scene, settings);
}

bool GraphicsSettingsManager::ResetReflections(Renderer& renderer, const Scene& scene,
                                               SceneRenderSettings& settings)
{
    const std::map<std::string, float> managedBefore = settings::TheScalability().ManagedValues();
    ApplyTab(DefaultSnapshot(), PTab::Reflections, renderer, const_cast<Scene&>(scene), const_cast<SceneRenderSettings&>(settings));
    return FinishReset(managedBefore, renderer, scene, settings);
}

bool GraphicsSettingsManager::ResetFog(Renderer& renderer, const Scene& scene,
                                       const SceneRenderSettings& settings)
{
    const std::map<std::string, float> managedBefore = settings::TheScalability().ManagedValues();
    ApplyTab(DefaultSnapshot(), PTab::Fog, renderer, const_cast<Scene&>(scene), const_cast<SceneRenderSettings&>(settings));
    return FinishReset(managedBefore, renderer, scene, settings);
}

bool GraphicsSettingsManager::ResetLod(Renderer& renderer, const Scene& scene,
                                       const SceneRenderSettings& settings)
{
    const std::map<std::string, float> managedBefore = settings::TheScalability().ManagedValues();
    ApplyTab(DefaultSnapshot(), PTab::Lod, renderer, const_cast<Scene&>(scene), const_cast<SceneRenderSettings&>(settings));
    return FinishReset(managedBefore, renderer, scene, settings);
}

bool GraphicsSettingsManager::ResetStreaming(Renderer& renderer, const Scene& scene,
                                             const SceneRenderSettings& settings)
{
    const std::map<std::string, float> managedBefore = settings::TheScalability().ManagedValues();
    ApplyTab(DefaultSnapshot(), PTab::Streaming, renderer, const_cast<Scene&>(scene), const_cast<SceneRenderSettings&>(settings));
    return FinishReset(managedBefore, renderer, scene, settings);
}

bool GraphicsSettingsManager::ResetGi(Renderer& renderer, const Scene& scene, SceneRenderSettings& settings)
{
    const std::map<std::string, float> managedBefore = settings::TheScalability().ManagedValues();
    ApplyTab(DefaultSnapshot(), PTab::Gi, renderer, const_cast<Scene&>(scene), const_cast<SceneRenderSettings&>(settings));
    return FinishReset(managedBefore, renderer, scene, settings);
}

bool GraphicsSettingsManager::ResetCsm(Renderer& renderer, Scene& scene,
                                       const SceneRenderSettings& settings)
{
    const std::map<std::string, float> managedBefore = settings::TheScalability().ManagedValues();
    scene.CascadeConfig() = DefaultSnapshot().csm;
    projectCsm_ = scene.CascadeConfig();
    return FinishReset(managedBefore, renderer, scene, settings);
}

bool GraphicsSettingsManager::ResetContactShadows(Renderer& renderer, const Scene& scene,
                                                   const SceneRenderSettings& settings)
{
    const std::map<std::string, float> managedBefore = settings::TheScalability().ManagedValues();
    ApplyTab(DefaultSnapshot(), PTab::Contact, renderer, const_cast<Scene&>(scene), const_cast<SceneRenderSettings&>(settings));
    return FinishReset(managedBefore, renderer, scene, settings);
}

bool GraphicsSettingsManager::ResetVsm(Renderer& renderer, const Scene& scene,
                                       const SceneRenderSettings& settings)
{
    const std::map<std::string, float> managedBefore = settings::TheScalability().ManagedValues();
    ApplyTab(DefaultSnapshot(), PTab::Vsm, renderer, const_cast<Scene&>(scene), const_cast<SceneRenderSettings&>(settings));
    return FinishReset(managedBefore, renderer, scene, settings);
}
