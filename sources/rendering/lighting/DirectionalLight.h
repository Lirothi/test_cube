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

    // The sun colour with its intensity already folded in -- what every shader should light with.
    // Kept as a derived accessor rather than baking it into color_ so the editor still round-trips
    // the authored colour and intensity separately.
    Math::float3 GetEffectiveColor() const { return color_ * sunIntensity_; }

    float GetSunIntensity() const { return sunIntensity_; }
    void SetSunIntensity(float intensity) { sunIntensity_ = intensity; }

    // Legacy whole-scene multiplier. After MigrateLegacyExposure this is 1.0 and multiplying by it
    // does nothing; it stays in the interface only so a missed consumer degrades to a no-op.
    float GetExposure() const;
    void SetExposure(float exposure);

    // Sky fill intensity. `GetAmbient` is the legacy name and still returns the value the OPAQUE
    // path should use, so existing call sites keep working.
    float GetAmbient() const;
    void SetAmbient(float ambient);

    // P4: the fill's own colour. Legacy lighting tinted ALL ambient by the sun colour
    // (`color = ambient * lightRgb` in lighting_cs), which is why shaded sand went orange at sunset
    // instead of sky-blue. Migration seeds this WITH the sun colour so nothing changes; untick
    // `ambientTintedBySun` to get a real sky-coloured fill, which is a deliberate retune.
    const Math::float3& GetAmbientColor() const { return ambientColor_; }
    void SetAmbientColor(const Math::float3& color) { ambientColor_ = color; }
    bool GetAmbientTintedBySun() const { return ambientTintedBySun_; }
    void SetAmbientTintedBySun(bool tinted) { ambientTintedBySun_ = tinted; }
    // The colour a sky fill should default to: daylight-sky hue, rescaled to the SAME luminance as
    // the sun colour handed in. Matching the luminance is the point -- unticking the tint is then a
    // pure hue change, so it cannot be mistaken for "the de-tint made everything dark", while still
    // visibly doing what the switch says it does. Shared with the inspector so the row it shows and
    // the value the runtime uses cannot drift apart.
    static Math::float3 DefaultSkyFillColor(const Math::float3& sunEffective)
    {
        const float lum = 0.2126f * sunEffective.x + 0.7152f * sunEffective.y + 0.0722f * sunEffective.z;
        const Math::float3 hue{ 0.45f, 0.66f, 1.0f }; // clear-sky ratio
        const float hueLum = 0.2126f * hue.x + 0.7152f * hue.y + 0.0722f * hue.z;
        const float k = (hueLum > 0.0f) ? (lum / hueLum) : 0.0f;
        return Math::float3(hue.x * k, hue.y * k, hue.z * k);
    }

    // What a shader should actually multiply the fill by, honouring the tint switch.
    // The tinted branch returns the EFFECTIVE sun colour, intensity included: legacy lighting_cs
    // computed the fill as `ambient * lightRgb` and then multiplied the lot by `exposure`, so the
    // fill did carry the sun's intensity. Returning the raw colour here would quietly darken every
    // shaded surface on any level whose legacy exposure was not 1.
    Math::float3 GetEffectiveAmbientColor() const
    {
        return ambientTintedBySun_ ? GetEffectiveColor() : ambientColor_;
    }

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

private:
    Math::float3 direction_;
    Math::float3 color_;
    float exposure_;
    float ambient_;
    float sunIntensity_ = 1.0f;
    Math::float3 ambientColor_{ 1.0f, 1.0f, 1.0f };
    bool ambientTintedBySun_ = true;  // legacy behaviour; false is the P4 retune
    std::uint32_t transformVersion_ = 0; // Step 11: bumped on SetDirection
};

