// Volumetric clouds, plan C1: the three noise textures the density model reads, built ONCE at load
// (and again when the seed changes). Schneider 2015: a Perlin-Worley base at 128^3, a Worley detail
// at 32^3, a 2D weather map. All tile: the lattice and the Worley cells wrap at the texture's period,
// so a sample at any scale is seamless.
//
// Three entry points, one file; the C++ side dispatches each over its own texture:
//   CSBase    128 x 128 x 128  RGBA8  r: Perlin-Worley (freq 4), gba: Worley at 8 / 16 / 32
//   CSDetail  128 x 128 x 128  RGBA8  rgb: Worley at 4 / 8 / 16, a: 0
//   CSWeather 512 x 512        RGBA8  r: coverage fbm, g: type fbm, b: 0, a: 0
// All values are in [0, 1]; the density model remaps them (cloud_common.hlsli).
//
// THE SMALLEST CELL IS AT LEAST FOUR TEXELS WIDE, in every channel. The first version ran the
// Worley octaves up to one texel per cell (a 32^3 detail with a frequency-16 octave, and its FBM
// doubling twice more on top), which makes the texture texel noise, and trilinear filtering of
// texel noise is a LATTICE: the trace read it back as long parallel streaks along the world axes,
// foreshortened into radial bands across every thick cloud (owner, 2026-09-12; shrinking the
// detail tile shrank the bands into a visible mesh, shrinking the base tile did nothing). So the
// detail texture is 128^3 like the base, and each FBM stops before its cells get finer than four
// texels -- the same rule any mip-mapped texture already obeys.
#define CLOUD_NOISE_RS "CBV(b0), DescriptorTable(UAV(u0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer NoiseCB : register(b0)
{
    uint4 noiseParams; // x: size (per axis), y: seed, z: 0, w: 0
};

// One UAV slot, two shapes: the weather kernel is compiled with CLOUD_NOISE_2D and sees a 2D target
// at u0, the two volume kernels see a 3D one. A single file cannot declare both at the same register.
#ifdef CLOUD_NOISE_2D
RWTexture2D<float4> OutPlane : register(u0);
#else
RWTexture3D<float4> OutVolume : register(u0);
#endif

// Integer hash (PCG family), seeded, so the whole field changes with the seed and nothing else.
uint CloudHashU(uint3 v)
{
    v = v * 1664525u + 1013904223u + noiseParams.y * 374761393u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v.x;
}
float3 CloudHash3(uint3 cell)
{
    const uint h = CloudHashU(cell);
    return float3(float(h & 1023u), float((h >> 10u) & 1023u), float((h >> 20u) & 1023u)) / 1023.0f;
}
float3 CloudGradient(uint3 cell)
{
    return normalize(CloudHash3(cell) * 2.0f - 1.0f + 1.0e-4f);
}

// Tiling gradient (Perlin) noise over `period` cells, [0, 1].
float CloudPerlin(float3 p, uint period)
{
    const float3 cellF = floor(p);
    const float3 f = p - cellF;
    const float3 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
    const int3 cell = int3(cellF);
    float n[8];
    [unroll] for (int i = 0; i < 8; ++i)
    {
        const int3 corner = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        const uint3 wrapped = uint3((cell + corner) % int(period) + int(period)) % period;
        n[i] = dot(CloudGradient(wrapped), f - float3(corner));
    }
    const float x0 = lerp(lerp(n[0], n[1], u.x), lerp(n[2], n[3], u.x), u.y);
    const float x1 = lerp(lerp(n[4], n[5], u.x), lerp(n[6], n[7], u.x), u.y);
    return saturate(lerp(x0, x1, u.z) * 0.5f + 0.5f);
}

float CloudPerlinFbm(float3 p, uint period, uint octaves)
{
    float sum = 0.0f, amp = 0.5f, norm = 0.0f;
    [loop] for (uint o = 0; o < octaves; ++o)
    {
        sum += amp * CloudPerlin(p, period);
        norm += amp;
        p *= 2.0f; period *= 2u; amp *= 0.5f;
    }
    return sum / max(norm, 1.0e-4f);
}

// Tiling Worley (cellular) noise, INVERTED so a cell centre is 1 and the boundary 0 -- the "puffy"
// convention Schneider uses.
float CloudWorley(float3 p, uint period)
{
    const float3 cellF = floor(p);
    const float3 f = p - cellF;
    const int3 cell = int3(cellF);
    float minDist = 1.0e9f;
    [unroll] for (int z = -1; z <= 1; ++z)
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        const int3 offset = int3(x, y, z);
        const uint3 wrapped = uint3((cell + offset) % int(period) + int(period)) % period;
        const float3 point_ = float3(offset) + CloudHash3(wrapped + 7919u);
        const float3 d = point_ - f;
        minDist = min(minDist, dot(d, d));
    }
    return saturate(1.0f - sqrt(minDist));
}

// Up to three octaves (0.625 / 0.25 / 0.125), renormalised when fewer fit under the texel limit.
float CloudWorleyFbm(float3 p, uint period, uint octaves)
{
    float sum = CloudWorley(p, period) * 0.625f, norm = 0.625f;
    if (octaves >= 2u) { sum += CloudWorley(p * 2.0f, period * 2u) * 0.25f; norm += 0.25f; }
    if (octaves >= 3u) { sum += CloudWorley(p * 4.0f, period * 4u) * 0.125f; norm += 0.125f; }
    return sum / norm;
}

// The octave count that keeps the finest cell at >= 4 texels: cells per axis double per octave.
uint CloudOctavesFor(uint baseFrequency, uint size, uint wanted)
{
    uint octaves = 0u;
    for (uint f = baseFrequency; octaves < wanted && f * 4u <= size; f *= 2u) { ++octaves; }
    return max(octaves, 1u);
}

float CloudRemapNoise(float v, float lo, float hi, float newLo, float newHi)
{
    return newLo + (v - lo) / max(hi - lo, 1.0e-5f) * (newHi - newLo);
}

#ifndef CLOUD_NOISE_2D
[numthreads(8, 8, 1)]
[RootSignature(CLOUD_NOISE_RS)]
void CSBase(uint3 id : SV_DispatchThreadID)
{
    const uint size = noiseParams.x;
    if (any(id >= size)) { return; }
    const float3 p = (float3(id) + 0.5f) / float(size); // [0, 1)
    // Perlin-Worley: a 4-period fbm Perlin dilated by a Worley fbm of the same frequency. Octaves
    // stop at four texels per cell (128^3: Perlin 4..32, Worley 4..16 / 8..32 / 16..32 / 32).
    const float perlin = CloudPerlinFbm(p * 4.0f, 4u, CloudOctavesFor(4u, size, 7u));
    const float worley4 = CloudWorleyFbm(p * 4.0f, 4u, CloudOctavesFor(4u, size, 3u));
    const float perlinWorley = saturate(CloudRemapNoise(perlin, 0.0f, 1.0f, worley4, 1.0f));
    const float w8 = CloudWorleyFbm(p * 8.0f, 8u, CloudOctavesFor(8u, size, 3u));
    const float w16 = CloudWorleyFbm(p * 16.0f, 16u, CloudOctavesFor(16u, size, 3u));
    const float w32 = CloudWorleyFbm(p * 32.0f, 32u, CloudOctavesFor(32u, size, 3u));
    OutVolume[id] = float4(perlinWorley, w8, w16, w32);
}

[numthreads(8, 8, 1)]
[RootSignature(CLOUD_NOISE_RS)]
void CSDetail(uint3 id : SV_DispatchThreadID)
{
    const uint size = noiseParams.x;
    if (any(id >= size)) { return; }
    const float3 p = (float3(id) + 0.5f) / float(size);
    // 128^3: 4..16 / 8..32 / 16..32 cells, never finer than four texels (see the header).
    OutVolume[id] = float4(CloudWorleyFbm(p * 4.0f, 4u, CloudOctavesFor(4u, size, 3u)),
                           CloudWorleyFbm(p * 8.0f, 8u, CloudOctavesFor(8u, size, 3u)),
                           CloudWorleyFbm(p * 16.0f, 16u, CloudOctavesFor(16u, size, 3u)), 0.0f);
}
#else // CLOUD_NOISE_2D

[numthreads(8, 8, 1)]
[RootSignature(CLOUD_NOISE_RS)]
void CSWeather(uint3 id : SV_DispatchThreadID)
{
    const uint size = noiseParams.x;
    if (any(id.xy >= size)) { return; }
    const float2 p = (float2(id.xy) + 0.5f) / float(size);
    // Two independent 2D fields (a fixed third coordinate picks a slice of the 3D lattice); the
    // coverage field is low-frequency so a cloud reads as a kilometre-scale object, the type field
    // lower still so a whole formation shares one character.
    const float coverage = CloudPerlinFbm(float3(p * 6.0f, 0.37f), 6u, 5u);
    const float type = CloudPerlinFbm(float3(p * 3.0f, 0.71f), 3u, 3u);
    // Stretch the coverage field's contrast so the threshold knob has room on both sides.
    const float c = saturate(CloudRemapNoise(coverage, 0.35f, 0.7f, 0.0f, 1.0f));
    OutPlane[id.xy] = float4(c, type, 0.0f, 0.0f);
}
#endif // CLOUD_NOISE_2D
