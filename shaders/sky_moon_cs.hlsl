// Time-of-day plan N1: the moon's near side for the drawn disc, PROCEDURAL (the owner's decision: no
// downloaded maps). One texel per point of the visible disc, orthographic: uv [0,1]^2 -> the disc's
// (x, y) in [-1, 1]^2, x right, y up, z toward the viewer. Out:
//   xyz  the surface normal in that frame (the sphere, bent by the craters);
//   w    the relative albedo (mean ~1 over the disc): bright highlands, dark maria, brighter young craters.
// Each mip is generated at its own resolution (SkyAtmosphere::BuildMoon), so a crater too small for the
// mip is simply left out instead of being averaged into mush by a downsample nobody runs.
//
// The noise is evaluated at the point on the unit SPHERE, not on the disc, so features keep their shape
// out to the limb (foreshortened by the projection, as on the real moon) and there is no seam or pole.
#define SKY_MOON_RS "CBV(b0), DescriptorTable(UAV(u0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer MoonCB : register(b0)
{
    uint MoonSize;  // this mip's width = height
    uint MoonMip;
    uint MoonSeed;
    uint MoonPad;
};

RWTexture2D<float4> MoonTexture : register(u0);

uint Hash(int3 c, uint salt)
{
    uint h = (asuint(c.x) * 73856093u) ^ (asuint(c.y) * 19349663u) ^ (asuint(c.z) * 83492791u) ^ (salt * 2654435761u);
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}
float Rand(uint h) { return float(h & 0x00ffffffu) / 16777216.0f; }

// Value noise in 3D, quintic-smoothed, [-1, 1].
float ValueNoise(float3 p, uint salt)
{
    const int3 i = int3(floor(p));
    const float3 f = p - floor(p);
    const float3 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
    float v[8];
    [unroll] for (int k = 0; k < 8; ++k)
    {
        v[k] = Rand(Hash(i + int3(k & 1, (k >> 1) & 1, k >> 2), salt)) * 2.0f - 1.0f;
    }
    const float x00 = lerp(v[0], v[1], u.x), x10 = lerp(v[2], v[3], u.x);
    const float x01 = lerp(v[4], v[5], u.x), x11 = lerp(v[6], v[7], u.x);
    return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z);
}

float Fbm(float3 p, uint octaves, uint salt)
{
    float sum = 0.0f, amplitude = 0.5f;
    [loop] for (uint o = 0; o < octaves; ++o)
    {
        sum += amplitude * ValueNoise(p, salt + o * 131u);
        p = p * 2.03f + 17.1f;
        amplitude *= 0.5f;
    }
    return sum;
}

// The maria: the large dark basalt plains, a third of the near side. Low-frequency, with a bias toward
// the upper half of the disc as the real near side has (Imbrium, Serenitatis, Tranquillitatis).
// DOMAIN-WARPED: a value-noise field this coarse -- two lattice cells across the disc -- traces its own
// lattice, and the first version's basins came out straight-sided, like polygons. The lookup is pushed
// around by a second field first, which leaves the coastlines ragged the way lava fronts are.
float MareMask(float3 p)
{
    const float3 warp = float3(Fbm(p * 2.1f + 11.3f, 3u, MoonSeed + 911u),
                               Fbm(p * 2.1f + 23.7f, 3u, MoonSeed + 912u),
                               Fbm(p * 2.1f + 37.1f, 3u, MoonSeed + 913u));
    const float field = Fbm(p * 1.7f + 0.6f * warp + float3(3.1f, 7.7f, 1.3f), 5u, MoonSeed) + 0.18f * p.y - 0.02f;
    return smoothstep(-0.02f, 0.2f, field);
}

// One octave of craters: a 3D cell grid at `frequency` (cells per unit of the sphere's radius), each cell
// holding a crater with probability `density`. The profile is a parabolic bowl (depth 0.2 of the radius:
// a simple crater's depth-to-diameter ratio of ~0.1) and a raised rim falling off outside it. `young`
// carries the brightness of fresh ejecta out to two radii for the few craters that are young.
float Craters(float3 p, float frequency, float density, uint salt, inout float young)
{
    const float3 q = p * frequency;
    const int3 base = int3(floor(q));
    float h = 0.0f;
    [loop] for (int z = -1; z <= 1; ++z)
    [loop] for (int y = -1; y <= 1; ++y)
    [loop] for (int x = -1; x <= 1; ++x)
    {
        const int3 cell = base + int3(x, y, z);
        const uint hc = Hash(cell, salt);
        if (Rand(hc) > density) { continue; }
        const float3 centre = float3(cell) + float3(Rand(Hash(cell, salt + 1u)), Rand(Hash(cell, salt + 2u)),
                                                    Rand(Hash(cell, salt + 3u)));
        // A crater must reach NO FURTHER THAN ONE CELL, the neighbourhood searched: q can sit anywhere in its
        // cell, and the 3x3x3 block is guaranteed to hold every centre only within one cell of it. The
        // first version let a crater of 0.48 cells reach 2.2 radii (1.06 cells), so a crater one cell
        // further out dropped out of the sum the moment q crossed a cell face -- a step in the height
        // whose gradient drew rows of black dots along the grid's planes. 0.45 x 2.0 = 0.9 cells.
        const float radius = lerp(0.22f, 0.45f, Rand(Hash(cell, salt + 4u)));
        const float d = length(q - centre) / radius;
        if (d > 2.0f) { continue; }
        const float bowl = d < 1.0f ? (d * d - 1.0f) * 0.2f : 0.0f;
        const float rim = 0.06f * exp(-((d - 1.0f) * (d - 1.0f)) / 0.04f);
        h += (bowl + rim) * radius / frequency;
        if (Rand(Hash(cell, salt + 5u)) < 0.12f)
        {
            young = max(young, 0.35f * saturate(1.0f - (d - 0.8f) / 1.2f));
        }
    }
    return h;
}

// Height of the surface at the sphere point p (unit), and its relative albedo.
float Surface(float3 p, out float albedo)
{
    const float mare = MareMask(p);
    float young = 0.0f;
    float h = -0.004f * mare + 0.0012f * Fbm(p * 9.0f, 3u, MoonSeed + 401u) * (1.0f - 0.6f * mare);
    // Octaves from 8 to 256 cells per radius; one too small for this mip -- under ~3 texels across --
    // is left out. The maria are younger lava plains, so they hold a third of the craters.
    const float texel = 2.0f / float(MoonSize);
    const float craterScale = lerp(1.0f, 0.35f, mare);
    float frequency = 6.0f;
    [loop] for (uint o = 0; o < 6u; ++o, frequency *= 2.0f)
    {
        if (0.8f / frequency < 3.0f * texel) { break; } // diameter ~0.7 / frequency
        h += craterScale * Craters(p, frequency, 0.55f, MoonSeed + 1000u + o * 17u, young);
    }
    albedo = lerp(1.22f, 0.62f, mare) * (1.0f + 0.10f * Fbm(p * 22.0f, 2u, MoonSeed + 77u));
    albedo = (albedo + young) / 1.06f;
    return h;
}

[numthreads(8, 8, 1)]
[RootSignature(SKY_MOON_RS)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= MoonSize)) { return; }
    float2 xy = (float2(id.xy) + 0.5f) / float(MoonSize) * 2.0f - 1.0f;
    xy.y = -xy.y; // texture rows run down, the disc's y up
    const float rr = dot(xy, xy);
    if (rr >= 1.0f)
    {
        // Outside the disc: the limb's normal, so a bilinear tap straddling the edge does not darken it.
        MoonTexture[id.xy] = float4(normalize(float3(xy, 1.0e-3f)), 1.0f);
        return;
    }
    const float3 p = float3(xy, sqrt(1.0f - rr));
    float albedo;
    const float h = Surface(p, albedo);
    // The tangential gradient of the height on the sphere, by central differences in 3D: the normal
    // bends away from where the surface rises, the difference taken over two texels so the normal is
    // as smooth as the mip it sits in (one texel turned the value noise into speckle along the
    // terminator, where the grazing sun reads every wobble). Exaggerated x1.5 -- at the moon's size on screen a true
    // crater relief only shows on the terminator, and that is where this reads -- and faded out over
    // the outer fifth toward the limb, where a bent normal turns away from the viewer and the first
    // version drew the edge as a ring of black dots.
    const float e = 2.0f / float(MoonSize);
    float unused;
    const float3 grad = float3(Surface(normalize(p + float3(e, 0, 0)), unused) - Surface(normalize(p - float3(e, 0, 0)), unused),
                               Surface(normalize(p + float3(0, e, 0)), unused) - Surface(normalize(p - float3(0, e, 0)), unused),
                               Surface(normalize(p + float3(0, 0, e)), unused) - Surface(normalize(p - float3(0, 0, e)), unused))
                        / (2.0f * e);
    const float3 tangential = grad - dot(grad, p) * p;
    const float3 n = normalize(p - 1.5f * saturate(p.z * 5.0f) * tangential);
    MoonTexture[id.xy] = float4(n, max(albedo, 0.05f));
}
