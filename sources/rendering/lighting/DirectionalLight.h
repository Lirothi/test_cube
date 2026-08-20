#pragma once

#include <cstdint>

#include "core/math/Math.h"

// P4 — the sun and the sky fill, with the camera taken out of them.
//
// The legacy model had ONE float named `exposure` that lighting_cs multiplied the entire lit result
// by (`color * exposure`, sun AND ambient), while the skybox carried its own, and the spot/point
// passes ignored it entirely. Once P1-P3 landed a real metering camera that knob stopped meaning
// what its name said: with auto-exposure on, MEASURED on wind_test, changing it barely moves the
// image's median at all (2.0 -> 4.0 moved it 0.2023 -> 0.2020) because the metering immediately
// cancels it. What it still moves is the RATIO between the things it scales (sun, ambient) and the
// things it does not (sky background, spot/point lights, emissive) -- p02 went 0.0322 -> 0.0423 over
// that same change. So it had quietly become a "scene versus sky" control wearing a camera's name.
//
// The replacement is two honest quantities:
//   - sunIntensity: how bright the sun is. Folded into the colour handed to every shader, so the
//     consumers that already did `sunColor * exposure` need no change at all.
//   - skyLightIntensity: how bright the sky fill is, independent of the sun.
//
// P16.2 renamed the first of those to `sunIlluminanceLux` and gave it a unit. See the accessor.
//
// GetExposure() is kept and returns 1.0 once migrated, so any consumer still multiplying by it is
// a no-op rather than a silent double-application. See MigrateLegacyExposure below for why the
// migration is lossless.
class DirectionalLight
{
public:
    DirectionalLight();

    const Math::float3& GetDirection() const;
    void SetDirection(const Math::float3& direction);

    // Rung 1 (Step 11) foundation: monotonic version bumped when the sun direction changes (it
    // drives the CSM projection); a directional-shadow cache compares it. No consumer yet.
    std::uint32_t GetTransformVersion() const { return transformVersion_; }

    const Math::float3& GetColor() const;
    void SetColor(const Math::float3& color);

    // Colour temperature, in Kelvin. OFF by default: the authored colour is used as-is, so no level
    // changes until someone asks for it.
    //
    // The locus is UE's `MaterialExpressionBlackBody` (MaterialTemplate.ush) -- the CIE 1960 UCS
    // rational fit, u/v -> xy -> XYZ -> linear sRGB -- but WITHOUT its `pow(0.0004*T, 4)` tail.
    // That tail is Stefan-Boltzmann radiant emittance: physically right for a black body, wrong for
    // a light's temperature control, where it would make 3000K a hundred times dimmer than 6500K
    // and the dial would read as a brightness slider that also happens to change hue. UE's own
    // light temperature (FLinearColor::MakeFromColorTemperature) leaves it out for the same reason.
    // The result is renormalised to unit luminance, so temperature moves HUE and nothing else --
    // brightness stays where sunIntensity put it.
    bool GetUseSunTemperature() const { return useSunTemperature_; }
    void SetUseSunTemperature(bool use) { useSunTemperature_ = use; }
    float GetSunTemperatureK() const { return sunTemperatureK_; }
    void SetSunTemperatureK(float kelvin) { sunTemperatureK_ = kelvin; }
    // Unit-luminance RGB for the current temperature; (1,1,1) when the control is off.
    Math::float3 GetTemperatureRgb() const;

    // The sun colour with its intensity already folded in -- what every shader should light with.
    // Kept as a derived accessor rather than baking it into color_ so the editor still round-trips
    // the authored colour and intensity separately.
    Math::float3 GetEffectiveColor() const
    {
        const Math::float3 t = GetTemperatureRgb();
        return Math::float3(color_.x * t.x, color_.y * t.y, color_.z * t.z) * sunIlluminanceLux_;
    }

    // P16.2 -- THE SUN'S ILLUMINANCE, IN LUX, measured perpendicular to the beam. 100,000 is a
    // sunny midday (Unreal's own default; 125,000 for full bright sun, ~20,000 for a heavy
    // overcast, ~1,000 for a very dark day, 0.25 for a full moon).
    //
    // It multiplies straight into the colour with NO conversion factor, because the engine's linear
    // unit is already photometric and always was. Two things pin it down and neither is new:
    //   * the shading has the right shape -- `diffBRDF` carries the 1/PI and the directional term is
    //     `diffBRDF * NdotL * lightRgb`, so `lightRgb` sits exactly where an illuminance goes and
    //     the product is a luminance;
    //   * the metering solves an EV100, and EV100 is DEFINED against cd/m2 by `L = 0.18 * 2^EV`.
    // So "one unit of light colour" has meant one lux since the exposure code landed. P16.2 only
    // writes that down and puts it in the field's name.
    //
    // Which is why the migration below is a no-op on the number: an existing level keeps its value
    // and its pixels, and simply learns that it had been authoring lux all along -- wind_test's sun
    // reads 2 lux, deep twilight, against a sky delivering thirteen. That gap is not a rounding
    // error to be papered over with a constant; it is the defect P16 exists to fix, and it belongs
    // on screen in a unit that can be argued with.
    float GetSunIlluminanceLux() const { return sunIlluminanceLux_; }
    void SetSunIlluminanceLux(float lux) { sunIlluminanceLux_ = lux; }

    // Legacy whole-scene multiplier. After MigrateLegacyExposure this is 1.0 and multiplying by it
    // does nothing; it stays in the interface only so a missed consumer degrades to a no-op.
    float GetExposure() const;
    void SetExposure(float exposure);

    // Sky fill intensity. `GetAmbient` is the legacy name and still returns the value the OPAQUE
    // path should use, so existing call sites keep working.
    float GetAmbient() const;
    void SetAmbient(float ambient);

    // The colour of the FLAT fallback fill, i.e. the sun colour with its intensity folded in.
    //
    // There used to be an `ambientTintedBySun` switch and an authored `ambientColor` beside it, so
    // a level could give its shadows a sky-blue fill instead of a sun-tinted one. F8 retired them:
    // once a sky supplies real irradiance, lighting_cs takes that branch and never reads this at
    // all, and the importer now produces the derivatives for every sky it converts. Keeping a pair
    // of controls that do nothing on any converted level -- greyed out or not -- is worse than not
    // having them, so they are gone. This remains for levels with no derivatives (and for no sky
    // at all), where it reproduces exactly what those levels always rendered.
    Math::float3 GetEffectiveAmbientColor() const { return GetEffectiveColor(); }

    // F8: strength of the SKY IRRADIANCE fill, used when the level's sky brought prefiltered
    // derivatives with it. Deliberately a SEPARATE field from `ambient`, not a reuse of it:
    // `ambient` means "this fraction of the sun colour bounces around", a number authored against a
    // completely different equation. Multiplying an absolute measured irradiance by a level's 0.05
    // buries the fill about twenty times too deep -- which is exactly what the first version of F8
    // did. 1 = the sky's own irradiance, physically what the cube says.
    float GetSkyFillIntensity() const { return skyFillIntensity_; }
    void SetSkyFillIntensity(float v) { skyFillIntensity_ = v; }

    // NOTE (P4, corrected 2026-08-16): there was a `unifiedSkyFill` switch here, on the premise that
    // the ocean's sky fill disagreed with the opaque one. That premise was WRONG, and measuring it
    // is what showed why. In `ocean_surface.hlsl` the `ambient` value reaches exactly one function,
    // `LitFoamColor`, where it is added to a hardcoded `kSkyColor` -- so it lights FOAM and nothing
    // else. The water surface receives no sky fill at all; its "ambient" is a constant baked into
    // the shader, plus reflection and subsurface. There is therefore nothing for a boolean to
    // reconcile, and the switch could only ever nudge the surf line (measured: 1557 pixels, mean
    // |delta| 0.004/255). Giving the water real sky irradiance is P5's job, not a flag's.

    // Fold a legacy `exposure` into the new fields WITHOUT changing a single pixel.
    //
    // Every consumer has the same shape -- lighting_cs computes
    //     (ambient*lightRgb + SUM brdf*lightRgb*shadow) * exposure
    // and the ocean, glass and RT reflection paths all light with `sunColor * exposure` -- so the
    // multiplier can be moved INTO the colour and out of the trailing multiply for an identical
    // product. Note what this means for the fill: because the opaque term is `ambient * lightRgb`,
    // it picks the factor up through the colour automatically, so `ambient_` must be left ALONE.
    // Scaling it here as well would apply the factor twice.
    void MigrateLegacyExposure(float legacyExposure);

    // P16.2: fold a pre-lux `sunIntensity` in. The number does not change -- see the accessor for
    // why there is nothing to convert -- so this exists to give the call site one honest name for
    // what it is doing rather than a bare assignment that hides a change of meaning.
    void MigrateLegacySunIntensity(float legacyIntensity);

private:
    Math::float3 direction_;
    Math::float3 color_;
    float exposure_;
    float ambient_;
    float sunIlluminanceLux_ = 100000.0f; // P16.2; a sunny midday
    float skyFillIntensity_ = 1.0f;   // F8; 1 = the irradiance cube taken at face value
    bool  useSunTemperature_ = false; // off = the authored colour is used as-is
    float sunTemperatureK_ = 6500.0f; // ~D65, i.e. a no-op hue once normalised
    std::uint32_t transformVersion_ = 0; // Step 11: bumped on SetDirection
};

