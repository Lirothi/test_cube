#pragma once

// T3 rollback switch (`--tod-legacy-light`): a level without a `timeOfDay` section lights with its
// `directionalLight` as authored instead of the converted time of day -- also the "before" of the
// conversion's same-frame check, in one binary.
inline bool g_timeOfDayConvertsDirectionalLight = true;

// Time-of-day plan T2 (docs/time_of_day_night_sky_plan.md): the LEVEL's clock, where its sun and moon
// ride, and how bright the moon is. The entity owns the scene's one directional light: every frame it
// places both bodies (rendering/lighting/Astronomy.h) and hands the light to whichever is up -- the sun
// while any of its disc is above the horizon, else the moon, else nobody (the sun, under the ground,
// lighting nothing). The sky always gets the SUN, so the twilight glow outlives the switch.
//
// The sun's own light properties (lux, colour, temperature, fill, ground albedo, shafts, angular size)
// are an ordinary DirectionalLight held beside this struct (Scene's time-of-day sun template), read
// from `timeOfDay.sun` with the very parser `directionalLight` uses -- so a converted level (T3) moves
// its old section across unchanged. The moon borrows the sun's scene-wide fields (ambient, sky fill,
// ground albedo, shafts) and brings its own brightness, colour and size, so the fill cannot jump when
// the light changes hands.
struct TimeOfDaySettings
{
    // The level has a `timeOfDay` section: the entity owns the directional light. Without it the level's
    // `directionalLight` is used as authored (until T3 converts the levels).
    bool present = false;

    // --- Clock ---------------------------------------------------------------------------------------
    float timeHours = 12.0f;       // local time, [0, 24)
    bool animate = false;          // the clock runs (off: the level is a photograph of one instant)
    float dayLengthMin = 30.0f;    // simple: real minutes from dawn to dusk (UDS's Day Length)
    float nightLengthMin = 15.0f;  // simple: real minutes from dusk to dawn (UDS's Night Length)
    float timeScale = 60.0f;       // real: game seconds per real second (60 = an hour a minute)

    // --- Where the bodies are ------------------------------------------------------------------------
    // false = SIMPLE (the default, UDS's): the sun rides a tilted circle from dawn to dusk, the moon its
    // own circle placed by the phase. true = REAL: date, latitude, longitude, UTC offset (NOAA sun,
    // Schlyter moon -- Astronomy.h).
    bool realPositions = false;
    // Simple sun path.
    float dawnHours = 6.0f;        // the sun crosses the horizon going up
    float duskHours = 18.0f;       // ... and going down
    float pathYawDeg = 180.0f;     // azimuth the path leans toward (180 = the noon sun stands south)
    float pathTiltDeg = 30.0f;     // the path's tilt from the zenith: the noon sun stands 90 - tilt up
    float heightOffset = 0.0f;     // lifts (+) or sinks (-) the whole path, as the sine of an angle
    float extendTwilight = 0.0f;   // 0..1: flattens the path near the horizon, dawn and dusk last longer
    // Simple moon.
    float moonPathYawDeg = 180.0f;
    float moonPathTiltDeg = 35.0f;
    float moonOrbitOffsetDeg = 0.0f; // turns the moon along its circle away from where the phase puts it
    float moonPhase = 0.5f;          // 0 new .. 0.5 full .. 1 new
    bool moonPhaseAuto = true;       // advances one cycle per 29.53 days while the clock runs
    // Real.
    int year = 2026;
    int month = 6;
    int day = 21;
    float latitudeDeg = -16.5f;      // Bora Bora: the atoll the shipped levels dress
    float longitudeDeg = -151.75f;   // positive EAST
    float utcOffsetHours = -10.0f;
    // Which world direction is north, turned clockwise seen from above (as azimuth runs); 0 = +Z.
    float northYawDeg = 0.0f;

    // --- The moon's light ----------------------------------------------------------------------------
    bool moonEnabled = true;
    // A FULL moon's illuminance outside the atmosphere at its mean distance. The sun's 127 klx times
    // the full moon's 14.0 magnitudes fainter (-12.74 against -26.74): 0.32 lux, ~0.27 at the ground
    // with the moon at the zenith. The phase law and (real) the distance scale it from there.
    float moonIlluminanceLux = 0.32f;
    // Moonlight is sunlight reddened by the regolith: the full moon's B-V is 0.92 against the sun's 0.65,
    // ~4950 K against 5780 K (Ballesteros) -- a shift of ~29 mired. What matters is that SHIFT, applied
    // to how this renderer draws the sun: white, at 6500 K. 154 + 29 mired = 5450 K, a white with a
    // touch of cream. (The first defaults were 4300 K, the moonlight AT THE GROUND with the air's
    // reddening the sky adds again, and then 4950 K, the shift applied to the real sun instead of to
    // ours: both drew a yellow disc, and the owner said rightly that moonlight is not yellow.) This is the
    // colour of the moon ITSELF as the SKY scatters it (the glow round it, its share of the night sky); the drawn
    // disc has its own colour (moonDiscColor), and what it lights is below.
    float moonTemperatureK = 5450.0f;
    // The colour the drawn DISC is seen in (owner, 2026-10-13: "the moon and its rays are still yellow"):
    // linear RGB at unit luminance, in place of the temperature's on the disc only -- its brightness stays
    // the light's (the light shafts grow out of it, then take the light's colour). White, as
    // Ultra Dynamic Sky draws its moon (its Moon_Color texture is a neutral grey): the 5450 K cream disc read
    // yellow against the blue night round it (sRGB 164/157/150 in a sky of 12/18/31, wind_test).
    float moonDiscColor[3] = {1.0f, 1.0f, 1.0f};
    // The GLOW round the disc (owner, 2026-10-13, with UDS's moon with its shafts off: "the halo is different,
    // and look at the length"): Ultra Dynamic Sky's, (1 - angle / radius)^2 x the peak -- its Moon material
    // function, live 0.05 of the moon's emissive (~16 % of the mean disc) out to 11.6 degrees. Its light shafts
    // seed on it and only widen it; without it ours seeded on the disc alone and drew the whole halo
    // themselves. The real moon's is the aureole of the haze in front of it. Peak: a fraction of the FULL
    // moon's mean disc radiance, seen as the disc is (Disc Adaptation), times the phase law; 0 = none.
    // 0.02, not UDS's ~0.16 of the disc: UDS's night sky is ~10x brighter on screen (sRGB 40/61/110 against
    // our 10/16/31 under the Eye curve), and what reads is the glow against the sky -- at 0.02 it is UDS's
    // ~3.6x the sky at the disc's edge (measured on the owner's UDS shots, 2026-10-14).
    float moonGlow = 0.02f;
    float moonGlowRadiusDeg = 11.6f;
    // The colour the moon LIGHTS the scene in (the ground light only). The owner (2026-10-13), shown the
    // physics -- a cream moonlight turned grey by the eye's rods: "a night must not be black and white,
    // there must be cold light". That is Ultra Dynamic Sky's night, and its Moon Light Colour is the
    // default: a beige beach lit by the warm physical moonlight stays beige (b/r 0.35 on wind_test), and
    // no tint the eye stage can afford turns it cold. Linear RGB used at unit luminance, so the lux above
    // stay physical and only the hue is the artist's.
    float moonLightColor[3] = {0.446f, 0.557f, 0.865f};
    float moonAngleDeg = 0.52f;      // angular diameter (mean): the drawn disc and the light's size
    float moonSoftAngleDeg = 0.0f;   // UE LightSourceSoftAngle of the moon light (DirectionalLight's), 0 = UE default
    // The eye's local adaptation to the drawn disc (N4), 0..1: 1 = the disc scaled into the display's range so
    // its maria and relief read and its edges blend (the eye sees the moon's face at night); 0 = the
    // physical disc a camera records under a night exposure, clipped flat white.
    float moonDiscAdaptation = 1.0f;
    // The light shafts from the moon (N4): they take over when the sun's are gone and the moon lights the
    // ground, with the MOON'S OWN bloom properties -- UE's per-light BloomScale / BloomThreshold /
    // BloomMaxBrightness / BloomTint, in Ultra Dynamic Sky's RELATION (read from its actor, 2026-10-13: Moon
    // Light Shaft Bloom Scale 0.4, Threshold 0.15, Max Brightness 0.15, Tint white). The low cap is the point:
    // UDS's 0.15 sits at the level of its moon glow (~0.19 on screen next to the disc), so the seed is the
    // glow AND the disc, wide and flat -- a soft halo and long soft rays through crowns, the shafts only
    // widening the glow (+40 %). Borrowing the sun's (threshold 2 -> 0.75, cap 100) seeded the disc alone,
    // and the blur's steps drew a BLOCKY halo of its copies; UDS's numbers as they are still seeded the disc
    // alone, because our night is ~10x darker on screen and the glow far under 0.15: a concentrated bright
    // halo (2026-10-14 grid). So the threshold sits just above our moonlit sky (~0.006 under the Eye curve)
    // and the cap at our glow's level next to the disc (~0.03), and the scale makes up the lower cap. A much
    // brighter night exposure lifts the sky over the threshold -- raise it then. Length, smoothing and depth
    // range stay the sun's.
    float moonLightShaftBloomScale = 1.0f;
    float moonLightShaftBloomThreshold = 0.008f;
    float moonLightShaftBloomMaxBrightness = 0.03f;
    // Their tint, in place of the sun's: it multiplies the moon's LIGHT colour (moonLightColor's hue) -- the
    // shafts are that light scattered by the air (grey shafts over dark crowns read olive).
    float moonLightShaftTint[3] = {1.0f, 1.0f, 1.0f};
    // The moon's glints on the water (N4), faded AGAIN by the phase law to this power: 0 = physics (the
    // glitter path follows the moon's light alone), 1 = the light's fade once more. Physically a thin
    // crescent's path is only ~3 stops dimmer than the full moon's on screen (its light is 6.6 stops
    // down, but the night's exposure rises 4), and beside a sliver of a moon it read as far too strong
    // (owner, 2026-10-13). 0.5: the full moon's path untouched, a quarter's 1.7 stops down, a crescent's 3.3.
    float moonGlintPhaseFalloff = 0.5f;
    // The ashen light on the moon's dark side -- sunlight off the Earth -- as a multiplier on its
    // physical estimate (~1.2e-4 of the full disc at new moon, falling with the Earth's own phase).
    float moonEarthshine = 1.0f;
    // The owner's manual multiplier on the night's light (1 = physics): the moon's light and the night
    // sky's own layers below (N2).
    float nightBrightness = 1.0f;

    // --- The night sky (plan N2) ---------------------------------------------------------------------
    // What a sky has without sun or moon, procedural (the owner's decision: no catalogue, no downloaded
    // maps): the AIRGLOW, the stars, the Milky Way. Physical by default -- a moonless sky far from any
    // town, in cd/m2 and lux -- and drawn wherever the sky is (its pixels, its captured picture the water
    // and the probes see, the clouds' light from below).
    bool nightSkyEnabled = true;
    // The natural night sky's own glow at the zenith, cd/m2: airglow and zodiacal light -- a dark site's
    // 21.8-22 mag/arcsec2 less the integrated starlight the stars below already carry. It grows toward the
    // horizon (van Rhijn: a layer ~90 km up seen edge-on, x6 at the horizon) before the air takes it back.
    float airglowZenith = 1.2e-4f;
    // Its hue (any scale: used at unit luminance). The 557.7 nm oxygen line makes it faintly green.
    float airglowColor[3] = {0.95f, 1.0f, 0.9f};
    // Multipliers on the stars (one per cell of a 6 x 256^2 sphere, magnitudes by the real counts: ~4500
    // brighter than 6) and on the Milky Way's diffuse band: 1 = physical.
    float starsBrightness = 1.0f;
    float milkyWayBrightness = 1.0f;
    float starTwinkle = 0.3f;      // 0..1 scintillation, stronger toward the horizon
    // SIMPLE mode's sidereal time at local midnight, hours: 18 = a southern winter's night with the
    // galaxy's core high at midnight. Real positions take it from the date and the longitude. Both use
    // latitudeDeg for the pole's height.
    float siderealAtMidnightHours = 18.0f;

    // --- Cost ----------------------------------------------------------------------------------------
    // The SCENE light turns only once the body has moved this far from where the light was last put
    // (every turn re-renders the sun's shadows and drops the shadow caches); the sky gets the exact
    // direction every frame. 0.05 degrees is 12 s of a real sun -- P1 measures it.
    float lightUpdateThresholdDeg = 0.05f;
};

// Time-of-day plan N1: the moon as the SKY sees it -- the atmosphere's second light (UE
// NUM_ATMOSPHERE_LIGHTS 2, light 1) and the drawn disc -- every frame, whoever lights the ground.
// Written by TimeOfDay, carried by SceneFrameData into SkyViewFrameData. `present` false = no moon
// in the sky at all (no time of day, or the moon switched off): the sky shaders skip light 1.
struct SkyMoonFrame
{
    bool present = false;
    float toward[3] = {0.0f, -1.0f, 0.0f}; // world, unit, TO the moon
    float illuminance[3] = {};              // outer space, RGB lux, as lit now (phase law, distance, night brightness)
    float fullIlluminance[3] = {};          // the same moon at full: the disc's brightness reference
    float halfApexRad = 0.0f;               // disc radius
    float phaseAngleRad = 0.0f;             // sun - moon - earth: 0 full, pi new
    // The disc is shaded by Lommel-Seeliger (no limb darkening, as the real full moon); this scales it so
    // the disc's integral follows the phase law the light uses (MoonPhaseBrightness / the LS phase integral).
    float litScale = 1.0f;
    float earthshine = 0.0f;                // dark side, as a fraction of the full disc (Earth's phase applied)
    bool lightsGround = false;              // the scene's light is the moon's now (N3: the clouds may take it)
    float discAdaptation = 1.0f;            // N4: the eye's adaptation to the drawn disc (moonDiscAdaptation)
    float glow = 0.0f;                      // N4: the glow's peak (moonGlow x the phase law)
    float glowRadiusRad = 0.0f;             // N4: and its radius
    // N4: the moon's light shafts -- its own UE bloom properties (moonLightShaftBloom*), their colour (the
    // light's hue x moonLightShaftTint). Bloom scale 0 = none.
    float lightShaftBloomScale = 0.0f;
    float lightShaftBloomThreshold = 0.15f;
    float lightShaftBloomMaxBrightness = 0.15f;
    float lightShaftTint[3] = {1.0f, 1.0f, 1.0f};
    // N4: the bright limb's position angle on the disc (radians, in the sky shader's disc frame: 0 = right,
    // pi/2 = up). Toward a VIRTUAL sun on the moon's own circle at the phase angle in Simple mode -- the
    // moon no longer moves with the phase, so the real sun cannot tell a waxing moon from a waning one --
    // and toward the real sun in Real mode.
    float limbAngle = 0.0f;
};

// Time-of-day plan N2: the night sky's layers as the sky draws them this frame. Written by TimeOfDay,
// carried by SceneFrameData to the skybox, the environment capture and the distant light. `present`
// false = nothing to draw (no time of day, the layers off, or a sun high enough that they cannot show).
struct NightSkyFrame
{
    bool present = false;
    // World direction (Y up) -> equatorial (x toward RA 0h, y toward RA 6h, z the north celestial pole),
    // row-major: the sky turns about the pole once a sidereal day.
    float worldToEquatorial[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float airglow[3] = {};       // zenith radiance, RGB cd/m2 (x nightBrightness)
    float stars = 0.0f;          // x physical (x nightBrightness)
    float milkyWay = 0.0f;       // x physical (x nightBrightness)
    float twinkle = 0.0f;        // 0..1
    float timeSeconds = 0.0f;    // drives the twinkle
};
