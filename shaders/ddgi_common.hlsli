// Interior lighting plan D: the GI probe volume (DDGI, Majercik et al. 2019 -- the RTXGI SDK's layout
// and formulas, file references below). Shared by the trace (ddgi_trace_cs.hlsl), the probe update
// (ddgi_update_cs.hlsl) and the consumers (lighting_cs.hlsl, the trace's own infinite bounce).
//
// What the probes hold is the BOUNCE only: light arriving from geometry. A ray that misses brings
// nothing back -- the sky reaches a surface through the RT sky visibility (plan S), at pixel resolution,
// and counting it here as well would light every surface with it twice. So the volume adds to the
// frame instead of replacing a term of it, and its edge fades an addition, not a seam between models.
//
// The grid is camera-centred and SCROLLS: probe slot (i, j, k) holds whichever world cell c in
// [base, base + dims) has c mod dims == slot, so a step of the camera re-targets one plane of slots and
// leaves the rest in place. Every slot records the cell (and the volume generation) it was traced for;
// a slot whose record does not match is a probe nobody has traced yet and weighs nothing.
#ifndef DDGI_COMMON_HLSLI
#define DDGI_COMMON_HLSLI

static const int3 kDdgiDims = int3(32, 16, 32);       // probes; mirrors DdgiVolume::kDims
static const uint kDdgiProbeCount = 32u * 16u * 32u;
static const uint kDdgiRays = 64u;                    // per probe per update; mirrors DdgiVolume::kRaysPerProbe
static const uint kDdgiIrrInterior = 6u;              // octahedral texels per side, + a 1-texel border each
static const uint kDdgiDepthInterior = 14u;
static const uint kDdgiStateValid = 1u;               // ProbeState.w bit 0: traced for the cell in .xyz
static const uint kDdgiStateInactive = 2u;            // bit 1: inside geometry (RTXGI probe classification)
static const uint kDdgiStateChangeShift = 2u;         // bits 2-3: consecutive large changes (ddgi_update_cs), bit 4 up

// RTXGI DDGISignNotZero / DDGIGetOctahedralCoordinates / DDGIGetOctahedralDirection (Irradiance.hlsl).
float2 DdgiSignNotZero(float2 v) { return float2(v.x >= 0.0f ? 1.0f : -1.0f, v.y >= 0.0f ? 1.0f : -1.0f); }
float2 DdgiOctEncode(float3 d)
{
    float2 uv = d.xy * (1.0f / (abs(d.x) + abs(d.y) + abs(d.z)));
    if (d.z < 0.0f) { uv = (1.0f - abs(uv.yx)) * DdgiSignNotZero(uv); }
    return uv;
}
float3 DdgiOctDecode(float2 uv)
{
    float3 d = float3(uv.x, uv.y, 1.0f - abs(uv.x) - abs(uv.y));
    if (d.z < 0.0f) { d.xy = (1.0f - abs(d.yx)) * DdgiSignNotZero(d.xy); }
    return normalize(d);
}

// RTXGI SphericalFibonacci: the n ray directions of one update, before the frame's random rotation.
float3 DdgiSphericalFibonacci(float i, float n)
{
    const float kPhiMinusOne = 0.6180339887f;
    const float fraction = frac(i * kPhiMinusOne);
    const float phi = 6.2831853f * fraction;
    const float cosTheta = 1.0f - (2.0f * i + 1.0f) / n;
    const float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

float DdgiSquare(float x) { return x * x; }
int3 DdgiPositiveMod(int3 a, int3 m) { return ((a % m) + m) % m; }
uint3 DdgiSlotOfCell(int3 cell) { return uint3(DdgiPositiveMod(cell, kDdgiDims)); }
uint DdgiSlotIndex(uint3 slot) { return slot.x + uint(kDdgiDims.x) * (slot.y + uint(kDdgiDims.y) * slot.z); }
uint3 DdgiSlotFromIndex(uint index)
{
    const uint x = index % uint(kDdgiDims.x);
    const uint yz = index / uint(kDdgiDims.x);
    return uint3(x, yz % uint(kDdgiDims.y), yz / uint(kDdgiDims.y));
}
// The world cell slot `slot` stands for in a volume starting at cell `base`.
int3 DdgiCellOfSlot(uint3 slot, int3 base) { return base + DdgiPositiveMod(int3(slot) - base, kDdgiDims); }
// The slot's tile in the atlases: x runs over i and j, y over k.
uint2 DdgiTile(uint3 slot) { return uint2(slot.x + uint(kDdgiDims.x) * slot.y, slot.z); }

// RTXGI DDGIProbeBorderUpdate: the interior texel a border texel of an (n + 2)^2 tile copies, so that
// bilinear filtering across the tile edge sees the octahedron's own neighbour.
uint2 DdgiBorderSource(uint2 t, uint n)
{
    const uint last = n + 1u;
    const bool cornerX = t.x == 0u || t.x == last;
    const bool cornerY = t.y == 0u || t.y == last;
    if (cornerX && cornerY) { return uint2(t.x > 0u ? 1u : n, t.y > 0u ? 1u : n); }
    if (t.y == 0u || t.y == last) { return uint2(last - t.x, t.y > 0u ? n : 1u); } // a row
    if (cornerX) { return uint2(t.x > 0u ? n : 1u, last - t.y); }                  // a column
    return t;
}
// The direction an interior texel (1..n in the tile) stands for.
float3 DdgiTexelDirection(uint2 interior, uint n)
{
    return DdgiOctDecode(((float2(interior) - 1.0f + 0.5f) / float(n)) * 2.0f - 1.0f);
}

struct DdgiVolumeDesc
{
    int3 base;          // the volume's first world cell
    uint generation;    // bumped when the volume starts over (spacing, level)
    float spacing;      // m between probes
};

// The probe in `cell` if it was traced for it in this generation and is not inside geometry.
bool DdgiProbeUsable(StructuredBuffer<uint4> state, DdgiVolumeDesc v, int3 cell)
{
    const uint4 s = state[DdgiSlotIndex(DdgiSlotOfCell(cell))];
    return all(int3(asint(s.x), asint(s.y), asint(s.z)) == cell) && (s.w >> 8) == v.generation &&
           (s.w & (kDdgiStateValid | kDdgiStateInactive)) == kDdgiStateValid;
}

float2 DdgiAtlasUv(uint3 slot, float3 dir, uint n, float2 invAtlasSize)
{
    const float2 tileOrigin = float2(DdgiTile(slot) * (n + 2u));
    return (tileOrigin + 1.0f + (DdgiOctEncode(dir) * 0.5f + 0.5f) * float(n)) * invAtlasSize;
}

// RTXGI DDGIGetVolumeIrradiance (Irradiance.hlsl), with the self-shadow bias of DDGIGetSurfaceBias
// (normal 0.2 / view 0.8 of 0.3 x 0.75 spacing). `weight` = how much of the volume this point is in:
// 1 inside, 0 at and beyond its last plane of probes (the caller fades the bounce with it), and 0
// when no usable probe surrounds it. P the surface, N its normal, V toward the camera (or the ray's
// origin, for the trace's infinite bounce).
float3 DdgiIrradiance(Texture2D irradiance, Texture2D depth, StructuredBuffer<uint4> state, SamplerState linearClamp,
                      DdgiVolumeDesc v, float3 P, float3 N, float3 V, out float weight)
{
    weight = 0.0f;
    const float3 biased = P + (N * 0.2f + V * 0.8f) * (0.75f * v.spacing * 0.3f);
    const float3 g = biased / v.spacing;
    const int3 cell0 = int3(floor(g));
    // Trilinear needs both planes of every axis inside the volume.
    if (any(cell0 < v.base) || any(cell0 + 1 > v.base + kDdgiDims - 1)) { return 0.0f.xxx; }
    const float3 alpha = g - float3(cell0);

    uint irrW, irrH, depW, depH;
    irradiance.GetDimensions(irrW, irrH);
    depth.GetDimensions(depW, depH);
    const float2 invIrr = 1.0f / float2(irrW, irrH);
    const float2 invDep = 1.0f / float2(depW, depH);

    float3 sum = 0.0f.xxx;
    float wsum = 0.0f;
    [unroll]
    for (uint i = 0u; i < 8u; ++i)
    {
        const int3 offset = int3(i & 1u, (i >> 1) & 1u, (i >> 2) & 1u);
        const int3 cell = cell0 + offset;
        if (!DdgiProbeUsable(state, v, cell)) { continue; }
        const uint3 slot = DdgiSlotOfCell(cell);
        const float3 probePos = float3(cell) * v.spacing;
        const float3 trilinear3 = lerp(1.0f - alpha, alpha, float3(offset));
        const float trilinear = trilinear3.x * trilinear3.y * trilinear3.z;

        // Smooth backface: a probe behind the surface counts for little.
        const float3 toProbe = normalize(probePos - P);
        float w = DdgiSquare((dot(toProbe, N) + 1.0f) * 0.5f) + 0.2f;

        // Chebyshev visibility against the probe's mean and mean-square distance in this direction.
        const float3 probeToBiased = biased - probePos;
        const float dist = length(probeToBiased);
        const float2 moments = depth.SampleLevel(linearClamp, DdgiAtlasUv(slot, probeToBiased / max(dist, 1.0e-6f),
                                                                          kDdgiDepthInterior, invDep), 0).rg;
        if (dist > moments.x)
        {
            const float variance = abs(DdgiSquare(moments.x) - moments.y);
            const float d = dist - moments.x;
            const float chebyshev = variance / (variance + d * d);
            w *= max(chebyshev * chebyshev * chebyshev, 0.0f);
        }
        w = max(1.0e-6f, w);
        // RTXGI: crush tiny weights so a nearly-occluded probe does not leak.
        const float kCrush = 0.2f;
        if (w < kCrush) { w *= w * w / (kCrush * kCrush); }
        w *= trilinear;

        sum += w * irradiance.SampleLevel(linearClamp, DdgiAtlasUv(slot, N, kDdgiIrrInterior, invIrr), 0).rgb;
        wsum += w;
    }
    if (wsum <= 0.0f) { return 0.0f.xxx; }

    // Fade over the outermost cell of the volume (the bounce, not a model, ends there).
    const float3 inside = min(g - float3(v.base), float3(v.base + kDdgiDims - 1) - g);
    weight = saturate(min(inside.x, min(inside.y, inside.z)));
    return sum / wsum;
}

#endif // DDGI_COMMON_HLSLI
