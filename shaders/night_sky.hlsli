#ifndef NIGHT_SKY_HLSLI
#define NIGHT_SKY_HLSLI

// TIME-OF-DAY PLAN N2: THE NIGHT SKY'S OWN LIGHT -- the airglow, the stars, the Milky Way -- shared by
// the sky pixel (skybox.hlsl), its captured picture (sky_ibl_capture_cs.hlsl), the distant light
// (sky_lut_distant_cs.hlsl) and the one-off map of the Milky Way (sky_night_cs.hlsl). Radiance in
// cd/m2, this renderer's raw sky radiance, BEFORE the air in front of it (each caller applies the
// transmittance toward the top of the atmosphere). Procedural throughout (the owner's decision: no
// catalogue, no downloaded maps), so the constellations are not the real ones even in Real mode -- but
// the sky turns about the real pole once a sidereal day, and the galaxy lies where the real one does.

static const float NightPi = 3.14159265358979323846f;

// The Milky Way map stores radiance in units of 1e-4 cd/m2: a dark site's night sky is ~2e-4, and
// R11G11B10's floats go denormal under 6e-5, where they keep almost no precision.
static const float kNightMapUnit = 1.0e-4f;

// Van Rhijn: a thin glowing layer h above a sphere of radius R, seen at zenith angle z, is brighter by
// the length of the path through it, 1 / sqrt(1 - (R / (R + h))^2 sin^2 z). The airglow layer is ~90 km
// up: x6 at the horizon, before the air's extinction (the caller's transmittance) takes most of it back.
float NightAirglowVanRhijn(float cosZenith)
{
    const float k = 0.97232f; // (6371 / 6461)^2
    const float sin2 = saturate(1.0f - cosZenith * cosZenith);
    return rsqrt(max(1.0f - k * sin2, 1.0e-4f));
}

// Equatorial unit vector (x toward RA 0h, y toward RA 6h, z the north celestial pole) <-> the map's
// equirectangular UV: u = RA / 2pi, v = 0 at the north pole, 1 at the south.
float2 NightMapUv(float3 eq)
{
    const float ra = atan2(eq.y, eq.x);
    return float2(frac(ra / (2.0f * NightPi) + 1.0f), acos(clamp(eq.z, -1.0f, 1.0f)) / NightPi);
}
float3 NightMapDir(float2 uv)
{
    const float ra = uv.x * 2.0f * NightPi;
    const float polar = uv.y * NightPi;
    return float3(sin(polar) * cos(ra), sin(polar) * sin(ra), cos(polar));
}

// The galactic frame (IAU, J2000) in equatorial coordinates: the north galactic pole at RA 192.85948,
// Dec +27.12825; the centre (l = 0) at RA 266.40500, Dec -28.93617; l = 90 toward Cygnus.
static const float3 kGalacticPole = float3(-0.867670f, -0.198090f, 0.455984f);
static const float3 kGalacticCentre = float3(-0.054878f, -0.873449f, -0.483835f);
static const float3 kGalactic90 = float3(0.494121f, -0.444832f, 0.746994f);

// Jarzynski & Olano 2020, pcg4d: four well-mixed 32-bit values from four.
uint4 NightPcg4d(uint4 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.w; v.y += v.z * v.x; v.z += v.x * v.y; v.w += v.y * v.z;
    v ^= v >> 16u;
    v.x += v.y * v.w; v.y += v.z * v.x; v.z += v.x * v.y; v.w += v.y * v.z;
    return v;
}
float4 NightHash4(uint4 v)
{
    return float4(NightPcg4d(v) >> 8u) * (1.0f / 16777216.0f); // 24 bits each: exact in a float
}

// Trilinear value noise in [0, 1] and its fBm. The octave count is a parameter (the map's coarse mips
// leave out what they cannot hold), but the loop's bound is the literal maximum.
float NightValueNoise(float3 x, uint seed)
{
    const float3 i = floor(x);
    const float3 f = x - i;
    const float3 u = f * f * (3.0f - 2.0f * f);
    const int3 c = int3(i);
    float corner[8];
    [unroll] for (uint k = 0; k < 8u; ++k)
    {
        const int3 o = int3(k & 1u, (k >> 1u) & 1u, (k >> 2u) & 1u);
        corner[k] = NightHash4(uint4(asuint(c + o), seed)).x;
    }
    const float x00 = lerp(corner[0], corner[1], u.x), x10 = lerp(corner[2], corner[3], u.x);
    const float x01 = lerp(corner[4], corner[5], u.x), x11 = lerp(corner[6], corner[7], u.x);
    return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z);
}
float NightFbm(float3 x, uint octaves, uint seed)
{
    float sum = 0.0f, amplitude = 0.5f, norm = 0.0f;
    [unroll] for (uint o = 0; o < 6u; ++o)
    {
        if (o < octaves)
        {
            sum += amplitude * NightValueNoise(x, seed + o * 7919u);
            norm += amplitude;
        }
        x *= 2.03f;
        amplitude *= 0.5f;
    }
    return norm > 0.0f ? sum / norm : 0.5f;
}

// A blackbody's colour in linear sRGB at unit luminance: the Planckian locus in CIE 1960 UCS (UE's
// MaterialExpressionBlackBody fit, valid ~1000-15000 K, clamped there), as DirectionalLight's CPU twin.
float3 NightBlackbody(float kelvin)
{
    const float t = clamp(kelvin, 1000.0f, 15000.0f);
    const float t2 = t * t;
    const float u = (0.860117757f + 1.54118254e-4f * t + 1.28641212e-7f * t2) / (1.0f + 8.42420235e-4f * t + 7.08145163e-7f * t2);
    const float v = (0.317398726f + 4.22806245e-5f * t + 4.20481691e-8f * t2) / (1.0f - 2.89741816e-5f * t + 1.61456053e-7f * t2);
    const float d = 2.0f * u - 8.0f * v + 4.0f;
    const float x = 3.0f * u / d, y = 2.0f * v / d;
    const float X = x / y, Z = (1.0f - x - y) / y;
    const float3 rgb = max(0.0f, float3(3.2404542f * X - 1.5371385f - 0.4985314f * Z,
                                       -0.9692660f * X + 1.8760108f + 0.0415560f * Z,
                                        0.0556434f * X - 0.2040259f + 1.0572252f * Z));
    return rgb / max(dot(rgb, float3(0.2126f, 0.7152f, 0.0722f)), 1.0e-6f);
}

// THE STARS. One candidate per cell of two 6 x kNightStarCells^2 cubes over the celestial sphere, the
// second turned against the first: with one grid the faint stars of the Milky Way's band -- one in every
// cell -- stood on a visible square lattice; two incommensurate lattices read as random. Each star's
// magnitude from the real counts, N(< m) ~ 10^(0.5 m + 0.65) over the whole sky (14 stars brighter than
// 1, ~4500 than 6, ~450000 than 10), shared over both grids' cells, so the faintest is ~9.7; its
// illuminance E = 10^(-0.4 (m + 14.18)) lux (a magnitude-0 star is 2.1e-6 lux). Faint ones (past 7) are
// thinned off the galaxy's plane by `density` (0..1), as the real ones crowd toward it.
//
// DRAWN AS A POINT, not a disc: a Gaussian of 0.65 pixels around its direction carrying exactly E, so
// the sum over the pixels it touches is the star's light whatever the sub-pixel jitter -- no growing
// with resolution, no flicker as DLSS's jitter walks the pixel grid across it. Its centre stays at least
// a quarter of a cell from the cell's edges so the footprint never needs a neighbour cell (one hash a
// pixel a grid).
//
// Returns radiance, cd/m2. `twinkle` 0..1 at the zenith, stronger toward the horizon (`horizonWeight`).
static const float kNightStarCells = 160.0f;
static const float kNightStarAllCells = 2.0f * 6.0f * kNightStarCells * kNightStarCells;
// The second grid's turn: Rz(0.6) Rx(0.9), any fixed rotation off the cube's axes would do.
static const float3x3 kNightStarTurn = float3x3(0.82534f, -0.35099f, 0.44230f,
                                                0.56464f, 0.51304f, -0.64651f,
                                                0.0f, 0.78333f, 0.62161f);

float3 NightStarGrid(float3 eq, uint seed, float pixelAngle, float density, float timeSeconds, float twinkle, float horizonWeight)
{
    const float3 a = abs(eq);
    uint face;
    float2 st;
    if (a.x >= a.y && a.x >= a.z) { face = eq.x > 0.0f ? 0u : 1u; st = eq.yz / a.x; }
    else if (a.y >= a.z)          { face = eq.y > 0.0f ? 2u : 3u; st = eq.xz / a.y; }
    else                          { face = eq.z > 0.0f ? 4u : 5u; st = eq.xy / a.z; }
    const float2 grid = (st * 0.5f + 0.5f) * kNightStarCells;
    const uint2 cell = min(uint2(grid), uint2(kNightStarCells - 1.0f, kNightStarCells - 1.0f));
    const float4 r = NightHash4(uint4(cell, face, seed));

    const float magnitude = 2.0f * (log10(max(r.x, 1.0e-7f) * kNightStarAllCells) - 0.65f);
    if (magnitude > 7.0f && r.y > density) return 0.0f;

    // Where it stands in its cell, back on the face, then on the sphere.
    const float2 stStar = (float2(cell) + 0.25f + 0.5f * r.zw) / kNightStarCells * 2.0f - 1.0f;
    float3 d;
    if (face < 2u)      d = float3(face == 0u ? 1.0f : -1.0f, stStar.x, stStar.y);
    else if (face < 4u) d = float3(stStar.x, face == 2u ? 1.0f : -1.0f, stStar.y);
    else                d = float3(stStar.x, stStar.y, face == 4u ? 1.0f : -1.0f);
    d = normalize(d);
    const float3 off = cross(eq, d);
    const float angle2 = dot(off, off); // sin^2 of the angle, which is the angle^2 this close
    const float sigma = max(0.65f * pixelAngle, 1.0e-6f);
    if (angle2 > 9.0f * sigma * sigma) return 0.0f;

    float e = pow(10.0f, -0.4f * (magnitude + 14.18f));
    const float4 q = NightHash4(uint4(cell, face, seed ^ 0xC0102u));
    // Scintillation: a smooth random walk a few times a second per star, deepest at the horizon.
    if (twinkle > 0.0f)
    {
        const float rate = 2.0f + 3.0f * q.y;
        const float phase = timeSeconds * rate + q.z * 1000.0f;
        const float step = floor(phase);
        const float n0 = NightHash4(uint4(cell, face ^ (uint(step) << 3u), seed ^ 0x7117u)).x;
        const float n1 = NightHash4(uint4(cell, face ^ (uint(step + 1.0f) << 3u), seed ^ 0x7117u)).x;
        const float n = lerp(n0, n1, smoothstep(0.0f, 1.0f, phase - step));
        e *= max(0.0f, 1.0f + twinkle * horizonWeight * (2.0f * n - 1.0f));
    }
    // Colour: the naked-eye stars run from hot blue-white to cool orange; spread evenly in mired between
    // 30000 K and 3200 K, weighted toward the cool end (the sky's mean ~5100 K).
    const float kelvin = 1.0e6f / (33.0f + 279.0f * pow(q.x, 0.7f));
    return NightBlackbody(kelvin) * (e * exp(-0.5f * angle2 / (sigma * sigma)) / (2.0f * NightPi * sigma * sigma));
}

float3 NightStars(float3 eq, float pixelAngle, float density, float timeSeconds, float twinkle, float horizonWeight)
{
    return NightStarGrid(eq, 0x5EA75u, pixelAngle, density, timeSeconds, twinkle, horizonWeight)
         + NightStarGrid(mul(kNightStarTurn, eq), 0xB16B0u, pixelAngle, density, timeSeconds, twinkle, horizonWeight);
}

// The stars' mean radiance over the sphere, for what cannot resolve a point -- the captured picture
// and the distant light: the integral of E dN over the counts above, from -1.5 to the cells' limit
// (~11.1), spread over 4 pi; ~5e-5 cd/m2, the real integrated starlight's ~23.3 mag/arcsec2.
static const float kNightStarsMeanRadiance = 4.5e-5f;
// The Milky Way map's own mean (sky_night_cs.hlsl), likewise.
static const float kNightMilkyWayMeanRadiance = 1.0e-5f;

#endif // NIGHT_SKY_HLSLI
