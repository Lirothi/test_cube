#ifndef ATMOSPHERE_HLSLI
#define ATMOSPHERE_HLSLI

// P7 -- global analytic aerial perspective, transcribed from Unreal's exponential height fog
// (Shaders/Private/HeightFogCommon.ush: CalculateLineIntegralShared + GetExponentialHeightFog)
// rather than derived from scratch. The first version here WAS derived from scratch and differed
// from theirs in four ways that all mattered; they are called out at each site below.
//
// Its own header rather than a block inside compose_cs, because three passes consume it now:
// compose (opaque), and both ocean surfaces (ocean_surface.hlsl and ocean_surface_legacy.hlsli).
//
// NO FROXEL VOLUME -- the interface contract keeps the first implementation analytic, which also
// means it costs a handful of ALU in passes that already have depth and the sky bound.
//
// UNITS: UE author `FogDensity` and `FogHeightFalloff` against a CENTIMETRE world; this engine is
// metres. Their literal defaults (0.02 / 0.2) therefore do NOT carry over, and no conversion factor
// is invented here -- the shape of the model is UE's, the magnitudes are this project's to tune.
// What does transfer unchanged is the dimensionless one: DirectionalInscatteringExponent = 4.

struct AtmosphereParams
{
    float density;            // extinction per world unit AT the reference height
    float heightFalloff;      // base-2 e-folding rate of density with world height
    float referenceHeight;    // world Y at which `density` is exactly the value above
    float startDistance;      // metres of fog-free air in front of the camera
    float maxOpacity;         // ceiling on the fog's coverage
    float sunScatterStrength; // weight of the forward-scattered sun lobe
    float sunScatterExponent; // UE's DirectionalInscatteringExponent; their default is 4
    float sunScatterStartDistance; // UE keep the sun lobe off the near field with its own distance
};

// UE's CalculateLineIntegralShared, including the two things a from-scratch version gets wrong.
//
// (1) BASE 2, NOT e. UE integrate and transmit in exp2. That is not cosmetic: it rescales what
//     `density` means by ln2, so a model written in `exp` cannot be compared against their numbers
//     at all.
// (2) THE REMOVABLE SINGULARITY GETS A TAYLOR EXPANSION, not a constant. At Falloff -> 0 the
//     integral tends to ln2, not 1, and UE carry the first-order term as well so the branch is
//     continuous in the derivative and not merely in the value. A hard 1.0 there -- which is what
//     the first version here used -- is both the wrong limit and a visible crease.
// (3) The Falloff clamp is theirs too, and it is not decoration: without it exp2 of a large
//     negative number is what they describe as going "crazy", and a horizon line of NaNs is
//     exactly where an unclamped version lands.
float AtmosphereLineIntegral(float heightFalloff, float rayDirectionY, float rayOriginTerms)
{
    const float falloff = max(-127.0f, heightFalloff * rayDirectionY);
    const float lineIntegral = (1.0f - exp2(-falloff)) / falloff;
    const float kLn2 = 0.69314718f;
    const float taylor = kLn2 - (0.5f * kLn2 * kLn2) * falloff;
    return rayOriginTerms * (abs(falloff) > 1.0e-6f ? lineIntegral : taylor);
}

// Optical depth along the segment from the camera to the shaded point. `rayOriginTerms` is UE's
// collapsed origin factor: the density re-based to the CAMERA's height, which is what stops an
// elevated view getting a uniform screen-space wash -- from altitude most of the ray is in thin air.
// UE compute the shared per-unit integral ONCE and multiply it by two different lengths -- the
// view ray's, and the sun lobe's own shorter one. Splitting it the same way here is not tidiness:
// recomputing it per term would let the two drift apart under edits.
float AtmosphereSharedIntegral(float distance, float cameraHeight, float pointHeight,
                               AtmosphereParams p)
{
    if (p.density <= 0.0f)
    {
        return 0.0f;
    }

    // UE's PreComputeFogOriginFactor, with their IEEE exponent clamp.
    const float originPower = clamp(-p.heightFalloff * (cameraHeight - p.referenceHeight),
                                    -125.0f, 126.0f);
    const float rayOriginTerms = p.density * exp2(originPower);

    // Their RayDirectionZ is the ray's height delta over its LENGTH, not a normalised direction:
    // the integral is per unit of travel, and the length multiplies back in below.
    const float rayDirectionY = (pointHeight - cameraHeight) / max(distance, 1.0e-4f);
    return AtmosphereLineIntegral(p.heightFalloff, rayDirectionY, rayOriginTerms);
}

// The view ray's own optical depth: the shared term over the fog-free start distance.
float AtmosphereOpticalDepth(float sharedIntegral, float distance, AtmosphereParams p)
{
    return sharedIntegral * max(distance - p.startDistance, 0.0f);
}

// (4) MAX OPACITY IS A FLOOR ON TRANSMITTANCE, NOT A SCALE ON COVERAGE. UE:
// `ExpFogFactor = max(saturate(exp2(-integral)), MinFogOpacity)`. The first version here scaled
// (1 - t) instead, which bends the whole curve rather than clipping its far end -- a different
// image everywhere, not just at distance.
float AtmosphereTransmittance(float opticalDepth, float maxOpacity)
{
    const float minTransmittance = 1.0f - saturate(maxOpacity);
    return max(saturate(exp2(-opticalDepth)), minTransmittance);
}

// UE's directional inscattering: a cosine lobe around the light, `pow(saturate(dot(V, L)), e)` with
// e = DirectionalInscatteringExponent (their default 4), gated by its OWN line integral so the sun
// term only builds up past `sunScatterStartDistance`. They also keep the lobe separate from the
// non-directional colour, which is why it is added rather than blended.
//
// DELIBERATE DEPARTURE: `skyAlongView` is the sky sampled DOWN THE VIEW RAY where UE use an
// authored FogInscatteringColor (or an inscattering cubemap). Sampling the sky is what makes the
// fog agree with the horizon by construction instead of by tuning -- at grazing angles the fog
// colour and the pixel behind it converge on the same sample, so there is no seam to hide. That is
// the plan's item 4, and it is the one place this file is knowingly not a transcription.
float3 AtmosphereInscatter(float3 skyAlongView, float3 sunColor, float viewDotSun,
                           float opticalDepthShared, float distance, AtmosphereParams p)
{
    const float lobe = pow(saturate(viewDotSun), max(p.sunScatterExponent, 1.0f));
    const float sunTravel = max(distance - p.sunScatterStartDistance, 0.0f);
    const float sunIntegral = opticalDepthShared * sunTravel;
    const float sunFactor = 1.0f - saturate(exp2(-sunIntegral));
    return skyAlongView + sunColor * (lobe * max(p.sunScatterStrength, 0.0f) * sunFactor);
}

#endif // ATMOSPHERE_HLSLI
