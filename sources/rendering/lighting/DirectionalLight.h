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
    // A REPLACEMENT light (the editor and the level loader build a fresh object and set its direction
    // once) restarts its version at 1, so a real direction change could leave the version equal to
    // the cache's -- the SDSM partition cache and the VSM stillness test key on it. The scene carries
    // the count forward: the previous light's version, +1 when the direction differs.
    void ContinueTransformVersion(const DirectionalLight& previous)
    {
        const bool moved = direction_.x != previous.direction_.x || direction_.y != previous.direction_.y ||
                           direction_.z != previous.direction_.z;
        transformVersion_ = previous.transformVersion_ + (moved ? 1u : 0u);
    }

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
    // THE SUN BEFORE THE ATMOSPHERE. This is what the sky's own LUTs integrate -- they compute the
    // extinction themselves, so handing them an already-attenuated sun would count the atmosphere
    // twice and the sky would go black as the sun set. UE keep the same split:
    // GetOuterSpaceIlluminance() is their GetColor(), and the transmittance is applied only where
    // light reaches a SURFACE (LightGridInjection.cpp:1064, SceneRendering.cpp:1477).
    Math::float3 GetOuterSpaceIlluminance() const
    {
        const Math::float3 t = GetTemperatureRgb();
        return Math::float3(color_.x * t.x, color_.y * t.y, color_.z * t.z) * sunIlluminanceLux_;
    }

    // THE SUN AS IT ARRIVES, for everything that lights a surface. Without the transmittance the sun
    // keeps its full outer-space illuminance at any elevation and only the N.L term dims it: at 2.8
    // degrees that still puts 4166 lux on horizontal ground, a bright overcast day's worth, and it
    // never reddens. That is what lit the ocean cyan under a sunset sky.
    Math::float3 GetEffectiveColor() const
    {
        return GetAboveCloudsColor() * cloudVisibility_;
    }

    // THE SUN ABOVE THE CLOUDS: through the atmosphere, not through the cloud deck. What lights the
    // clouds themselves (VolumetricCloud) -- an overcast deck's top is in full sun however dark the
    // ground under it is.
    Math::float3 GetAboveCloudsColor() const
    {
        const Math::float3 o = GetOuterSpaceIlluminance();
        return Math::float3(o.x * atmosphereTransmittance_.x,
                            o.y * atmosphereTransmittance_.y,
                            o.z * atmosphereTransmittance_.z);
    }

    // Set once per frame by the scene: how much of the sun gets through the cloud deck to the ground,
    // 1 under anything but a closing overcast (CloudSunGroundVisibility). The cloud shadow map does
    // this per point inside its 20 km; this is the overcast's verdict for every consumer of
    // GetEffectiveColor -- the lighting past the map, the fog's sun, the water's glints.
    void SetCloudVisibility(float v) { cloudVisibility_ = v; }
    float GetCloudVisibility() const { return cloudVisibility_; }

    // Any of the light arrives: something is left after the atmosphere, the horizon and the cloud deck.
    // False under a closed overcast, with the body wholly below the horizon, or with the light switched
    // off -- then nothing receives it and its shadow passes are skipped (FrameDecisions::sunShadowsOff);
    // the shadow readers branch on the same GetEffectiveColor() being zero, so they never read the
    // atlas nobody wrote.
    bool ReachesGround() const
    {
        const Math::float3 c = GetEffectiveColor();
        return c.x > 0.0f || c.y > 0.0f || c.z > 0.0f;
    }

    // Set once per frame by the renderer: the atmosphere's transmittance from the ground toward this
    // light, or WHITE when the level has no procedural atmosphere -- UE's own fallback
    // (SkyAtmosphereRendering.cpp:610). Mirrors their SetAtmosphereRelatedProperties.
    void SetAtmosphereTransmittance(const Math::float3& t) { atmosphereTransmittance_ = t; }
    const Math::float3& GetAtmosphereTransmittance() const { return atmosphereTransmittance_; }

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

    // P16.12: the GROUND's diffuse reflectance, which is the other half of the fill. The sky cube
    // answers "what arrives from above"; nothing answered "what comes back up off the floor",
    // because the cube's lower hemisphere is the HDRI's own ground and not this scene's. Measured
    // on `wind_test`: about 1.2 stops missing on shaded vertical surfaces over sunlit sand.
    //
    // It lives on the light rather than on the sky because it is driven by the ILLUMINANCE the
    // scene has, sun included -- see GroundBounceOverPi in lighting_cs.hlsl. Being a reflectance
    // and not a light, it cannot brighten a scene whose sun has set, and it needs no separate
    // strength knob: 0 switches the whole term off, 0.4 is dry sand, 0.2 grass, 0.06 water.
    const Math::float3& GetGroundAlbedo() const { return groundAlbedo_; }
    void SetGroundAlbedo(const Math::float3& v) { groundAlbedo_ = v; }

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

    // Plan A7 light shafts -- UE's per-light bloom properties (ULightComponent: bEnableLightShaftBloom,
    // BloomScale .2, BloomThreshold 0, BloomMaxBrightness 100, BloomTint white -- LightComponent.cpp:483-487;
    // LightShaftOcclusionDepthRange 100000 cm on the directional light). The threshold and the cap are in
    // the PRE-EXPOSED scene units, as UE's are. The defaults are not UE's:
    //   * enabled = true (UE off): the pass costs nothing with the sun behind the camera, and the shafts
    //     are the whole point of the plan;
    //   * scale 0.3, threshold 0.75, max brightness 0.35: ULTRA DYNAMIC SKY'S sun (read from its actor,
    //     2026-10-13; the owner: "do the sun like theirs, and their length"). The LOW CAP is what gives their
    //     look: the bright sky round the sun seeds wide but flat, so the blur carries the crowns' shadows
    //     far across the scene -- long rays over the sand -- and no single pixel dominates. Before, at
    //     threshold 2 and UE's cap of 100, the disc alone seeded at thousands: a starburst over the crowns
    //     (+100 sRGB at the sun, palm view) and rays that ended near it. The sky's lift stays small: +1..3 sRGB
    //     across the frame at a pinned exposure. (Threshold 0, UE's, lifted the whole sky x1.2 -- a wash.)
    bool GetLightShaftsEnabled() const { return lightShaftsEnabled_; }
    void SetLightShaftsEnabled(bool on) { lightShaftsEnabled_ = on; }
    float GetLightShaftBloomScale() const { return lightShaftBloomScale_; }
    void SetLightShaftBloomScale(float v) { lightShaftBloomScale_ = v; }
    float GetLightShaftBloomThreshold() const { return lightShaftBloomThreshold_; }
    void SetLightShaftBloomThreshold(float v) { lightShaftBloomThreshold_ = v; }
    float GetLightShaftBloomMaxBrightness() const { return lightShaftBloomMaxBrightness_; }
    void SetLightShaftBloomMaxBrightness(float v) { lightShaftBloomMaxBrightness_ = v; }
    const Math::float3& GetLightShaftBloomTint() const { return lightShaftBloomTint_; }
    void SetLightShaftBloomTint(const Math::float3& v) { lightShaftBloomTint_ = v; }
    float GetLightShaftOcclusionDepthRange() const { return lightShaftOcclusionDepthRange_; }
    // Not UE's: how slowly the shafts follow what the sun shines through -- the time constant
    // (seconds) of the temporal blend of their mask; 0 = this frame's mask as is. UE runs its TAA on
    // the mask instead; without either the shafts trembled with every leaf (light_shafts_cs.hlsl).
    float GetLightShaftSmoothingSec() const { return lightShaftSmoothingSec_; }
    void SetLightShaftSmoothingSec(float v) { lightShaftSmoothingSec_ = v; }
    void SetLightShaftOcclusionDepthRange(float metres) { lightShaftOcclusionDepthRange_ = metres; }
    // Not UE's, 0..2, 1 = UE's. UNDER 1: how far towards the light the radial blur reaches, as a fraction of
    // the way -- every blur pass's vector is scaled by it, so the passes still tile the ray; a ray ends
    // sooner past the gap it comes through (at 0.5 about 3x the gap's distance from the light, at 0.3 about
    // 1.7x). OVER 1 (owner, 2026-10-13: "I wanted them longer"): UE's rays end where their brightness has
    // fallen as 1/distance from the light (every pixel averages the whole way back to it); the apply lifts
    // that fall-off to 1/distance^(2 - length) -- 2 = rays that do not fade at all (light_shafts_cs.hlsl).
    float GetLightShaftLength() const { return lightShaftLength_; }
    void SetLightShaftLength(float v) { lightShaftLength_ = v; }
    // Not UE's: how much of the SUN GLARE (Bloom > Sun glare) joins the shafts' seed (owner, 2026-10-14:
    // "blur after the glare is laid"). UE's seed is the scene over the threshold; Ultra Dynamic Sky's bright
    // sky lets a wide disc of it through, and its rays run long through crowns, while our darker sky let
    // only the disc through. The glare is that bright sky round the sun as the picture shows it, so with
    // it in the seed the gaps in the crowns seed too. 0 = UE's seed, 1 = the glare as drawn. The sun only.
    float GetLightShaftGlareSeed() const { return lightShaftGlareSeed_; }
    void SetLightShaftGlareSeed(float v) { lightShaftGlareSeed_ = v; }

    // THE SUN'S ANGULAR SIZE -- UE's LightSourceAngle (DirectionalLightComponent.cpp:1024: 0.5357 deg,
    // "Angle of earth's sun"), an APEX angle in degrees, i.e. the disc's angular diameter; the renderer
    // takes half of it in radians (GetSunLightHalfApexAngleRadian, :630-633). One quantity with two
    // consumers, exactly as in UE: the drawn sun disc (skybox.hlsl) and the floor on the analytic
    // specular lobe (lighting_cs.hlsl, EvalBRDF), so the glint on smooth water is the disc's own size.
    //
    // It used to be a PROJECT graphics setting (`sunAngularSize` in graphics_settings.json, 0.01 rad of
    // radius, i.e. a sun 2.1x wider than the real one and 4.4x its area): a look property of this
    // level's sun sitting next to the DLSS mode. Review of part B, 2026-09-11.
    float GetLightSourceAngleDegrees() const { return lightSourceAngleDeg_; }
    void SetLightSourceAngleDegrees(float apexDegrees) { lightSourceAngleDeg_ = apexDegrees; }
    float GetSunHalfApexRadians() const { return 0.5f * lightSourceAngleDeg_ * 3.14159265358979323846f / 180.0f; }
    void SetSunHalfApexRadians(float radians) { lightSourceAngleDeg_ = 2.0f * radians * 180.0f / 3.14159265358979323846f; }

private:
    Math::float3 direction_;
    Math::float3 color_;
    float exposure_;
    float ambient_;
    float sunIlluminanceLux_ = 100000.0f; // P16.2; a sunny midday
    // White until a procedural atmosphere says otherwise, so an HDRI level is bit-identical.
    Math::float3 atmosphereTransmittance_{1.0f, 1.0f, 1.0f};
    float cloudVisibility_ = 1.0f; // 1 = no overcast deck between the sun and the ground
    float skyFillIntensity_ = 1.0f;   // F8; 1 = the irradiance cube taken at face value
    // P16.12; a mid-neutral ground. Not zero: the term is physically present in every outdoor
    // scene and shipping it off would leave the defect it fixes as the default. Not sand-coloured
    // either -- a tint belongs to the level that has sand in it.
    Math::float3 groundAlbedo_{ 0.25f, 0.25f, 0.25f };
    bool  useSunTemperature_ = false; // off = the authored colour is used as-is
    float sunTemperatureK_ = 6500.0f; // ~D65, i.e. a no-op hue once normalised
    std::uint32_t transformVersion_ = 0; // Step 11: bumped on SetDirection
    // Plan A7 light shafts (Ultra Dynamic Sky's sun, see the accessors).
    bool  lightShaftsEnabled_ = true;
    float lightShaftBloomScale_ = 0.3f;          // UDS's sun (UE 0.2), see the accessors
    float lightShaftBloomThreshold_ = 0.75f;     // UDS's sun (UE 0)
    float lightShaftBloomMaxBrightness_ = 0.35f; // UDS's sun (UE 100)
    Math::float3 lightShaftBloomTint_{ 1.0f, 1.0f, 1.0f };
    float lightShaftOcclusionDepthRange_ = 1000.0f; // metres (UE 100000 cm)
    float lightShaftSmoothingSec_ = 0.15f;
    float lightShaftLength_ = 1.0f; // UE's reach, see the accessors
    float lightShaftGlareSeed_ = 1.0f; // the glare as drawn joins the seed, see the accessors
    float lightSourceAngleDeg_ = 0.5357f; // UE's LightSourceAngle: the real sun's angular diameter
};

