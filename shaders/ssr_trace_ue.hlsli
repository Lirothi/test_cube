#ifndef SSR_TRACE_UE_HLSLI
#define SSR_TRACE_UE_HLSLI

// Unreal's OWN screen-space reflection ray cast, transcribed byte-for-byte:
// SSRT/SSRTRayCast.ush `InitScreenSpaceRayFromWorldSpace` + `CastScreenSpaceRay`, exactly as
// SSRTReflections.usf's `RayCast()` invokes them:
//
//   * StartMipLevel = 1.0 and SlopeCompareToleranceScale = 4.0 are HARDCODED, as in their
//     RayCast(). The knobs this port used to expose (start mip, tolerance scale) and the
//     full-depth confirm/refine guard this port used to ADD are gone -- the guard was our own
//     invention layered on top of their acceptance rule, and the user's verdict on the composite
//     was "оно странное". This file now contains their math and nothing else.
//   * bExtendRayToScreenBorder = true (their SSR call): the ray is scaled to end exactly on the
//     viewport edge, and the tolerance derives from the ray's own depth span.
//   * the march is batches of four fixed steps against the FURTHEST HZB at a mip that creeps
//     upward with roughness only; the batch BREAKS on the first hit and the hit sample is
//     resolved AFTER the loop by their earliest-sample cascade + segment interpolation.
//   * a hit is `abs(diff + tol) < tol` with far-plane (device Z 0) hits rejected.
//
// The hit carries NO visibility of its own -- Unreal modulate the resolved colour by the
// reprojection vignette, the roughness fade and the intensity in SSRTReflections.usf, and the
// hosts (ssr_cs.hlsl, ocean_reflection_cs.hlsl) now do exactly that. The LogMarch visibility
// ladder (distance fade, grazing fade, thickness fade) that earlier versions of this port routed
// hits through belongs to the LogMarch, not to UE, and was a large part of why the two
// techniques could never be compared: they disagreed about MODULATION, not only about the search.
//
// P13's lesson stands and is preserved: the interleaved-gradient phase MUST be fed integer pixel
// coordinates (normalising them correlates adjacent pixels into diagonal combs) and the frame
// index MUST advance mod 8 (a spatial-only hash leaves the same sparse holes every frame).
//
// Requires from the host shader: `HzbFurthest`, `gSmpPoint`, `proj`.

// GetStepScreenFactorToClipAtScreenEdge, verbatim: scale RayStepScreen so the ray ends exactly on
// the viewport edge -- NumSteps then spans the VISIBLE part of the ray.
float SsrUeStepFactorToClipAtScreenEdge(float2 rayStartScreen, float2 rayStepScreen)
{
    const float rayStepScreenInvFactor = 0.5f * length(rayStepScreen);
    const float2 s = 1.0f - max(abs(rayStepScreen + rayStartScreen * rayStepScreenInvFactor) -
                                rayStepScreenInvFactor, 0.0f) / abs(rayStepScreen);
    return min(s.x, s.y) / rayStepScreenInvFactor;
}

// RandomInterleavedGradientNoise.ush verbatim. Integer pixel coordinates + StateFrameIndexMod8.
float SsrUeInterleavedGradientNoise(float2 uv, float frameId)
{
    uv += frameId * (float2(47.0f, 17.0f) * 0.695f);
    const float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
    return frac(magic.z * frac(dot(uv, magic.xy)));
}

// RandomPCG.ush::Rand3DPCG16, MonteCarlo.ush::Hammersley16 and the Duff tangent basis -- the
// exact building blocks SSRTReflections.usf uses for High/Epic roughness-aware ray directions.
uint3 SsrUeRand3DPCG16(int3 p)
{
    uint3 v = uint3(p);
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v >> 16u;
}

float2 SsrUeHammersley16(uint index, uint numSamples, uint2 random)
{
    const float e1 = frac((float)index / max((float)numSamples, 1.0f) +
                          (float)random.x * (1.0f / 65536.0f));
    const float e2 = (float)((reversebits(index) >> 16u) ^ random.y) * (1.0f / 65536.0f);
    return float2(e1, e2);
}

float3x3 SsrUeTangentBasis(float3 tangentZ)
{
    const float signZ = tangentZ.z >= 0.0f ? 1.0f : -1.0f;
    const float a = -rcp(signZ + tangentZ.z);
    const float b = tangentZ.x * tangentZ.y * a;
    const float3 tangentX = float3(1.0f + signZ * a * tangentZ.x * tangentZ.x,
                                   signZ * b, -signZ * tangentZ.x);
    const float3 tangentY = float3(b, signZ + a * tangentZ.y * tangentZ.y, -tangentZ.y);
    return float3x3(tangentX, tangentY, tangentZ);
}

// MonteCarlo.ush::ImportanceSampleVisibleGGX. SSR only consumes the sampled micronormal; UE
// defer the environment BRDF to composition too.
float3 SsrUeImportanceSampleVisibleGGX(float2 e, float alpha, float3 v)
{
    const float2 alpha2 = max(alpha, 1.0e-4f).xx;
    const float3 vh = normalize(float3(alpha2 * v.xy, v.z));
    const float phi = 6.28318530718f * e.x;

    const float a = saturate(alpha);
    const float s = 1.0f + length(v.xy);
    const float aSq = a * a;
    const float sSq = s * s;
    const float k = (sSq - aSq * sSq) / max(sSq + aSq * v.z * v.z, 1.0e-6f);
    const float z = lerp(1.0f, -k * vh.z, e.y);
    const float sinTheta = sqrt(saturate(1.0f - z * z));
    float3 h = float3(sinTheta * cos(phi), sinTheta * sin(phi), z) + vh;
    h = normalize(float3(alpha2 * h.xy, max(0.0f, h.z)));
    return h;
}

// Common.ush::Luminance -- UE's classic 0.3/0.59/0.11 weights, which is what BOTH sides of the
// multi-ray firefly compression in SSRTReflections.usf feed. Do not "fix" this to Rec.709: the
// pair `1/(1+L)` .. `1/(1-L)` only cancels when the SAME luminance is used on both ends.
float SsrUeLuminance(float3 c)
{
    return dot(c, float3(0.3f, 0.59f, 0.11f));
}

// SSRTRayCast.ush::ComputeHitVignetteFromScreenPos, verbatim.
float SsrUeHitVignette(float2 screenPos)
{
    const float2 vignette = saturate(abs(screenPos) * 5.0f - 4.0f);
    return saturate(1.0f - dot(vignette, vignette));
}

// InitScreenSpaceRayFromWorldSpace + CastScreenSpaceRay, fused exactly as RayCast() composes
// them. We are already in view space, so their TranslatedWorldToView is the identity and both
// TranslatedWorldToClip and ViewToClip are `proj`; our HZB covers its whole texture, so their
// HZBUvFactorAndInvFactor is 1 and drops out. Returns whether a hit was found; hitUVz is the
// resolved hit (uv + the RAY's device Z at the hit, which is what ReprojectHit consumes).
bool SsrUeRayCast(float3 Pv, float3 rayDirection, float roughness,
                  uint numSteps, float stepOffset, out float3 hitUVz)
{
    hitUVz = float3(0.0f, 0.0f, 0.0f);

    // --- InitScreenSpaceRayFromWorldSpace: WorldTMax = SceneDepth = Pv.z (their RayCast call) --
    const float sceneDepth = Pv.z;
    const float rayEndDistance = rayDirection.z < 0.0f
        ? min(-0.95f * sceneDepth / rayDirection.z, sceneDepth)
        : sceneDepth;

    const float3 rayEndView = Pv + rayDirection * rayEndDistance;

    const float4 rayStartClip = mul(float4(Pv, 1.0f), proj);
    const float4 rayEndClip = mul(float4(rayEndView, 1.0f), proj);
    const float3 rayStartScreen = rayStartClip.xyz * rcp(rayStartClip.w);
    const float3 rayEndScreen = rayEndClip.xyz * rcp(rayEndClip.w);

    const float4 rayDepthClip = rayStartClip + mul(float4(0.0f, 0.0f, rayEndDistance, 0.0f), proj);
    const float3 rayDepthScreen = rayDepthClip.xyz * rcp(rayDepthClip.w);

    float3 rayStepScreen = rayEndScreen - rayStartScreen;
    // bExtendRayToScreenBorder = true: the factor is applied unclamped.
    rayStepScreen *= SsrUeStepFactorToClipAtScreenEdge(rayStartScreen.xy, rayStepScreen.xy);

    float compareTolerance = max(abs(rayStepScreen.z),
                                 (rayStartScreen.z - rayDepthScreen.z) * 4.0f);

    // --- CastScreenSpaceRay, StartMipLevel = 1.0 --------------------------------------------
    float3 rayStartUVz = float3(rayStartScreen.xy * float2(0.5f, -0.5f) + 0.5f, rayStartScreen.z);
    float3 rayStepUVz = float3(rayStepScreen.xy * float2(0.5f, -0.5f), rayStepScreen.z);

    const float step = 1.0f / (float)numSteps;
    compareTolerance *= step;
    rayStepUVz *= step;

    const float3 rayUVz = rayStartUVz + rayStepUVz * stepOffset;

    float level = 1.0f;
    float lastDiff = 0.0f;
    float4 sampleDepthDiff = 0.0f.xxxx;
    bool4 sampleHit = bool4(false, false, false, false);
    bool foundHit = false;

    uint i;
    [loop] for (i = 0u; i < numSteps; i += 4u)
    {
        float2 samplesUV[4];
        float4 samplesZ;
        float4 samplesMip;
        [unroll] for (uint j = 0u; j < 4u; ++j)
        {
            samplesUV[j] = rayUVz.xy + ((float)i + (float)(j + 1u)) * rayStepUVz.xy;
            samplesZ[j] = rayUVz.z + ((float)i + (float)(j + 1u)) * rayStepUVz.z;
        }

        samplesMip.xy = level;
        level += (8.0f / (float)numSteps) * roughness;
        samplesMip.zw = level;
        level += (8.0f / (float)numSteps) * roughness;

        float4 sampleDepth;
        [unroll] for (uint k = 0u; k < 4u; ++k)
        {
            sampleDepth[k] = HzbFurthest.SampleLevel(gSmpPoint, samplesUV[k], samplesMip[k]).r;
        }

        // Do not report a hit on the far clip (device Z 0 under reversed-Z = the sky).
        sampleDepthDiff = samplesZ - sampleDepth;
        sampleHit = and(abs(sampleDepthDiff + compareTolerance) < compareTolerance,
                        sampleDepth != 0.0f);

        foundHit = any(sampleHit);
        [branch] if (foundHit)
        {
            break;
        }

        lastDiff = sampleDepthDiff.w;
    }

    [branch] if (foundHit)
    {
        // Their earliest-sample cascade, then segment interpolation.
        float depthDiff0 = sampleDepthDiff[2];
        float depthDiff1 = sampleDepthDiff[3];
        float time0 = 3.0f;

        [flatten] if (sampleHit[2])
        {
            depthDiff0 = sampleDepthDiff[1];
            depthDiff1 = sampleDepthDiff[2];
            time0 = 2.0f;
        }
        [flatten] if (sampleHit[1])
        {
            depthDiff0 = sampleDepthDiff[0];
            depthDiff1 = sampleDepthDiff[1];
            time0 = 1.0f;
        }
        [flatten] if (sampleHit[0])
        {
            depthDiff0 = lastDiff;
            depthDiff1 = sampleDepthDiff[0];
            time0 = 0.0f;
        }

        time0 += (float)i;

        const float timeLerp = saturate(depthDiff0 / (depthDiff0 - depthDiff1));
        const float intersectTime = time0 + timeLerp;

        hitUVz = rayUVz + rayStepUVz * intersectTime;
    }

    return foundHit;
}

#endif // SSR_TRACE_UE_HLSLI
