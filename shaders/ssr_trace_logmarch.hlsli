#ifndef SSR_TRACE_LOGMARCH_HLSLI
#define SSR_TRACE_LOGMARCH_HLSLI

// Mirrors `SsrTechnique` in SceneFrameData.h. It lives in the shared trace header rather than in
// one host shader because more than one pass now chooses between the two searches.
static const uint SSR_TECHNIQUE_LOGMARCH = 0u;
static const uint SSR_TECHNIQUE_UE = 1u;

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

// Opt-in from the host shader's compile defines. The legacy math and 16-step refinement remain
// selected when this is 0, so the optimization can be A/B tested without changing any setting.
#ifndef SSR_LOGMARCH_OPTIMIZED
#define SSR_LOGMARCH_OPTIMIZED 0
#endif

// Independent experiment layered on the optimized path: issue four coarse depth reads before
// consuming their results, matching UE's latency-hiding shape. Keep it off unless the material
// explicitly requests the permutation; it may read up to three taps beyond an early hit.
#ifndef SSR_LOGMARCH_BATCH4
#define SSR_LOGMARCH_BATCH4 0
#endif

#if SSR_LOGMARCH_BATCH4 && !SSR_LOGMARCH_OPTIMIZED
#error SSR_LOGMARCH_BATCH4 requires SSR_LOGMARCH_OPTIMIZED
#endif

static const float ssrMaxDistanceVS = 100.0f; // maxDistance (view units)
static const int ssrRefineSteps = 16; // number of refinement iterations
#if SSR_LOGMARCH_OPTIMIZED
static const int ssrOptimizedRefineSteps = 12; // conservative sub-pixel refinement budget
#endif
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
    // Device-Z on the ray at the accepted hit. UE reprojects the complete HitUVz, not just UV.
    float deviceZ;
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
    outv.deviceZ = depthRaw;

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

#if SSR_LOGMARCH_BATCH4
// Batch4 changes only the coarse request schedule. Once its first crossing is known, refinement is
// the same optimized bisection used by the scalar path and returns the same hit/miss decision.
SSRHit RefineSsrLogMarchBatchHit(float3 Pv, float3 unitPositionFrom, float3 pivot, float3 origin,
                                float tPrev, float tCurr, float thick, float2 firstHitUV,
                                float firstHitDepthRaw, float firstHitViewZ)
{
    SSRHit outv;
    outv.uv = 0.0f.xx;
    outv.deviceZ = 0.0f;
    outv.visibility = 0.0f;
    outv.hit = 0;

    float tLow = tPrev;
    float tHigh = tCurr;
    float2 uvHigh = firstHitUV;
    float depthHighRaw = firstHitDepthRaw;
    float hitHighViewZ = firstHitViewZ;

    for (int j = 0; j < ssrOptimizedRefineSteps; ++j)
    {
        const float tMid = 0.5f * (tLow + tHigh);
        const float3 midVS = origin + pivot * tMid;
        const float4 midClip = mul(float4(midVS, 1.0f), SSR_TRACE_PROJ);
        if (midClip.w <= 0.0f)
        {
            tHigh = tMid;
            continue;
        }

        const float invMidW = rcp(midClip.w);
        const float2 midUV = float2(midClip.x, -midClip.y) * (0.5f * invMidW) + 0.5f;
        if (any(midUV < 0.0f) || any(midUV > 1.0f))
        {
            tHigh = tMid;
            continue;
        }

        const float midDepthRaw = SSR_TRACE_READ_DEPTH(midUV);
        if (midClip.z * invMidW < midDepthRaw)
        {
            tHigh = tMid;
            uvHigh = midUV;
            depthHighRaw = midDepthRaw;
            hitHighViewZ = midVS.z;
        }
        else
        {
            tLow = tMid;
        }
    }

    const float diffHigh = hitHighViewZ - SSR_TRACE_DEPTH_TO_VIEW_Z(depthHighRaw);
    if (diffHigh < thick)
    {
        return BuildSsrHit(pivot, unitPositionFrom, Pv, uvHigh, depthHighRaw, thick, diffHigh);
    }
    return outv;
}
#endif

// Hybrid logarithmic screen-space tracing inspired by Mara & McGuire's
// "Efficient GPU Screen-Space Ray Tracing".
SSRHit TraceSSR_LogMarch(float3 Pv, float3 Nv, float2 pixelCoord)
{
    SSRHit outv;
    outv.uv = 0.0f.xx;
    outv.deviceZ = 0.0f;
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

#if SSR_LOGMARCH_BATCH4
    // The t/stride sequence is depth-independent, so four candidates can be projected and sampled
    // together. All four reads are deliberately issued before the first result is inspected; that
    // exposes texture latency but can over-fetch by up to three taps on the terminal batch.
    for (int i = 0; i < ssrLogMarchSteps; i += 4)
    {
        // Build the same recurrence explicitly, but keep only compact float4 state. The first
        // version carried four arrays of per-candidate state and lost to register pressure.
        const float step1 = step * ssrStrideGrowth;
        const float t1 = tCurr + step1;
        const float step2 = step1 * ssrStrideGrowth;
        const float t2 = t1 + step2;
        const float step3 = step2 * ssrStrideGrowth;
        const float t3 = t2 + step3;
        const float step4 = step3 * ssrStrideGrowth;

        const float thickGrowth = ssrStrideGrowth * 1.01f;
        const float thick1 = thick * thickGrowth;
        const float thick2 = thick1 * thickGrowth;
        const float thick3 = thick2 * thickGrowth;
        const float thick4 = thick3 * thickGrowth;

        const float4 tValues = float4(tCurr, t1, t2, t3);
        const float4 tPrevValues = float4(tPrev, tCurr, t1, t2);
        const float4 thickValues = float4(thick, thick1, thick2, thick3);
        float2 sampleUV[4];
        float4 sampleDeviceZ;
        float4 depthRaw;
        bool sampleValid[4];

        bool validPrefix = true;

        [unroll] for (uint k = 0u; k < 4u; ++k)
        {
            const float3 sampleVS = origin + pivot * tValues[k];
            const float4 sampleClip = mul(float4(sampleVS, 1.0f), SSR_TRACE_PROJ);

            bool valid = validPrefix && tValues[k] <= ssrMaxDistanceVS && sampleClip.w > 0.0f;
            float2 uv = 0.0f.xx;
            float deviceZ = 0.0f;
            if (valid)
            {
                const float invW = rcp(sampleClip.w);
                uv = float2(sampleClip.x, -sampleClip.y) * (0.5f * invW) + 0.5f;
                valid = !any(uv < 0.0f) && !any(uv > 1.0f);
                deviceZ = sampleClip.z * invW;
            }

            sampleValid[k] = valid;
            validPrefix = valid;
            sampleUV[k] = valid ? uv : 0.0f.xx;
            sampleDeviceZ[k] = deviceZ;
        }

        // Separate unrolled loop is intentional: no crossing decision depends on an earlier read,
        // so DXC/hardware can keep all four texture requests in flight together.
        [unroll] for (uint readIndex = 0u; readIndex < 4u; ++readIndex)
        {
            depthRaw[readIndex] = SSR_TRACE_READ_DEPTH(sampleUV[readIndex]);
        }

        [unroll] for (uint hitIndex = 0u; hitIndex < 4u; ++hitIndex)
        {
            if (sampleValid[hitIndex] && sampleDeviceZ[hitIndex] < depthRaw[hitIndex])
            {
                return RefineSsrLogMarchBatchHit(
                    Pv, unitPositionFrom, pivot, origin,
                    tPrevValues[hitIndex], tValues[hitIndex], thickValues[hitIndex],
                    sampleUV[hitIndex], depthRaw[hitIndex],
                    origin.z + pivot.z * tValues[hitIndex]);
            }
        }

        if (!sampleValid[3])
        {
            break;
        }

        step = step4;
        tPrev = t3;
        tCurr = t3 + step4;
        thick = thick4;
    }
#else
    for (int i = 0; i < ssrLogMarchSteps && tCurr <= ssrMaxDistanceVS; ++i)
    {
        float3 sampleVS = origin + pivot * tCurr;
        float4 sampleClip = mul(float4(sampleVS, 1.0f), SSR_TRACE_PROJ);
        if (sampleClip.w <= 0.0f)
        {
            break;
        }

#if SSR_LOGMARCH_OPTIMIZED
        const float invSampleW = rcp(sampleClip.w);
        float2 sampleUV = float2(sampleClip.x, -sampleClip.y) * (0.5f * invSampleW) + 0.5f;
#else
        float2 sampleUV = float2(sampleClip.x / sampleClip.w * 0.5f + 0.5f,
                                 -sampleClip.y / sampleClip.w * 0.5f + 0.5f);
#endif

        if (any(sampleUV < 0.0f) || any(sampleUV > 1.0f))
        {
            break;
        }

        float depthRaw = SSR_TRACE_READ_DEPTH(sampleUV);
#if SSR_LOGMARCH_OPTIMIZED
        // Under reversed Z this is exactly the same crossing test as comparing linear view depth,
        // but it reuses clip.w and avoids DepthToViewZ's reciprocal on every coarse miss.
        const bool crossedSurface = sampleClip.z * invSampleW < depthRaw;
        float depthDiff = 0.0f;
#else
        float depthLin = SSR_TRACE_DEPTH_TO_VIEW_Z(depthRaw);
        float depthDiff = sampleVS.z - depthLin;
        const bool crossedSurface = depthDiff > 0.0f;
#endif

        if (crossedSurface)
        {
            float tLow = tPrev;
            float tHigh = tCurr;
            float2 uvHigh = sampleUV;
            float depthHighRaw = depthRaw;
#if SSR_LOGMARCH_OPTIMIZED
            float hitHighViewZ = sampleVS.z;
#else
            float diffHigh = depthDiff;
#endif

#if SSR_LOGMARCH_OPTIMIZED
            const int refineSteps = ssrOptimizedRefineSteps;
#else
            const int refineSteps = ssrRefineSteps;
#endif
            for (int j = 0; j < refineSteps; ++j)
            {
                float tMid = 0.5f * (tLow + tHigh);
                float3 midVS = origin + pivot * tMid;
                float4 midClip = mul(float4(midVS, 1.0f), SSR_TRACE_PROJ);
                if (midClip.w <= 0.0f)
                {
                    tHigh = tMid;
                    continue;
                }

#if SSR_LOGMARCH_OPTIMIZED
                const float invMidW = rcp(midClip.w);
                float2 midUV = float2(midClip.x, -midClip.y) * (0.5f * invMidW) + 0.5f;
#else
                float2 midUV = float2(midClip.x / midClip.w * 0.5f + 0.5f,
                                      -midClip.y / midClip.w * 0.5f + 0.5f);
#endif

                if (any(midUV < 0.0f) || any(midUV > 1.0f))
                {
                    tHigh = tMid;
                    continue;
                }

                float midDepthRaw = SSR_TRACE_READ_DEPTH(midUV);
#if SSR_LOGMARCH_OPTIMIZED
                const bool midCrossedSurface = midClip.z * invMidW < midDepthRaw;
#else
                float midDepthLin = SSR_TRACE_DEPTH_TO_VIEW_Z(midDepthRaw);
                float diffMid = midVS.z - midDepthLin;
                const bool midCrossedSurface = diffMid > 0.0f;
#endif

                if (midCrossedSurface)
                {
                    tHigh = tMid;
                    uvHigh = midUV;
                    depthHighRaw = midDepthRaw;
#if SSR_LOGMARCH_OPTIMIZED
                    hitHighViewZ = midVS.z;
#else
                    diffHigh = diffMid;
#endif
                }
                else
                {
                    tLow = tMid;
                }
            }

#if SSR_LOGMARCH_OPTIMIZED
            const float diffHigh = hitHighViewZ - SSR_TRACE_DEPTH_TO_VIEW_Z(depthHighRaw);
#endif
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
#endif

    return outv;
}

#endif // SSR_TRACE_LOGMARCH_HLSLI
