// RootSignature: CBV(b0) TABLE(SRV(t0)) TABLE(UAV(u0)) TABLE(SAMPLER(s0))
// t0: SSR input (RGB premultiplied, A=visibility)
// u0: SSR output (premultiplied RGBA)
// s0: LinearClamp
Texture2D SSRIn : register(t0);
RWTexture2D<float4> SSROut : register(u0);
SamplerState gSmp : register(s0);

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

    const float w[5] = {0.227027f, 0.1945946f, 0.1216216f, 0.054054f, 0.016216f};
    float2 texDim = float2(width, height);
    float2 uv = (float2(dispatchThreadId.xy) + 0.5f) / texDim;
    float2 stepv = dir * radius;

    float4 c = SSRIn.SampleLevel(gSmp, uv, 0) * w[0];
    [unroll]
    for(int k=1; k < 5; ++k){
        float2 off = stepv * k;
        c += SSRIn.SampleLevel(gSmp, uv + off, 0) * w[k];
        c += SSRIn.SampleLevel(gSmp, uv - off, 0) * w[k];
    }

    SSROut[dispatchThreadId.xy] = c;
}
