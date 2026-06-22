// Temporal reflection denoise (S11). cs_6_6 + SM6.6 dynamic resources.
//
// Accumulates the (jittered, noisy) RT reflection over frames: reprojects the
// previous accumulated result by the motion vector (prevUv = uv - motion, where
// motion = currUv - prevUv from gbVelocity), clamps it to the current frame's
// 3x3 neighbourhood (anti-ghosting), and blends. Writes the result to BOTH the
// reflection buffer (for the downstream spatial blur + compose) and the current history
// texture (next frame's "previous"). Premultiplied throughout.
#define RT_DENOISE_CS_RS \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "CBV(b0)," \
    "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP)"

#pragma pack_matrix(row_major)

cbuffer Denoise : register(b0)
{
    uint  rawIndex;        // this frame's raw reflection (SRV)
    uint  histPrevIndex;   // previous accumulated reflection (SRV)
    uint  velocityIndex;   // gbVelocity (SRV); motion = currUv - prevUv
    uint  reflectionUavIndex;     // denoised output -> reflection (UAV), feeds blur + compose
    uint  histCurrUavIndex;// current accumulated (UAV) -> next frame's prev
    uint  outWidth;
    uint  outHeight;
    float alpha;           // weight of the current frame (small = more accumulation)
}

SamplerState gSmp : register(s0);

[numthreads(8, 8, 1)]
[RootSignature(RT_DENOISE_CS_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= outWidth || dtid.y >= outHeight)
    {
        return;
    }

    Texture2D            rawTex  = ResourceDescriptorHeap[rawIndex];
    Texture2D            histTex = ResourceDescriptorHeap[histPrevIndex];
    Texture2D            velTex  = ResourceDescriptorHeap[velocityIndex];
    RWTexture2D<float4>  reflectionOut = ResourceDescriptorHeap[reflectionUavIndex];
    RWTexture2D<float4>  histOut = ResourceDescriptorHeap[histCurrUavIndex];

    int2 px = int2(dtid.xy);
    int2 maxPx = int2(int(outWidth) - 1, int(outHeight) - 1);
    float2 uv = (float2(px) + 0.5f) / float2(outWidth, outHeight);

    float4 raw = rawTex.Load(int3(px, 0));

    // Current 3x3 neighbourhood bounds (anti-ghosting clamp for the history).
    float4 nmin = raw, nmax = raw;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            int2 p = clamp(px + int2(x, y), int2(0, 0), maxPx);
            float4 s = rawTex.Load(int3(p, 0));
            nmin = min(nmin, s);
            nmax = max(nmax, s);
        }
    }

    float2 motion = velTex.SampleLevel(gSmp, uv, 0).xy; // currUv - prevUv
    float2 prevUv = uv - motion;

    float4 acc;
    if (all(prevUv >= 0.0f) && all(prevUv <= 1.0f))
    {
        float4 hist = histTex.SampleLevel(gSmp, prevUv, 0);
        hist = clamp(hist, nmin, nmax);
        acc = lerp(hist, raw, alpha);
    }
    else
    {
        acc = raw; // disocclusion / history off-screen -> no accumulation this frame
    }

    reflectionOut[px] = acc;
    histOut[px] = acc;
}
