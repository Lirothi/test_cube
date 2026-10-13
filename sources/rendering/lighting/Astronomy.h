#pragma once

// Time-of-day plan T1 (docs/time_of_day_night_sky_plan.md): where the sun, the moon and the stars are.
// Pure functions, no renderer: the Time of Day entity (T2) calls them every frame and turns the
// answers into the scene's directional light, the sky's second light and the star sphere.
//
// Two ways to place the bodies:
//   * SIMPLE (the default; Ultra Dynamic Sky's own default): the sun rides a tilted circle that rises
//     at `riseHours` and sets at `setHours`; the moon rides its own circle, its place on it set by the
//     phase. Art-directable, and an authored sun direction can be reproduced exactly (level migration).
//   * REAL: date, latitude, longitude, UTC offset. The sun is NOAA's solar calculator exactly as UE's
//     SunPosition plugin has it (Plugins/Runtime/SunPosition/.../SunPosition.cpp:11-150, UE 5.8.2),
//     but with the Julian day taken from UTC as NOAA does (UE feeds it the LOCAL time). The moon is
//     Paul Schlyter's "How to compute planetary positions" (perturbed Keplerian elements, ~1-2 arc
//     minutes) with the topocentric parallax, the phase from Meeus ch. 48.
//
// Conventions: angles in degrees; azimuth clockwise from NORTH (east = 90); longitude positive EAST;
// the UTC offset in hours (UTC+10 = 10). World frame: Y up, north = +Z and east = +X before the
// scene's north yaw -- the same azimuth the editor's light inspector uses (atan2(x, z)).
namespace astro
{
struct Horizontal
{
    double altitudeDeg = 0.0;  // geometric (no refraction)
    double azimuthDeg = 0.0;   // clockwise from north
};

struct SunState
{
    Horizontal position;
    double refractedAltitudeDeg = 0.0; // with NOAA's refraction (what the eye sees near the horizon)
    double eclipticLongitudeDeg = 0.0; // apparent
    double rightAscensionDeg = 0.0;
    double declinationDeg = 0.0;
};

struct MoonState
{
    Horizontal position;               // TOPOCENTRIC (parallax applied: up to ~1 degree lower)
    double eclipticLongitudeDeg = 0.0; // geocentric, equinox of date
    double eclipticLatitudeDeg = 0.0;
    double distanceKm = 0.0;
    double rightAscensionDeg = 0.0;
    double declinationDeg = 0.0;
    double phaseAngleDeg = 0.0;        // sun - moon - earth; 0 = full, 180 = new
    double illuminatedFraction = 0.0;  // 0 new .. 1 full
    bool waxing = true;
};

// Gregorian calendar date + UTC hours (may run past 0..24) -> Julian day (Meeus ch. 7).
double JulianDayUtc(int year, int month, int day, double utcHours);
// Local mean sidereal time, degrees [0, 360).
double LocalSiderealDeg(double longitudeDeg, double jdUtc);
// Equatorial (RA, Dec) -> horizontal at a place and an instant.
Horizontal EquatorialToHorizontal(double rightAscensionDeg, double declinationDeg, double latitudeDeg,
                                  double longitudeDeg, double jdUtc);

SunState SunPosition(double latitudeDeg, double longitudeDeg, double jdUtc);
MoonState MoonPosition(double latitudeDeg, double longitudeDeg, double jdUtc);

// The moon's brightness relative to full at the same distance, from the phase angle (Allen /
// Meeus: magnitude grows by 0.026 |a| + 4e-9 a^4) -- 1 at full, ~0.1 at a quarter, ~0 at new.
double MoonPhaseBrightness(double phaseAngleDeg);

// --- SIMPLE mode ----------------------------------------------------------------------------------
struct SimplePath
{
    double riseHours = 6.0;     // the body crosses the horizon going up
    double setHours = 18.0;     // ... and going down
    double yawDeg = 180.0;      // azimuth the path leans toward (180 = the noon sun stands south)
    double tiltDeg = 30.0;      // the path's tilt from the zenith: noon altitude = 90 - tilt
    double heightOffset = 0.0;  // lifts (+) or sinks (-) the whole circle, sine of an angle
    double flatten = 0.0;       // 0..1: lengthens dawn and dusk by flattening the path near the horizon
};
// The unit direction TO the body in the local frame (x east, y up, z north) at local hours `hours`.
// Day half: rise -> top -> set over [riseHours, setHours]; night half: the rest of the 24 h.
void SimpleDirection(const SimplePath& path, double hours, double out[3]);
// The simple moon: the same construction on its own path, its place on the circle `phaseCycle`
// (0 new .. 0.5 full .. 1 new) of a turn behind the sun's: full = opposite the sun.
void SimpleMoonDirection(const SimplePath& moonPath, const SimplePath& sunPath, double hours,
                         double phaseCycle, double orbitOffsetDeg, double out[3]);

// The inverse, for converting a level's authored sun (T3): the local hours and the path yaw at which
// SimpleDirection puts the sun exactly at `toward` (local frame). Keeps the rise/set hours; zeroes the
// height offset and the flattening (the solve is exact without them); lowers the tilt only when the
// sun stands higher (or lower) than the path can reach (|elevation| > 90 - tilt). Of the two solutions
// it takes the afternoon / evening one. Returns the hours.
double SolveSimpleTime(const double toward[3], SimplePath& path);

// --- Frames ---------------------------------------------------------------------------------------
// Horizontal -> unit direction TO the body in the local frame (x east, y up, z north).
void HorizontalToLocal(const Horizontal& h, double out[3]);
// Local (x east, y up, z north) -> world (Y up), the scene's north turned by `northYawDeg`
// about +Y (clockwise seen from above, as azimuth runs).
void LocalToWorld(const double local[3], double northYawDeg, double out[3]);
// The celestial sphere: rows = the world-space images of the equatorial axes (x -> RA 0h, y -> RA 6h,
// z -> the north celestial pole) at a latitude and a local sidereal time. Stars at (RA, Dec) are
// placed by this rotation; in SIMPLE mode pass the sidereal angle the time of day implies.
void CelestialToLocal(double latitudeDeg, double localSiderealDeg, double out3x3[9]);

// `--tod-selftest`: the sun against NOAA's values (the 14 cases of UE's SunPosition test), the moon
// against Meeus' worked examples and known phases, the frames and the simple paths against their own
// definitions. One line per case and the verdict in the session log; returns the failure count.
int RunSelfTest();
}
