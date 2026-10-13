#include "rendering/lighting/Astronomy.h"

#include <algorithm>
#include <cmath>

#include "core/logging/Log.h"

namespace astro
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr double kAuKm = 149597870.7;
constexpr double kEarthRadiusKm = 6378.14;

double Rad(double deg) { return deg * kDeg; }
double Deg(double rad) { return rad / kDeg; }
double Wrap360(double deg)
{
    const double w = std::fmod(deg, 360.0);
    return w < 0.0 ? w + 360.0 : w;
}
double Clamp1(double v) { return v < -1.0 ? -1.0 : (v > 1.0 ? 1.0 : v); }
// Signed smallest difference a - b on the circle, degrees in (-180, 180].
double AngleDiff(double a, double b)
{
    double d = Wrap360(a - b);
    return d > 180.0 ? d - 360.0 : d;
}

void HorizontalBasis(double azimuthDeg, double out[3]) // unit horizontal toward an azimuth (x east, z north)
{
    out[0] = std::sin(Rad(azimuthDeg));
    out[1] = 0.0;
    out[2] = std::cos(Rad(azimuthDeg));
}

void Normalize(double v[3])
{
    const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 1.0e-12) { v[0] /= len; v[1] /= len; v[2] /= len; }
}

// A body on a tilted circle at angle `theta` from the circle's TOP, increasing toward its setting
// point: top = cos(tilt) up + sin(tilt) H(yaw); rise = H(yaw - 90); set = H(yaw + 90).
void OnCircle(const SimplePath& path, double theta, double out[3])
{
    double rise[3], lean[3];
    HorizontalBasis(path.yawDeg - 90.0, rise);
    HorizontalBasis(path.yawDeg, lean);
    const double ct = std::cos(Rad(path.tiltDeg)), st = std::sin(Rad(path.tiltDeg));
    const double top[3] = {st * lean[0], ct, st * lean[2]};
    const double c = std::cos(theta), s = std::sin(theta);
    for (int i = 0; i < 3; ++i) { out[i] = c * top[i] - s * rise[i]; }
    // Lift / sink the whole circle, then flatten near the horizon (dawn and dusk last longer).
    out[1] += path.heightOffset;
    Normalize(out);
    if (path.flatten > 0.0)
    {
        const double p = 1.0 + 2.0 * std::clamp(path.flatten, 0.0, 1.0);
        out[1] = (out[1] < 0.0 ? -1.0 : 1.0) * std::pow(std::fabs(out[1]), p);
        Normalize(out);
    }
}
}

double JulianDayUtc(int year, int month, int day, double utcHours)
{
    int y = year, m = month;
    if (m <= 2) { y -= 1; m += 12; }
    const int a = y / 100;
    const int b = 2 - a + a / 4; // Gregorian
    return std::floor(365.25 * (y + 4716)) + std::floor(30.6001 * (m + 1)) + day + b - 1524.5 + utcHours / 24.0;
}

double LocalSiderealDeg(double longitudeDeg, double jdUtc)
{
    // Meeus 12.4, the mean sidereal time at Greenwich, then the place's east longitude.
    const double t = (jdUtc - 2451545.0) / 36525.0;
    const double gmst = 280.46061837 + 360.98564736629 * (jdUtc - 2451545.0) + 0.000387933 * t * t - t * t * t / 38710000.0;
    return Wrap360(gmst + longitudeDeg);
}

Horizontal EquatorialToHorizontal(double rightAscensionDeg, double declinationDeg, double latitudeDeg,
                                  double longitudeDeg, double jdUtc)
{
    const double h = Rad(LocalSiderealDeg(longitudeDeg, jdUtc) - rightAscensionDeg);
    const double dec = Rad(declinationDeg), lat = Rad(latitudeDeg);
    const double east = -std::cos(dec) * std::sin(h);
    const double north = std::sin(dec) * std::cos(lat) - std::cos(dec) * std::cos(h) * std::sin(lat);
    const double up = std::sin(dec) * std::sin(lat) + std::cos(dec) * std::cos(h) * std::cos(lat);
    Horizontal out;
    out.altitudeDeg = Deg(std::asin(Clamp1(up)));
    out.azimuthDeg = Wrap360(Deg(std::atan2(east, north)));
    return out;
}

SunState SunPosition(double latitudeDeg, double longitudeDeg, double jdUtc)
{
    // NOAA's solar calculator, term for term as UE SunPosition.cpp:33-139 has it -- with the Julian
    // day from UTC (theirs is the local time's, a few hours of declination off).
    const double t = (jdUtc - 2451545.0) / 36525.0;
    const double meanLong = Wrap360(280.46646 + t * (36000.76983 + t * 0.0003032));
    const double meanAnom = 357.52911 + t * (35999.05029 - 0.0001537 * t);
    const double ecc = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);
    const double centre = std::sin(Rad(meanAnom)) * (1.914602 - t * (0.004817 + 0.000014 * t))
                        + std::sin(Rad(2.0 * meanAnom)) * (0.019993 - 0.000101 * t)
                        + std::sin(Rad(3.0 * meanAnom)) * 0.000289;
    const double trueLong = meanLong + centre;
    const double omega = 125.04 - 1934.136 * t;
    const double appLong = trueLong - 0.00569 - 0.00478 * std::sin(Rad(omega));
    const double meanObliq = 23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0;
    const double obliq = meanObliq + 0.00256 * std::cos(Rad(omega));
    const double ra = Deg(std::atan2(std::cos(Rad(obliq)) * std::sin(Rad(appLong)), std::cos(Rad(appLong))));
    const double dec = Deg(std::asin(std::sin(Rad(obliq)) * std::sin(Rad(appLong))));
    const double vy = std::pow(std::tan(Rad(obliq) / 2.0), 2.0);
    const double eqTimeMin = 4.0 * Deg(vy * std::sin(2.0 * Rad(meanLong)) - 2.0 * ecc * std::sin(Rad(meanAnom))
        + 4.0 * ecc * vy * std::sin(Rad(meanAnom)) * std::cos(2.0 * Rad(meanLong))
        - 0.5 * vy * vy * std::sin(4.0 * Rad(meanLong)) - 1.25 * ecc * ecc * std::sin(2.0 * Rad(meanAnom)));

    // True solar time from the UTC time of day (NOAA: local + eqTime + 4 lon - 60 tz).
    const double utcMinutes = (jdUtc + 0.5 - std::floor(jdUtc + 0.5)) * 1440.0;
    double trueSolarMin = std::fmod(utcMinutes + eqTimeMin + 4.0 * longitudeDeg, 1440.0);
    if (trueSolarMin < 0.0) { trueSolarMin += 1440.0; }
    const double hourAngle = trueSolarMin / 4.0 - 180.0;

    const double lat = Rad(latitudeDeg);
    const double cosZenith = Clamp1(std::sin(lat) * std::sin(Rad(dec)) + std::cos(lat) * std::cos(Rad(dec)) * std::cos(Rad(hourAngle)));
    const double zenith = Deg(std::acos(cosZenith));
    const double elevation = 90.0 - zenith;

    // NOAA's refraction (arc seconds), UE :112-132 -- with the 1/3600 applied once to the whole sum.
    double refraction = 0.0;
    if (elevation <= 85.0)
    {
        const double te = std::tan(Rad(elevation));
        if (elevation > 5.0) { refraction = 58.1 / te - 0.07 / std::pow(te, 3.0) + 0.000086 / std::pow(te, 5.0); }
        else if (elevation > -0.575) { refraction = 1735.0 + elevation * (-518.2 + elevation * (103.4 + elevation * (-12.79 + elevation * 0.711))); }
        else { refraction = -20.772 / te; }
        refraction /= 3600.0;
    }

    double azimuth = 0.0;
    const double denom = std::cos(lat) * std::sin(Rad(zenith));
    if (std::fabs(denom) > 1.0e-9)
    {
        const double tmp = Deg(std::acos(Clamp1((std::sin(lat) * cosZenith - std::sin(Rad(dec))) / denom)));
        azimuth = hourAngle > 0.0 ? Wrap360(tmp + 180.0) : Wrap360(540.0 - tmp);
    }

    SunState s;
    s.position.altitudeDeg = elevation;
    s.position.azimuthDeg = azimuth;
    s.refractedAltitudeDeg = elevation + refraction;
    s.eclipticLongitudeDeg = Wrap360(appLong);
    s.rightAscensionDeg = Wrap360(ra);
    s.declinationDeg = dec;
    return s;
}

MoonState MoonPosition(double latitudeDeg, double longitudeDeg, double jdUtc)
{
    // Schlyter: days from 2000 Jan 0.0 UT (= JD 2451543.5); elements of the date.
    const double d = jdUtc - 2451543.5;
    const double N = Wrap360(125.1228 - 0.0529538083 * d);
    const double i = 5.1454;
    const double w = Wrap360(318.0634 + 0.1643573223 * d);
    const double a = 60.2666; // Earth radii
    const double e = 0.054900;
    const double M = Wrap360(115.3654 + 13.0649929509 * d);
    // The sun's mean anomaly and longitude (for the perturbations).
    const double Ms = Wrap360(356.0470 + 0.9856002585 * d);
    const double ws = 282.9404 + 4.70935e-5 * d;
    const double Ls = Wrap360(Ms + ws);

    // Kepler's equation.
    double E = M + Deg(e * std::sin(Rad(M)) * (1.0 + e * std::cos(Rad(M))));
    for (int k = 0; k < 8; ++k)
    {
        const double dE = (E - Deg(e * std::sin(Rad(E))) - M) / (1.0 - e * std::cos(Rad(E)));
        E -= dE;
        if (std::fabs(dE) < 1.0e-6) { break; }
    }
    const double xv = a * (std::cos(Rad(E)) - e);
    const double yv = a * (std::sqrt(1.0 - e * e) * std::sin(Rad(E)));
    const double v = Deg(std::atan2(yv, xv));
    double r = std::sqrt(xv * xv + yv * yv);

    const double vw = Rad(v + w);
    const double xh = r * (std::cos(Rad(N)) * std::cos(vw) - std::sin(Rad(N)) * std::sin(vw) * std::cos(Rad(i)));
    const double yh = r * (std::sin(Rad(N)) * std::cos(vw) + std::cos(Rad(N)) * std::sin(vw) * std::cos(Rad(i)));
    const double zh = r * (std::sin(vw) * std::sin(Rad(i)));
    double lon = Deg(std::atan2(yh, xh));
    double lat = Deg(std::atan2(zh, std::sqrt(xh * xh + yh * yh)));

    // The largest perturbations (Schlyter's list: ~1-2 arc minutes left).
    const double Lm = N + w + M;
    const double D = Lm - Ls;
    const double F = Lm - N;
    auto s = [](double deg) { return std::sin(Rad(deg)); };
    auto c = [](double deg) { return std::cos(Rad(deg)); };
    lon += -1.274 * s(M - 2 * D) + 0.658 * s(2 * D) - 0.186 * s(Ms) - 0.059 * s(2 * M - 2 * D)
         - 0.057 * s(M - 2 * D + Ms) + 0.053 * s(M + 2 * D) + 0.046 * s(2 * D - Ms) + 0.041 * s(M - Ms)
         - 0.035 * s(D) - 0.031 * s(M + Ms) - 0.015 * s(2 * F - 2 * D) + 0.011 * s(M - 4 * D);
    lat += -0.173 * s(F - 2 * D) - 0.055 * s(M - F - 2 * D) - 0.046 * s(M + F - 2 * D) + 0.033 * s(F + 2 * D)
         + 0.017 * s(2 * M + F);
    r += -0.58 * c(M - 2 * D) - 0.46 * c(2 * D);
    lon = Wrap360(lon);

    // Ecliptic -> equatorial with the obliquity of the date.
    const double ecl = 23.4393 - 3.563e-7 * d;
    const double xg = std::cos(Rad(lon)) * std::cos(Rad(lat));
    const double yg = std::sin(Rad(lon)) * std::cos(Rad(lat));
    const double zg = std::sin(Rad(lat));
    const double xe = xg;
    const double ye = yg * std::cos(Rad(ecl)) - zg * std::sin(Rad(ecl));
    const double ze = yg * std::sin(Rad(ecl)) + zg * std::cos(Rad(ecl));
    const double ra = Wrap360(Deg(std::atan2(ye, xe)));
    const double dec = Deg(std::atan2(ze, std::sqrt(xe * xe + ye * ye)));

    MoonState m;
    m.eclipticLongitudeDeg = lon;
    m.eclipticLatitudeDeg = lat;
    m.distanceKm = r * kEarthRadiusKm;
    m.rightAscensionDeg = ra;
    m.declinationDeg = dec;
    // Topocentric: the moon is ~60 Earth radii away, so from the surface it sits up to ~1 degree lower.
    m.position = EquatorialToHorizontal(ra, dec, latitudeDeg, longitudeDeg, jdUtc);
    const double parallax = Deg(std::asin(1.0 / std::max(r, 1.0)));
    m.position.altitudeDeg -= parallax * std::cos(Rad(m.position.altitudeDeg));

    // Phase (Meeus ch. 48): elongation from the sun, then the phase angle with both distances.
    const SunState sun = SunPosition(latitudeDeg, longitudeDeg, jdUtc);
    const double elongation = Deg(std::acos(Clamp1(std::cos(Rad(lat)) * std::cos(Rad(lon - sun.eclipticLongitudeDeg)))));
    const double phase = Deg(std::atan2(kAuKm * std::sin(Rad(elongation)), m.distanceKm - kAuKm * std::cos(Rad(elongation))));
    m.phaseAngleDeg = phase;
    m.illuminatedFraction = 0.5 * (1.0 + std::cos(Rad(phase)));
    m.waxing = AngleDiff(lon, sun.eclipticLongitudeDeg) > 0.0;
    return m;
}

double MoonPhaseBrightness(double phaseAngleDeg)
{
    const double a = std::fabs(phaseAngleDeg);
    const double magnitude = 0.026 * a + 4.0e-9 * a * a * a * a;
    return std::pow(10.0, -0.4 * magnitude);
}

void SimpleDirection(const SimplePath& path, double hours, double out[3])
{
    double dayLength = path.setHours - path.riseHours;
    if (dayLength <= 0.0) { dayLength += 24.0; }
    dayLength = std::clamp(dayLength, 0.01, 23.99);
    double t = std::fmod(hours - path.riseHours, 24.0);
    if (t < 0.0) { t += 24.0; }
    // Day: rise (theta -90) -> top (0) -> set (+90) across the day; night: set -> bottom -> rise.
    const double theta = t < dayLength ? -0.5 * kPi + kPi * (t / dayLength)
                                       : 0.5 * kPi + kPi * ((t - dayLength) / (24.0 - dayLength));
    OnCircle(path, theta, out);
}

double SolveSimpleTime(const double toward[3], SimplePath& path)
{
    double d[3] = {toward[0], toward[1], toward[2]};
    Normalize(d);
    path.heightOffset = 0.0;
    path.flatten = 0.0;
    // On the circle the elevation is asin(cos(theta) cos(tilt)): a sun higher than 90 - tilt is out of
    // the path's reach, so the tilt gives way.
    const double elevationDeg = Deg(std::asin(Clamp1(d[1])));
    path.tiltDeg = std::clamp(path.tiltDeg, 0.0, std::max(0.0, 90.0 - std::fabs(elevationDeg)));
    const double ct = std::cos(Rad(path.tiltDeg)), st = std::sin(Rad(path.tiltDeg));
    const double theta = std::acos(ct > 1.0e-12 ? Clamp1(d[1] / ct) : (d[1] >= 0.0 ? 1.0 : -1.0)); // [0, pi]
    // The horizontal part is cos(theta) st H(yaw) - sin(theta) H(yaw - 90), whose azimuth is
    // yaw + atan2(sin(theta), cos(theta) st).
    const double azimuthDeg = Deg(std::atan2(d[0], d[2]));
    path.yawDeg = Wrap360(azimuthDeg - Deg(std::atan2(std::sin(theta), std::cos(theta) * st)));
    // SimpleDirection's time -> theta, inverted: the day half runs rise -> set over theta -90 -> +90.
    double dayLength = path.setHours - path.riseHours;
    if (dayLength <= 0.0) { dayLength += 24.0; }
    dayLength = std::clamp(dayLength, 0.01, 23.99);
    const double sinceRise = theta <= 0.5 * kPi ? dayLength * (theta + 0.5 * kPi) / kPi
                                                : dayLength + (24.0 - dayLength) * (theta - 0.5 * kPi) / kPi;
    const double hours = std::fmod(path.riseHours + sinceRise, 24.0);
    return hours < 0.0 ? hours + 24.0 : hours;
}

void SimpleMoonDirection(const SimplePath& moonPath, const SimplePath& sunPath, double hours,
                         double phaseCycle, double orbitOffsetDeg, double out[3])
{
    // The sun's angle from its top, uniform in time (the moon does not share the sun's day/night split).
    double noon = 0.5 * (sunPath.riseHours + sunPath.setHours);
    if (sunPath.setHours < sunPath.riseHours) { noon += 12.0; }
    const double sunTheta = 2.0 * kPi * (hours - noon) / 24.0;
    // Full (0.5) = opposite the sun; first quarter (0.25) tops out six hours after the sun.
    const double theta = sunTheta + kPi - 2.0 * kPi * (phaseCycle - 0.5) + Rad(orbitOffsetDeg);
    OnCircle(moonPath, theta, out);
}

void HorizontalToLocal(const Horizontal& h, double out[3])
{
    const double ca = std::cos(Rad(h.altitudeDeg));
    out[0] = ca * std::sin(Rad(h.azimuthDeg));
    out[1] = std::sin(Rad(h.altitudeDeg));
    out[2] = ca * std::cos(Rad(h.azimuthDeg));
}

void LocalToWorld(const double local[3], double northYawDeg, double out[3])
{
    // Azimuth runs from +Z toward +X; turning north by `yaw` turns every azimuth by the same.
    const double c = std::cos(Rad(northYawDeg)), s = std::sin(Rad(northYawDeg));
    out[0] = local[0] * c + local[2] * s;
    out[1] = local[1];
    out[2] = -local[0] * s + local[2] * c;
}

void CelestialToLocal(double latitudeDeg, double localSiderealDeg, double m[9])
{
    // local (east, up, north) = M * equatorial (x -> RA 0h, y -> RA 6h, z -> the pole); H = LST - RA.
    const double sl = std::sin(Rad(localSiderealDeg)), cl = std::cos(Rad(localSiderealDeg));
    const double sp = std::sin(Rad(latitudeDeg)), cp = std::cos(Rad(latitudeDeg));
    m[0] = -sl;      m[1] = cl;       m[2] = 0.0;
    m[3] = cp * cl;  m[4] = cp * sl;  m[5] = sp;
    m[6] = -sp * cl; m[7] = -sp * sl; m[8] = cp;
}

int RunSelfTest()
{
    int failures = 0;
    auto check = [&failures](const char* what, double got, double want, double tolerance, bool angle) {
        const double err = angle ? std::fabs(AngleDiff(got, want)) : std::fabs(got - want);
        const bool ok = err <= tolerance;
        if (!ok) { ++failures; }
        LOG_INFO(logging::LogCategory::Scene, "tod self-test {} {}: got {:.4f} want {:.4f} err {:.4f} (tol {:.4f})",
                 ok ? "ok  " : "FAIL", what, got, want, err, tolerance);
    };

    // 1. The sun: the 14 cases UE's SunPosition test takes from NOAA's calculator (SunPosition.cpp:
    //    184-213; their inputs as given, Sydney's longitude included).
    struct SunCase { double lat, lon, tz; int y, m, d, h, min; double az, el, elCorr; };
    const SunCase sun[] = {
        {-33, -151, 10, 2017, 12, 21, 0, 0, 126.557, -9.975, -9.942},
        {-33, -151, 10, 2017, 12, 21, 6, 30, 70.526, 67.682, 67.689},
        {-33, -151, 10, 2017, 12, 21, 12, 42, 259.318, 28.670, 28.700},
        {-33, -151, 10, 2017, 4, 30, 0, 0, 93.712, -33.187, -33.178},
        {-33, -151, 10, 2017, 4, 30, 6, 30, 28.233, 37.537, 37.558},
        {-33, -151, 10, 2017, 4, 30, 12, 42, 293.350, 7.800, 7.911},
        {45, -73, -5, 2017, 12, 21, 0, 0, 6.197, -68.339, -68.337},
        {45, -73, -5, 2017, 12, 21, 6, 30, 113.524, -9.733, -9.699},
        {45, -73, -5, 2017, 12, 21, 12, 42, 192.659, 20.556, 20.599},
        {45, -73, -5, 2017, 7, 1, 0, 0, 1.026, -21.904, -21.889},
        {45, -73, -5, 2017, 7, 1, 6, 30, 78.768, 21.932, 21.971},
        {45, -73, -5, 2017, 7, 1, 12, 42, 206.978, 66.127, 66.134},
        {45, -73, -5, 2018, 9, 1, 5, 0, 74.982, -3.237, -3.135},
        {45, -73, -5, 2018, 9, 1, 5, 30, 80.308, 1.937, 2.225},
    };
    for (const SunCase& c : sun)
    {
        const double jd = JulianDayUtc(c.y, c.m, c.d, c.h + c.min / 60.0 - c.tz);
        const SunState s = SunPosition(c.lat, c.lon, jd);
        check("sun azimuth", s.position.azimuthDeg, c.az, 0.05, true);
        check("sun elevation", s.position.altitudeDeg, c.el, 0.05, false);
        check("sun refracted elevation", s.refractedAltitudeDeg, c.elCorr, 0.05, false);
        // The generic equatorial -> horizontal transform (GMST, used by the moon and the stars) must
        // put the same sun in the same place as NOAA's hour-angle route.
        const Horizontal h = EquatorialToHorizontal(s.rightAscensionDeg, s.declinationDeg, c.lat, c.lon, jd);
        check("sun via sidereal time, azimuth", h.azimuthDeg, s.position.azimuthDeg, 0.05, true);
        check("sun via sidereal time, elevation", h.altitudeDeg, s.position.altitudeDeg, 0.05, false);
    }

    // 2. The moon: Meeus' worked examples 47.a / 48.a (1992 April 12, 0h TD) and the phases of
    //    January 2024 (new 11th 11:57 UT, first quarter 18th 03:53, full 25th 17:54, last quarter
    //    Feb 2nd 23:18).
    {
        const MoonState m = MoonPosition(0.0, 0.0, JulianDayUtc(1992, 4, 12, 0.0));
        check("moon longitude (Meeus 47.a)", m.eclipticLongitudeDeg, 133.162655, 0.1, true);
        check("moon latitude (Meeus 47.a)", m.eclipticLatitudeDeg, -3.229126, 0.05, false);
        check("moon distance km (Meeus 47.a)", m.distanceKm, 368409.7, 500.0, false);
        check("moon illuminated fraction (Meeus 48.a)", m.illuminatedFraction, 0.6786, 0.01, false);
    }
    check("new moon fraction", MoonPosition(0, 0, JulianDayUtc(2024, 1, 11, 11.95)).illuminatedFraction, 0.0, 0.01, false);
    check("full moon fraction", MoonPosition(0, 0, JulianDayUtc(2024, 1, 25, 17.9)).illuminatedFraction, 1.0, 0.01, false);
    {
        const MoonState q1 = MoonPosition(0, 0, JulianDayUtc(2024, 1, 18, 3.88));
        check("first quarter fraction", q1.illuminatedFraction, 0.5, 0.03, false);
        check("first quarter waxing", q1.waxing ? 1.0 : 0.0, 1.0, 0.0, false);
        const MoonState q3 = MoonPosition(0, 0, JulianDayUtc(2024, 2, 2, 23.3));
        check("last quarter fraction", q3.illuminatedFraction, 0.5, 0.03, false);
        check("last quarter waning", q3.waxing ? 1.0 : 0.0, 0.0, 0.0, false);
    }
    check("moon brightness at quarter", MoonPhaseBrightness(90.0), 0.0911, 0.002, false);

    // 3. Frames: the celestial pole stands at the latitude, due north, at any sidereal time.
    for (double lat : {-33.0, 0.0, 51.5})
    {
        for (double lst : {0.0, 123.0, 270.0})
        {
            double m[9];
            CelestialToLocal(lat, lst, m);
            const double up = m[5], north = m[8], east = m[2];
            check("pole altitude", Deg(std::asin(Clamp1(up))), lat, 1.0e-6, false);
            check("pole azimuth (east part)", east, 0.0, 1.0e-9, false);
            (void)north;
        }
    }
    // ...and a star lands where the general equatorial -> horizontal transform puts it (N2 turns the
    // star sphere with this rotation): Sirius over Bora Bora on the June solstice.
    {
        const double jd = JulianDayUtc(2026, 6, 21, 10.0), lat = -16.5, lon = -151.75;
        const double raDeg = 101.287, decDeg = -16.716;
        double m[9];
        CelestialToLocal(lat, LocalSiderealDeg(lon, jd), m);
        const double eq[3] = {std::cos(Rad(decDeg)) * std::cos(Rad(raDeg)), std::cos(Rad(decDeg)) * std::sin(Rad(raDeg)),
                              std::sin(Rad(decDeg))};
        double viaTransform[3];
        HorizontalToLocal(EquatorialToHorizontal(raDeg, decDeg, lat, lon, jd), viaTransform);
        for (int r = 0; r < 3; ++r)
        {
            const double viaMatrix = m[r * 3] * eq[0] + m[r * 3 + 1] * eq[1] + m[r * 3 + 2] * eq[2];
            check("star via the celestial rotation", viaMatrix, viaTransform[r], 1.0e-9, false);
        }
    }

    // 4. The simple path: rise / top / set / bottom where its definition puts them.
    {
        SimplePath p; p.riseHours = 6.0; p.setHours = 18.0; p.yawDeg = 180.0; p.tiltDeg = 30.0;
        auto at = [&p](double hours) {
            double v[3]; SimpleDirection(p, hours, v);
            Horizontal h; h.altitudeDeg = Deg(std::asin(Clamp1(v[1]))); h.azimuthDeg = Wrap360(Deg(std::atan2(v[0], v[2])));
            return h;
        };
        check("simple rise altitude", at(6.0).altitudeDeg, 0.0, 1.0e-6, false);
        check("simple rise azimuth", at(6.0).azimuthDeg, 90.0, 1.0e-6, true);
        check("simple noon altitude", at(12.0).altitudeDeg, 60.0, 1.0e-6, false);
        check("simple noon azimuth", at(12.0).azimuthDeg, 180.0, 1.0e-6, true);
        check("simple set altitude", at(18.0).altitudeDeg, 0.0, 1.0e-6, false);
        check("simple set azimuth", at(18.0).azimuthDeg, 270.0, 1.0e-6, true);
        check("simple midnight altitude", at(0.0).altitudeDeg, -60.0, 1.0e-6, false);
        double moon[3], sunAtNoon[3];
        SimpleMoonDirection(p, p, 12.0, 0.5, 0.0, moon);
        SimpleDirection(p, 12.0, sunAtNoon);
        check("simple full moon opposite the sun", moon[0] * sunAtNoon[0] + moon[1] * sunAtNoon[1] + moon[2] * sunAtNoon[2], -1.0, 1.0e-6, false);
    }

    // 5. The level converter's solve (T3): every shipped level's authored sun, a high one, a set one, a
    // lopsided day -- SimpleDirection at the solved time and yaw must land on the authored direction.
    {
        struct Case { double x, y, z, rise, set; };
        const Case cases[] = {
            {0.507, 0.469, 0.723, 6.0, 18.0},   // wind_test / occlusion_test
            {1.477, 0.682, 0.473, 6.0, 18.0},   // atoll
            {0.928, 0.233, 0.289, 6.0, 18.0},   // demo / fog_spot_test
            {1.5, 0.7, 0.5, 6.0, 18.0},         // demo1 and the test levels
            {0.55, 0.62, -0.56, 6.0, 18.0},     // roughness_sweep
            {-0.25, 1.0, -0.4, 6.0, 18.0},      // wind_shadow_test: 64 degrees, above the default path
            {0.0, -0.3, 1.0, 6.0, 18.0},        // under the horizon
            {0.3, 0.2, -0.9, 5.0, 19.5},        // a long day
            {0.0, 1.0, 0.0, 6.0, 18.0},         // the zenith
        };
        for (const Case& c : cases)
        {
            const double want[3] = {c.x, c.y, c.z};
            SimplePath p; p.riseHours = c.rise; p.setHours = c.set;
            const double hours = SolveSimpleTime(want, p);
            double got[3];
            SimpleDirection(p, hours, got);
            double w[3] = {c.x, c.y, c.z};
            Normalize(w);
            const double dot = got[0] * w[0] + got[1] * w[1] + got[2] * w[2];
            check("simple solve (degrees off the authored sun)", Deg(std::acos(Clamp1(dot))), 0.0, 1.0e-4, false);
        }
    }

    if (failures == 0) { LOG_INFO(logging::LogCategory::Scene, "tod self-test: PASS"); }
    else { LOG_ERROR(logging::LogCategory::Scene, "tod self-test: FAIL ({} checks)", failures); }
    return failures;
}
}
