#include "app/GraphicsSettings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>

#include "app/scene/SceneFrameData.h"
#include "app/scene/Scene.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/VisibilityStats.h"
#include "rendering/debug/LodDebugView.h"
#include "rendering/meshes/LodSelect.h"
#include "rendering/shadows/ShadowSettings.h"
#include "rendering/shadows/VirtualShadowMap.h"
#include "rendering/visibility/OcclusionHistory.h"
#include "third_party/json/json.hpp"

namespace
{
    using json = nlohmann::json;

    struct GraphicsSettingsSnapshot
    {
        bool asyncCompute = true;
        bool visibilityChunkMask = true;
        int occlusionMethod = static_cast<int>(vis::OcclusionMethod::Queries);
        int occlusionQueryLatency = static_cast<int>(vis::kOcclusionBufferedFrames);
        bool occlusionIndirectQueries = false;
        bool indirectGBuffer = true;
        bool gbufferHzbCull = true;
        int fogGridPixels = static_cast<int>(render::kFogGridPixels); // volumetric fog cell size, powers of two 4..64
        int fogGridZ = static_cast<int>(render::kFogGridZ);           // volumetric fog slices, 16..128

        bool dlssEnabled = false;
        sl::DLSSMode dlssMode = sl::DLSSMode::eBalanced;
        float renderScale = 1.0f;
        bool fxaa = false;

        SsrTechnique ssrTechnique = SsrTechnique::LogMarch;
        UeSsrSettings ssrUe{};
        bool reflectionTemporal = true;
        float reflectionTemporalBlend = 0.125f;
        float reflectionTemporalStillInertia = 0.8f;
        float reflectionResolution = 0.5f;
        float oceanReflectionResolution = 0.5f;
        float reflectionGlossyScale = 1.0f;
        float sunMetalSpecInfluence = 0.0f;
        float sunAngularSize = 0.01f;
        ReflectionSource reflectionSource = ReflectionSource::RT;
        std::uint32_t rtAlphaMode = 2u;
        float rtAlphaMissKeep = 0.15f;
        bool rtWindBlas = true;
        float rtWindBlasRadius = 40.0f;

        bool lodEnabled = true;
        float lodBound0 = 10.0f;
        float lodBound1 = 20.0f;
        float lodBound2 = 40.0f;
        float lodFadeBand = 0.10f;
        float chunkLodDistance = 24.0f;
        float chunkLodFactor = 2.0f;

        render::ShadowMode shadowMode = render::ShadowMode::VSM;
        bool giIndirectShadows = true;
        CascadeShadowConfig csm{};

        bool contactEnabled = false;
        std::uint32_t contactLocalMode = 1u;
        bool contactTemporalDither = true;
        bool contactLengthInWorldSpace = false;
        float contactLength = 0.05f;
        float contactIntensity = 1.0f;
        std::uint32_t contactSteps = 8u;
        float contactMaxThickness = 0.5f;
        float contactNormalOffset = 0.04f;
        float contactGrazingFade = 0.15f;
        float contactMinDistance = 0.0f;
        float contactMaxDistance = 0.0f;
        float contactFadeBand = 10.0f;

        int shadowLodBias = 1;
        bool shadowLodBiasNearTier = false;
        int shadowLodTierStride = 2;
        float vsmLodRefDistance = vsm::kLodRefDist;
        std::uint32_t vsmRequestDownscale = vsm::kRequestDownscale;
        std::uint32_t vsmLruFrames = vsm::kLruFrameThreshold;
        float vsmClipmapBaseExtent = 8.0f;
        bool vsmClipmapBlend = true;
        float vsmClipmapBlendWidth = 0.12f;
        std::uint32_t smrtRayCount = 7u;
        std::uint32_t smrtSamplesPerRay = 8u;
        bool smrtTemporalDither = true;
        std::uint32_t smrtAdaptiveRayCount = 1u;
        float smrtLevelMargin = 1.0f;
        float smrtSunAngleDeg = 0.5357f;
        float smrtTexelDitherScale = 2.0f;
        float smrtRayLengthScale = 1.5f;
        float vsmClipmapDepthBias = 0.0002f;
        float vsmClipmapDepthBiasDecay = 1.0f;
        float vsmClipmapDepthBiasFloorTexels = 0.0f;
        float vsmClipmapNormalBias = 1.0f;
        float vsmLocalLateralTexels = 1.0f;
        float vsmLocalDepthPushTexels = 0.5f;
        bool vsmResidentOnly = true;
        bool vsmSingleDraw = true;
        bool vsmHzbCull = false;
        bool vsmPageCaching = true;
        std::uint32_t vsmWindAnimateMaxLevel = 4u;
    };

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
        return mode == render::ShadowMode::Legacy ? "legacy" : "vsm";
    }

    render::ShadowMode ParseShadowMode(const json& value, render::ShadowMode fallback)
    {
        if (!value.is_string()) { return fallback; }
        const std::string name = value.get<std::string>();
        if (name == "legacy") { return render::ShadowMode::Legacy; }
        if (name == "vsm")    { return render::ShadowMode::VSM; }
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

    void Sanitize(GraphicsSettingsSnapshot& s)
    {
        const auto finite = [](float value, float fallback)
        {
            return std::isfinite(value) ? value : fallback;
        };

        const int dlss = static_cast<int>(s.dlssMode);
        if (dlss < static_cast<int>(sl::DLSSMode::eOff) ||
            dlss > static_cast<int>(sl::DLSSMode::eDLAA))
        {
            s.dlssMode = sl::DLSSMode::eBalanced;
        }
        s.renderScale = std::clamp(finite(s.renderScale, 1.0f), 0.1f, 1.0f);
        s.occlusionMethod = std::clamp(s.occlusionMethod, 0, 2);
        s.occlusionQueryLatency = std::clamp(
            s.occlusionQueryLatency, 1, static_cast<int>(vis::kOcclusionBufferedFrames));

        s.ssrUe.numSteps = std::clamp(s.ssrUe.numSteps, 4u, 64u);
        s.ssrUe.numSteps = std::min(64u, (s.ssrUe.numSteps + 3u) & ~3u);
        s.ssrUe.numRays = std::clamp(s.ssrUe.numRays, 1u, 12u);
        s.ssrUe.roughnessOverride = std::clamp(finite(s.ssrUe.roughnessOverride, 0.0f), 0.0f, 1.0f);
        s.ssrUe.intensity = std::clamp(finite(s.ssrUe.intensity, 1.0f), 0.0f, 1.0f);
        s.ssrUe.maxRoughness = std::clamp(finite(s.ssrUe.maxRoughness, 0.6f), 0.01f, 1.0f);
        s.reflectionTemporalBlend = std::clamp(finite(s.reflectionTemporalBlend, 0.125f), 0.02f, 1.0f);
        s.reflectionTemporalStillInertia = std::clamp(finite(s.reflectionTemporalStillInertia, 0.8f), 0.0f, 2.0f);
        s.reflectionResolution = std::clamp(finite(s.reflectionResolution, 0.5f), 0.25f, 1.0f);
        s.oceanReflectionResolution = std::clamp(finite(s.oceanReflectionResolution, 0.5f), 0.25f, 1.0f);
        s.reflectionGlossyScale = std::clamp(finite(s.reflectionGlossyScale, 1.0f), 0.0f, 24.0f);
        s.sunMetalSpecInfluence = std::clamp(finite(s.sunMetalSpecInfluence, 0.0f), 0.0f, 16.0f);
        s.sunAngularSize = std::clamp(finite(s.sunAngularSize, 0.01f), 0.0f, 0.25f);
        s.rtAlphaMode = std::min(s.rtAlphaMode, 2u);
        s.rtAlphaMissKeep = std::clamp(finite(s.rtAlphaMissKeep, 0.15f), 0.0f, 1.0f);
        s.rtWindBlasRadius = std::clamp(finite(s.rtWindBlasRadius, 40.0f), 0.0f, 100.0f);

        s.lodBound0 = std::clamp(finite(s.lodBound0, 10.0f), 2.0f, 60.0f);
        s.lodBound1 = std::clamp(finite(s.lodBound1, 20.0f), 4.0f, 120.0f);
        s.lodBound2 = std::clamp(finite(s.lodBound2, 40.0f), 8.0f, 240.0f);
        s.lodFadeBand = std::clamp(finite(s.lodFadeBand, 0.10f), 0.0f, 0.35f);
        s.chunkLodDistance = std::clamp(finite(s.chunkLodDistance, 24.0f), 24.0f, 400.0f);
        s.chunkLodFactor = std::clamp(finite(s.chunkLodFactor, 2.0f), 1.2f, 4.0f);

        s.contactLocalMode = std::min(s.contactLocalMode, 2u);
        s.contactLength = std::max(0.0f, finite(s.contactLength, 0.05f));
        s.contactIntensity = std::clamp(finite(s.contactIntensity, 1.0f), 0.0f, 1.0f);
        s.contactSteps = std::clamp(s.contactSteps, 1u, 16u);
        s.contactMaxThickness = std::clamp(finite(s.contactMaxThickness, 0.5f), 0.0f, 3.0f);
        s.contactNormalOffset = std::clamp(finite(s.contactNormalOffset, 0.04f), 0.0f, 0.5f);
        s.contactGrazingFade = std::clamp(finite(s.contactGrazingFade, 0.15f), 0.0f, 0.5f);
        s.contactMinDistance = std::clamp(finite(s.contactMinDistance, 0.0f), 0.0f, 200.0f);
        s.contactMaxDistance = std::clamp(finite(s.contactMaxDistance, 0.0f), 0.0f, 2000.0f);
        s.contactFadeBand = std::clamp(finite(s.contactFadeBand, 10.0f), 0.1f, 200.0f);

        s.shadowLodBias = std::clamp(s.shadowLodBias, -2, 3);
        s.shadowLodTierStride = std::clamp(s.shadowLodTierStride, 1, 8);
        s.vsmLodRefDistance = std::clamp(finite(s.vsmLodRefDistance, vsm::kLodRefDist), 1.0f, 40.0f);
        s.vsmRequestDownscale = std::clamp(s.vsmRequestDownscale, 1u, 8u);
        s.vsmLruFrames = std::clamp(s.vsmLruFrames, 1u, 120u);
        s.vsmClipmapBaseExtent = std::clamp(finite(s.vsmClipmapBaseExtent, 8.0f), 4.0f, 200.0f);
        s.vsmClipmapBlendWidth = std::clamp(finite(s.vsmClipmapBlendWidth, 0.12f), 0.0f, 0.30f);
        s.smrtRayCount = std::min(s.smrtRayCount, vsm::kSmrtMaxRays);
        s.smrtSamplesPerRay = std::clamp(s.smrtSamplesPerRay, 1u, vsm::kSmrtMaxSamplesPerRay);
        s.smrtAdaptiveRayCount = std::min(s.smrtAdaptiveRayCount, 8u);
        s.smrtLevelMargin = std::clamp(finite(s.smrtLevelMargin, 1.0f), 0.25f, 1.0f);
        s.smrtSunAngleDeg = std::clamp(finite(s.smrtSunAngleDeg, 0.5357f), 0.0f, 8.0f);
        s.smrtTexelDitherScale = std::clamp(finite(s.smrtTexelDitherScale, 2.0f), 0.0f, 4.0f);
        s.smrtRayLengthScale = std::clamp(finite(s.smrtRayLengthScale, 1.5f), 0.0f, 4.0f);
        s.vsmClipmapDepthBias = std::clamp(finite(s.vsmClipmapDepthBias, 0.0002f), 0.0f, 0.01f);
        s.vsmClipmapDepthBiasDecay = std::clamp(finite(s.vsmClipmapDepthBiasDecay, 1.0f), 0.25f, 1.0f);
        s.vsmClipmapDepthBiasFloorTexels = std::clamp(finite(s.vsmClipmapDepthBiasFloorTexels, 0.0f), 0.0f, 1.5f);
        s.vsmClipmapNormalBias = std::clamp(finite(s.vsmClipmapNormalBias, 1.0f), 0.0f, 4.0f);
        s.vsmLocalLateralTexels = std::clamp(finite(s.vsmLocalLateralTexels, 1.0f), 0.0f, 4.0f);
        s.vsmLocalDepthPushTexels = std::clamp(finite(s.vsmLocalDepthPushTexels, 0.5f), 0.0f, 4.0f);
        s.vsmWindAnimateMaxLevel = std::min(s.vsmWindAnimateMaxLevel, vsm::kNumClipmapLevels);

        s.csm.maxDistance = std::clamp(finite(s.csm.maxDistance, 300.0f), 20.0f, 1000.0f);
        for (float& split : s.csm.sliceDistances)
        {
            split = std::clamp(finite(split, s.csm.maxDistance), 0.5f, 1000.0f);
        }
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

    GraphicsSettingsSnapshot Capture(const Renderer& renderer, const Scene& scene,
                                     const SceneRenderSettings& settings)
    {
        GraphicsSettingsSnapshot s{};
        s.asyncCompute = !render::g_noAsyncCompute;
        s.visibilityChunkMask = render::g_visChunkMask;
        s.occlusionMethod = vis::g_occlusion.method;
        s.occlusionQueryLatency = vis::g_occlusion.queryLatency;
        s.occlusionIndirectQueries = vis::g_occlusion.indirectQueries;
        s.indirectGBuffer = render::g_indirectGBufferEnabled;
        s.gbufferHzbCull = render::g_gbufferHzbCullEnabled;
        s.fogGridPixels = static_cast<int>(render::g_fogGridPixels);
        s.fogGridZ = static_cast<int>(render::g_fogGridZ);
        s.dlssEnabled = renderer.IsDlssRequestedActive();
        s.dlssMode = renderer.GetDlssMode();
        s.renderScale = renderer.GetRenderResolutionScale();
        s.fxaa = settings.doFxaa;

        s.ssrTechnique = settings.ssrTechnique;
        s.ssrUe = settings.ssrUe;
        s.reflectionTemporal = settings.ssrTemporal;
        s.reflectionTemporalBlend = settings.ssrTemporalBlendWeight;
        s.reflectionTemporalStillInertia = settings.ssrTemporalClampExpand;
        s.reflectionResolution = renderer.GetReflectionTextureScale().x;
        s.oceanReflectionResolution = renderer.GetOceanReflectionTextureScale().x;
        s.reflectionGlossyScale = settings.reflectionGlossyScale;
        s.sunMetalSpecInfluence = settings.sunMetalSpecInfluence;
        s.sunAngularSize = settings.sunAngularSize;
        s.reflectionSource = settings.reflectionSource;
        s.rtAlphaMode = settings.rtAlphaMode;
        s.rtAlphaMissKeep = settings.rtAlphaMissKeep;
        s.rtWindBlas = settings.rtWindBlas;
        s.rtWindBlasRadius = settings.rtWindBlasRadius;

        s.lodEnabled = render::g_lodEnabled;
        s.lodBound0 = render::g_lodBound0;
        s.lodBound1 = render::g_lodBound1;
        s.lodBound2 = render::g_lodBound2;
        s.lodFadeBand = render::g_lodFadeBand;
        s.chunkLodDistance = render::g_chunkLodDist0;
        s.chunkLodFactor = render::g_chunkLodDistFactor;

        s.shadowMode = render::g_shadowModeFromCli ? render::g_shadowModePersisted : render::g_shadowMode;
        s.giIndirectShadows = render::g_giIndirectShadowsEnabled;
        s.csm = scene.CascadeConfig();

        s.contactEnabled = render::contact::g_enabled;
        s.contactLocalMode = render::contact::g_localMode;
        s.contactTemporalDither = render::contact::g_temporalDither;
        s.contactLengthInWorldSpace = render::contact::g_lengthInWorldSpace;
        s.contactLength = render::contact::g_length;
        s.contactIntensity = render::contact::g_intensity;
        s.contactSteps = render::contact::g_steps;
        s.contactMaxThickness = render::contact::g_maxThicknessFrac;
        s.contactNormalOffset = render::contact::g_normalOffsetFrac;
        s.contactGrazingFade = render::contact::g_grazingFadeNdotL;
        s.contactMinDistance = render::contact::g_minDistanceM;
        s.contactMaxDistance = render::contact::g_maxDistanceM;
        s.contactFadeBand = render::contact::g_fadeBandM;

        s.shadowLodBias = render::g_shadowLodBias;
        s.shadowLodBiasNearTier = render::g_shadowLodBiasNearTier;
        s.shadowLodTierStride = render::g_shadowLodTierStride;
        s.vsmLodRefDistance = vsm::g_refDist;
        s.vsmRequestDownscale = vsm::g_requestDownscale;
        s.vsmLruFrames = vsm::g_lruThreshold;
        s.vsmClipmapBaseExtent = vsm::g_clipmapBaseExtent;
        s.vsmClipmapBlend = vsm::g_clipmapBlendEnabled;
        s.vsmClipmapBlendWidth = vsm::g_clipmapBlendWidth;
        s.smrtRayCount = vsm::g_smrtRayCount;
        s.smrtSamplesPerRay = vsm::g_smrtSamplesPerRay;
        s.smrtTemporalDither = vsm::g_smrtTemporalDither;
        s.smrtAdaptiveRayCount = vsm::g_smrtAdaptiveRayCount;
        s.smrtLevelMargin = vsm::g_smrtLevelMargin;
        s.smrtSunAngleDeg = vsm::g_smrtSourceAngleDeg;
        s.smrtTexelDitherScale = vsm::g_smrtTexelDitherScale;
        s.smrtRayLengthScale = vsm::g_smrtRayLengthScale;
        s.vsmClipmapDepthBias = vsm::g_clipmapDepthBias;
        s.vsmClipmapDepthBiasDecay = vsm::g_clipmapDepthBiasDecay;
        s.vsmClipmapDepthBiasFloorTexels = vsm::g_clipmapDepthBiasFloorTexels;
        s.vsmClipmapNormalBias = vsm::g_clipmapNormalBias;
        s.vsmLocalLateralTexels = vsm::g_localLateralTexels;
        s.vsmLocalDepthPushTexels = vsm::g_localDepthPushTexels;
        s.vsmResidentOnly = vsm::g_residentIterOnly;
        s.vsmSingleDraw = vsm::g_pageDrawSingle;
        s.vsmHzbCull = vsm::g_hzbCull;
        s.vsmPageCaching = vsm::g_pageCaching;
        s.vsmWindAnimateMaxLevel = vsm::g_windAnimateMaxLevel;
        Sanitize(s);
        return s;
    }

    void ApplyRender(const GraphicsSettingsSnapshot& s, Renderer& renderer, SceneRenderSettings& settings)
    {
        render::g_noAsyncCompute = !s.asyncCompute;
        render::g_visChunkMask = s.visibilityChunkMask;
        vis::g_occlusion.method = s.occlusionMethod;
        vis::g_occlusion.queryLatency = s.occlusionQueryLatency;
        vis::g_occlusion.indirectQueries = s.occlusionIndirectQueries;
        settings.doFxaa = s.fxaa;
        render::g_fogGridPixels = static_cast<unsigned>(std::clamp(s.fogGridPixels, 4, 64)); // the renderer rounds to a power of two
        render::g_fogGridZ = static_cast<unsigned>(std::clamp(s.fogGridZ, 16, 128));
        settings.ssrTechnique = s.ssrTechnique;
        settings.ssrUe = s.ssrUe;
        settings.ssrTemporal = s.reflectionTemporal;
        settings.ssrTemporalBlendWeight = s.reflectionTemporalBlend;
        settings.ssrTemporalClampExpand = s.reflectionTemporalStillInertia;
        settings.reflectionGlossyScale = s.reflectionGlossyScale;
        settings.sunMetalSpecInfluence = s.sunMetalSpecInfluence;
        settings.sunAngularSize = s.sunAngularSize;
        settings.reflectionSource = s.reflectionSource;
        settings.rtAlphaMode = s.rtAlphaMode;
        settings.rtAlphaMissKeep = s.rtAlphaMissKeep;
        settings.rtWindBlas = s.rtWindBlas;
        settings.rtWindBlasRadius = s.rtWindBlasRadius;

        renderer.SetReflectionTextureScale(s.reflectionResolution);
        renderer.SetOceanReflectionTextureScale(s.oceanReflectionResolution);
        renderer.SetDlssMode(s.dlssMode);
        renderer.SetDlssActive(s.dlssEnabled && s.dlssMode != sl::DLSSMode::eOff);
        if (!s.dlssEnabled || s.dlssMode == sl::DLSSMode::eOff)
        {
            renderer.SetRenderResolutionScale(s.renderScale);
        }
    }

    void ApplyLod(const GraphicsSettingsSnapshot& s)
    {
        render::g_lodEnabled = s.lodEnabled;
        render::g_lodBound0 = s.lodBound0;
        render::g_lodBound1 = s.lodBound1;
        render::g_lodBound2 = s.lodBound2;
        render::g_lodFadeBand = s.lodFadeBand;
        render::g_chunkLodDist0 = s.chunkLodDistance;
        render::g_chunkLodDistFactor = s.chunkLodFactor;
    }

    void ApplyContact(const GraphicsSettingsSnapshot& s)
    {
        render::contact::g_enabled = s.contactEnabled;
        render::contact::g_localMode = s.contactLocalMode;
        render::contact::g_temporalDither = s.contactTemporalDither;
        render::contact::g_lengthInWorldSpace = s.contactLengthInWorldSpace;
        render::contact::g_length = s.contactLength;
        render::contact::g_intensity = s.contactIntensity;
        render::contact::g_steps = s.contactSteps;
        render::contact::g_maxThicknessFrac = s.contactMaxThickness;
        render::contact::g_normalOffsetFrac = s.contactNormalOffset;
        render::contact::g_grazingFadeNdotL = s.contactGrazingFade;
        render::contact::g_minDistanceM = s.contactMinDistance;
        render::contact::g_maxDistanceM = s.contactMaxDistance;
        render::contact::g_fadeBandM = s.contactFadeBand;
    }

    void ApplyVsm(const GraphicsSettingsSnapshot& s)
    {
        // A `--shadow-mode=` boot override keeps the mode it set; the file's value is remembered
        // so a save in that session writes the file's own mode back, not the override.
        render::g_shadowModePersisted = s.shadowMode;
        if (!render::g_shadowModeFromCli) { render::g_shadowMode = s.shadowMode; }
        render::g_giIndirectShadowsEnabled = s.giIndirectShadows;
        render::g_indirectGBufferEnabled = s.indirectGBuffer;
        render::g_gbufferHzbCullEnabled = s.gbufferHzbCull;
        render::g_shadowLodBias = s.shadowLodBias;
        render::g_shadowLodBiasNearTier = s.shadowLodBiasNearTier;
        render::g_shadowLodTierStride = s.shadowLodTierStride;
        vsm::g_refDist = s.vsmLodRefDistance;
        vsm::g_requestDownscale = s.vsmRequestDownscale;
        vsm::g_lruThreshold = s.vsmLruFrames;
        vsm::g_clipmapBaseExtent = s.vsmClipmapBaseExtent;
        vsm::g_clipmapBlendEnabled = s.vsmClipmapBlend;
        vsm::g_clipmapBlendWidth = s.vsmClipmapBlendWidth;
        vsm::g_smrtRayCount = s.smrtRayCount;
        vsm::g_smrtSamplesPerRay = s.smrtSamplesPerRay;
        vsm::g_smrtTemporalDither = s.smrtTemporalDither;
        vsm::g_smrtAdaptiveRayCount = s.smrtAdaptiveRayCount;
        vsm::g_smrtLevelMargin = s.smrtLevelMargin;
        vsm::g_smrtSourceAngleDeg = s.smrtSunAngleDeg;
        vsm::g_smrtTexelDitherScale = s.smrtTexelDitherScale;
        vsm::g_smrtRayLengthScale = s.smrtRayLengthScale;
        vsm::g_clipmapDepthBias = s.vsmClipmapDepthBias;
        vsm::g_clipmapDepthBiasDecay = s.vsmClipmapDepthBiasDecay;
        vsm::g_clipmapDepthBiasFloorTexels = s.vsmClipmapDepthBiasFloorTexels;
        vsm::g_clipmapNormalBias = s.vsmClipmapNormalBias;
        vsm::g_localLateralTexels = s.vsmLocalLateralTexels;
        vsm::g_localDepthPushTexels = s.vsmLocalDepthPushTexels;
        vsm::g_residentIterOnly = s.vsmResidentOnly;
        vsm::g_pageDrawSingle = s.vsmSingleDraw;
        vsm::g_hzbCull = s.vsmHzbCull;
        vsm::g_pageCaching = s.vsmPageCaching;
        vsm::g_windAnimateMaxLevel = s.vsmWindAnimateMaxLevel;
    }

    void ApplyAll(const GraphicsSettingsSnapshot& s, Renderer& renderer, Scene& scene,
                  SceneRenderSettings& settings)
    {
        ApplyRender(s, renderer, settings);
        ApplyLod(s);
        ApplyContact(s);
        ApplyVsm(s);
        scene.CascadeConfig() = s.csm;
    }

    json ToJson(const GraphicsSettingsSnapshot& s)
    {
        return json{
            { "version", 1 },
            { "scope", "Global runtime graphics quality. Scene look stays in level environment data." },
            { "performance", {
                { "asyncCompute", s.asyncCompute },
                { "gpuDrivenGBuffer", s.indirectGBuffer },
                { "gbufferHzbCull", s.gbufferHzbCull },
                { "fogGridPixels", s.fogGridPixels },
                { "fogGridZ", s.fogGridZ }
            } },
            { "visibility", {
                { "chunkMask", s.visibilityChunkMask },
                { "occlusionMethod", s.occlusionMethod },
                { "queryLatency", s.occlusionQueryLatency },
                { "queryGpuDrivenObjects", s.occlusionIndirectQueries }
            } },
            { "antiAliasing", {
                { "dlssEnabled", s.dlssEnabled },
                { "dlssMode", DlssModeName(s.dlssMode) },
                { "renderScale", s.renderScale },
                { "fxaa", s.fxaa }
            } },
            { "reflections", {
                { "source", ReflectionSourceName(s.reflectionSource) },
                { "ssrTechnique", SsrTechniqueName(s.ssrTechnique) },
                { "ueSsr", {
                    { "preset", static_cast<std::uint32_t>(s.ssrUe.preset) },
                    { "stepsPerRay", s.ssrUe.numSteps },
                    { "raysPerPixel", s.ssrUe.numRays },
                    { "glossyRays", s.ssrUe.glossyRays },
                    { "useSurfaceRoughness", s.ssrUe.useSurfaceRoughness },
                    { "roughnessOverride", s.ssrUe.roughnessOverride },
                    { "intensity", s.ssrUe.intensity },
                    { "maxRoughness", s.ssrUe.maxRoughness }
                } },
                { "temporal", s.reflectionTemporal },
                { "temporalBlend", s.reflectionTemporalBlend },
                { "temporalStillInertia", s.reflectionTemporalStillInertia },
                { "resolution", s.reflectionResolution },
                { "oceanResolution", s.oceanReflectionResolution },
                { "glossyBlur", s.reflectionGlossyScale },
                { "sunMetalSpec", s.sunMetalSpecInfluence },
                { "sunAngularSize", s.sunAngularSize },
                { "rtFoliageAlphaMode", s.rtAlphaMode },
                { "rtFoliageFill", s.rtAlphaMissKeep },
                { "rtWindSway", s.rtWindBlas },
                { "rtWindRadius", s.rtWindBlasRadius }
            } },
            { "lod", {
                { "enabled", s.lodEnabled },
                { "regularMeshRatios", { s.lodBound0, s.lodBound1, s.lodBound2 } },
                { "crossfadeBand", s.lodFadeBand },
                { "chunkDistance", s.chunkLodDistance },
                { "chunkFactor", s.chunkLodFactor }
            } },
            { "shadows", {
                { "mode", ShadowModeName(s.shadowMode) },
                { "gpuInstancedCasters", s.giIndirectShadows },
                { "contact", {
                    { "enabled", s.contactEnabled },
                    { "localLightMode", s.contactLocalMode },
                    { "temporalDither", s.contactTemporalDither },
                    { "lengthInWorldSpace", s.contactLengthInWorldSpace },
                    { "length", s.contactLength },
                    { "intensity", s.contactIntensity },
                    { "steps", s.contactSteps },
                    { "maxThickness", s.contactMaxThickness },
                    { "normalOffset", s.contactNormalOffset },
                    { "grazingFade", s.contactGrazingFade },
                    { "minDistance", s.contactMinDistance },
                    { "maxDistance", s.contactMaxDistance },
                    { "fadeBand", s.contactFadeBand }
                } },
                { "legacyCsm", {
                    { "maxDistance", s.csm.maxDistance },
                    { "autoSplits", s.csm.useUeSplitDistribution },
                    { "distributionExponent", s.csm.cascadeDistributionExponent },
                    { "splitDistances", s.csm.sliceDistances },
                    { "overlapTexels", s.csm.overlapInTexels },
                    { "zPadding", s.csm.zPadding },
                    { "casterReach", s.csm.casterReachWS },
                    { "scissorOptim", s.csm.scissorOptim },
                    { "scissorPadTexels", s.csm.scissorPadTexels },
                    { "accurateCasterCull", s.csm.accurateCasterCull },
                    { "hzbCull", s.csm.hzbCull },
                    { "pancakeCasters", s.csm.pancakeCasters },
                    { "pancakeSlack", s.csm.pancakeSlackWS },
                    { "depthBiasTexels", s.csm.depthBiasInTexels },
                    { "slopeScale", s.csm.slopeScale },
                    { "maxSlope", s.csm.maxSlope },
                    { "normalBiasTexels", s.csm.normalBiasInTexels },
                    { "blendFraction", s.csm.blendFraction },
                    { "distanceFadeFraction", s.csm.distanceFadeFraction },
                    { "filterMode", s.csm.filterMode },
                    { "filterSharpen", s.csm.shadowFilterSharpen },
                    { "receiverBias", s.csm.csmReceiverBias },
                    { "pcfOverBlurCorrection", s.csm.pcfOverBlurCorrection }
                } },
                { "vsm", {
                    { "shadowLodBias", s.shadowLodBias },
                    { "biasNearestTier", s.shadowLodBiasNearTier },
                    { "shadowTiersPerLod", s.shadowLodTierStride },
                    { "lodRefDistance", s.vsmLodRefDistance },
                    { "requestDownscale", s.vsmRequestDownscale },
                    { "lruFrames", s.vsmLruFrames },
                    { "clipmapBaseExtent", s.vsmClipmapBaseExtent },
                    { "clipmapBlend", s.vsmClipmapBlend },
                    { "clipmapBlendWidth", s.vsmClipmapBlendWidth },
                    { "smrtRayCount", s.smrtRayCount },
                    { "smrtSamplesPerRay", s.smrtSamplesPerRay },
                    { "smrtTemporalDither", s.smrtTemporalDither },
                    { "smrtAdaptiveAfterRays", s.smrtAdaptiveRayCount },
                    { "smrtLevelMargin", s.smrtLevelMargin },
                    { "smrtSunAngleDeg", s.smrtSunAngleDeg },
                    { "smrtTexelDither", s.smrtTexelDitherScale },
                    { "smrtRayLengthScale", s.smrtRayLengthScale },
                    { "clipmapDepthBias", s.vsmClipmapDepthBias },
                    { "depthBiasDecay", s.vsmClipmapDepthBiasDecay },
                    { "depthBiasFloorTexels", s.vsmClipmapDepthBiasFloorTexels },
                    { "clipmapNormalBias", s.vsmClipmapNormalBias },
                    { "localLateralBiasTexels", s.vsmLocalLateralTexels },
                    { "localDepthPushTexels", s.vsmLocalDepthPushTexels },
                    { "residentOnly", s.vsmResidentOnly },
                    { "singleDraw", s.vsmSingleDraw },
                    { "hzbCull", s.vsmHzbCull },
                    { "pageCaching", s.vsmPageCaching },
                    { "windAnimateBelowLevel", s.vsmWindAnimateMaxLevel }
                } }
            } }
        };
    }

    GraphicsSettingsSnapshot FromJson(const json& root)
    {
        GraphicsSettingsSnapshot s{};
        const json& performance = Section(root, "performance");
        const json& visibility = Section(root, "visibility");
        const json& aa = Section(root, "antiAliasing");
        const json& reflections = Section(root, "reflections");
        const json& ue = Section(reflections, "ueSsr");
        const json& lod = Section(root, "lod");
        const json& shadows = Section(root, "shadows");
        const json& contact = Section(shadows, "contact");
        const json& csm = Section(shadows, "legacyCsm");
        const json& vsmSettings = Section(shadows, "vsm");

        Read(performance, "asyncCompute", s.asyncCompute);
        Read(performance, "gpuDrivenGBuffer", s.indirectGBuffer);
        Read(performance, "gbufferHzbCull", s.gbufferHzbCull);
        Read(performance, "fogGridPixels", s.fogGridPixels);
        Read(performance, "fogGridZ", s.fogGridZ);
        Read(visibility, "chunkMask", s.visibilityChunkMask);
        Read(visibility, "occlusionMethod", s.occlusionMethod);
        Read(visibility, "queryLatency", s.occlusionQueryLatency);
        Read(visibility, "queryGpuDrivenObjects", s.occlusionIndirectQueries);
        Read(aa, "dlssEnabled", s.dlssEnabled);
        if (const auto it = aa.find("dlssMode"); it != aa.end())
        {
            s.dlssMode = ParseDlssMode(*it, s.dlssMode);
        }
        Read(aa, "renderScale", s.renderScale);
        Read(aa, "fxaa", s.fxaa);

        if (const auto it = reflections.find("source"); it != reflections.end())
        {
            s.reflectionSource = ParseReflectionSource(*it, s.reflectionSource);
        }
        if (const auto it = reflections.find("ssrTechnique"); it != reflections.end())
        {
            s.ssrTechnique = ParseSsrTechnique(*it, s.ssrTechnique);
        }
        std::uint32_t preset = static_cast<std::uint32_t>(s.ssrUe.preset);
        Read(ue, "preset", preset);
        if (preset < static_cast<std::uint32_t>(UeSsrQualityPreset::Count))
        {
            s.ssrUe.preset = static_cast<UeSsrQualityPreset>(preset);
        }
        Read(ue, "stepsPerRay", s.ssrUe.numSteps);
        Read(ue, "raysPerPixel", s.ssrUe.numRays);
        Read(ue, "glossyRays", s.ssrUe.glossyRays);
        Read(ue, "useSurfaceRoughness", s.ssrUe.useSurfaceRoughness);
        Read(ue, "roughnessOverride", s.ssrUe.roughnessOverride);
        Read(ue, "intensity", s.ssrUe.intensity);
        Read(ue, "maxRoughness", s.ssrUe.maxRoughness);
        Read(reflections, "temporal", s.reflectionTemporal);
        Read(reflections, "temporalBlend", s.reflectionTemporalBlend);
        Read(reflections, "temporalStillInertia", s.reflectionTemporalStillInertia);
        Read(reflections, "resolution", s.reflectionResolution);
        Read(reflections, "oceanResolution", s.oceanReflectionResolution);
        Read(reflections, "glossyBlur", s.reflectionGlossyScale);
        Read(reflections, "sunMetalSpec", s.sunMetalSpecInfluence);
        Read(reflections, "sunAngularSize", s.sunAngularSize);
        Read(reflections, "rtFoliageAlphaMode", s.rtAlphaMode);
        Read(reflections, "rtFoliageFill", s.rtAlphaMissKeep);
        Read(reflections, "rtWindSway", s.rtWindBlas);
        Read(reflections, "rtWindRadius", s.rtWindBlasRadius);

        Read(lod, "enabled", s.lodEnabled);
        const auto ratios = lod.find("regularMeshRatios");
        if (ratios != lod.end() && ratios->is_array() && ratios->size() >= 3)
        {
            if ((*ratios)[0].is_number()) { s.lodBound0 = (*ratios)[0].get<float>(); }
            if ((*ratios)[1].is_number()) { s.lodBound1 = (*ratios)[1].get<float>(); }
            if ((*ratios)[2].is_number()) { s.lodBound2 = (*ratios)[2].get<float>(); }
        }
        Read(lod, "crossfadeBand", s.lodFadeBand);
        Read(lod, "chunkDistance", s.chunkLodDistance);
        Read(lod, "chunkFactor", s.chunkLodFactor);

        if (const auto it = shadows.find("mode"); it != shadows.end())
        {
            s.shadowMode = ParseShadowMode(*it, s.shadowMode);
        }
        Read(shadows, "gpuInstancedCasters", s.giIndirectShadows);
        Read(contact, "enabled", s.contactEnabled);
        Read(contact, "localLightMode", s.contactLocalMode);
        Read(contact, "temporalDither", s.contactTemporalDither);
        Read(contact, "lengthInWorldSpace", s.contactLengthInWorldSpace);
        Read(contact, "length", s.contactLength);
        Read(contact, "intensity", s.contactIntensity);
        Read(contact, "steps", s.contactSteps);
        Read(contact, "maxThickness", s.contactMaxThickness);
        Read(contact, "normalOffset", s.contactNormalOffset);
        Read(contact, "grazingFade", s.contactGrazingFade);
        Read(contact, "minDistance", s.contactMinDistance);
        Read(contact, "maxDistance", s.contactMaxDistance);
        Read(contact, "fadeBand", s.contactFadeBand);

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

        Read(vsmSettings, "shadowLodBias", s.shadowLodBias);
        Read(vsmSettings, "biasNearestTier", s.shadowLodBiasNearTier);
        Read(vsmSettings, "shadowTiersPerLod", s.shadowLodTierStride);
        Read(vsmSettings, "lodRefDistance", s.vsmLodRefDistance);
        Read(vsmSettings, "requestDownscale", s.vsmRequestDownscale);
        Read(vsmSettings, "lruFrames", s.vsmLruFrames);
        Read(vsmSettings, "clipmapBaseExtent", s.vsmClipmapBaseExtent);
        Read(vsmSettings, "clipmapBlend", s.vsmClipmapBlend);
        Read(vsmSettings, "clipmapBlendWidth", s.vsmClipmapBlendWidth);
        Read(vsmSettings, "smrtRayCount", s.smrtRayCount);
        Read(vsmSettings, "smrtSamplesPerRay", s.smrtSamplesPerRay);
        Read(vsmSettings, "smrtTemporalDither", s.smrtTemporalDither);
        Read(vsmSettings, "smrtAdaptiveAfterRays", s.smrtAdaptiveRayCount);
        Read(vsmSettings, "smrtLevelMargin", s.smrtLevelMargin);
        Read(vsmSettings, "smrtSunAngleDeg", s.smrtSunAngleDeg);
        Read(vsmSettings, "smrtTexelDither", s.smrtTexelDitherScale);
        Read(vsmSettings, "smrtRayLengthScale", s.smrtRayLengthScale);
        Read(vsmSettings, "clipmapDepthBias", s.vsmClipmapDepthBias);
        Read(vsmSettings, "depthBiasDecay", s.vsmClipmapDepthBiasDecay);
        Read(vsmSettings, "depthBiasFloorTexels", s.vsmClipmapDepthBiasFloorTexels);
        Read(vsmSettings, "clipmapNormalBias", s.vsmClipmapNormalBias);
        Read(vsmSettings, "localLateralBiasTexels", s.vsmLocalLateralTexels);
        Read(vsmSettings, "localDepthPushTexels", s.vsmLocalDepthPushTexels);
        Read(vsmSettings, "residentOnly", s.vsmResidentOnly);
        Read(vsmSettings, "singleDraw", s.vsmSingleDraw);
        Read(vsmSettings, "hzbCull", s.vsmHzbCull);
        Read(vsmSettings, "pageCaching", s.vsmPageCaching);
        Read(vsmSettings, "windAnimateBelowLevel", s.vsmWindAnimateMaxLevel);

        Sanitize(s);
        return s;
    }

    bool LoadSnapshot(GraphicsSettingsSnapshot& out, std::string& error)
    {
        std::ifstream file(GraphicsSettingsManager::kPath, std::ios::binary);
        if (!file)
        {
            error = "file not found";
            return false;
        }
        const json root = json::parse(file, nullptr, false, /*ignore_comments=*/true);
        if (!root.is_object())
        {
            error = "invalid JSON root";
            return false;
        }
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

    bool SaveSnapshot(const GraphicsSettingsSnapshot& snapshot, std::string& error)
    {
        std::ofstream file(GraphicsSettingsManager::kPath, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            error = "cannot open file for writing";
            return false;
        }
        file << ToJson(snapshot).dump(2) << '\n';
        if (!file.good())
        {
            error = "write failed";
            return false;
        }
        return true;
    }
}

void GraphicsSettingsManager::Initialize(Renderer& renderer, Scene& scene,
                                         SceneRenderSettings& settings)
{
    initialized_ = true;
    GraphicsSettingsSnapshot loaded{};
    std::string error;
    if (LoadSnapshot(loaded, error))
    {
        ApplyAll(loaded, renderer, scene, settings);
        status_ = std::string("Loaded ") + kPath;
    }
    else
    {
        std::error_code existsError;
        const bool exists = std::filesystem::exists(kPath, existsError);
        if (!exists && !existsError)
        {
            ApplyAll(loaded, renderer, scene, settings);
            if (SaveSnapshot(Capture(renderer, scene, settings), error))
            {
                status_ = std::string("Created ") + kPath;
            }
            else
            {
                status_ = std::string("Could not create ") + kPath + ": " + error;
            }
        }
        else
        {
            status_ = std::string("Could not load ") + kPath + ": " + error;
        }
    }
    projectCsm_ = scene.CascadeConfig();
    savePending_ = false;
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

bool GraphicsSettingsManager::Reload(Renderer& renderer, Scene& scene,
                                     SceneRenderSettings& settings)
{
    GraphicsSettingsSnapshot loaded{};
    std::string error;
    if (!LoadSnapshot(loaded, error))
    {
        status_ = std::string("Could not reload ") + kPath + ": " + error;
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
    GraphicsSettingsSnapshot defaults{};
    ApplyAll(defaults, renderer, scene, settings);
    return SaveCurrent(renderer, scene, settings);
}

bool GraphicsSettingsManager::ResetControl(GraphicsControl control, Renderer& renderer,
                                           Scene& scene, SceneRenderSettings& settings)
{
    GraphicsSettingsSnapshot current = Capture(renderer, scene, settings);
    const GraphicsSettingsSnapshot defaults{};

    switch (control)
    {
    case GraphicsControl::AsyncCompute:                   current.asyncCompute = defaults.asyncCompute; break;
    case GraphicsControl::VisibilityChunkMask:             current.visibilityChunkMask = defaults.visibilityChunkMask; break;
    case GraphicsControl::OcclusionMethod:                 current.occlusionMethod = defaults.occlusionMethod; break;
    case GraphicsControl::OcclusionQueryLatency:           current.occlusionQueryLatency = defaults.occlusionQueryLatency; break;
    case GraphicsControl::OcclusionIndirectQueries:        current.occlusionIndirectQueries = defaults.occlusionIndirectQueries; break;
    case GraphicsControl::DlssEnabled:                    current.dlssEnabled = defaults.dlssEnabled; break;
    case GraphicsControl::DlssMode:                       current.dlssMode = defaults.dlssMode; break;
    case GraphicsControl::Fxaa:                           current.fxaa = defaults.fxaa; break;
    case GraphicsControl::SsrTechnique:                   current.ssrTechnique = defaults.ssrTechnique; break;
    case GraphicsControl::UeSsrQuality:                   current.ssrUe = defaults.ssrUe; break;
    case GraphicsControl::UeSsrSteps:
        current.ssrUe.numSteps = defaults.ssrUe.numSteps;
        current.ssrUe.preset = UeSsrQualityPreset::Custom;
        break;
    case GraphicsControl::UeSsrRays:
        current.ssrUe.numRays = defaults.ssrUe.numRays;
        current.ssrUe.preset = UeSsrQualityPreset::Custom;
        break;
    case GraphicsControl::UeSsrGlossyRays:
        current.ssrUe.glossyRays = defaults.ssrUe.glossyRays;
        current.ssrUe.preset = UeSsrQualityPreset::Custom;
        break;
    case GraphicsControl::UeSsrUseSurfaceRoughness:       current.ssrUe.useSurfaceRoughness = defaults.ssrUe.useSurfaceRoughness; break;
    case GraphicsControl::UeSsrRoughnessOverride:         current.ssrUe.roughnessOverride = defaults.ssrUe.roughnessOverride; break;
    case GraphicsControl::UeSsrIntensity:                 current.ssrUe.intensity = defaults.ssrUe.intensity; break;
    case GraphicsControl::UeSsrMaxRoughness:              current.ssrUe.maxRoughness = defaults.ssrUe.maxRoughness; break;
    case GraphicsControl::ReflectionTemporal:             current.reflectionTemporal = defaults.reflectionTemporal; break;
    case GraphicsControl::ReflectionTemporalBlend:        current.reflectionTemporalBlend = defaults.reflectionTemporalBlend; break;
    case GraphicsControl::ReflectionTemporalStillInertia: current.reflectionTemporalStillInertia = defaults.reflectionTemporalStillInertia; break;
    case GraphicsControl::ReflectionResolution:           current.reflectionResolution = defaults.reflectionResolution; break;
    case GraphicsControl::ReflectionGlossyScale:          current.reflectionGlossyScale = defaults.reflectionGlossyScale; break;
    case GraphicsControl::SunMetalSpecInfluence:          current.sunMetalSpecInfluence = defaults.sunMetalSpecInfluence; break;
    case GraphicsControl::SunAngularSize:                 current.sunAngularSize = defaults.sunAngularSize; break;
    case GraphicsControl::OceanReflectionResolution:      current.oceanReflectionResolution = defaults.oceanReflectionResolution; break;
    case GraphicsControl::ReflectionSource:               current.reflectionSource = defaults.reflectionSource; break;
    case GraphicsControl::RtAlphaMode:                    current.rtAlphaMode = defaults.rtAlphaMode; break;
    case GraphicsControl::RtAlphaMissKeep:                current.rtAlphaMissKeep = defaults.rtAlphaMissKeep; break;
    case GraphicsControl::RtWindBlas:                     current.rtWindBlas = defaults.rtWindBlas; break;
    case GraphicsControl::RtWindBlasRadius:               current.rtWindBlasRadius = defaults.rtWindBlasRadius; break;
    case GraphicsControl::RenderScale:                    current.renderScale = defaults.renderScale; break;
    case GraphicsControl::LodEnabled:                     current.lodEnabled = defaults.lodEnabled; break;
    case GraphicsControl::LodBound0:                      current.lodBound0 = defaults.lodBound0; break;
    case GraphicsControl::LodBound1:                      current.lodBound1 = defaults.lodBound1; break;
    case GraphicsControl::LodBound2:                      current.lodBound2 = defaults.lodBound2; break;
    case GraphicsControl::LodFadeBand:                    current.lodFadeBand = defaults.lodFadeBand; break;
    case GraphicsControl::ChunkLodDistance:               current.chunkLodDistance = defaults.chunkLodDistance; break;
    case GraphicsControl::ChunkLodFactor:                 current.chunkLodFactor = defaults.chunkLodFactor; break;
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
    case GraphicsControl::ContactEnabled:                 current.contactEnabled = defaults.contactEnabled; break;
    case GraphicsControl::ContactLocalMode:               current.contactLocalMode = defaults.contactLocalMode; break;
    case GraphicsControl::ContactTemporal:                current.contactTemporalDither = defaults.contactTemporalDither; break;
    case GraphicsControl::ContactLengthWorldSpace:        current.contactLengthInWorldSpace = defaults.contactLengthInWorldSpace; break;
    case GraphicsControl::ContactLength:                  current.contactLength = defaults.contactLength; break;
    case GraphicsControl::ContactIntensity:               current.contactIntensity = defaults.contactIntensity; break;
    case GraphicsControl::ContactSteps:                   current.contactSteps = defaults.contactSteps; break;
    case GraphicsControl::ContactThickness:               current.contactMaxThickness = defaults.contactMaxThickness; break;
    case GraphicsControl::ContactNormalOffset:            current.contactNormalOffset = defaults.contactNormalOffset; break;
    case GraphicsControl::ContactGrazingFade:              current.contactGrazingFade = defaults.contactGrazingFade; break;
    case GraphicsControl::ContactMinDistance:             current.contactMinDistance = defaults.contactMinDistance; break;
    case GraphicsControl::ContactMaxDistance:             current.contactMaxDistance = defaults.contactMaxDistance; break;
    case GraphicsControl::ContactFadeBand:                current.contactFadeBand = defaults.contactFadeBand; break;
    case GraphicsControl::GiIndirectShadows:              current.giIndirectShadows = defaults.giIndirectShadows; break;
    case GraphicsControl::IndirectGBuffer:                 current.indirectGBuffer = defaults.indirectGBuffer; break;
    case GraphicsControl::GbufferHzb:                     current.gbufferHzbCull = defaults.gbufferHzbCull; break;
    case GraphicsControl::FogGridPixels:                  current.fogGridPixels = defaults.fogGridPixels; break;
    case GraphicsControl::FogGridZ:                       current.fogGridZ = defaults.fogGridZ; break;
    case GraphicsControl::ShadowLodBias:                  current.shadowLodBias = defaults.shadowLodBias; break;
    case GraphicsControl::ShadowLodBiasNearTier:          current.shadowLodBiasNearTier = defaults.shadowLodBiasNearTier; break;
    case GraphicsControl::ShadowLodTierStride:            current.shadowLodTierStride = defaults.shadowLodTierStride; break;
    case GraphicsControl::VsmRefDistance:                 current.vsmLodRefDistance = defaults.vsmLodRefDistance; break;
    case GraphicsControl::VsmRequestDownscale:            current.vsmRequestDownscale = defaults.vsmRequestDownscale; break;
    case GraphicsControl::VsmLru:                         current.vsmLruFrames = defaults.vsmLruFrames; break;
    case GraphicsControl::VsmClipmapBaseExtent:           current.vsmClipmapBaseExtent = defaults.vsmClipmapBaseExtent; break;
    case GraphicsControl::VsmClipmapBlend:                current.vsmClipmapBlend = defaults.vsmClipmapBlend; break;
    case GraphicsControl::VsmClipmapBlendWidth:           current.vsmClipmapBlendWidth = defaults.vsmClipmapBlendWidth; break;
    case GraphicsControl::VsmSmrtEnabled:
    case GraphicsControl::VsmSmrtRays:                    current.smrtRayCount = defaults.smrtRayCount; break;
    case GraphicsControl::VsmSmrtSamples:                 current.smrtSamplesPerRay = defaults.smrtSamplesPerRay; break;
    case GraphicsControl::VsmSmrtTemporal:                current.smrtTemporalDither = defaults.smrtTemporalDither; break;
    case GraphicsControl::VsmSmrtAdaptive:                current.smrtAdaptiveRayCount = defaults.smrtAdaptiveRayCount; break;
    case GraphicsControl::VsmSmrtMargin:                  current.smrtLevelMargin = defaults.smrtLevelMargin; break;
    case GraphicsControl::VsmSmrtSunAngle:                current.smrtSunAngleDeg = defaults.smrtSunAngleDeg; break;
    case GraphicsControl::VsmSmrtTexelDither:             current.smrtTexelDitherScale = defaults.smrtTexelDitherScale; break;
    case GraphicsControl::VsmSmrtRayLength:               current.smrtRayLengthScale = defaults.smrtRayLengthScale; break;
    case GraphicsControl::VsmDepthBias:                   current.vsmClipmapDepthBias = defaults.vsmClipmapDepthBias; break;
    case GraphicsControl::VsmDepthBiasDecay:              current.vsmClipmapDepthBiasDecay = defaults.vsmClipmapDepthBiasDecay; break;
    case GraphicsControl::VsmDepthBiasFloor:              current.vsmClipmapDepthBiasFloorTexels = defaults.vsmClipmapDepthBiasFloorTexels; break;
    case GraphicsControl::VsmNormalBias:                  current.vsmClipmapNormalBias = defaults.vsmClipmapNormalBias; break;
    case GraphicsControl::VsmLocalLateralBias:            current.vsmLocalLateralTexels = defaults.vsmLocalLateralTexels; break;
    case GraphicsControl::VsmLocalDepthPush:              current.vsmLocalDepthPushTexels = defaults.vsmLocalDepthPushTexels; break;
    case GraphicsControl::VsmResidentOnly:                current.vsmResidentOnly = defaults.vsmResidentOnly; break;
    case GraphicsControl::VsmSingleDraw:                  current.vsmSingleDraw = defaults.vsmSingleDraw; break;
    case GraphicsControl::VsmHzbCull:                     current.vsmHzbCull = defaults.vsmHzbCull; break;
    case GraphicsControl::VsmPageCaching:                 current.vsmPageCaching = defaults.vsmPageCaching; break;
    case GraphicsControl::VsmWindAnimateMaxLevel:         current.vsmWindAnimateMaxLevel = defaults.vsmWindAnimateMaxLevel; break;
    }

    Sanitize(current);
    const auto value = static_cast<unsigned>(control);
    if (value <= static_cast<unsigned>(GraphicsControl::RenderScale))
    {
        ApplyRender(current, renderer, settings);
    }
    else if (value <= static_cast<unsigned>(GraphicsControl::ChunkLodFactor))
    {
        ApplyLod(current);
    }
    else if (value >= static_cast<unsigned>(GraphicsControl::CsmMaxDistance) &&
             value <= static_cast<unsigned>(GraphicsControl::CsmOverBlur))
    {
        scene.CascadeConfig() = current.csm;
    }
    else if (value >= static_cast<unsigned>(GraphicsControl::ContactEnabled) &&
             value <= static_cast<unsigned>(GraphicsControl::ContactFadeBand))
    {
        ApplyContact(current);
    }
    else
    {
        ApplyVsm(current);
    }
    return SaveCurrent(renderer, scene, settings);
}

bool GraphicsSettingsManager::ResetRender(Renderer& renderer, const Scene& scene,
                                          SceneRenderSettings& settings)
{
    ApplyRender(GraphicsSettingsSnapshot{}, renderer, settings);
    return SaveCurrent(renderer, scene, settings);
}

bool GraphicsSettingsManager::ResetLod(Renderer& renderer, const Scene& scene,
                                       const SceneRenderSettings& settings)
{
    ApplyLod(GraphicsSettingsSnapshot{});
    return SaveCurrent(renderer, scene, settings);
}

bool GraphicsSettingsManager::ResetCsm(Renderer& renderer, Scene& scene,
                                       const SceneRenderSettings& settings)
{
    scene.CascadeConfig() = CascadeShadowConfig{};
    projectCsm_ = scene.CascadeConfig();
    return SaveCurrent(renderer, scene, settings);
}

bool GraphicsSettingsManager::ResetContactShadows(Renderer& renderer, const Scene& scene,
                                                   const SceneRenderSettings& settings)
{
    ApplyContact(GraphicsSettingsSnapshot{});
    return SaveCurrent(renderer, scene, settings);
}

bool GraphicsSettingsManager::ResetVsm(Renderer& renderer, const Scene& scene,
                                       const SceneRenderSettings& settings)
{
    ApplyVsm(GraphicsSettingsSnapshot{});
    return SaveCurrent(renderer, scene, settings);
}
