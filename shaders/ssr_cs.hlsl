#define SSR_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=8, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE))"
// t0: LightTarget            (HDR color sampled at the marched hit)
// t1: GB1 (reflector normal.rgb, shading model ID in A; SSR currently consumes RGB only)
// t2: Depth  (R32F) marched against in screen space (the OPAQUE scene depth)
// t3: OriginDepth (R32F) reconstructs the reflector surface position. For opaque this is the
//     same texture as t2; for glass it is the glass front-face depth (origin) while t2 stays
//     the opaque depth (march), so glass rays reflect the opaque scene.
// t4: Hzb (P6C) -- the FURTHEST depth pyramid (min device Z), the same one GTAO reads. That is
//     what Unreal's own SSR binds (EHZBType::FurthestHZB); it is used as cheap pre-filtered depth,
//     not as a hierarchy. Built from the same opaque depth as t2.
// t5: previous full-HDR SceneColor (UE's temporal SceneColor input)
// t6: current G-buffer motion, currUv - prevUv
// t7: GB0 packed roughness/metallic; UE High/Epic use roughness for their GGX ray directions
// u0: SSR output (premultiplied RGBA)
// s0: LinearClamp, s1: PointClamp

#pragma pack_matrix(row_major)
#include "utils.hlsli" // UnpackRM if needed

Texture2D   LightTarget : register(t0);
Texture2D   GB1         : register(t1);
Texture2D   DepthT      : register(t2);
Texture2D   OriginDepthT : register(t3);
Texture2D   HzbFurthest : register(t4);
Texture2D   PrevSceneColor : register(t5);
Texture2D   VelocityT    : register(t6);
Texture2D   GB0          : register(t7);
RWTexture2D<float4> SsrOut : register(u0);
SamplerState gSmp       : register(s0);
SamplerState gSmpPoint  : register(s1);

cbuffer PerFrame : register(b0)
{
    float4x4 view, proj, invView, invProj;
    float4x4 clipToPrevClip;
    float    depthA, depthB, zNear, zFar;
    float2   screenSize;
    float2   invScreenSize;
    uint     tech;
    // 0 = no depth pyramid exists yet (before the first build), so the UE march must not read it.
    uint     useHzb;
    uint     hzbMipCount;
    uint     frameIndexMod8;
    float2   hzbSize;     // pyramid mip 0, in texels (HALF the render resolution)
    float2   hzbInvSize;
    uint     sceneColorHistoryValid;
    uint     ueNumSteps;
    uint     ueNumRays;
    uint     ueGlossyRays;
    float    ueStartMipLevel;
    float    ueSlopeCompareToleranceScale;
    uint     ueConfirmRetries;
    uint     ueRefineSteps;
    uint     ueUseRoughnessTexture;
    float    ueRoughnessOverride;
    // P16.1: 1 / the pre-exposure the PREVIOUS frame's scene colour was written with. This pass
    // feeds compose, and compose scales what it writes -- so a hit colour taken from the history
    // has to come back to raw radiance first or it carries the factor twice. UE spell this
    // View.PrevSceneColorPreExposureCorrection. 1.0 when nothing is pre-exposed.
    float    invPrevPreExposure;
    // P16.8: the CURRENT frame's pre-exposure. The multi-ray compression below is a Reinhard curve,
    // and a Reinhard curve is only well conditioned near 1 -- which is exactly what pre-exposure
    // makes the scene. UE run the same compression on scene colour that is already pre-exposed.
    float    preExposure;
}

static const float kEps = 1e-6f;

// SSR_TECHNIQUE_* moved into ssr_trace_logmarch.hlsli below -- the ocean's planar reflection picks
// between the same two searches now. (A third option, a fixed-step screen-space march after
// Lettier's article, was removed with P6C step 6: LogMarch strictly dominated it and a third path
// turned every SSR comparison into a three-way.)

float  DepthToViewZ_Fast(float d){ return depthB / (d - depthA); }
float3 ReconstructPosVS(float2 uv, float d){
    float2 ndc=UVtoNDC(uv); float4 clip=float4(ndc,d,1);
    float4 v=mul(clip, invProj); return v.xyz / max(v.w, kEps);
}
float  ReadDepth(float2 uv){ return DepthT.SampleLevel(gSmpPoint, uv, 0).r; }
#include "ssr_trace_logmarch.hlsli"
#include "ssr_trace_ue.hlsli" // Unreal's own SSR ray cast; reuses SSRHit / BuildSsrHit above

// Port of SSRT/SSRTRayCast.ush::ComputeHitVignetteFromScreenPos. It rejects both a hit that
// leaves the current view and its reprojected location leaving the previous view.
float ComputeUeHitVignette(float2 screenPos)
{
    float2 vignette = saturate(abs(screenPos) * 5.0f - 4.0f);
    return saturate(1.0f - dot(vignette, vignette));
}

// Port of SSRT/SSRTRayCast.ush::ReprojectHit adapted to this renderer's plain RG16F velocity.
// UE's encoded velocity has an explicit validity sentinel; ours is cleared to zero, so zero uses
// the camera transform. A nonzero value is already currUv-prevUv and can be subtracted directly.
void ReprojectUeHit(float3 hitUVz, out float2 prevUV, out float vignette)
{
    const float2 thisScreen = UVtoNDC(hitUVz.xy);
    const float4 thisClip = float4(thisScreen, hitUVz.z, 1.0f);
    const float4 prevClip = mul(thisClip, clipToPrevClip);
    const bool validPrevClip = abs(prevClip.w) > kEps;
    float2 prevScreen = validPrevClip ? prevClip.xy / prevClip.w : float2(2.0f, 2.0f);
    prevUV = NDCToUV(prevScreen);

    const float2 velocity = VelocityT.SampleLevel(gSmpPoint, hitUVz.xy, 0.0f).xy;
    if (any(abs(velocity) > 1.0e-7f))
    {
        prevUV = hitUVz.xy - velocity;
        prevScreen = UVtoNDC(prevUV);
    }

    vignette = min(ComputeUeHitVignette(thisScreen), ComputeUeHitVignette(prevScreen));
}

float SsrUeLuminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

float4 ResolveUeHit(SSRHit ssr, bool compressForMultiRay)
{
    float vis = ssr.visibility;
    float3 c;
    if (sceneColorHistoryValid != 0u)
    {
        float2 prevUV;
        float hitVignette;
        ReprojectUeHit(float3(ssr.uv, ssr.deviceZ), prevUV, hitVignette);
        // Mirrors UE SampleScreenColor: bilinear history sample, NaNs/negative HDR -> black.
        c = PrevSceneColor.SampleLevel(gSmp, prevUV, 0.0f).rgb * invPrevPreExposure; // P16.1
        c = -min(-c, 0.0f);
        vis *= hitVignette;
    }
    else
    {
        uint renderWidth, renderHeight;
        LightTarget.GetDimensions(renderWidth, renderHeight);
        int2 ip = int2(ssr.uv * float2(renderWidth, renderHeight) + 0.5f);
        ip = clamp(ip, int2(0, 0), int2(int(renderWidth) - 1, int(renderHeight) - 1));
        c = LightTarget.Load(int3(ip, 0)).rgb;
    }

    if (compressForMultiRay)
    {
        // P16.8: compress in PRE-EXPOSED space. `c` is raw radiance -- thousands of cd/m2 since the
        // lights went physical -- and `L/(1+L)` maps that to 0.9997, where the inverse `1/(1-L)` is
        // one catastrophic cancellation away from nonsense. Pre-exposed, the same hit is near 1 and
        // the pair is exact. The caller divides the factor back out.
        c *= preExposure;
        c *= rcp(1.0f + SsrUeLuminance(c));
    }
    return float4(c * vis, vis);
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

    // Origin (reflector) depth: glass front-face depth for the glass pass, opaque depth for the
    // opaque pass (where t3 == t2). Pixels with no reflector (cleared depth 0) are skipped.
    float depth = OriginDepthT.SampleLevel(gSmpPoint, uv, 0).r;
    float4 result = float4(0, 0, 0, 0);

    if (depth > 1e-6f)
    {
        float3 N_ws = normalize(GB1.SampleLevel(gSmp, uv, 0).rgb * 2 - 1);
        float3 Pv   = ReconstructPosVS(uv, depth);
        float3 Nv   = normalize(mul(N_ws, (float3x3)view));

        if (tech == SSR_TECHNIQUE_UE && useHzb != 0u)
        {
            const float roughness = ueUseRoughnessTexture != 0u
                ? saturate(UnpackRM(GB0.SampleLevel(gSmpPoint, uv, 0.0f).a).x)
                : saturate(ueRoughnessOverride);
            const float3 unitPositionFrom = normalize(Pv);
            const float3 viewToCamera = -unitPositionFrom;

            uint numSteps = clamp(ueNumSteps, 4u, 64u);
            uint numRays = ueGlossyRays != 0u ? clamp(ueNumRays, 1u, 12u) : 1u;
            bool glossy = ueGlossyRays != 0u && numRays > 1u;

            // SSRTReflections.usf collapses High/Epic's multi-ray budget into one 24-step
            // geometric mirror ray below roughness 0.1.
            if (glossy && roughness < 0.1f)
            {
                numSteps = min(numSteps * numRays, 24u);
                numRays = 1u;
                glossy = false;
            }

            const float3x3 tangentBasis = SsrUeTangentBasis(Nv);
            const float3 tangentV = mul(tangentBasis, viewToCamera);
            const uint2 random = SsrUeRand3DPCG16(
                int3(int2(dispatchThreadId.xy), (int)frameIndexMod8)).xy;

            [loop] for (uint rayIndex = 0u; rayIndex < numRays; ++rayIndex)
            {
                float3 rayDirection;
                if (glossy)
                {
                    const float2 e = SsrUeHammersley16(rayIndex, numRays, random);
                    const float alpha = roughness * roughness;
                    const float3 tangentH = SsrUeImportanceSampleVisibleGGX(e, alpha, tangentV);
                    const float3 h = mul(tangentH, tangentBasis);
                    rayDirection = normalize(2.0f * dot(viewToCamera, h) * h - viewToCamera);
                }
                else
                {
                    rayDirection = normalize(reflect(unitPositionFrom, Nv));
                }

                const SSRHit ssr = TraceSSR_UeHzbRay(
                    Pv, unitPositionFrom, rayDirection, roughness, uv,
                    float2(dispatchThreadId.xy), frameIndexMod8, numSteps);
                if (ssr.hit != 0)
                {
                    result += ResolveUeHit(ssr, glossy);
                }
            }

            // P16.8 -- THE INVERSE HAS TO SEE WHAT THE COMPRESSION PRODUCED, and that is the mean
            // of the RAYS THAT HIT, not the mean over every ray fired.
            //
            // It used to divide by `numRays` first, mixing the misses in as zeros, and then invert.
            // `L/(1+L)` and `1/(1-L)` are exact inverses of each other only for a single unweighted
            // sample; feed the second one a coverage-weighted average and it is not an inverse at
            // all. With two of four rays hitting a surface of 3000 cd/m2 the old form returned
            // luminance 0.999 instead of ~1500 -- and it had been returning 0.33 instead of 0.5 for
            // years, which nobody could see while the whole scene sat near 1. That is why the
            // bronze floor's GLOSSY reflections went black the day the lights became physical, and
            // why the sharp ones (roughness < 0.1, single ray, never compressed) stayed correct.
            if (glossy)
            {
                const float coverage = result.a; // sum of the per-hit visibilities
                if (coverage > 1.0e-4f)
                {
                    float3 mean = result.rgb / coverage;   // un-premultiply: the mean COMPRESSED hit
                    mean *= rcp(max(1.0f - SsrUeLuminance(mean), 1.0e-4f));
                    mean *= rcp(max(preExposure, 1.0e-12f)); // back out of pre-exposed space
                    result.rgb = mean * coverage;          // and back to premultiplied
                }
                else
                {
                    result.rgb = 0.0f.xxx;
                }
            }
            result *= rcp((float)max(numRays, 1u));
        }
        else
        {
            // LogMarch, and the safety net for a frame with no pyramid yet.
            float2 seed = float2(dispatchThreadId.xy);
            const SSRHit ssr = TraceSSR_LogMarch(Pv, Nv, seed);
            if (ssr.hit != 0)
            {
                int2 ip = int2(ssr.uv * float2(renderWidth, renderHeight) + 0.5f);
                ip = clamp(ip, int2(0, 0), int2(int(renderWidth) - 1, int(renderHeight) - 1));
                const float3 c = LightTarget.Load(int3(ip, 0)).rgb;
                result = float4(c * ssr.visibility, ssr.visibility);
            }
        }
    }

    SsrOut[dispatchThreadId.xy] = result;
}
