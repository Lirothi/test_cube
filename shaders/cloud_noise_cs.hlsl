// Volumetric clouds, plan C1: the textures the density model reads (cloud_common.hlsli), generated on
// the GPU when the cloud resources are created and again when their inputs change (VolumetricCloud::
// BuildNoise). All tile: the lattice and the Worley cells wrap at the texture's period, so a sample
// at any scale is seamless.
//
// Four entry points, one file; the C++ side dispatches each over its own texture:
//   CSFormation 256 x 256 x 128  RGBA8  r: the formation field, b: the floor variation (seed, roughness)
//   CSCells     64 x 64 x 64     RGBA8  r: the erosion, g: unused, b: the fine detail's mask (seed)
//   CSWeather   512 x 512        RGBA8  r: the macro variation's field, g: a second field, unused (seed)
//   CSProfile   64 x 256         RGBA8  the profile LUT: R envelope, G density, B erosion (its knobs)
// All values are in [0, 1].
//
// THE SMALLEST CELL IS AT LEAST TWO TEXELS WIDE (four wherever the fit allowed it). The first noise
// set ran its Worley octaves down to one texel per cell, which makes the texture texel noise, and
// trilinear filtering of texel noise is a LATTICE: the trace read it back as long parallel streaks
// along the world axes, foreshortened into radial bands across every thick cloud (owner,
// 2026-09-12) -- the rule any mip-mapped texture already obeys.
#define CLOUD_NOISE_RS "CBV(b0), DescriptorTable(UAV(u0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer NoiseCB : register(b0)
{
    uint4 noiseParams;      // x: size (per axis; the width for the formation/profile), y: seed, z: depth (formation) / height (profile), w: 0
    float4 formationParams; // x: formation roughness (fBm gain), yzw: unused
    float4 profileA;        // x: height scale, y: dome, z: top softness, w: base softness
    float4 profileB;        // x: small-cloud density, y: body density, z: base erosion, w: erosion rise
    float4 profileC;        // x: heavy-cover erosion cut, y: heavy-cover thinning, zw: unused
};

// One UAV slot, two shapes: the plane kernels (weather, profile) are compiled with CLOUD_NOISE_2D and
// see a 2D target at u0, the volume kernels a 3D one. A single file cannot declare both at one register.
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

float CloudRemapNoise(float v, float lo, float hi, float newLo, float newHi)
{
    return newLo + (v - lo) / max(hi - lo, 1.0e-5f) * (newHi - newLo);
}

// ---------------------------------------------------------------------------------------------
// THE DENSITY MODEL's volumes (cloud_common.hlsli): ours, in the layout of Ultra Dynamic Sky's
// FormationVolume / 3D_Cells, tuned offline against their statistics (2026-10-12) -- the correlation
// length and the time-axis rate of the formation, the cell scales of the erosion, each channel's value
// distribution, and the connectivity of its high and low parts -- and generated here, at load.
// A SALT offsets the hash per channel so the channels are independent fields of one lattice.
float CloudPerlinSalted(float3 p, uint period, uint salt)
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
        n[i] = dot(CloudGradient(wrapped + salt), f - float3(corner));
    }
    const float x0 = lerp(lerp(n[0], n[1], u.x), lerp(n[2], n[3], u.x), u.y);
    const float x1 = lerp(lerp(n[4], n[5], u.x), lerp(n[6], n[7], u.x), u.y);
    return saturate(lerp(x0, x1, u.z) * 0.5f + 0.5f);
}

// fBm with a gain; returns (value - 0.5) / its expected deviation -- a STANDARD score, so the remap
// tables below keep the target distribution whatever the gain (measured: gain 0.55 and 0.85 both
// land on the target mean and deviation to 3 decimals).
static const float kCloudPerlinSigma = 0.0962f; // one octave's deviation (offline, 400k samples)
float CloudPerlinFbmZ(float3 p, uint period, uint octaves, float gain, uint salt)
{
    float sum = 0.0f, amp = 0.5f, norm = 0.0f, squares = 0.0f;
    [loop] for (uint o = 0; o < octaves; ++o)
    {
        sum += amp * CloudPerlinSalted(p, period, salt);
        norm += amp; squares += amp * amp;
        p *= 2.0f; period *= 2u; amp *= gain;
    }
    return (sum / norm - 0.5f) / (kCloudPerlinSigma * sqrt(squares) / norm);
}

float CloudWorleySalted(float3 p, uint period, uint salt)
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
        const float3 point_ = float3(offset) + CloudHash3(wrapped + 7919u + salt);
        const float3 d = point_ - f;
        minDist = min(minDist, dot(d, d));
    }
    return saturate(sqrt(minDist)); // F1 distance: 0 on a feature point
}

// F1 Worley fBm (0.625 / 0.25 / 0.125) as a standard score, as above.
static const float kCloudWorleyMean = 0.5205f, kCloudWorleySigma = 0.1726f;
float CloudWorleyFbmZ(float3 p, uint period, uint octaves, uint salt)
{
    float sum = CloudWorleySalted(p, period, salt) * 0.625f, norm = 0.625f, squares = 0.625f * 0.625f;
    if (octaves >= 2u) { sum += CloudWorleySalted(p * 2.0f, period * 2u, salt) * 0.25f; norm += 0.25f; squares += 0.0625f; }
    if (octaves >= 3u) { sum += CloudWorleySalted(p * 4.0f, period * 4u, salt) * 0.125f; norm += 0.125f; squares += 0.015625f; }
    return (sum / norm - kCloudWorleyMean) / (kCloudWorleySigma * sqrt(squares) / norm);
}

// Standard score -> the channel's target distribution: 33 knots over z in [-3.5, 3.5], from the
// offline quantile match (a Beta law with the channel's target mean and deviation; the cells' r and b
// and the formation's b a shifted Gamma that also carries the target skew).
static const float kFormationRRemap[33] = { 0.0275f, 0.0375f, 0.0479f, 0.0580f, 0.0685f, 0.0805f, 0.0931f, 0.1064f, 0.1205f, 0.1354f, 0.1508f, 0.1673f, 0.1843f, 0.2020f, 0.2205f, 0.2398f, 0.2598f, 0.2806f, 0.3020f, 0.3241f, 0.3466f, 0.3697f, 0.3937f, 0.4183f, 0.4433f, 0.4691f, 0.4952f, 0.5211f, 0.5468f, 0.5733f, 0.6003f, 0.6296f, 0.6545f };
static const float kFormationBRemap[33] = { 0.1955f, 0.2052f, 0.2185f, 0.2307f, 0.2443f, 0.2574f, 0.2706f, 0.2846f, 0.2992f, 0.3144f, 0.3303f, 0.3465f, 0.3634f, 0.3809f, 0.3989f, 0.4182f, 0.4380f, 0.4586f, 0.4799f, 0.5025f, 0.5263f, 0.5516f, 0.5781f, 0.6057f, 0.6360f, 0.6682f, 0.7024f, 0.7379f, 0.7768f, 0.8195f, 0.8635f, 0.9155f, 0.9775f };
static const float kCellsRRemap[33] = { 0.0637f, 0.0637f, 0.0908f, 0.1187f, 0.1530f, 0.1845f, 0.2196f, 0.2499f, 0.2796f, 0.3088f, 0.3382f, 0.3676f, 0.3966f, 0.4258f, 0.4552f, 0.4852f, 0.5160f, 0.5474f, 0.5795f, 0.6127f, 0.6466f, 0.6824f, 0.7205f, 0.7598f, 0.8023f, 0.8457f, 0.8883f, 0.9305f, 0.9750f, 1.0000f, 1.0000f, 1.0000f, 1.0000f };
static const float kCellsGRemap[33] = { 0.0489f, 0.0667f, 0.0972f, 0.1266f, 0.1555f, 0.1826f, 0.2127f, 0.2423f, 0.2719f, 0.3007f, 0.3300f, 0.3598f, 0.3896f, 0.4193f, 0.4496f, 0.4803f, 0.5114f, 0.5425f, 0.5737f, 0.6048f, 0.6359f, 0.6662f, 0.6968f, 0.7261f, 0.7548f, 0.7824f, 0.8101f, 0.8370f, 0.8624f, 0.8854f, 0.9096f, 0.9348f, 0.9752f };
static const float kCellsBRemap[33] = { 0.0436f, 0.0436f, 0.0436f, 0.0436f, 0.0921f, 0.1252f, 0.1624f, 0.1899f, 0.2208f, 0.2527f, 0.2862f, 0.3209f, 0.3569f, 0.3924f, 0.4288f, 0.4658f, 0.5035f, 0.5425f, 0.5833f, 0.6257f, 0.6696f, 0.7166f, 0.7664f, 0.8191f, 0.8749f, 0.9355f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f };
float CloudRemapZ(float z, const float table[33])
{
    const float t = clamp((z + 3.5f) / 7.0f * 32.0f, 0.0f, 32.0f);
    const uint i = min((uint)t, 31u);
    return lerp(table[i], table[i + 1u], t - (float)i);
}

float CloudSmooth(float e0, float e1, float x)
{
    const float t = saturate((x - e0) / max(e1 - e0, 1.0e-6f));
    return t * t * (3.0f - 2.0f * t);
}

#ifndef CLOUD_NOISE_2D
// The formation volume, 256 x 256 x 128: x/y the cloud field over one tile, z a LOOPING time axis --
// one isotropic tiling 3D field, so z changes ~1.75x faster per slice than x per texel, as UDS's does.
//   r: the formation -- Perlin fBm, 6 cells a period, 4 octaves (never finer than 4 texels), gain =
//      roughness, remapped to mean 0.267 / dev 0.093. 7 cells at gain 0.7 matched UDS's correlation
//      profile, and still read as round, peaked puffs: inside a coverage-0.4 blob the field was more
//      peaked (share above 0.25 of the part above 0.1: 0.165, UDS 0.142) and the blob edges smoother
//      (edge / area 0.313, UDS 0.389) -- a peaked blob makes a tall round cloud, a flat ragged one the
//      flat, stretched-looking clouds UDS has. 6 cells at 0.85: 0.151 / 0.374, blobs 117 px (UDS 122);
//      at coverage 0.5 edge 0.249 (UDS 0.252).
//   g: unused (UDS's layout);
//   b: the floor variation (how far a cloud's base is lifted, cloud_common.hlsli): 0.40 of the
//      formation's OWN standard score + 0.92 of a Perlin fBm (7 cells, 4 octaves, gain 0.5), remapped
//      to mean 0.447 / dev 0.097 / skew +0.62 (a shifted Gamma). UDS's B is not independent of R: it
//      correlates 0.42 with it, so bases lift most UNDER the dense parts (0.468 inside a coverage-0.6
//      cloud, 0.401 outside); the first version was an independent fBm (correlation 0.00, skew +0.09)
//      that scattered the ragged bases at random. Measured on the generated volume (2026-10-12):
//      correlation with R 0.415 (UDS 0.416), mean inside / outside a coverage-0.6 cloud 0.467 / 0.402
//      (UDS 0.468 / 0.401), skew +0.56, xy autocorrelation at 2/4/8/16 texels 0.95/0.87/0.69/0.33
//      (UDS 0.93/0.85/0.66/0.31), along the time axis at 1/4/8 slices 0.96/0.69/0.35 (0.94/0.68/0.33).
//      THE SCALE IS WHAT THE EYE SEES: the floor is relief, and near the horizon it is seen edge-on.
//      8 cells at gain 0.6 had the same moments and correlation but 27 % of its variance at 8-32
//      cycles a tile (UDS 18 %) and too little at 2-4 (23 %, UDS 31 %), and at coverage 0.8 that fine
//      relief drew dark "hills" along the horizon UDS never shows. Share of the variance per octave
//      band 1-2 / 2-4 / 4-8 / 8-16 / 16-32 / 32-64 cycles: 0.083 0.301 0.424 0.134 0.045 0.013 (UDS
//      0.059 0.308 0.427 0.135 0.043 0.019); fitted offline with a twin of this kernel (scratch
//      formB_search.py, which reproduced the old kernel's bands to 0.006). The table and the seed were
//      ruled out first: a symmetric table and another seed drew the hills too.
[numthreads(8, 8, 1)]
[RootSignature(CLOUD_NOISE_RS)]
void CSFormation(uint3 id : SV_DispatchThreadID)
{
    const uint width = noiseParams.x, depth = noiseParams.z;
    if (any(id.xy >= width) || id.z >= depth) { return; }
    const float3 p = (float3(id) + 0.5f) / float3(width, width, depth);
    const float zR = CloudPerlinFbmZ(p * 6.0f, 6u, 4u, formationParams.x, 0u);
    const float r = CloudRemapZ(zR, kFormationRRemap);
    const float zB = 0.40f * zR + 0.91652f * CloudPerlinFbmZ(p * 7.0f, 7u, 4u, 0.5f, 1013u); // 0.91652 = sqrt(1 - 0.40^2)
    const float b = CloudRemapZ(zB, kFormationBRemap);
    OutVolume[id] = float4(r, 0.0f, b, 1.0f);
}

// The erosion volume, 64^3 (UDS's 3D_Cells layout), each channel a standard-score mix remapped to its
// target law (mean / deviation / skew 0.520 / 0.112 / +0.19, 0.512 / 0.139, 0.510 / 0.128 / +0.33):
//   r  the erosion: F1 Worley fBm, 8 cells, 3 octaves, 0.7 of it + 0.3 Perlin fBm, 6 cells;
//   g  F1 Worley fBm, 8 cells, 2 octaves (unused by the clouds, UDS's layout);
//   b  the high-frequency mask: 0.5 F1 Worley fBm 6 cells 2 octaves + 0.5 Perlin fBm 4 cells.
// WHY THE MIXES (2026-10-12): a single-octave Worley -- the first version, which matched UDS's
// histogram and autocorrelation -- is a lattice of identical round cells, and where erosion is weak
// (its low values, the cell centres) those are ISOLATED pockets: the erosion leaves one ball per cell,
// the "popcorn" the owner saw. Neither the histogram nor the autocorrelation can see that (both are
// the same for x and its re-arrangements); connectivity can. Of the lowest 20 % of r, the largest
// connected piece held 85 % in UDS's volume, 10 % in that version, 56 % in this one (as generated
// here, seed 666; 0.55-0.75 over seeds offline); the small isolated pockets went 38 -> 11 per 10^4
// texels (UDS 9.9). b: 0.07 -> 0.81 (UDS 0.76). Measured with --cloud-dump (scratch struct_cmp.py).
[numthreads(8, 8, 1)]
[RootSignature(CLOUD_NOISE_RS)]
void CSCells(uint3 id : SV_DispatchThreadID)
{
    const uint size = noiseParams.x;
    if (any(id >= size)) { return; }
    const float3 p = (float3(id) + 0.5f) / float(size);
    const float r = 0.7f * CloudWorleyFbmZ(p * 8.0f, 8u, 3u, 2027u) + 0.3f * CloudPerlinFbmZ(p * 6.0f, 6u, 3u, 0.5f, 2039u);
    const float b = 0.5f * CloudWorleyFbmZ(p * 6.0f, 6u, 2u, 4057u) + 0.5f * CloudPerlinFbmZ(p * 4.0f, 4u, 3u, 0.5f, 4073u);
    OutVolume[id] = float4(CloudRemapZ(r, kCellsRRemap),
                           CloudRemapZ(CloudWorleyFbmZ(p * 8.0f, 8u, 2u, 3041u), kCellsGRemap),
                           CloudRemapZ(b, kCellsBRemap), 1.0f);
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

// THE PROFILE LUT (cloud_common.hlsli CloudSampleProfile), 64 x 256: u = coverage column, row 0 =
// the top of the layer. Ours: a formula written from a design, its fixed constants and the knobs'
// defaults FITTED to UDS's LUT (2026-10-12, least squares over coverage 0.2 .. 1; rms vs their LUT, mid
// 0.2-0.7 / heavy 0.75-1: R 0.029 / 0.029, G 0.026 / 0.029, B 0.036 / 0.019). The first fit covered
// 0.2 .. 0.7 only and left heavy cover at R 0.137 / B 0.091: UDS's heavy columns do not close their
// envelope at the layer top -- their density (G) shapes the top alone -- and their floor does not
// erode, which a formula without heavy-cover terms could not follow.
// Every named term is a level knob (VolumetricCloudSettings profile*):
//   top(u)  how high a formation reaches, a fraction of the layer -- quadratic in coverage (0.20 at
//           none, 0.5 at 0.30, the whole layer from 0.53), times the height scale: fair-weather puffs
//           stay low, heavy cover fills the layer. Every shape below is a function of x = h / top,
//           so a column is one cloud stretched to its top;
//   R       the envelope, a DOME: whole over the lowest 11 % of x (a full base -- tapering it from
//           the floor thinned every small cloud's footprint, 15 % less cloud at coverage 0.4), then
//           declining by `dome` per unit of x, so only a formation's dense middle reaches up, and
//           closing over the last `top softness` of x. Under heavy cover (heavyR, from ~0.45) the
//           whole part reaches up to 84 % of x, the dome flattens by up to 59 % and the closing
//           fades out: an overcast deck is full to the top of the layer;
//   G       density (cubed in the shader): `small density` at no coverage rising to `body density`
//           at full (80 % of the way by 0.45 -- a developed cumulus field), a softer underside over
//           `base softness` of the height (much softer under heavy cover), cover above ~0.4 thinning
//           with height by up to `heavy thinning`, and a fade over the top quarter of x (a little
//           lower under heavy cover);
//   B       detail erosion: `base erosion` at the floor (none under heavy cover: a smooth deck's
//           underside) rising to full by `erosion rise` of the top, heavy cover eroding up to `heavy
//           cut` less.
[numthreads(8, 8, 1)]
[RootSignature(CLOUD_NOISE_RS)]
void CSProfile(uint3 id : SV_DispatchThreadID)
{
    const uint width = noiseParams.x, height = noiseParams.z;
    if (id.x >= width || id.y >= height) { return; }
    const float u = (float(id.x) + 0.5f) / float(width);
    const float h = 1.0f - (float(id.y) + 0.5f) / float(height);
    const float top = min(max(profileA.x, 0.05f) * (0.20f + 0.25f * u + 2.54f * u * u), 1.0f);
    const float x = h / top;
    const float heavyR = CloudSmooth(0.42f, 1.37f, u);
    const float closing = saturate((1.0f - x) / max(profileA.z, 0.01f));
    const float r = CloudSmooth(0.0f, 0.01f, h)
                  * saturate(1.0f - profileA.y * (1.0f - 0.59f * heavyR) * max(x - (0.11f + 0.73f * heavyR), 0.0f))
                  * lerp(closing, 1.0f, heavyR);
    const float developed = 0.85f * min(u, 0.45f) + 0.4175f * CloudSmooth(0.15f, 0.45f, u) + 0.2f * CloudSmooth(0.45f, 0.8f, u);
    const float body = lerp(profileB.x, profileB.y, saturate(developed));
    const float heavy = CloudSmooth(0.5f, 1.0f, u);
    const float base = 0.31f + 0.69f * CloudSmooth(0.0f, profileA.w * min(top, 1.0f), h)
                     - 0.46f * heavy * (1.0f - CloudSmooth(0.0f, 0.31f, h));
    const float thinning = 1.0f - profileC.y * CloudSmooth(0.38f, 0.78f, u) * saturate(x);
    const float fadeShift = 0.056f * CloudSmooth(0.58f, 0.83f, u);
    float g = body * base * thinning * (1.0f - CloudSmooth(0.73f - fadeShift, 1.0f - fadeShift, x));
    if (h < 0.5f / float(height) + 1.0e-6f) { g = 0.05f; } // the floor row: barely there
    const float erosionMax = 1.0f - profileC.x * CloudSmooth(0.43f, 0.83f, u);
    const float rise = min(top, 1.0f) * (profileB.w + 0.39f * CloudSmooth(0.3f, 0.8f, u));
    const float baseErosion = profileB.z * (1.0f - CloudSmooth(0.57f, 0.77f, u));
    const float b = baseErosion + (erosionMax - baseErosion) * CloudSmooth(0.0f, rise, h);
    OutPlane[id.xy] = float4(saturate(r), saturate(g), saturate(b), 1.0f);
}
#endif // CLOUD_NOISE_2D
