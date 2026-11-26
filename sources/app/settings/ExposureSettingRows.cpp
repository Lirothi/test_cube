#include "app/scene/Scene.h"
#include "app/settings/SettingContext.h"
#include "core/settings/SettingsRegistry.h"
#include "rendering/core/PhotographicSettings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// --set / --sweep / MCP set_settings names exposure.* and color.* (the camera's exposure, the colour pipeline)
// (architecture plan R2.2). Each row is the branch App.cpp's ApplySweepValue had, its clamp included, and the reader
// ReadSweepValue had (R2.2 gave every simple name one first, so --settings-dump compared the move); a row without a
// reader had none.
namespace settings
{
namespace
{
const SettingDesc kExposureRows[] = {
    // REMOVED (P16.2): `light.legacySplit`, the P4 equivalence probe. It claimed that putting
    // the factor back on the retired whole-scene multiplier reproduces the migrated image
    // exactly, and MEASURED on wind_test it does not -- 27.6 meanabs against a 0.81 noise floor,
    // the frame 27/255 brighter. The knob was not broken by P16.2; it was invalidated by F8 and
    // F9 and nobody re-ran it. `lighting_cs` still ends in `color * exposure + skySpecular`, so
    // that multiplier now scales the SKY IRRADIANCE FILL (which the sun's intensity does not
    // touch) and skips the sky specular (added after it) -- two terms that did not exist when
    // the equivalence was true. A probe asserting an invariant that no longer holds is worse
    // than no probe, so it is gone rather than caveated.
    //
    // The P16.2 migration has its own equivalence check and it needs no code: run the level
    // once as authored and once with `--set=light.sunIlluminanceLux:<the level's own value>`.
    // Identical frames mean the new key and the legacy fold arrive at the same light.
    //
    // With this gone, `DirectionalLight::exposure_` can no longer be set to anything but 1.0,
    // so the trailing multiply in the shaders is provably an identity. Retiring that plumbing
    // is a separate change; it touches the ocean, glass and RT reflection constant buffers.
    TC_SETTING("exposure.lowPercentile", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().lowPercentile = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().lowPercentile); return true; }),
    TC_SETTING("exposure.highPercentile", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().highPercentile = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().highPercentile); return true; }),
    TC_SETTING("exposure.compensationEv", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().compensationEv = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().compensationEv); return true; }),
    // P16.13: manual mode has its own trim; the auto key above no longer reaches it.
    TC_SETTING("exposure.manualCompensationEv", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().manualCompensationEv = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().manualCompensationEv); return true; }),
    // P16.6: the camera. `exposure.manualEv100` is kept because every measurement recipe in
    // the plan spells it that way -- it back-solves the aperture, so it still means exactly what
    // it always meant.
    TC_SETTING("exposure.aperture", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().apertureFStop = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().apertureFStop); return true; }),
    TC_SETTING("exposure.shutter", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().shutterSpeedSec = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().shutterSpeedSec); return true; }),
    TC_SETTING("exposure.iso", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().isoSensitivity = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().isoSensitivity); return true; }),
    TC_SETTING("exposure.manualEv100", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().apertureFStop = render::ApertureFromEv100(value, c.scene.CameraExposureRef().shutterSpeedSec, c.scene.CameraExposureRef().isoSensitivity); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().apertureFStop); return true; }),
    TC_SETTING("exposure.minEv100", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().minEv100 = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().minEv100); return true; }),
    TC_SETTING("exposure.maxEv100", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().maxEv100 = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().maxEv100); return true; }),
    TC_SETTING("exposure.speedUp", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().speedUp = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().speedUp); return true; }),
    TC_SETTING("exposure.meterMaskStrength", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().meterMaskStrength = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().meterMaskStrength); return true; }),
    TC_SETTING("exposure.meterMaskInnerRadius", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().meterMaskInnerRadius = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().meterMaskInnerRadius); return true; }),
    TC_SETTING("exposure.meterMaskOuterRadius", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().meterMaskOuterRadius = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().meterMaskOuterRadius); return true; }),
    TC_SETTING("exposure.meterMaskSkyBias", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().meterMaskSkyBias = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().meterMaskSkyBias); return true; }),
    TC_SETTING("exposure.speedDown", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().speedDown = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().speedDown); return true; }),
    TC_SETTING("exposure.adaptationStartDistance", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().adaptationStartDistance = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().adaptationStartDistance); return true; }),
    TC_SETTING("exposure.blackBucketInfluence", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().blackBucketInfluence = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().blackBucketInfluence); return true; }),
    TC_SETTING("exposure.enabled", Bool, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().enabled = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().enabled); return true; }),
    TC_SETTING("exposure.autoExposure", Bool, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().autoExposure = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().autoExposure); return true; }),
    // 0 legacy, 1 AgX, 2 the film curve (render::ToneCurve). Any non-zero value used to mean AgX, so the film
    // curve every level ships with could not be set back after an A/B.
    TC_SETTING("color.toneCurve", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.ColorPipelineRef().toneCurve = value >= 1.5f ? render::ToneCurve::Filmic : value >= 0.5f ? render::ToneCurve::AgX : render::ToneCurve::LegacyAces; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.ColorPipelineRef().toneCurve); return true; }),
    TC_SETTING("color.ueGamut", Bool, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.ColorPipelineRef().ueGamut = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.ColorPipelineRef().ueGamut); return true; }),
    TC_SETTING("color.expandGamut", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.ColorPipelineRef().expandGamut = std::clamp(value, 0.0f, 4.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.ColorPipelineRef().expandGamut); return true; }),
    TC_SETTING("color.blueCorrection", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.ColorPipelineRef().blueCorrection = std::clamp(value, 0.0f, 1.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.ColorPipelineRef().blueCorrection); return true; }),
    TC_SETTING("color.vignette", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.ColorPipelineRef().vignetteIntensity = std::clamp(value, 0.0f, 1.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.ColorPipelineRef().vignetteIntensity); return true; }),
    TC_SETTING("color.agxSlope", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.ColorPipelineRef().agxSlope = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.ColorPipelineRef().agxSlope); return true; }),
    TC_SETTING("color.agxPower", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.ColorPipelineRef().agxPower = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.ColorPipelineRef().agxPower); return true; }),
    TC_SETTING("color.agxSaturation", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.ColorPipelineRef().agxSaturation = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.ColorPipelineRef().agxSaturation); return true; }),
    TC_SETTING("color.gradeSaturation", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.ColorPipelineRef().gradeSaturation = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.ColorPipelineRef().gradeSaturation); return true; }),
    TC_SETTING("color.gradeContrast", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.ColorPipelineRef().gradeContrast = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.ColorPipelineRef().gradeContrast); return true; }),
    TC_SETTING("color.gradeGamma", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.ColorPipelineRef().gradeGamma = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.ColorPipelineRef().gradeGamma); return true; }),
    TC_SETTING("color.gradeGain", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.ColorPipelineRef().gradeGain = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.ColorPipelineRef().gradeGain); return true; }),
    TC_SETTING("color.gradeOffset", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.ColorPipelineRef().gradeOffset = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.ColorPipelineRef().gradeOffset); return true; }),
    // P3B moved onto the camera. The `color.*` spellings are kept as aliases so older notes,
    // scripts and the plan's recorded sweeps keep resolving.
    TC_SETTING("exposure.localHighlightContrast", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().localHighlightContrast = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().localHighlightContrast); return true; }),
    // the same as exposure.localHighlightContrast (one branch answered both)
    TC_SETTING("color.localHighlightContrast", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().localHighlightContrast = value; return true; },
               nullptr),
    TC_SETTING("exposure.localShadowContrast", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().localShadowContrast = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().localShadowContrast); return true; }),
    // the same as exposure.localShadowContrast (one branch answered both)
    TC_SETTING("color.localShadowContrast", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().localShadowContrast = value; return true; },
               nullptr),
    TC_SETTING("exposure.localDetailStrength", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().localDetailStrength = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().localDetailStrength); return true; }),
    // the same as exposure.localDetailStrength (one branch answered both)
    TC_SETTING("color.localDetailStrength", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().localDetailStrength = value; return true; },
               nullptr),
    // E2
    TC_SETTING("exposure.nightVision", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().nightVision = std::clamp(value, 0.0f, 1.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().nightVision); return true; }),
    TC_SETTING("exposure.nightDesaturation", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().nightDesaturation = std::clamp(value, 0.0f, 1.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().nightDesaturation); return true; }),
    // Time-of-day E1: the compensation curve, 0 = none, 1 = the eye preset (the A/B of a night), 2 = UDS's,
    // 3 = UDS's with a steady night, 4 = UE's (no curve: one constant +0.5, the owner's UE PPV).
    TC_SETTING("exposure.curve", Bool, SettingTransient,
               [](SettingContext& c, float value)
               {
                   if (value >= 3.5f) { render::SetUeExposureCurve(c.scene.CameraExposureRef()); }
                   else if (value >= 2.5f) { render::SetUdsSteadyNightExposureCurve(c.scene.CameraExposureRef()); }
                   else if (value >= 1.5f) { render::SetUdsExposureCurve(c.scene.CameraExposureRef()); }
                   else if (value != 0.0f) { render::SetEyeExposureCurve(c.scene.CameraExposureRef()); }
                   else { c.scene.CameraExposureRef().exposureCurvePoints = 0u; }
                   return true;
               },
               nullptr),
    // ...and point by point, so a running editor's set_settings can shape one live:
    // `exposure.curvePoints` (0..12), `exposure.curveEv<i>` / `exposure.curveComp<i>` (i 0..11). The points must
    // stay in ascending EV, as the level's own are -- the caller's job, here as in the Inspector's rows.
    TC_SETTING("exposure.curvePoints", Int, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().exposureCurvePoints = static_cast<std::uint32_t>(std::clamp( value, 0.0f, static_cast<float>(render::CameraExposureSettings::kExposureCurveMaxPoints))); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().exposureCurvePoints); return true; }),
    // P3B bilateral base: 1 = the pre-grid blur-only base (the A/B arm), UE's default 0.6.
    TC_SETTING("exposure.localBlurredBlend", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().localBlurredBlend = value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().localBlurredBlend); return true; }),
    // UE's LocalExposureBlurredLuminanceKernelSizePercent (default 50).
    TC_SETTING("exposure.localBlurredKernelPercent", Float, SettingLevelOwned,
               [](SettingContext& c, float value) { c.scene.CameraExposureRef().localBlurredKernelPercent = std::clamp(value, 0.0f, 100.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.CameraExposureRef().localBlurredKernelPercent); return true; }),
    // Composite: both local contrast scales at once. The two are independent branches of the
    // same function (base above / below middle grey), so sweeping them together is the only way
    // to measure the base EXPANSION the plan's P3B target needs -- one knob alone can move only
    // one end of the histogram, and the reference wants both ends moved in the same shot.
    TC_SETTING("exposure.localContrast", Float, SettingTransient,
               [](SettingContext& c, float value)
               {
                   c.scene.CameraExposureRef().localHighlightContrast = value;
                   c.scene.CameraExposureRef().localShadowContrast = value;
                   return true;
               },
               nullptr),
    // the same as exposure.localContrast (one branch answered both)
    TC_SETTING("color.localContrast", Float, SettingTransient,
               [](SettingContext& c, float value)
               {
                   c.scene.CameraExposureRef().localHighlightContrast = value;
                   c.scene.CameraExposureRef().localShadowContrast = value;
                   return true;
               },
               nullptr),
};
TC_SETTINGS(kExposureRows);

// ...and the exposure curve point by point, so a running editor's set_settings can shape one live:
// `exposure.curveEv<i>` / `exposure.curveComp<i>` (i 0..11; `exposure.curvePoints` above sets how many). The points must
// stay in ascending EV, as the level's own are -- the caller's job, here as in the Inspector's rows. `key` is the index.
bool IsCurveIndex(std::string_view key)
{
    return (key.size() == 1u || key.size() == 2u) && std::all_of(key.begin(), key.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
}

template <float* (*Field)(SettingContext&)>
bool SetCurvePoint(SettingContext& c, std::string_view key, float value)
{
    const int i = std::stoi(std::string(key));
    if (i >= static_cast<int>(render::CameraExposureSettings::kExposureCurveMaxPoints)) { return false; }
    Field(c)[i] = std::clamp(value, -20.0f, 30.0f);
    return true;
}

template <float* (*Field)(SettingContext&)>
bool ReadCurvePoint(SettingContext& c, std::string_view key, float& out)
{
    const int i = std::stoi(std::string(key));
    if (i >= static_cast<int>(render::CameraExposureSettings::kExposureCurveMaxPoints)) { return false; }
    out = Field(c)[i];
    return true;
}

float* CurveEv(SettingContext& c) { return c.scene.CameraExposureRef().exposureCurveEv; }
float* CurveComp(SettingContext& c) { return c.scene.CameraExposureRef().exposureCurveComp; }

void CurveIndices(std::vector<std::string>& out)
{
    for (std::uint32_t i = 0; i < render::CameraExposureSettings::kExposureCurveMaxPoints; ++i) { out.push_back(std::to_string(i)); }
}

const SettingFamily kExposureFamilies[] = {
    TC_SETTING_FAMILY("exposure.curveEv", "exposure curve points", Float, SettingLevelOwned, &IsCurveIndex,
                      &SetCurvePoint<&CurveEv>, &ReadCurvePoint<&CurveEv>, &CurveIndices),
    TC_SETTING_FAMILY("exposure.curveComp", "exposure curve points", Float, SettingLevelOwned, &IsCurveIndex,
                      &SetCurvePoint<&CurveComp>, &ReadCurvePoint<&CurveComp>, &CurveIndices),
};
TC_SETTING_FAMILIES(kExposureFamilies);
}
}
