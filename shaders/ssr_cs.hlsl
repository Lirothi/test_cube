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
    // SSRParams.r / .g of SSRTReflections.usf: intensity in 0..1 and the roughness-fade scale
    // derived on the CPU from MaxRoughness exactly as ComputeSSRParams does.
    float    ueIntensity;
    float    ueRoughnessMaskScale;
    uint     uePad0;
    uint     uePad1;
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

    vignette = min(SsrUeHitVignette(thisScreen), SsrUeHitVignette(prevScreen));
}

// SSRTReflections.usf hit resolve, verbatim structure. The colour source is the previous frame's
// full-HDR scene colour (UE bind their TAA history there; the current lit target is the fallback
// exactly as their InputColor falls back to CurrentSceneColor when no history exists). The values
// STAY in their stored pre-exposed space through the firefly compression -- the space UE's own
// compression sees, and the P16.8 conditioning argument is the same one -- and the pre-exposure
// correction comes at the END, as SSRTReflections.usf line 404 applies it.
float4 SampleUeScreenColor(float3 hitUVz)
{
    float2 prevUV;
    float hitVignette;
    ReprojectUeHit(hitUVz, prevUV, hitVignette);

    float3 c;
    if (sceneColorHistoryValid != 0u)
    {
        // SampleScreenColor: bilinear, NaNs/negative HDR -> black.
        c = PrevSceneColor.SampleLevel(gSmp, prevUV, 0.0f).rgb;
    }
    else
    {
        // No history yet (first frame after a resize / level switch): the current lit target,
        // brought into current-pre-exposed space so the tail correction below stays uniform.
        c = LightTarget.SampleLevel(gSmp, hitUVz.xy, 0.0f).rgb * preExposure;
    }
    c = -min(-c, 0.0f);
    return float4(c, 1.0f) * hitVignette;
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
    // SNAPPED to the centre of the render texel that contains this SSR texel's centre. At a
    // fractional target scale the unsnapped centre lands BETWEEN render texels: point-sampling
    // depth there is a numerical coin flip between neighbours, bilinear normals a maximal blend
    // of four surfaces — and the DLSS jitter re-tosses the coin every frame, which is exactly
    // the no-temporal flicker this fixed. One snapped uv guarantees depth, normal and roughness
    // describe the SAME surface.
    float2 pixel = (float2(dispatchThreadId.xy) + 0.5f) * pixelScale;
    float2 snappedPixel = floor(pixel) + 0.5f;
    float2 uv = snappedPixel / fullRes;

    // Origin (reflector) depth: glass front-face depth for the glass pass, opaque depth for the
    // opaque pass (where t3 == t2). Pixels with no reflector (cleared depth 0) are skipped.
    float depth = OriginDepthT.SampleLevel(gSmpPoint, uv, 0).r;
    float4 result = float4(0, 0, 0, 0);

    if (depth > 1e-6f)
    {
        // POINT sample on purpose: packed unit normals must not be bilinearly mixed for ray
        // generation — an edge blend yields a direction no actual surface has (and uv is snapped
        // to a texel centre above, so point is exact rather than a boundary coin flip).
        float3 N_ws = normalize(GB1.SampleLevel(gSmpPoint, uv, 0).rgb * 2 - 1);
        float3 Pv   = ReconstructPosVS(uv, depth);
        float3 Nv   = normalize(mul(N_ws, (float3x3)view));

        if (tech == SSR_TECHNIQUE_UE && useHzb != 0u)
        {
            // ScreenSpaceReflections() of SSRTReflections.usf, in its exact order: roughness fade
            // early-out, quality permutation, one interleaved-gradient step offset per pixel, the
            // multi-ray GGX loop with the firefly-compression pair, then fade * intensity and the
            // pre-exposure correction. The one addition their shader does not need is the final
            // division back to RAW radiance -- their whole pipeline stays pre-exposed, ours hands
            // compose raw HDR, so the adapter is one rcp(preExposure) at the very end.
            //
            // NOTE their multi-ray tail divides by NumRays -- misses count as zeros -- and runs
            // the `1/(1-L)` expansion on that miss-diluted mean. P16.8 replaced that here with a
            // coverage-weighted inverse once; byte-for-byte restores THEIR form, whose
            // conditioning is fine BECAUSE the values are pre-exposed (near 1), which was the
            // actual P16.8 defect. Partial-coverage glossy is slightly dimmer than the coverage
            // form -- that is UE's own look.
            const float roughness = ueUseRoughnessTexture != 0u
                ? saturate(UnpackRM(GB0.SampleLevel(gSmpPoint, uv, 0.0f).a).x)
                : saturate(ueRoughnessOverride);
            // GetRoughnessFade: min(Roughness * SSRParams.y + 2, 1), and <= 0 traces nothing.
            const float roughnessFade = min(roughness * ueRoughnessMaskScale + 2.0f, 1.0f);

            [branch] if (roughnessFade > 0.0f)
            {
                const float3 unitPositionFrom = normalize(Pv);
                const float3 viewToCamera = -unitPositionFrom;
                const float a = roughness * roughness;

                uint numSteps = clamp(ueNumSteps, 4u, 64u);
                uint numRays = ueGlossyRays != 0u ? clamp(ueNumRays, 1u, 12u) : 1u;

                // One offset per pixel: InterleavedGradientNoise(SvPosition.xy,
                // StateFrameIndexMod8) - 0.5, shared by every ray of the loop.
                const float stepOffset =
                    SsrUeInterleavedGradientNoise(float2(dispatchThreadId.xy),
                                                  (float)frameIndexMod8) - 0.5f;

                [branch] if (numRays > 1u)
                {
                    const uint2 random = SsrUeRand3DPCG16(
                        int3(int2(dispatchThreadId.xy), (int)frameIndexMod8)).xy;
                    const float3x3 tangentBasis = SsrUeTangentBasis(Nv);
                    const float3 tangentV = mul(tangentBasis, viewToCamera);

                    // The mirror collapse: below roughness 0.1 the whole budget becomes one
                    // 24-step-capped geometric ray.
                    const bool mirror = roughness < 0.1f;
                    if (mirror)
                    {
                        numSteps = min(numSteps * numRays, 24u);
                        numRays = 1u;
                    }

                    [loop] for (uint rayIndex = 0u; rayIndex < numRays; ++rayIndex)
                    {
                        const float2 e = SsrUeHammersley16(rayIndex, numRays, random);
                        const float3 h = mul(SsrUeImportanceSampleVisibleGGX(e, a, tangentV),
                                             tangentBasis);
                        float3 rayDirection = 2.0f * dot(viewToCamera, h) * h - viewToCamera;
                        if (mirror)
                        {
                            rayDirection = reflect(unitPositionFrom, Nv);
                        }

                        float3 hitUVz;
                        [branch] if (SsrUeRayCast(Pv, rayDirection, roughness,
                                                  numSteps, stepOffset, hitUVz))
                        {
                            float4 sampleColor = SampleUeScreenColor(hitUVz);
                            sampleColor.rgb *= rcp(1.0f + SsrUeLuminance(sampleColor.rgb));
                            result += sampleColor;
                        }
                    }

                    result /= max((float)numRays, 0.0001f);
                    result.rgb *= rcp(1.0f - SsrUeLuminance(result.rgb));
                }
                else
                {
                    // Low/Medium: one mirror ray. (Their single-ray glossy variant exists only
                    // under the SSD denoiser, which this engine does not run.)
                    const float3 rayDirection = reflect(unitPositionFrom, Nv);
                    float3 hitUVz;
                    [branch] if (SsrUeRayCast(Pv, rayDirection, roughness,
                                              numSteps, stepOffset, hitUVz))
                    {
                        result = SampleUeScreenColor(hitUVz);
                    }
                }

                result *= roughnessFade;
                result *= ueIntensity;
                // PrevSceneColorPreExposureCorrection: View.PreExposure / PrevPreExposure when the
                // colour came from the history, 1 when it came from the current target -- then the
                // engine adapter back to raw radiance for compose.
                result.rgb *= (sceneColorHistoryValid != 0u)
                    ? preExposure * invPrevPreExposure : 1.0f;
                result.rgb *= rcp(max(preExposure, 1.0e-12f));
            }
        }
        else
        {
            // LogMarch, and the safety net for a frame with no pyramid yet.
            float2 seed = float2(dispatchThreadId.xy);
            const SSRHit ssr = TraceSSR_LogMarch(Pv, Nv, seed);
            if (ssr.hit != 0)
            {
                // BILINEAR hit colour, as UE's SampleScreenColor does. A point Load flips a whole
                // texel of frond-green against sky whenever jitter nudges the hit sub-texel — the
                // measured tremble of reflected palm crowns at grazing views was exactly that.
                // NaN/negative guard verbatim from UE.
                float3 c = LightTarget.SampleLevel(gSmp, ssr.uv, 0.0f).rgb;
                c = -min(-c, 0.0f);
                result = float4(c * ssr.visibility, ssr.visibility);
            }
        }
    }

    SsrOut[dispatchThreadId.xy] = result;
}
