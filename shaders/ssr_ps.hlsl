// RootSignature: CBV(b0) TABLE(SRV(t0) SRV(t1) SRV(t2)) TABLE(SAMPLER(s0) SAMPLER(s1))
// t0: LightTarget            (HDR color)
// t1: GB1 (normal.xy in 0..1, rough in A)
// t2: Depth (R32F SRV created from the DSV)
// s0: LinearClamp, s1: PointClamp

#pragma pack_matrix(row_major)
#include "utils.hlsl" // UnpackRM if needed

Texture2D   LightTarget : register(t0);
Texture2D   GB1         : register(t1);
Texture2D   DepthT      : register(t2);
SamplerState gSmp       : register(s0);
SamplerState gSmpPoint  : register(s1);

cbuffer PerFrame : register(b0)
{
    float4x4 view, proj, invView, invProj;
    float    depthA, depthB, zNear, zFar;
    float2   screenSize;
}

static const float ssrMaxDistanceVS = 100.0f; // maxDistance (view units)
static const float ssrResolution = 0.9f; // 0..1 (coarse-pass step size in screen space)
static const int ssrRefineSteps = 12; // number of refinement iterations
static const float ssrThicknessVS = 0.15f; // thickness (view units)
static const float ssrEdgeFadePx = 32.0f; // Smooth fade width near the screen border in pixels (16–48)
static const float ssrJitterStrength = 0.5f; // 0..1 — pixel offset applied to the start
static const float ssrGrazingMinZ = 0.01f; // Start fading reflections when Rv.z falls below this
static const float ssrGrazingMaxZ = 0.05f; // Fully enable reflections by this value
static const float kEps = 1e-6f;

struct VSOut { float4 H:SV_POSITION; float2 UV:TEXCOORD0; };
VSOut VSMain(uint vid:SV_VertexID){
    VSOut o; float2 p=float2(vid==2?3:-1, vid==1?3:-1);
    o.H=float4(p,0,1); o.UV=float2(p.x*0.5+0.5, 1-(p.y*0.5+0.5)); return o;
}

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
        float3 positionTo = ReconstructPosVS(uv, ReadDepth(uv));

        // 1 - facing the camera (as in the article: dot(-unitPos, pivot))
        {
            visibility *= (1.0f - max(dot(-unitPositionFrom, pivot), 0.0f));
        }
        // Proximity to the found hit
        {
            visibility *= (1.0f - clamp(depthDiff / thickness, 0.0f, 1.0f));
        }
        // Distance-based fade
        {
            visibility *= (1.0f - clamp(length(positionTo - Pv) / ssrMaxDistanceVS, 0.0f, 1.0f));
        }
        // Outside of the screen
        {
            visibility *= ((uv.x < 0.0f || uv.x > 1.0f) ? 0.0f : 1.0f) * ((uv.y < 0.0f || uv.y > 1.0f) ? 0.0f : 1.0f);
        }

        {
            visibility *= EdgeFadePx(uv);
            float grazing = saturate((pivot.z - ssrGrazingMinZ) / (ssrGrazingMaxZ - ssrGrazingMinZ));
            visibility *= grazing;
        }

        {
            visibility = clamp(visibility, 0.0f, 1.0f);
        }
    }

    outv.uv = uv;
    outv.visibility = visibility;
    outv.hit = (visibility > 0.0f) ? 1 : 0;
    return outv;
}

float4 PSMain(VSOut i) : SV_Target
{
    // Do not output anything for sky background
    float d = ReadDepth(i.UV);
    if (d >= 1.0f - 1e-6f) { return float4(0,0,0,0); }

    // Input vectors
    float3 N_ws = normalize(GB1.SampleLevel(gSmp, i.UV, 0).rgb * 2 - 1);
    float3 Pv   = ReconstructPosVS(i.UV, d);
    float3 Nv   = normalize(mul(N_ws, (float3x3)view));

    // Trace the ray
    SSRHit ssr = TraceSSR_Lettier(Pv, Nv);
    if (ssr.hit == 0) { return float4(0,0,0,0); }

    // Fetch color from LightTarget exactly at the hit pixel (premultiplied alpha)
    int2 ip = int2(ssr.uv * screenSize + 0.5);
    ip = clamp(ip, int2(0,0), int2(screenSize)-int2(1,1));
    float3 c = LightTarget.Load(int3(ip,0)).rgb;

    // premultiplied: rgb *= visibility, a = visibility
    float vis = ssr.visibility;
    return float4(c * vis, vis);
}