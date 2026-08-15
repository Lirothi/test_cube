#define TONEMAP_CS_RS "CBV(b0), DescriptorTable(SRV(t0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE), UAV(u1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

Texture2D HDRColor : register(t0);
RWTexture2D<float4> LdrTarget : register(u0);
// P2: the persistent exposure record, read-only here. Bound as a UAV rather than an SRV purely so
// it never leaves its canonical UNORDERED_ACCESS state -- an SRV binding would cost a transition
// down and back every frame for 16 bytes nobody writes in this pass.
// This runs AFTER the DLSS resolve (the upscaler evaluates earlier in this same pass) and BEFORE
// the tone curve, which is the ordering the plan's section 6.3 fixes. NGX keeps its own internal
// auto-exposure -- nothing here is handed to it.
RWByteAddressBuffer ExposureValue : register(u1);
SamplerState gSmp : register(s0);

cbuffer TonemapCB : register(b0)
{
    // 0 = dormant. Kept as an explicit flag rather than writing a neutral EV into the buffer so
    // the disabled path multiplies by a literal 1.0 and is bit-identical to the pre-plan image.
    uint exposureEnabled;
    uint tonemapPad0, tonemapPad1, tonemapPad2;
};

#include "utils.hlsli"

// ---- named constants ----
static const float kGammaOut = 2.2;
static const float kDitherAmplitude = 1.0 / 255.0; // enough to break banding

// ACES fitted (K. Narkowicz)
float3 TonemapACES(float3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Stable, cheap hash based on the pixel coordinate
float Dither(uint2 p)
{
    float n = frac(sin(dot(float2(p), float2(12.9898, 78.233))) * 43758.5453);
    return n - 0.5; // [-0.5, 0.5)
}

[numthreads(8,8,1)]
[RootSignature(TONEMAP_CS_RS)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width, height;
    LdrTarget.GetDimensions(width, height);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
    {
        return;
    }

    float2 uv = (float2(dispatchThreadId.xy) + 0.5f) / float2(width, height);
    float3 hdr = HDRColor.SampleLevel(gSmp, uv, 0).rgb;

    // Exposure applied exactly once, here, immediately before the tone curve.
    // Mirrors render::ExposureMultiplierFromEv100 in PhotographicSettings.h.
    if (exposureEnabled != 0)
    {
        const float ev100 = asfloat(ExposureValue.Load(0));
        if (!isnan(ev100) && !isinf(ev100))
        {
            hdr *= 1.0f / (1.2f * exp2(ev100));
        }
    }

    float3 mapped = TonemapACES(hdr);
    float3 ldr = LinearToGamma(mapped, kGammaOut);

    // Optional: add identical noise to every channel — sufficient to break banding
    //float d = Dither(dispatchThreadId.xy) * kDitherAmplitude;
    //ldr += d;

    LdrTarget[dispatchThreadId.xy] = float4(saturate(ldr), 1.0);
}
