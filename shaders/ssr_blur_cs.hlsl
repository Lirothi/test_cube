// RootSignature: CBV(b0) TABLE(SRV(t0)) TABLE(UAV(u0)) TABLE(SAMPLER(s0))
// t0: SSR input (RGB premultiplied, A=visibility)
// u0: SSR output (premultiplied RGBA)
// s0: LinearClamp
Texture2D SSRIn : register(t0);
RWTexture2D<float4> SSROut : register(u0);
SamplerState gSmp : register(s0);

#ifndef SSR_BLUR_TAP_COUNT
#define SSR_BLUR_TAP_COUNT 5
#endif

#if (SSR_BLUR_TAP_COUNT != 3) && (SSR_BLUR_TAP_COUNT != 5)
#error "SSR_BLUR_TAP_COUNT must be either 3 or 5"
#endif

cbuffer BlurCB : register(b0){
    float2 dir;         // (1/width,0) for X, (0,1/height) for Y
    float radius;     // 1..3
    float _pad;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width, height;
    SSRIn.GetDimensions(width, height);

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
    float2 stepv = dir * radius;

    float4 c = SSRIn.SampleLevel(gSmp, uv, 0) * weights[0];
    [unroll]
    for(uint k = 1; k < SSR_BLUR_TAP_COUNT; ++k){
        float2 off = stepv * k;
        float w = weights[k];
        c += SSRIn.SampleLevel(gSmp, uv + off, 0) * w;
        c += SSRIn.SampleLevel(gSmp, uv - off, 0) * w;
    }

    SSROut[dispatchThreadId.xy] = c;
}
