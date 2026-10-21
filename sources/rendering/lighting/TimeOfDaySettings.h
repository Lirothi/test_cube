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
    // The least the moon's LIGHT falls to with the phase, as a share of the full moon's: 0 = physics (a new moon
    // sheds nothing). Ultra Dynamic Sky's New Moon Light Brightness is 0.015 -- read off its moon (2026-10-16):
    // 0.15 full, 0.0265 at the quarter, 0.00225 new -- and with its constant blue Night Sky Glow that is why its
    // moonless night is a dark blue sky, 1.3 stops under its full moon on screen, where ours went black-olive.
    float moonNewBrightness = 0.0f;
    // NO MOON TEMPERATURE (removed 2026-10-15). It was the moon's colour as the sky scattered it -- 5450 K,
    // sunlight reddened by the regolith (B-V 0.92 against the sun's 0.65) shifted onto our 6500 K white sun.
    // Every hue it fed now has its own: the disc and its glow moonDiscColor, the ground and, with Ultra
    // Dynamic Sky's night Rayleigh, the atmosphere moonLightColor (TimeOfDay::Tick). A temperature only
    // sets a hue (unit luminance), so the control could no longer move a pixel. Old levels' `temperatureK`
    // is dropped by WithAllKeys.
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
    // The colour the moon LIGHTS the scene in -- the ground and, since 2026-10-15, the atmosphere (UDS's moon
    // lights its SkyAtmosphere in this colour, under its night Rayleigh; see twilight below). The owner (2026-10-13), shown the
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
    // sky's own layers below (N2) -- and the night lamps' (plan L: lights and glows "onAt": "night"), so a lamp
    // keeps its outdoor ratio to the moonlight (~100x under it) whatever the night is lifted by.
    float nightBrightness = 1.0f;
    // The owner's manual multiplier on the SKY BY DAY (1 = as the atmosphere gives it): the sky's light into every
    // LUT -- the sky itself, the aerial perspective, the distant light and the capture, so the sky, its
    // reflections and the ambient it sheds stay one sky (SkyAtmosphereSettings::luminanceScale, the frame's copy).
    // It comes in with the day: the twilight's day blend (rayleighDayStartDeg .. rayleighDayFullDeg, UDS's own
    // switch from its night sky to its day one), so the night and the moonlit dawn keep their darkness. The owner
    // (2026-10-16): "our sky is darker than theirs by day, but I like our darkness at night". Measured on
    // wind_test at noon: sky 2380 cd/m2 at 75 degrees from the sun against a CIE clear sky's ~3300-4900, sunlit
    // sand 12540 (albedo 0.4 at ~100 klx, right).
    float daySkyBrightness = 1.0f;
    // The owner's "night darker / lighter" (2026-10-20), in STOPS of exposure: added to the camera's compensation
    // (CameraExposureSettings::compensationEv, the FRAME's copy) by the night's share -- 0 with the sun above -6
    // degrees (the end of civil twilight), all of it from -12 (the end of nautical), smoothstep between. The camera
    // only: the light, the meter, the curve's key (the scene's own EV) and the night vision (the eye) stay as they
    // are, so it moves the night's picture and nothing else. Negative darkens; 0 = none.
    float nightExposureEv = 0.0f;

    // --- Night lamps (plan L, rendering/lighting/NightLamps.h) ---------------------------------------
    // Lights and glowing meshes marked "onAt": "night" (or "day") switch when the SUN crosses this elevation:
    // 0 = its centre at the horizon, the moment Ultra Dynamic Sky fires Sunset / Sunrise (its Sunset Event
    // Offset is a time; ours an elevation, the same in simple and real positions). Each comes on after a delay
    // of up to `lampRandomDelaySec` (a smooth noise of its position: a street's lamps one by one, a lamp and its
    // bulb together), over `lampTurnOnSec` along t^`lampTurnOnExponent`, flickering by up to `lampTurnOnFlicker`
    // while it warms; it goes out at once. UDS's Light Day/Night Toggle defaults, read off the owner's editor
    // (2026-10-20): Random Delay 1, Turn On Animation Length 1.5, Flicker 0.2, Intensity Curve Exponent 0.6.
    float lampSunElevationDeg = 0.0f;
    float lampRandomDelaySec = 1.0f;
    float lampTurnOnSec = 1.5f;
    float lampTurnOnFlicker = 0.2f;
    float lampTurnOnExponent = 0.6f;
    // An ALWAYS-ON lamp's multiplier by day (no "onAt"; 1 = physics), as nightBrightness is the night light's: its
    // lumens stay a bulb's, the day around it is what is weighed. UE with Ultra Dynamic Sky lights its day with a sun
    // of 5 lux (UDS Sun Light Intensity, read off the owner's level 2026-10-21) where ours is 120000, so a lamp there
    // shows by day and here it vanishes; 120000 / 5 = 24000 weighs it as UDS does. By night it takes nightBrightness,
    // as the night lamps do, through the night's share (TimeOfDay::NightShare) in between.
    float lampDayBrightness = 1.0f;

    // --- Twilight, as Ultra Dynamic Sky draws it -----------------------------------------------------
    // THE SUN LEAVES THE ATMOSPHERE before the night falls: its light into the sky -- the sky's LUTs, the
    // aerial perspective, the distant light and the environment capture, the clouds -- is 0 at and below
    // `sunSkyStartDeg` of elevation and rises linearly to full at `sunSkyFullDeg`. UDS's own sun intensity,
    // read off its actor (2026-10-15): 0 from -9.03 degrees down, 2.45 of 5 at -7.79, 5 from -6.49 up.
    // Physics keeps a glow to -18, and over our physical moonlit sky (the moon 1/375000 of the sun; UDS's
    // is 1/33 of its sun, so its moonlit sky outshines the glow) a sun at -13 drew a red band over a black
    // sky that UDS never shows (owner, 2026-10-15). The ground's light is not this: the sun lights it only
    // while its disc is up. Both at -90 = physics.
    float sunSkyStartDeg = -9.03f;
    float sunSkyFullDeg = -6.49f;
    // UDS's NIGHT RAYLEIGH: the atmosphere's Rayleigh scattering, per channel, times this tint while the
    // sun is down -- the ratio of UDS's night Rayleigh colour (0.241, 0.347, 0.609) to its day one (0.169,
    // 0.409, 1.0), read off its SkyAtmosphere component -- and back to the authored colour from
    // `rayleighDayStartDeg` to `rayleighDayFullDeg`, easing out (UDS's blend, read at 15 elevations, is
    // 1 - (1 - t)^2 over that range to 0.01). Less blue scattered: a paler moonlit sky and a less purple
    // twilight. The FRAME's atmosphere only (the level keeps its Rayleigh scale as written), and all of it,
    // the sun's own reddening included, as UDS's component is. (1, 1, 1) = none.
    float rayleighNightTint[3] = {1.426f, 0.848f, 0.609f};
    float rayleighDayStartDeg = -2.07f;
    float rayleighDayFullDeg = 3.68f;
    // UDS's TWILIGHT OZONE ("Other Absorption"): the atmosphere's ozone, per channel, times `ozoneNightTint`
    // with the sun at or below -ozoneBlendDeg, `ozoneSunriseTint` at 0, the authored ozone from +ozoneBlendDeg
    // up, linear between. UDS's live values over the range (2026-10-15) against Earth's ozone (UE's default,
    // 0.00065 / 0.001881 / 0.000085 per km): night (1, 0.849, 0.030) x 0.004 = x(6.15, 1.81, 1.43), sunrise
    // (1, 0.976, 0.114) x 0.003 = x(4.62, 1.56, 4.03), each step of their blend within 5 % of these keys.
    // Earth's ozone absorbs GREEN most, so the long-path light of a twilight leaves a MAGENTA sky; UDS's
    // absorbs red as hard, and its twilight sky is blue. The day keeps the level's own ozone (UDS's day,
    // x(2.76, 1.06, 2.24), is not ported: the day's look is the level's). (1, 1, 1) twice = none.
    float ozoneNightTint[3] = {6.154f, 1.806f, 1.428f};
    float ozoneSunriseTint[3] = {4.615f, 1.557f, 4.031f};
    float ozoneBlendDeg = 1.95f;
    // THE HAND-OVER, ONE LIGHT AT A TIME, CROSS-FADED: the scene light's body fades out over this many degrees
    // of SUN elevation before the light changes hands (the sun's disc wholly under the horizon) and the
    // incoming body fades in over as many after -- smoothstep, so both meet at 0. At dawn the moon fades
    // from -(sun radius + this) to -(sun radius), the sun from there to +this; at dusk the reverse. Measured
    // on wind_test (moon 1/33 of the sun, 2-degree sun, the water's glint in pre-exposed screen units): the
    // moon's glint fell 8.8 -> 0 and the sun's rose 0 -> 27 -> 107 within 1.3 degrees of each other (owner,
    // 2026-10-19: "a sharp jump from the moon's specular straight to the sun's"). Only the scene light: the
    // sky's sun (its disc, glare, the sky) is untouched. 0 = the hard switch.
    float lightHandOverDeg = 3.0f;

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
