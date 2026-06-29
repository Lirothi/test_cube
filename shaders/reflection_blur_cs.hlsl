#define SSR_BLUR_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
// t0: reflection input (RGB premultiplied, A=visibility)
// t1: GB0 (albedo + packed rough/metal in .a) — the reflector's roughness drives the
//     glossy blur width: mirror-smooth -> sharp, rough -> wide blur (per-pixel).
// u0: reflection output (premultiplied RGBA)
// s0: LinearClamp
#pragma pack_matrix(row_major)
#include "utils.hlsl" // UnpackRM
Texture2D ReflectionIn : register(t0);
Texture2D GB0          : register(t1);
RWTexture2D<float4> ReflectionOut : register(u0);
SamplerState gSmp : register(s0);

#ifndef SSR_BLUR_TAP_COUNT
#define SSR_BLUR_TAP_COUNT 3
#endif

#if (SSR_BLUR_TAP_COUNT != 3) && (SSR_BLUR_TAP_COUNT != 5)
#error "SSR_BLUR_TAP_COUNT must be either 3 or 5"
#endif

cbuffer BlurCB : register(b0){
    float2 dir;          // (1/width,0) for X, (0,1/height) for Y
    float radius;        // base blur radius (smooths the half-res reflection); applied at roughness 0
    float glossyScale;   // extra blur radius at full roughness -> glossy reflections (0 = sharp/mirror)
}

[numthreads(8, 8, 1)]
[RootSignature(SSR_BLUR_CS_RS)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width, height;
    ReflectionIn.GetDimensions(width, height);

    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
    {
        return;
    }

    // Gaussian weights normalized for sigma=1.77538 (matches previous 5-tap kernel).
#if SSR_BLUR_TAP_COUNT == 5
    static const float weights[SSR_BLUR_TAP_COUNT] = {
        0.2270269f,
        0.19372463f,
        0.12036701f,
        0.05445593f,
        0.01793899f
    };
#elif SSR_BLUR_TAP_COUNT == 3
    static const float weights[SSR_BLUR_TAP_COUNT] = {
        0.26546327f,
        0.22652283f,
        0.14074554f
    };
#endif
    float2 texDim = float2(width, height);
    float2 uv = (float2(dispatchThreadId.xy) + 0.5f) / texDim;
    // Per-pixel blur width from the reflector's roughness (gb0.a). Mirror (rough~0) -> `radius`
    // (a light smooth of the half-res reflection); rough -> wider blur = glossy reflection.
    float rough = saturate(UnpackRM(GB0.SampleLevel(gSmp, uv, 0).a).x);
    float2 stepv = dir * (radius + rough * glossyScale);

    float4 c = ReflectionIn.SampleLevel(gSmp, uv, 0) * weights[0];
    [unroll]
    for(uint k = 1; k < SSR_BLUR_TAP_COUNT; ++k){
        float2 off = stepv * k;
        float w = weights[k];
        c += ReflectionIn.SampleLevel(gSmp, uv + off, 0) * w;
        c += ReflectionIn.SampleLevel(gSmp, uv - off, 0) * w;
    }

    ReflectionOut[dispatchThreadId.xy] = c;
}
