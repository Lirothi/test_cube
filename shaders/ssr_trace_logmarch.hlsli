#ifndef SSR_TRACE_LOGMARCH_HLSLI
#define SSR_TRACE_LOGMARCH_HLSLI

#ifndef SSR_TRACE_PROJ
#define SSR_TRACE_PROJ proj
#endif

#ifndef SSR_TRACE_INV_SCREEN_SIZE
#define SSR_TRACE_INV_SCREEN_SIZE invScreenSize
#endif

#ifndef SSR_TRACE_SCREEN_SIZE
#define SSR_TRACE_SCREEN_SIZE screenSize
#endif

#ifndef SSR_TRACE_READ_DEPTH
#define SSR_TRACE_READ_DEPTH(uv) ReadDepth(uv)
#endif

#ifndef SSR_TRACE_DEPTH_TO_VIEW_Z
#define SSR_TRACE_DEPTH_TO_VIEW_Z(depthRaw) DepthToViewZ_Fast(depthRaw)
#endif

#ifndef SSR_TRACE_RECONSTRUCT_POS_VS
#define SSR_TRACE_RECONSTRUCT_POS_VS(uv, depthRaw) ReconstructPosVS(uv, depthRaw)
#endif

static const float ssrMaxDistanceVS = 100.0f; // maxDistance (view units)
static const int ssrRefineSteps = 16; // number of refinement iterations
static const int ssrLogMarchSteps = 128; // number of logarithmic steps for the hybrid tracer
static const float ssrMinStrideVS = 0.05f; // minimum ray step in view space
static const float ssrStrideGrowth = 1.02f; // multiplicative stride growth per step
static const float ssrThicknessVS = 0.05f; // thickness (view units)
static const float ssrEdgeFadePx = 32.0f; // Smooth fade width near the screen border in pixels
static const float ssrJitterStrength = 0.5f; // 0..1 pixel offset applied to the start
static const float ssrGrazingMinZ = 0.01f; // Start fading reflections when Rv.z falls below this
static const float ssrGrazingMaxZ = 0.05f; // Fully enable reflections by this value

struct SSRHit
{
    float2 uv;
    float visibility;
    int hit;
};

float Hash12(float2 p)
{
    p = frac(p * float2(0.1031f, 0.11369f));
    p += dot(p, p.yx + 33.33f);
    return frac((p.x + p.y) * p.x);
}

float EdgeFadePx(float2 uv)
{
    float2 dist = min(uv, 1.0f - uv) * SSR_TRACE_SCREEN_SIZE;
    float m = min(dist.x, dist.y);
    return saturate(m / ssrEdgeFadePx);
}

SSRHit BuildSsrHit(float3 pivot, float3 unitPositionFrom, float3 Pv, float2 uv, float depthRaw, float thicknessVS, float depthDiff)
{
    SSRHit outv;
    outv.uv = uv;

    float visibility = 1.0f;
    float3 positionTo = SSR_TRACE_RECONSTRUCT_POS_VS(uv, depthRaw);
    visibility *= (1.0f - max(dot(-unitPositionFrom, pivot), 0.0f));
    float thicknessSafe = max(thicknessVS, 1e-4f);
    visibility *= (1.0f - clamp(depthDiff / thicknessSafe, 0.0f, 1.0f));
    visibility *= (1.0f - clamp(length(positionTo - Pv) / ssrMaxDistanceVS, 0.0f, 1.0f));
    visibility *= EdgeFadePx(uv);
    float grazing = saturate((pivot.z - ssrGrazingMinZ) / (ssrGrazingMaxZ - ssrGrazingMinZ));
    visibility *= grazing;
    visibility = clamp(visibility, 0.0f, 1.0f);

    outv.visibility = visibility;
    outv.hit = (visibility > 0.0f) ? 1 : 0;
    return outv;
}

// Hybrid logarithmic screen-space tracing inspired by Mara & McGuire's
// "Efficient GPU Screen-Space Ray Tracing".
SSRHit TraceSSR_LogMarch(float3 Pv, float3 Nv, float2 pixelCoord)
{
    SSRHit outv;
    outv.uv = 0.0f.xx;
    outv.visibility = 0.0f;
    outv.hit = 0;

    float3 unitPositionFrom = normalize(Pv);
    float3 pivot = normalize(reflect(unitPositionFrom, Nv));

    if (pivot.z <= 0.0f)
    {
        return outv;
    }

    float2 jitterSeed = pixelCoord * SSR_TRACE_INV_SCREEN_SIZE;
    float jitter = (Hash12(jitterSeed) * 2.0f - 1.0f) * ssrJitterStrength;
    float3 origin = Pv + Nv * ssrThicknessVS;
    origin += pivot * (jitter * ssrThicknessVS);

    float step = max(ssrMinStrideVS, length(Pv) * 0.02f);
    float tPrev = 0.0f;
    float tCurr = step;
    float thick = ssrThicknessVS;

    for (int i = 0; i < ssrLogMarchSteps && tCurr <= ssrMaxDistanceVS; ++i)
    {
        float3 sampleVS = origin + pivot * tCurr;
        float4 sampleClip = mul(float4(sampleVS, 1.0f), SSR_TRACE_PROJ);
        if (sampleClip.w <= 0.0f)
        {
            break;
        }

        float2 sampleUV = float2(sampleClip.x / sampleClip.w * 0.5f + 0.5f,
                                 -sampleClip.y / sampleClip.w * 0.5f + 0.5f);

        if (any(sampleUV < 0.0f) || any(sampleUV > 1.0f))
        {
            break;
        }

        float depthRaw = SSR_TRACE_READ_DEPTH(sampleUV);
        float depthLin = SSR_TRACE_DEPTH_TO_VIEW_Z(depthRaw);
        float depthDiff = sampleVS.z - depthLin;

        if (depthDiff > 0.0f)
        {
            float tLow = tPrev;
            float tHigh = tCurr;
            float2 uvHigh = sampleUV;
            float depthHighRaw = depthRaw;
            float diffHigh = depthDiff;

            for (int j = 0; j < ssrRefineSteps; ++j)
            {
                float tMid = 0.5f * (tLow + tHigh);
                float3 midVS = origin + pivot * tMid;
                float4 midClip = mul(float4(midVS, 1.0f), SSR_TRACE_PROJ);
                if (midClip.w <= 0.0f)
                {
                    tHigh = tMid;
                    continue;
                }

                float2 midUV = float2(midClip.x / midClip.w * 0.5f + 0.5f,
                                      -midClip.y / midClip.w * 0.5f + 0.5f);

                if (any(midUV < 0.0f) || any(midUV > 1.0f))
                {
                    tHigh = tMid;
                    continue;
                }

                float midDepthRaw = SSR_TRACE_READ_DEPTH(midUV);
                float midDepthLin = SSR_TRACE_DEPTH_TO_VIEW_Z(midDepthRaw);
                float diffMid = midVS.z - midDepthLin;

                if (diffMid > 0.0f)
                {
                    tHigh = tMid;
                    uvHigh = midUV;
                    depthHighRaw = midDepthRaw;
                    diffHigh = diffMid;
                }
                else
                {
                    tLow = tMid;
                }
            }

            if (diffHigh < thick)
            {
                return BuildSsrHit(pivot, unitPositionFrom, Pv, uvHigh, depthHighRaw, thick, diffHigh);
            }

            break;
        }

        tPrev = tCurr;
        step *= ssrStrideGrowth;
        thick *= ssrStrideGrowth * 1.01f;
        tCurr += step;
    }

    return outv;
}

#endif // SSR_TRACE_LOGMARCH_HLSLI
