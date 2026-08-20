#pragma once

#include <algorithm>
#include <string>
#include <utility>

#include "rendering/core/PhotographicSettings.h"

#pragma warning(push)
#pragma warning(disable: 26819)
#include "third_party/json/json.hpp"
#pragma warning(pop)

// JSON round-trip for the photographic camera settings, kept in its own header for the same reason
// OceanRenderConfigJson is: Scene.h must not drag nlohmann into every translation unit that only
// wants the plain struct.
//
// Both the boot loader (JsonLevel) and the editor's live-edit path (EnvironmentRuntime) go through
// ApplyOverrides, so the defaults exist in exactly one place -- PhotographicSettings.h -- and a
// level that omits a field cannot disagree with a level that omits the whole section.
namespace render::PhotographicSettingsJson
{

inline void ApplyOverrides(const nlohmann::json& j, CameraExposureSettings& out)
{
    if (!j.is_object())
    {
        return;
    }
    out.enabled        = j.value("enabled",        out.enabled);
    out.autoExposure   = j.value("autoExposure",   out.autoExposure);
    out.compensationEv = j.value("compensationEv", out.compensationEv);
    out.manualCompensationEv = j.value("manualCompensationEv", out.manualCompensationEv);
    out.minEv100       = j.value("minEv100",       out.minEv100);
    out.maxEv100       = j.value("maxEv100",       out.maxEv100);
    out.lowPercentile  = j.value("lowPercentile",  out.lowPercentile);
    out.highPercentile = j.value("highPercentile", out.highPercentile);
    out.speedUp        = j.value("speedUp",        out.speedUp);
    out.speedDown      = j.value("speedDown",      out.speedDown);
    // P16.6: the camera is three settings; EV100 is derived from them. A level carrying the old
    // hand-solved `manualEv100` is MIGRATED rather than given a second control -- shutter and ISO
    // hold at whatever it authored (or the reference values) and the aperture is solved to
    // reproduce the EV exactly, so the frame does not move.
    out.apertureFStop   = j.value("apertureFStop",   out.apertureFStop);
    out.shutterSpeedSec = j.value("shutterSpeedSec", out.shutterSpeedSec);
    out.isoSensitivity  = j.value("isoSensitivity",  out.isoSensitivity);
    if (!j.contains("apertureFStop") && j.contains("manualEv100"))
    {
        out.apertureFStop = ApertureFromEv100(j.value("manualEv100", 0.0f),
                                              out.shutterSpeedSec, out.isoSensitivity);
    }
    out.meterMaskStrength    = j.value("meterMaskStrength",    out.meterMaskStrength);
    out.meterMaskInnerRadius = j.value("meterMaskInnerRadius", out.meterMaskInnerRadius);
    out.meterMaskOuterRadius = j.value("meterMaskOuterRadius", out.meterMaskOuterRadius);
    out.meterMaskSkyBias     = j.value("meterMaskSkyBias",     out.meterMaskSkyBias);
    out.adaptationStartDistance = j.value("adaptationStartDistance", out.adaptationStartDistance);
    out.blackBucketInfluence    = j.value("blackBucketInfluence",    out.blackBucketInfluence);

    // Clamp at the settings boundary only (plan section 6.2: no hidden clamps scattered through
    // shaders). An inverted or degenerate authored range would otherwise reach the P2 solve.
    if (out.maxEv100 < out.minEv100)
    {
        std::swap(out.minEv100, out.maxEv100);
    }
    out.lowPercentile  = std::clamp(out.lowPercentile,  0.0f, 1.0f);
    out.highPercentile = std::clamp(out.highPercentile, 0.0f, 1.0f);
    if (out.highPercentile < out.lowPercentile)
    {
        std::swap(out.lowPercentile, out.highPercentile);
    }
    out.speedUp   = std::max(out.speedUp,   0.0f);
    out.speedDown = std::max(out.speedDown, 0.0f);

    out.localHighlightContrast  = j.value("localHighlightContrast",  out.localHighlightContrast);
    out.localShadowContrast     = j.value("localShadowContrast",     out.localShadowContrast);
    out.localDetailStrength     = j.value("localDetailStrength",     out.localDetailStrength);
    out.localHighlightThreshold = j.value("localHighlightThreshold", out.localHighlightThreshold);
    out.localShadowThreshold    = j.value("localShadowThreshold",    out.localShadowThreshold);
    out.localHighlightContrast  = std::clamp(out.localHighlightContrast,  0.1f, 2.0f);
    out.localShadowContrast     = std::clamp(out.localShadowContrast,     0.1f, 2.0f);
    out.localDetailStrength     = std::clamp(out.localDetailStrength,     0.0f, 3.0f);
    out.localHighlightThreshold = std::clamp(out.localHighlightThreshold, 0.0f, 8.0f);
    out.localShadowThreshold    = std::clamp(out.localShadowThreshold,    0.0f, 8.0f);

    out.adaptationStartDistance = std::clamp(out.adaptationStartDistance, 0.01f, 20.0f);
    out.blackBucketInfluence    = std::clamp(out.blackBucketInfluence, 0.0f, 1.0f);
    out.meterMaskStrength = std::clamp(out.meterMaskStrength, 0.0f, 1.0f);
    out.meterMaskSkyBias  = std::clamp(out.meterMaskSkyBias,  0.0f, 1.0f);
    out.meterMaskInnerRadius = std::clamp(out.meterMaskInnerRadius, 0.0f, 2.0f);
    out.meterMaskOuterRadius = std::clamp(out.meterMaskOuterRadius, 0.0f, 2.0f);
    // An inverted pair would make smoothstep run backwards and INVERT the mask, weighting the
    // frame edges instead of its centre. Nudge rather than swap: which one the author dragged is
    // information, and swapping silently would hide the mistake.
    if (out.meterMaskOuterRadius <= out.meterMaskInnerRadius)
    {
        out.meterMaskOuterRadius = out.meterMaskInnerRadius + 0.01f;
    }
}

// P3B fields moved from `colorPipeline` to `cameraExposure` (they are an exposure operation, not a
// grade). Levels written before the move still carry them in the old block, so JsonLevel and the
// editor runtime call this AFTER parsing both sections: anything still sitting in `colorPipeline`
// is lifted onto the camera. Absent keys change nothing, so a converted level is unaffected and
// re-saving writes them in the new place.
inline void MigrateLegacyLocalExposure(const nlohmann::json& colorPipelineJson,
                                       const nlohmann::json& cameraJson,
                                       CameraExposureSettings& camera)
{
    if (!colorPipelineJson.is_object())
    {
        return;
    }
    // ONLY lift a key the camera block does not already own. Lifting unconditionally means the
    // stale `colorPipeline` copy overwrites whatever was just edited on the camera -- which is
    // exactly what happened: every inspector edit was stomped back by the old value and the
    // control looked dead.
    const auto lift = [&](const char* key, float& dst)
    {
        if (cameraJson.is_object() && cameraJson.contains(key)) { return; }
        const auto it = colorPipelineJson.find(key);
        if (it != colorPipelineJson.end() && it->is_number()) { dst = it->get<float>(); }
    };
    lift("localHighlightContrast",  camera.localHighlightContrast);
    lift("localShadowContrast",     camera.localShadowContrast);
    lift("localDetailStrength",     camera.localDetailStrength);
    lift("localHighlightThreshold", camera.localHighlightThreshold);
    lift("localShadowThreshold",    camera.localShadowThreshold);
}

// P3. The tone curve serialises as a NAME, not an index: a level that says "agx" keeps meaning
// what it says if the enum ever gains a member, and it is readable in a diff.
inline void ApplyOverrides(const nlohmann::json& j, ColorPipelineSettings& out)
{
    if (!j.is_object())
    {
        return;
    }
    if (const auto it = j.find("toneCurve"); it != j.end() && it->is_string())
    {
        const std::string name = it->get<std::string>();
        if (name == "legacy" || name == "aces") { out.toneCurve = ToneCurve::LegacyAces; }
        else if (name == "agx")                 { out.toneCurve = ToneCurve::AgX; }
        else if (name == "filmic" || name == "film") { out.toneCurve = ToneCurve::Filmic; }
    }
    out.agxSlope      = j.value("agxSlope",      out.agxSlope);
    out.agxPower      = j.value("agxPower",      out.agxPower);
    out.agxSaturation = j.value("agxSaturation", out.agxSaturation);
    out.gradeSaturation = j.value("gradeSaturation", out.gradeSaturation);
    out.gradeContrast   = j.value("gradeContrast",   out.gradeContrast);
    out.gradeGamma      = j.value("gradeGamma",      out.gradeGamma);
    out.gradeGain       = j.value("gradeGain",       out.gradeGain);
    out.gradeOffset     = j.value("gradeOffset",     out.gradeOffset);
    out.filmSlope     = j.value("filmSlope",     out.filmSlope);
    out.filmToe       = j.value("filmToe",       out.filmToe);
    out.filmShoulder  = j.value("filmShoulder",  out.filmShoulder);
    out.filmBlackClip = j.value("filmBlackClip", out.filmBlackClip);
    out.filmWhiteClip = j.value("filmWhiteClip", out.filmWhiteClip);

    out.agxSlope      = std::clamp(out.agxSlope,      0.0f, 4.0f);
    out.agxPower      = std::clamp(out.agxPower,      0.1f, 4.0f);
    out.agxSaturation = std::clamp(out.agxSaturation, 0.0f, 4.0f);
    out.gradeSaturation = std::clamp(out.gradeSaturation, 0.0f, 4.0f);
    out.gradeContrast   = std::clamp(out.gradeContrast,   0.1f, 4.0f);
    out.gradeGamma      = std::clamp(out.gradeGamma,      0.1f, 4.0f);
    out.gradeGain       = std::clamp(out.gradeGain,       0.0f, 4.0f);
    out.gradeOffset     = std::clamp(out.gradeOffset,    -1.0f, 1.0f);
    out.filmSlope     = std::clamp(out.filmSlope,     0.1f, 2.0f);
    out.filmToe       = std::clamp(out.filmToe,       0.0f, 1.0f);
    out.filmShoulder  = std::clamp(out.filmShoulder,  0.0f, 1.0f);
    out.filmBlackClip = std::clamp(out.filmBlackClip, 0.0f, 1.0f);
    out.filmWhiteClip = std::clamp(out.filmWhiteClip, 0.0f, 1.0f);
}

inline nlohmann::json ToJson(const ColorPipelineSettings& s)
{
    return nlohmann::json{
        { "toneCurve",     s.toneCurve == ToneCurve::AgX ? "agx"
                           : (s.toneCurve == ToneCurve::Filmic ? "filmic" : "legacy") },
        { "agxSlope",      s.agxSlope },
        { "agxPower",      s.agxPower },
        { "agxSaturation", s.agxSaturation },
        { "gradeSaturation", s.gradeSaturation },
        { "gradeContrast",   s.gradeContrast },
        { "gradeGamma",      s.gradeGamma },
        { "gradeGain",       s.gradeGain },
        { "gradeOffset",     s.gradeOffset },
        { "filmSlope",     s.filmSlope },
        { "filmToe",       s.filmToe },
        { "filmShoulder",  s.filmShoulder },
        { "filmBlackClip", s.filmBlackClip },
        { "filmWhiteClip", s.filmWhiteClip },
    };
}

inline nlohmann::json ToJson(const CameraExposureSettings& s)
{
    return nlohmann::json{
        { "enabled",        s.enabled },
        { "autoExposure",   s.autoExposure },
        { "compensationEv", s.compensationEv },
        { "manualCompensationEv", s.manualCompensationEv },
        { "localHighlightContrast",  s.localHighlightContrast },
        { "localShadowContrast",     s.localShadowContrast },
        { "localDetailStrength",     s.localDetailStrength },
        { "localHighlightThreshold", s.localHighlightThreshold },
        { "localShadowThreshold",    s.localShadowThreshold },
        { "minEv100",       s.minEv100 },
        { "maxEv100",       s.maxEv100 },
        { "lowPercentile",  s.lowPercentile },
        { "highPercentile", s.highPercentile },
        { "speedUp",        s.speedUp },
        { "speedDown",      s.speedDown },
        { "apertureFStop",   s.apertureFStop },
        { "shutterSpeedSec", s.shutterSpeedSec },
        { "isoSensitivity",  s.isoSensitivity },
        { "meterMaskStrength",    s.meterMaskStrength },
        { "meterMaskInnerRadius", s.meterMaskInnerRadius },
        { "meterMaskOuterRadius", s.meterMaskOuterRadius },
        { "meterMaskSkyBias",     s.meterMaskSkyBias },
        { "adaptationStartDistance", s.adaptationStartDistance },
        { "blackBucketInfluence",    s.blackBucketInfluence },
    };
}

} // namespace render::PhotographicSettingsJson
