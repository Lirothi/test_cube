#define GTAO_FILTER_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE))"

// P6B item 3 -- bilateral denoise of the raw GTAO, half resolution in and out.
//
// The raw pass takes `numAngles * numSteps` taps per pixel (2 x 6 by default) with a per-pixel
// rotated direction set, so its output is a low-frequency signal buried in per-pixel noise. This
// pass removes the noise WITHOUT crossing a silhouette, which is the only thing that makes a wide
// blur acceptable on an occlusion term: blur across a contact edge and the contact itself is what
// you lose.
//
// Transcribed from Unreal's `GTAOSpatialFilterCS` (PostProcessAmbientOcclusion.usf), minus their
// LDS staging (they run 5x5 over a 16x8 group and cache a 20x12 border in groupshared; at our tap
// count the direct loads are not the bottleneck and the LDS version is a lot of code to get wrong).
// Two things are theirs and are the reason this works at all:
//
//   * THE WEIGHT IS AGAINST A FITTED PLANE, not against the centre depth. A floor seen at a grazing
//     angle has an enormous depth range inside a 5x5 window and a plain |z - zCentre| test would
//     refuse to blur it at all -- precisely where the noise is worst.
//   * THE GRADIENT IS FITTED IN DEVICE Z. For a planar surface, device z is an affine function of
//     1/viewZ, and 1/viewZ interpolates LINEARLY in screen space. So a plane really is a plane in
//     device z and the two-tap extrapolation below is exact for one; in linear depth it would not
//     be. This is not a detail -- it is why the plane fit is worth doing.
//
// Added beyond UE: a NORMAL term. The plan asks for "depth and normal discontinuities" and depth
// alone cannot separate two faces of a convex edge -- they are depth-continuous and the AO on them
// is not. UE gets away with depth alone because their AO is consumed at a coarser scale.
//
// P16.4: two channels now, not one. The weights are computed from depth and normal alone, so both
// scales are filtered by the SAME kernel with the SAME weights -- which is what keeps them
// comparable at the consumer, where the two are combined per pixel.
//
// t0: raw AO (half res, RG8 -- .x contact scale, .y sky scale)
// t1: scene depth (render res; sampled at the AO texel's own UV, see DeviceZAt)
// t2: GB1 -- world normal encoded in xyz
// u0: filtered AO (half res, RG8)

Texture2D AoTex : register(t0);
Texture2D DepthTex : register(t1);
Texture2D GB1 : register(t2);
RWTexture2D<float2> AoOut : register(u0);

SamplerState gSmpPoint : register(s0);
SamplerState gSmpLinear : register(s1);

// One layout shared by all three P6B filter kernels -- see GtaoFilterConstants on the C++ side.
// Each kernel reads the subset it needs; a single layout is one struct, one Populate and one
// writer instead of three that drift apart.
cbuffer GtaoFilterCB : register(b0)
{
    float2 aoSize;         // half-res AO grid, texels
    float2 invAoSize;
    float2 outSize;        // this kernel's output grid (== aoSize here)
    float2 invOutSize;
    float  depthA;         // linearZ = depthB / (deviceZ - depthA)
    float  depthB;
    float  planeTolerance; // WORLD metres off the fitted plane before a tap stops counting
    float  blendWeight;    // temporal kernel only
    float  upsampleTolerance; // upsample kernel only
    uint   historyValid;   // temporal kernel only
    uint   filterRadius;   // taps each side; 2 = the 5x5 UE uses
    float  temporalClampRange; // temporal kernel only
};

// The exponent on the normal term. 8 keeps a 45-degree crease apart (0.707^8 = 0.06) while leaving
// the few degrees of interpolation wobble inside a smooth surface untouched (0.99^8 = 0.92).
static const float kNormalSharpness = 8.0f;

float LinearZ(float deviceZ)
{
    return depthB / max(deviceZ - depthA, 1e-8f);
}

int2 ClampAo(int2 p)
{
    return clamp(p, int2(0, 0), int2(aoSize) - int2(1, 1));
}

// Depth for an AO texel, fetched exactly the way the raw pass fetched it: point-sampled at the AO
// texel's own UV. Deriving the full-res texel arithmetically instead would disagree with the raw
// pass whenever the render resolution is odd, and then the plane fit would be fitting a plane
// through depths that belong to different pixels than the AO does.
float DeviceZAt(int2 aoPx)
{
    const float2 uv = (float2(ClampAo(aoPx)) + 0.5f) * invAoSize;
    return DepthTex.SampleLevel(gSmpPoint, uv, 0).r;
}

float3 NormalAt(int2 aoPx)
{
    const float2 uv = (float2(ClampAo(aoPx)) + 0.5f) * invAoSize;
    return normalize(GB1.SampleLevel(gSmpPoint, uv, 0).xyz * 2.0f - 1.0f);
}

// UE's one-sided gradient pick: extrapolate the centre from each side and keep the side that
// predicts it better. On a silhouette one side belongs to the other object and its extrapolation
// misses badly, so this chooses the side the centre pixel actually lies on.
float AxisGradient(float centreZ, float m2, float m1, float p1, float p2)
{
    const float predictedFromMinus = abs((m1 + (m1 - m2)) - centreZ);
    const float predictedFromPlus = abs((p1 + (p1 - p2)) - centreZ);
    return (predictedFromMinus < predictedFromPlus) ? (m1 - m2) : (p2 - p1);
}

[numthreads(8, 8, 1)]
[RootSignature(GTAO_FILTER_CS_RS)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)aoSize.x || tid.y >= (uint)aoSize.y)
    {
        return;
    }

    const int2 px = int2(tid.xy);
    const float2 centreAo = AoTex.Load(int3(px, 0)).rg;
    const float centreZ = DeviceZAt(px);
    const float centreLinearZ = LinearZ(centreZ);
    const float3 centreN = NormalAt(px);

    const float2 gradient = float2(
        AxisGradient(centreZ, DeviceZAt(px - int2(2, 0)), DeviceZAt(px - int2(1, 0)),
                              DeviceZAt(px + int2(1, 0)), DeviceZAt(px + int2(2, 0))),
        AxisGradient(centreZ, DeviceZAt(px - int2(0, 2)), DeviceZAt(px - int2(0, 1)),
                              DeviceZAt(px + int2(0, 1)), DeviceZAt(px + int2(0, 2))));

    // The tolerance is authored in WORLD metres and converted here, because a fixed device-z
    // tolerance means a different physical distance at every depth. d(linearZ)/d(deviceZ) is
    // -linearZ^2 / depthB, so `metres` off the plane is `metres * depthB / linearZ^2` in device z.
    const float deviceTolerance =
        max(planeTolerance * depthB / max(centreLinearZ * centreLinearZ, 1e-6f), 1e-9f);

    const int radius = (int)min(filterRadius, 4u);

    float2 sumAo = float2(0.0f, 0.0f);
    float sumWeight = 0.0f;
    for (int y = -radius; y <= radius; ++y)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            const int2 tap = ClampAo(px + int2(x, y));
            const float tapZ = DeviceZAt(tap);

            // Where the centre's plane says this tap's depth should be.
            const float planeZ = centreZ + gradient.x * (float)x + gradient.y * (float)y;
            const float depthWeight = 1.0f - saturate(abs(planeZ - tapZ) / deviceTolerance);

            const float3 tapN = NormalAt(tap);
            const float normalWeight = pow(saturate(dot(tapN, centreN)), kNormalSharpness);

            const float weight = depthWeight * normalWeight;
            sumAo += AoTex.Load(int3(tap, 0)).rg * weight;
            sumWeight += weight;
        }
    }

    // Every neighbour rejected (an isolated pixel of geometry, or the centre normal is garbage):
    // keep the raw value rather than writing a zero-weight average.
    AoOut[px] = (sumWeight > 1e-5f) ? (sumAo / sumWeight) : centreAo;
}
