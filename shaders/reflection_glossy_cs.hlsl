#define REFLECTION_GLOSSY_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE))"

// Glossy RT reflections: the sharp mirror reflection spread over its GGX lobe, in the reflector's screen space.
//
// The RT trace casts ONE mirror ray a pixel whatever the roughness (stochastic GGX rays need a production denoiser:
// the RT reflections plan's S11 tried and parked them), so a surface's reflection must be blurred by how wide its
// specular lobe is AS SEEN ON THE SCREEN. Through a planar reflector the reflected point at `hitT` behind it is a
// virtual image `d + hitT` from the eye (d = the reflector's distance), and the lobe's angular spread around the
// mirror direction covers hitT x tan(angle) there -- an angle of tan(angle) x hitT / (d + hitT) from the eye, times
// the projection's pixels per radian. A mirror stays sharp, a near object reflected in a glossy floor stays crisp at
// its contact, a far one spreads (2026-10-26, owner: "the wood under the bathroom window reflects too much" -- its
// roughness 0.40 got a near-mirror image: S16's roughness-scaled blur added 0.4 px there, the lobe ~35).
//
// The lobe: GGX's normal distribution falls to half its peak where tan^2(theta_h) = alpha^2 (sqrt 2 - 1) (from
// D = alpha^2 / (pi cos^4 (alpha^2 + tan^2)^2)); the reflected direction turns twice the half vector's angle; that
// half width at half maximum is taken as a Gaussian's (sigma = HWHM / sqrt(2 ln 2)). GGX's long tail is not kept.
//
// The gather: kGlossyTaps taps on a golden-angle spiral out to 2 sigma, each read from the mip of the pyramids
// (reflection_pyramid_cs.hlsl) whose texel matches the taps' spacing, so a wide kernel of few taps does not alias into
// copies. Weighted by the Gaussian and by being the SAME surface: a tap whose position lies off the centre's plane, or
// whose normal disagrees, is another surface with another reflection (the bathtub in front of the panelling). The
// tap's normal and depth come from the GEOMETRY pyramid at the same mip -- read from the full-resolution G-buffer at
// taps a lobe apart they missed the cache and cost 0.11 ms of the gather's 0.13 (wind_test's bathroom); a mip mixing
// two surfaces averages them into a position and a normal that fit neither, so its tap is rejected, conservatively.
// The kernel is a fixed pattern turned per pixel by interleaved gradient noise -- nothing changes from frame to frame,
// so nothing dances.
//
// t0: the reflection's pyramid (premultiplied rgb, coverage), all mips
// t1: the geometry pyramid (xyz the world normal, w the device depth), all mips
// t2: GB0 (packed roughness/metal in .a)
// t3: the RT payload -- .a is the hit distance (0 = miss, negative = reused from the screen; rt_reflect_common.hlsli)
// u0: the reflection compose reads
#pragma pack_matrix(row_major)
#include "utils.hlsli" // ReconstructPosWS, UnpackRM, kPi

Texture2D<float4> Pyramid : register(t0);
Texture2D<float4> GeomPyramid : register(t1);
Texture2D GB0 : register(t2);
Texture2D<float4> Payload : register(t3);
RWTexture2D<float4> ReflectionOut : register(u0);
SamplerState gSmpLinear : register(s0); // trilinear clamp: the pyramids between their mips
SamplerState gSmpPoint : register(s1);

cbuffer ReflectionGlossyCB : register(b0)
{
    float4x4 invView;
    float4x4 invProj;
    uint2 outSize;          // the reflection target, texels
    float2 invOutSize;
    float pixelsPerRadian;  // reflection texels per radian of view angle: 0.5 x height x P[1][1]
    float planeTolerance;   // a tap off the centre's plane by more than this share of the view distance is not it
    float normalPower;      // ...and its normals' agreement raised to this
    uint mipCount;          // the pyramids' levels
}

static const uint kGlossyTaps = 16;
static const float kGoldenAngle = 2.39996323f;
static const float kHalfMaxTan = 0.64359425f;   // sqrt(sqrt(2) - 1): GGX's half-maximum, tan(theta_h) / alpha
static const float kHwhmToSigma = 1.0f / 1.17741002f; // 1 / sqrt(2 ln 2)

[numthreads(8, 8, 1)]
[RootSignature(REFLECTION_GLOSSY_CS_RS)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (any(tid.xy >= outSize))
    {
        return;
    }
    const float2 uv = (float2(tid.xy) + 0.5f) * invOutSize;
    const float4 sharp = Pyramid.SampleLevel(gSmpPoint, uv, 0.0f);
    const float4 geom = GeomPyramid.SampleLevel(gSmpPoint, uv, 0.0f);
    const float z = geom.w;
    if (z <= 1.0e-6f)
    {
        ReflectionOut[tid.xy] = sharp; // the sky: no reflector
        return;
    }
    const float rough = saturate(UnpackRM(GB0.SampleLevel(gSmpPoint, uv, 0.0f).a).x);
    const float3 P = ReconstructPosWS(uv, z, invProj, invView);
    const float3 camPos = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), invView).xyz;
    const float d = max(length(P - camPos), 1.0e-3f);
    // A miss sees the sky at infinity: the virtual image's distance ratio is 1.
    const float hitT = abs(Payload.SampleLevel(gSmpPoint, uv, 0.0f).a);
    const float ratio = hitT > 0.0f ? hitT / (d + hitT) : 1.0f;

    const float alpha = rough * rough;
    const float lobeAngle = 2.0f * atan(alpha * kHalfMaxTan) * kHwhmToSigma; // the reflected direction's sigma
    const float sigma = tan(min(lobeAngle, 1.4f)) * ratio * pixelsPerRadian;
    if (sigma < 0.5f)
    {
        ReflectionOut[tid.xy] = sharp; // under a texel: the mirror as it is
        return;
    }

    const float3 N = normalize(geom.xyz);
    const float radius = 2.0f * sigma;
    // Each tap stands for an area pi R^2 / N of the disk: read it from the mip whose texel is that wide.
    const float spacing = radius * sqrt(kPi / float(kGlossyTaps));
    const float mip = clamp(log2(max(spacing, 1.0f)), 0.0f, float(mipCount) - 1.0f);
    const float turn = 2.0f * kPi * frac(52.9829189f * frac(dot(float2(tid.xy), float2(0.06711056f, 0.00583715f))));
    const float invTwoSigma2 = 0.5f / (sigma * sigma);

    float4 sum = Pyramid.SampleLevel(gSmpLinear, uv, mip); // the centre, weight 1
    float weightSum = 1.0f;
    [loop]
    for (uint i = 0; i < kGlossyTaps; ++i)
    {
        const float r = radius * sqrt((float(i) + 0.5f) / float(kGlossyTaps));
        const float phi = float(i) * kGoldenAngle + turn;
        const float2 uvTap = uv + float2(cos(phi), sin(phi)) * r * invOutSize;
        if (any(uvTap < 0.0f) || any(uvTap > 1.0f))
        {
            continue;
        }
        const float4 geomTap = GeomPyramid.SampleLevel(gSmpLinear, uvTap, mip);
        if (geomTap.w <= 1.0e-6f)
        {
            continue; // the sky
        }
        const float3 PTap = ReconstructPosWS(uvTap, geomTap.w, invProj, invView);
        const float wPlane = saturate(1.0f - abs(dot(PTap - P, N)) / (planeTolerance * d));
        const float3 NTap = geomTap.xyz * rsqrt(max(dot(geomTap.xyz, geomTap.xyz), 1.0e-8f));
        const float w = exp(-r * r * invTwoSigma2) * wPlane * pow(saturate(dot(N, NTap)), normalPower);
        sum += Pyramid.SampleLevel(gSmpLinear, uvTap, mip) * w;
        weightSum += w;
    }
    ReflectionOut[tid.xy] = sum / weightSum;
}
