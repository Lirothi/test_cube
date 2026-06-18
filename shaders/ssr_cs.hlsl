#define SSR_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=3, flags=DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=2))"
// t0: LightTarget            (HDR color)
// t1: GB1 (normal.xy in 0..1, rough in A)
// t2: Depth (R32F SRV created from the DSV)
// u0: SSR output (premultiplied RGBA)
// s0: LinearClamp, s1: PointClamp

#pragma pack_matrix(row_major)
#include "utils.hlsl" // UnpackRM if needed

Texture2D   LightTarget : register(t0);
Texture2D   GB1         : register(t1);
Texture2D   DepthT      : register(t2);
RWTexture2D<float4> SsrOut : register(u0);
SamplerState gSmp       : register(s0);
SamplerState gSmpPoint  : register(s1);

cbuffer PerFrame : register(b0)
{
    float4x4 view, proj, invView, invProj;
    float    depthA, depthB, zNear, zFar;
    float2   screenSize;
    float2   invScreenSize;
    uint     tech;
    float3   _padding;
}

static const float ssrMaxDistanceVS = 100.0f; // maxDistance (view units)
static const float ssrResolution = 0.6f; // 0..1 (coarse-pass step size in screen space)
static const int ssrRefineSteps = 16; // number of refinement iterations
static const int ssrLogMarchSteps = 128; // number of logarithmic steps for the hybrid tracer
static const float ssrMinStrideVS = 0.01f; // minimum ray step in view space
static const float ssrStrideGrowth = 1.02f; // multiplicative stride growth per step
static const float ssrThicknessVS = 0.05f; // thickness (view units)
static const float ssrEdgeFadePx = 32.0f; // Smooth fade width near the screen border in pixels (16–48)
static const float ssrJitterStrength = 0.5f; // 0..1 — pixel offset applied to the start
static const float ssrGrazingMinZ = 0.01f; // Start fading reflections when Rv.z falls below this
static const float ssrGrazingMaxZ = 0.05f; // Fully enable reflections by this value
static const float kEps = 1e-6f;

static const uint SSR_TECHNIQUE_LETTIER = 0u;
static const uint SSR_TECHNIQUE_LOGMARCH = 1u;

float  DepthToViewZ_Fast(float d){ return depthB / (d - depthA); }
float3 ReconstructPosVS(float2 uv, float d){
    float2 ndc=UVtoNDC(uv); float4 clip=float4(ndc,d,1);
    float4 v=mul(clip, invProj); return v.xyz / max(v.w, kEps);
}
float  ReadDepth(float2 uv){ return DepthT.SampleLevel(gSmpPoint, uv, 0).r; }
float  EdgeFadePx(float2 uv){
    float2 dist=min(uv,1-uv)*screenSize; float m=min(dist.x,dist.y);
    return saturate(m/ssrEdgeFadePx);
}
float Hash12(float2 p)
{
    p = frac(p * float2(0.1031f, 0.11369f));
    p += dot(p, p.yx + 33.33f);
    return frac((p.x + p.y) * p.x);
}

struct SSRHit { float2 uv; float visibility; int hit; };

SSRHit BuildSsrHit(float3 pivot, float3 unitPositionFrom, float3 Pv, float2 uv, float depthRaw, float thicknessVS, float depthDiff)
{
    SSRHit outv;
    outv.uv = uv;

    float visibility = 1.0f;
    float3 positionTo = ReconstructPosVS(uv, depthRaw);
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

SSRHit TraceSSR_Lettier(float3 Pv, float3 Nv)
{
    SSRHit outv;
    outv.uv = 0.0f.xx;
    outv.visibility = 0.0f;
    outv.hit = 0;

    // Pivot is the reflected direction (see article: reflect(unitPositionFrom, normal))
    float3 unitPositionFrom = normalize(Pv);
    float3 pivot = normalize(reflect(unitPositionFrom, Nv));

    if (pivot.z <= 0.0f)
    {
        SSRHit r;
        r.uv = 0.0f.xx;
        r.visibility = 0.0f;
        r.hit = 0;
        return r;
    }

    float bias = saturate((1.0f - dot(pivot, Nv)));
    bias = bias * 0.5f;
    float thickness = ssrThicknessVS + bias;

    // Start/end of the ray in view space
    float3 startView = Pv + pivot * 0.0f;
    float3 endView = Pv + pivot * ssrMaxDistanceVS;

    // Convert to screen-space frame coordinates (pixels)
    float4 sClip = mul(float4(startView, 1), proj);
    float4 eClip = mul(float4(endView, 1), proj);
    sClip.xyz /= max(kEps, sClip.w);
    eClip.xyz /= max(kEps, eClip.w);
    float2 sFrag = float2((sClip.x * 0.5f + 0.5f) * screenSize.x,
                       (-sClip.y * 0.5f + 0.5f) * screenSize.y);
    float2 eFrag = float2((eClip.x * 0.5f + 0.5f) * screenSize.x,
                       (-eClip.y * 0.5f + 0.5f) * screenSize.y);

    // Coarse march along the screen-space line
    float deltaX = eFrag.x - sFrag.x;
    float deltaY = eFrag.y - sFrag.y;
    float useX = (abs(deltaX) >= abs(deltaY)) ? 1.0f : 0.0f;
    float delta = lerp(abs(deltaY), abs(deltaX), useX) * clamp(ssrResolution, 0.0f, 1.0f);
    float2 incr = float2(deltaX, deltaY) / max(delta, 0.001f);

    float search0 = 0.0f; // last miss
    float search1 = 0.0f; // current

    int hit0 = 0;
    int hit1 = 0;

    float viewDistance = startView.z; // In our setup view distance equals z (LH: +Z forward)
    float depthDiff = thickness;

    float2 frag = sFrag; // Current screen-space point (pixels)

    float2 uv; // Current UV (0..1)

    // First pass: coarse march in screen space
    int coarseCount = (int) delta;
    for (int i = 0; i < coarseCount; ++i)
    {
        frag += incr;
        uv = frag / screenSize;

        if (any(uv < 0.0f) || any(uv > 1.0f))
        {
            break;
        }

    // 1) Fraction traveled along the screen-space line
        search1 = lerp((frag.y - sFrag.y) / deltaY, (frag.x - sFrag.x) / deltaX, useX);
        search1 = saturate(search1);

    // 2) Scene position and depth at this sample
        float d = ReadDepth(uv);
        float dLin = DepthToViewZ_Fast(d);

    // 3) Perspective-correct ray length up to the current step (per Lettier)
        viewDistance = (startView.z * endView.z) / lerp(endView.z, startView.z, search1);

    // 4) View-z comparison (thickness is in the same units)
        depthDiff = viewDistance - dLin;

        if (depthDiff > 0.0f && depthDiff < thickness)
        {
            hit0 = 1;
            break;
        }
        else
        {
            search0 = search1;
        }
    }

    search1 = search0 + ((search1 - search0) / 2.0f);

    // Second pass: refinement (binary search)
    int refineSteps = hit0 * ssrRefineSteps;
    for (int i = 0; i < refineSteps; ++i)
    {
        float2 fragMix = lerp(sFrag, eFrag, search1);
        uv = fragMix / screenSize;

        if (any(uv < 0.0f) || any(uv > 1.0f))
        {
            hit1 = 0;
            break;
        }

        float d = ReadDepth(uv);
        float dLin = DepthToViewZ_Fast(d);
        viewDistance = (startView.z * endView.z) / lerp(endView.z, startView.z, search1);
        depthDiff = viewDistance - dLin;

        if (depthDiff > 0.0f && depthDiff < thickness)
        {
            hit1 = 1;
            search1 = search0 + ((search1 - search0) * 0.5f);
        }
        else
        {
            float temp = search1;
            search1 = search1 + ((search1 - search0) * 0.5f);
            search0 = temp;
        }
    }

    // Visibility (fades) and final reflection UVs
    float visibility = (float) hit1;
    if (visibility > 0.0f)
    {
        const float depthRaw = ReadDepth(uv);
        outv = BuildSsrHit(pivot, unitPositionFrom, Pv, uv, depthRaw, thickness, depthDiff);
    }
    else
    {
        outv.uv = uv;
        outv.visibility = 0.0f;
        outv.hit = 0;
    }

    return outv;
}

// Hybrid logarithmic screen-space tracing inspired by Mara & McGuire's "Efficient GPU Screen-Space Ray Tracing".
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

    float2 jitterSeed = pixelCoord * invScreenSize;
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
        float4 sampleClip = mul(float4(sampleVS, 1.0f), proj);
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

        float depthRaw = ReadDepth(sampleUV);
        float depthLin = DepthToViewZ_Fast(depthRaw);
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
                float4 midClip = mul(float4(midVS, 1.0f), proj);
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

                float midDepthRaw = ReadDepth(midUV);
                float midDepthLin = DepthToViewZ_Fast(midDepthRaw);
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

            //if (diffHigh < ssrThicknessVS)
            if (diffHigh < thick)
            {
                //return BuildSsrHit(pivot, unitPositionFrom, Pv, uvHigh, depthHighRaw, ssrThicknessVS, diffHigh);
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

[numthreads(8, 8, 1)]
[RootSignature(SSR_CS_RS)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint renderWidth, renderHeight;
    LightTarget.GetDimensions(renderWidth, renderHeight);
    uint ssrWidth, ssrHeight;
    SsrOut.GetDimensions(ssrWidth, ssrHeight);

    if (dispatchThreadId.x >= ssrWidth || dispatchThreadId.y >= ssrHeight)
    {
        return;
    }

    float2 fullRes = float2(renderWidth, renderHeight);
    float2 ssrRes = float2(max(ssrWidth, 1u), max(ssrHeight, 1u));
    float2 pixelScale = fullRes / ssrRes;
    float2 pixel = (float2(dispatchThreadId.xy) + 0.5f) * pixelScale;
    float2 uv = pixel / fullRes;

    float depth = ReadDepth(uv);
    float4 result = float4(0, 0, 0, 0);

    if (depth > 1e-6f)
    {
        float3 N_ws = normalize(GB1.SampleLevel(gSmp, uv, 0).rgb * 2 - 1);
        float3 Pv   = ReconstructPosVS(uv, depth);
        float3 Nv   = normalize(mul(N_ws, (float3x3)view));

        SSRHit ssr;
        if (tech == SSR_TECHNIQUE_LOGMARCH)
        {
            float2 seed = float2(dispatchThreadId.xy);
            ssr = TraceSSR_LogMarch(Pv, Nv, seed);
        }
        else
        {
            ssr = TraceSSR_Lettier(Pv, Nv);
        }
        if (ssr.hit != 0)
        {
            int2 ip = int2(ssr.uv * float2(renderWidth, renderHeight) + 0.5);
            ip = clamp(ip, int2(0, 0), int2(int(renderWidth) - 1, int(renderHeight) - 1));
            float3 c = LightTarget.Load(int3(ip, 0)).rgb;
            float vis = ssr.visibility;
            result = float4(c * vis, vis);
        }
    }

    SsrOut[dispatchThreadId.xy] = result;
}
