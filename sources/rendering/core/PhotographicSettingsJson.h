#pragma once

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "rendering/core/PhotographicSettings.h"

#pragma warning(push)
#pragma warning(disable: 26819)
#include "third_party/json/json.hpp"
#pragma warning(pop)
#include "core/schema/KeyTable.h" // R9.2: the section's table of keys

// JSON round-trip for the photographic camera settings, kept in its own header for the same reason
// OceanRenderConfigJson is: Scene.h must not drag nlohmann into every translation unit that only
// wants the plain struct.
//
// Both the boot loader (JsonLevel) and the editor's live-edit path (EnvironmentRuntime) go through
// ApplyOverrides, so the defaults exist in exactly one place -- PhotographicSettings.h -- and a
// level that omits a field cannot disagree with a level that omits the whole section.
namespace render::PhotographicSettingsJson
{

// What the camera's keys mean together, after every key is read (the hand code did these between its reads; each
// touches only keys read before it, so they run here in the same order). P16.6: the camera is three settings; EV100 is
// derived from them -- a level carrying the old hand-solved `manualEv100` is MIGRATED rather than given a second
// control: shutter and ISO hold at whatever it authored (or the reference values) and the aperture is solved to
// reproduce the EV exactly, so the frame does not move.
inline void FinishCamera(const nlohmann::json& j, CameraExposureSettings& out)
{
    if (!j.contains("apertureFStop") && j.contains("manualEv100"))
    {
        out.apertureFStop = ApertureFromEv100(j.value("manualEv100", 0.0f),
                                              out.shutterSpeedSec, out.isoSensitivity);
    }
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
    out.localBlurredBlend       = std::clamp(out.localBlurredBlend,       0.0f, 1.0f);
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

// Architecture plan R9.2: the camera's table of keys (core/schema/KeyTable.h), FinishCamera after the last.
inline const schema::Table<CameraExposureSettings>& CameraKeys()
{
    using S = CameraExposureSettings;
    using J = nlohmann::json;
    static const schema::Table<S> table{ {
        schema::Field("enabled", &S::enabled),
        schema::Field("autoExposure", &S::autoExposure),
        schema::Field("compensationEv", &S::compensationEv),
        schema::Field("manualCompensationEv", &S::manualCompensationEv),
        schema::Field("minEv100", &S::minEv100),
        schema::Field("maxEv100", &S::maxEv100),
        schema::Field("lowPercentile", &S::lowPercentile),
        schema::Field("highPercentile", &S::highPercentile),
        schema::Field("speedUp", &S::speedUp),
        schema::Field("speedDown", &S::speedDown),
        schema::Field("apertureFStop", &S::apertureFStop),
        schema::Field("shutterSpeedSec", &S::shutterSpeedSec),
        schema::Field("isoSensitivity", &S::isoSensitivity),
        schema::Field("meterMaskStrength", &S::meterMaskStrength),
        schema::Field("meterMaskInnerRadius", &S::meterMaskInnerRadius),
        schema::Field("meterMaskOuterRadius", &S::meterMaskOuterRadius),
        schema::Field("meterMaskSkyBias", &S::meterMaskSkyBias),
        schema::Field("adaptationStartDistance", &S::adaptationStartDistance),
        schema::Field("blackBucketInfluence", &S::blackBucketInfluence),
        schema::Field("localHighlightContrast", &S::localHighlightContrast),
        schema::Field("localShadowContrast", &S::localShadowContrast),
        schema::Field("localDetailStrength", &S::localDetailStrength),
        schema::Field("localHighlightThreshold", &S::localHighlightThreshold),
        schema::Field("localShadowThreshold", &S::localShadowThreshold),
        schema::Field("localBlurredBlend", &S::localBlurredBlend),
        schema::Field("localBlurredKernelPercent", &S::localBlurredKernelPercent).Clamp(0.0f, 100.0f),
        schema::Field("nightVision", &S::nightVision).Clamp(0.0f, 1.0f), // time-of-day E2
        schema::Field("nightDesaturation", &S::nightDesaturation).Clamp(0.0f, 1.0f),
        schema::Custom<S>("nightTint",
            [](const J& j, S& out) {
                if (const auto it = j.find("nightTint"); it != j.end() && it->is_array() && it->size() >= 3)
                {
                    for (int c = 0; c < 3; ++c)
                    {
                        out.nightTint[c] = (*it)[c].is_number() ? std::max((*it)[c].get<float>(), 0.0f) : out.nightTint[c];
                    }
                }
            },
            [](J& j, const S& s) { j["nightTint"] = { s.nightTint[0], s.nightTint[1], s.nightTint[2] }; }),
        // Time-of-day E1: "exposureCurve": [[sceneEv100, compensationEv], ...] -- up to six, sorted here by EV
        // so a hand-edited file cannot hand the shader a curve that runs backwards.
        schema::Custom<S>("exposureCurve",
            [](const J& j, S& out) {
                if (const auto it = j.find("exposureCurve"); it != j.end() && it->is_array())
                {
                    std::vector<std::pair<float, float>> points;
                    for (const nlohmann::json& p : *it)
                    {
                        if (p.is_array() && p.size() >= 2 && p[0].is_number() && p[1].is_number())
                        {
                            points.emplace_back(std::clamp(p[0].get<float>(), -16.0f, 24.0f), std::clamp(p[1].get<float>(), -10.0f, 10.0f));
                        }
                    }
                    std::sort(points.begin(), points.end());
                    out.exposureCurvePoints = static_cast<std::uint32_t>(
                        std::min<std::size_t>(points.size(), CameraExposureSettings::kExposureCurveMaxPoints));
                    for (std::uint32_t i = 0; i < out.exposureCurvePoints; ++i)
                    {
                        out.exposureCurveEv[i] = points[i].first;
                        out.exposureCurveComp[i] = points[i].second;
                    }
                }
            },
            [](J& j, const S& s) {
                nlohmann::json curve = nlohmann::json::array();
                for (std::uint32_t i = 0; i < std::min(s.exposureCurvePoints, CameraExposureSettings::kExposureCurveMaxPoints); ++i)
                {
                    curve.push_back(nlohmann::json::array({ s.exposureCurveEv[i], s.exposureCurveComp[i] }));
                }
                j["exposureCurve"] = std::move(curve);
            }),
    }, &FinishCamera };
    return table;
}

inline void ApplyOverrides(const nlohmann::json& j, CameraExposureSettings& out)
{
    CameraKeys().Read(j, out);
}

// P3B fields moved from `colorPipeline` to `cameraExposure` (they are an exposure operation, not a grade). A level
// written before the move still carries them in the old block: levelmigrate::Migrate (app/levels/LevelMigration.cpp,
// R9.5) copies each one the camera does not own onto the camera before anything reads the level -- ONLY those: lifting
// unconditionally let the stale copy stomp every Inspector edit back, and the control looked dead.

// Architecture plan R9.2: the colour pipeline's table of keys (core/schema/KeyTable.h). The tone curve serialises as a
// NAME (P3); every number clamps into its range after it is read, as the hand code clamped the struct at its end.
inline const schema::Table<ColorPipelineSettings>& ColorPipelineKeys()
{
    using S = ColorPipelineSettings;
    static const schema::Table<S> table{ {
        schema::Custom<S>("toneCurve",
            [](const nlohmann::json& j, S& out) {
                if (const auto it = j.find("toneCurve"); it != j.end() && it->is_string())
                {
                    const std::string name = it->get<std::string>();
                    if (name == "legacy" || name == "aces") { out.toneCurve = ToneCurve::LegacyAces; }
                    else if (name == "agx")                 { out.toneCurve = ToneCurve::AgX; }
                    else if (name == "filmic" || name == "film") { out.toneCurve = ToneCurve::Filmic; }
                }
            },
            [](nlohmann::json& j, const S& s) {
                j["toneCurve"] = s.toneCurve == ToneCurve::AgX ? "agx" : (s.toneCurve == ToneCurve::Filmic ? "filmic" : "legacy");
            }),
        schema::Field("agxSlope", &S::agxSlope).Clamp(0.0f, 4.0f),
        schema::Field("agxPower", &S::agxPower).Clamp(0.1f, 4.0f),
        schema::Field("agxSaturation", &S::agxSaturation).Clamp(0.0f, 4.0f),
        schema::Field("gradeSaturation", &S::gradeSaturation).Clamp(0.0f, 4.0f),
        schema::Field("gradeContrast", &S::gradeContrast).Clamp(0.1f, 4.0f),
        schema::Field("gradeGamma", &S::gradeGamma).Clamp(0.1f, 4.0f),
        schema::Field("gradeGain", &S::gradeGain).Clamp(0.0f, 4.0f),
        schema::Field("gradeOffset", &S::gradeOffset).Clamp(-1.0f, 1.0f),
        schema::Field("filmSlope", &S::filmSlope).Clamp(0.1f, 2.0f),
        schema::Field("filmToe", &S::filmToe).Clamp(0.0f, 1.0f),
        schema::Field("filmShoulder", &S::filmShoulder).Clamp(0.0f, 1.0f),
        schema::Field("filmBlackClip", &S::filmBlackClip).Clamp(0.0f, 1.0f),
        schema::Field("filmWhiteClip", &S::filmWhiteClip).Clamp(0.0f, 1.0f),
        schema::Field("ueGamut", &S::ueGamut),
        schema::Field("expandGamut", &S::expandGamut).Clamp(0.0f, 4.0f),
        schema::Field("blueCorrection", &S::blueCorrection).Clamp(0.0f, 1.0f),
        schema::Field("vignetteIntensity", &S::vignetteIntensity).Clamp(0.0f, 1.0f),
    } };
    return table;
}

inline void ApplyOverrides(const nlohmann::json& j, ColorPipelineSettings& out)
{
    ColorPipelineKeys().Read(j, out);
}

inline nlohmann::json ToJson(const ColorPipelineSettings& s)
{
    return ColorPipelineKeys().Write(s);
}

inline nlohmann::json ToJson(const CameraExposureSettings& s)
{
    return CameraKeys().Write(s);
}

} // namespace render::PhotographicSettingsJson

