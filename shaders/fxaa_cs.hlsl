// RootSignature: CBV(b0) TABLE(SRV(t0)) TABLE(UAV(u0)) TABLE(SAMPLER(s0))
// t0: LDR color input (R8G8B8A8)
// u0: FXAA output (R8G8B8A8)
// s0: LinearClamp

Texture2D<float4> InputColor : register(t0);
RWTexture2D<float4> OutputColor : register(u0);
SamplerState gSmp : register(s0);

// FXAA configuration constants. The runtime values match the knobs exposed by the FXAA 3.11
// reference shader:
//   * invResolution    : size of one pixel in UV space (1 / width, 1 / height)
//   * subpix           : linear blend between the original color and the FXAA filtered result.
//                        Set to 1.0 to match the reference output, or reduce towards 0 to retain
//                        more of the untouched image.
//   * edgeThreshold    : relative contrast required to treat a pixel as an edge (default 1/8).
//   * edgeThresholdMin : absolute minimum contrast required to trigger edge processing (default 1/24).
cbuffer FxaaCB : register(b0)
{
    float2 invResolution;
    float subpix;
    float edgeThreshold;
    float edgeThresholdMin;
};

// Core algorithm adapted from the FXAA 3.11 reference implementation by Timothy Lottes / NVIDIA.
static const float FXAA_SEARCH_THRESHOLD = 1.0f / 4.0f;
static const int FXAA_SEARCH_STEPS = 32;
static const float FXAA_SUBPIX_TRIM = 1.0f / 4.0f;
static const float FXAA_SUBPIX_TRIM_SCALE = 1.0f / (1.0f - FXAA_SUBPIX_TRIM);
static const float FXAA_SUBPIX_CAP = 3.0f / 4.0f;

float3 FxaaTexSample(float2 uv)
{
    return InputColor.SampleLevel(gSmp, uv, 0).rgb;
}

float3 FxaaTexOff(float2 uv, int2 offset, float2 rcpFrame)
{
    return FxaaTexSample(uv + float2(offset) * rcpFrame);
}

float FxaaLuma(float3 rgb)
{
    return rgb.y * (0.587f / 0.299f) + rgb.x;
}

float3 FxaaLerp3(float3 a, float3 b, float amountOfA)
{
    return ((a - b) * amountOfA) + b;
}

float3 FxaaPixelShader(float2 uv, float2 rcpFrame, out float3 rgbMOut)
{
    float3 rgbN = FxaaTexOff(uv, int2(0, -1), rcpFrame);
    float3 rgbW = FxaaTexOff(uv, int2(-1, 0), rcpFrame);
    float3 rgbM = FxaaTexOff(uv, int2(0, 0), rcpFrame);
    float3 rgbE = FxaaTexOff(uv, int2(1, 0), rcpFrame);
    float3 rgbS = FxaaTexOff(uv, int2(0, 1), rcpFrame);

    rgbMOut = rgbM;

    float lumaN = FxaaLuma(rgbN);
    float lumaW = FxaaLuma(rgbW);
    float lumaM = FxaaLuma(rgbM);
    float lumaE = FxaaLuma(rgbE);
    float lumaS = FxaaLuma(rgbS);

    float rangeMin = min(lumaM, min(min(lumaN, lumaW), min(lumaS, lumaE)));
    float rangeMax = max(lumaM, max(max(lumaN, lumaW), max(lumaS, lumaE)));

    float range = rangeMax - rangeMin;
    if (range < max(edgeThresholdMin, rangeMax * edgeThreshold))
    {
        return rgbM;
    }

    float3 rgbL = rgbN + rgbW + rgbM + rgbE + rgbS;

    float lumaL = (lumaN + lumaW + lumaE + lumaS) * 0.25f;
    float rangeL = abs(lumaL - lumaM);
    float blendL = max(0.0f, (rangeL / range) - FXAA_SUBPIX_TRIM) * FXAA_SUBPIX_TRIM_SCALE;
    blendL = min(FXAA_SUBPIX_CAP, blendL);

    float3 rgbNW = FxaaTexOff(uv, int2(-1, -1), rcpFrame);
    float3 rgbNE = FxaaTexOff(uv, int2(1, -1), rcpFrame);
    float3 rgbSW = FxaaTexOff(uv, int2(-1, 1), rcpFrame);
    float3 rgbSE = FxaaTexOff(uv, int2(1, 1), rcpFrame);
    rgbL += (rgbNW + rgbNE + rgbSW + rgbSE);
    rgbL *= 1.0f / 9.0f;

    float lumaNW = FxaaLuma(rgbNW);
    float lumaNE = FxaaLuma(rgbNE);
    float lumaSW = FxaaLuma(rgbSW);
    float lumaSE = FxaaLuma(rgbSE);

    float edgeVert =
        abs((0.25f * lumaNW) + (-0.5f * lumaN) + (0.25f * lumaNE)) +
        abs((0.50f * lumaW) + (-1.0f * lumaM) + (0.50f * lumaE)) +
        abs((0.25f * lumaSW) + (-0.5f * lumaS) + (0.25f * lumaSE));
    float edgeHorz =
        abs((0.25f * lumaNW) + (-0.5f * lumaW) + (0.25f * lumaSW)) +
        abs((0.50f * lumaN) + (-1.0f * lumaM) + (0.50f * lumaS)) +
        abs((0.25f * lumaNE) + (-0.5f * lumaE) + (0.25f * lumaSE));

    bool horzSpan = edgeHorz >= edgeVert;
    float lengthSign = horzSpan ? -rcpFrame.y : -rcpFrame.x;

    if (!horzSpan)
    {
        lumaN = lumaW;
        lumaS = lumaE;
    }

    float gradientN = abs(lumaN - lumaM);
    float gradientS = abs(lumaS - lumaM);
    lumaN = (lumaN + lumaM) * 0.5f;
    lumaS = (lumaS + lumaM) * 0.5f;

    if (gradientN < gradientS)
    {
        lumaN = lumaS;
        gradientN = gradientS;
        lengthSign *= -1.0f;
    }

    float2 posN;
    posN.x = uv.x + (horzSpan ? 0.0f : lengthSign * 0.5f);
    posN.y = uv.y + (horzSpan ? lengthSign * 0.5f : 0.0f);

    float gradientThreshold = gradientN * FXAA_SEARCH_THRESHOLD;

    float2 posP = posN;
    float2 offNP = horzSpan ? float2(rcpFrame.x, 0.0f) : float2(0.0f, rcpFrame.y);
    float lumaEndN = lumaN;
    float lumaEndP = lumaN;
    bool doneN = false;
    bool doneP = false;
    posN += offNP * float2(-1.0f, -1.0f);
    posP += offNP * float2(1.0f, 1.0f);

    [loop]
    for (int i = 0; i < FXAA_SEARCH_STEPS; ++i)
    {
        if (!doneN)
        {
            lumaEndN = FxaaLuma(FxaaTexSample(posN));
        }
        if (!doneP)
        {
            lumaEndP = FxaaLuma(FxaaTexSample(posP));
        }

        doneN = doneN || (abs(lumaEndN - lumaN) >= gradientThreshold);
        doneP = doneP || (abs(lumaEndP - lumaN) >= gradientThreshold);

        if (doneN && doneP)
        {
            break;
        }
        if (!doneN)
        {
            posN -= offNP;
        }
        if (!doneP)
        {
            posP += offNP;
        }
    }

    float dstN = horzSpan ? uv.x - posN.x : uv.y - posN.y;
    float dstP = horzSpan ? posP.x - uv.x : posP.y - uv.y;
    bool directionN = dstN < dstP;
    float lumaEnd = directionN ? lumaEndN : lumaEndP;

    if (((lumaM - lumaN) < 0.0f) == ((lumaEnd - lumaN) < 0.0f))
    {
        lengthSign = 0.0f;
    }

    float spanLength = dstP + dstN;
    dstN = directionN ? dstN : dstP;
    float invSpanLength = (spanLength != 0.0f) ? (-1.0f / spanLength) : 0.0f;
    float subPixelOffset = (0.5f + (dstN * invSpanLength)) * lengthSign;

    float2 finalUv = uv;
    if (horzSpan)
    {
        finalUv.y += subPixelOffset;
    }
    else
    {
        finalUv.x += subPixelOffset;
    }

    float3 rgbF = FxaaTexSample(finalUv);
    return FxaaLerp3(rgbL, rgbF, blendL);
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

    float3 rgbM;
    float3 fxaaColor = FxaaPixelShader(uv, invResolution, rgbM);

    float subpixStrength = saturate(subpix);
    float3 blended = lerp(rgbM, fxaaColor, subpixStrength);

    OutputColor[dispatchThreadId.xy] = float4(blended, 1.0f);
}