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
    // Moonlight is sunlight reddened by the regolith: ~4300 K. The blue of a moonlit night is the
    // eye's (plan part E), not the light's.
    float moonTemperatureK = 4300.0f;
    float moonAngleDeg = 0.52f;      // angular diameter (mean); the drawn disc is N1's
    // The owner's manual multiplier on the night's light (1 = physics). T2: the moon's light; N2 adds
    // the night sky's layers.
    float nightBrightness = 1.0f;

    // --- Cost ----------------------------------------------------------------------------------------
    // The SCENE light turns only once the body has moved this far from where the light was last put
    // (every turn re-renders the sun's shadows and drops the shadow caches); the sky gets the exact
    // direction every frame. 0.05 degrees is 12 s of a real sun -- P1 measures it.
    float lightUpdateThresholdDeg = 0.05f;
};
