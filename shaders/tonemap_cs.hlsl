#define TONEMAP_CS_RS "DescriptorTable(SRV(t0, flags=DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DATA_VOLATILE)), DescriptorTable(Sampler(s0))"

Texture2D HDRColor : register(t0);
RWTexture2D<float4> LdrTarget : register(u0);
SamplerState gSmp : register(s0);

#include "utils.hlsl"

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

    float3 mapped = TonemapACES(hdr);
    float3 ldr = LinearToGamma(mapped, kGammaOut);

    // Optional: add identical noise to every channel — sufficient to break banding
    //float d = Dither(dispatchThreadId.xy) * kDitherAmplitude;
    //ldr += d;

    LdrTarget[dispatchThreadId.xy] = float4(saturate(ldr), 1.0);
}
