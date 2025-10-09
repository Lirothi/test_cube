// RootSignature: CBV(b0) TABLE(SRV(t0)) TABLE(UAV(u0)) TABLE(SAMPLER(s0))
// t0: LDR color input (R8G8B8A8)
// u0: FXAA output (R8G8B8A8)
// s0: LinearClamp

Texture2D<float4> InputColor : register(t0);
RWTexture2D<float4> OutputColor : register(u0);
SamplerState gSmp : register(s0);

// FXAA configuration constants. The defaults mirror the reference values from NVIDIA's
// implementation. Adjust them on the CPU side to trade off sharpness vs. aliasing removal:
//   * invResolution    : size of one pixel in UV space (1 / width, 1 / height)
//   * subpix           : sub-pixel aliasing removal strength (0 disables, higher softens more)
//   * edgeThreshold    : relative contrast required to treat a pixel as an edge (lower = more edges)
//   * edgeThresholdMin : absolute minimum contrast required (lower catches fine edges, higher skips noise)
cbuffer FxaaCB : register(b0)
{
    float2 invResolution;
    float   subpix;
    float   edgeThreshold;
    float   edgeThresholdMin;
};

static const float3 kLumaWeights = float3(0.299f, 0.587f, 0.114f);

float ComputeLuma(float3 rgb)
{
    return dot(rgb, kLumaWeights);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width, height;
    OutputColor.GetDimensions(width, height);

    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
    {
        return;
    }

    float2 uv = (float2(dispatchThreadId.xy) + 0.5f) * invResolution;

    float3 rgbM = InputColor.SampleLevel(gSmp, uv, 0).rgb;
    float lumaM = ComputeLuma(rgbM);

    float lumaN = ComputeLuma(InputColor.SampleLevel(gSmp, uv + float2(0.0f, invResolution.y), 0).rgb);
    float lumaS = ComputeLuma(InputColor.SampleLevel(gSmp, uv - float2(0.0f, invResolution.y), 0).rgb);
    float lumaE = ComputeLuma(InputColor.SampleLevel(gSmp, uv + float2(invResolution.x, 0.0f), 0).rgb);
    float lumaW = ComputeLuma(InputColor.SampleLevel(gSmp, uv - float2(invResolution.x, 0.0f), 0).rgb);

    float rangeMin = min(lumaM, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    float rangeMax = max(lumaM, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    float range = rangeMax - rangeMin;

    if (range < max(edgeThresholdMin, rangeMax * edgeThreshold))
    {
        OutputColor[dispatchThreadId.xy] = float4(rgbM, 1.0f);
        return;
    }

    float lumaNW = ComputeLuma(InputColor.SampleLevel(gSmp, uv + float2(-invResolution.x, +invResolution.y), 0).rgb);
    float lumaNE = ComputeLuma(InputColor.SampleLevel(gSmp, uv + float2(+invResolution.x, +invResolution.y), 0).rgb);
    float lumaSW = ComputeLuma(InputColor.SampleLevel(gSmp, uv + float2(-invResolution.x, -invResolution.y), 0).rgb);
    float lumaSE = ComputeLuma(InputColor.SampleLevel(gSmp, uv + float2(+invResolution.x, -invResolution.y), 0).rgb);

    float2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float lumaSum = lumaN + lumaS + lumaE + lumaW;
    float dirReduce = max(lumaSum * (0.25f * subpix), edgeThresholdMin);
    float rcpDirMin = 1.0f / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, -8.0f, 8.0f) * float2(invResolution.x, invResolution.y);

    float3 rgbA = 0.5f * (
        InputColor.SampleLevel(gSmp, uv + dir * (1.0f / 3.0f - 0.5f), 0).rgb +
        InputColor.SampleLevel(gSmp, uv + dir * (2.0f / 3.0f - 0.5f), 0).rgb);

    float3 rgbB = rgbA * 0.5f +
        0.25f * (
            InputColor.SampleLevel(gSmp, uv + dir * -0.5f, 0).rgb +
            InputColor.SampleLevel(gSmp, uv + dir * 0.5f, 0).rgb);

    float lumaB = ComputeLuma(rgbB);
    float rangeMin2 = min(rangeMin, min(lumaNW, min(lumaNE, min(lumaSW, lumaSE))));
    float rangeMax2 = max(rangeMax, max(lumaNW, max(lumaNE, max(lumaSW, lumaSE))));

    float3 finalColor = ((lumaB < rangeMin2) || (lumaB > rangeMax2)) ? rgbA : rgbB;

    OutputColor[dispatchThreadId.xy] = float4(finalColor, 1.0f);
}
