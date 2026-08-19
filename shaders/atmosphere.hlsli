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

// THE FOG'S SKY SAMPLE IS BLURRED BY HOW LITTLE FOG THERE IS, and the two ends are not a
// compromise -- each is the physically right answer at its end:
//
// * FULLY FOGGED (transmittance -> 0, e.g. the horizon). The sky IS fog of infinite depth, the same
//   integral this file evaluates with no surface to stop it. So a surface the fog has fully taken
//   over MUST converge on the sky in exactly that direction, at mip 0, or the horizon shows a seam
//   between water that is 90% fog and sky that is 100% of it. Measured: blurring here moves the
//   horizon band by up to 19/255 and the water reads a shade cooler than the sky above it.
// * BARELY FOGGED (transmittance -> 1, e.g. palms a few tens of metres away). Light scattered
//   towards the eye over a SHORT column arrives from the whole sphere weighted by the phase
//   function -- it is not an image of whatever happens to stand behind the surface. Reading mip 0
//   there made it exactly that image: cloud edges and the sunset band printed themselves onto the
//   palms in front of them, which is what the user caught on the in-scattering debug view.
//
// The forward-scattered part of the phase function is not lost by blurring the near end: it is
// added separately as the sun lobe, the same split UE make between a non-directional inscattering
// colour and a directional one.
//
// Shared by compose and both ocean surfaces on purpose: the water and the land must not read the
// sky at different blurs or they meet at the shoreline in different weather. `skyBlur` 0 restores
// the original mip-0 read exactly, which is what the A/B for this was built on.
float AtmosphereSkyRoughness(float headroom, float skyBlur)
{
    return max(skyBlur, 0.0f) * saturate(headroom);
}

// How far this pixel still is from the fog's own ceiling: 1 where the air has done nothing at all,
// 0 where it has taken over as completely as the ceiling allows. NOT the same as transmittance --
// it is measured against the floor currently in force, so it reaches 0 exactly where the fog stops
// growing. Takes the EFFECTIVE floor (AtmosphereMinTransmittance), not the authored maxOpacity: the
// floor is released with depth, and a headroom computed against the authored value would never
// reach 0 for a released pixel -- leaving the sky blur and the sun lobe alive at the horizon, which
// is the seam this pair exists to prevent.
//
// Three things ride on it and all need the SAME zero: the sky blur, the sun lobe, and the phase.
float AtmosphereHeadroom(float transmittance, float minTransmittance)
{
    return saturate((transmittance - minTransmittance) / max(1.0f - minTransmittance, 1.0e-4f));
}

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
    float skyBackScatter;     // phase function: haze brightness with the sun BEHIND you (1 = flat)
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
//
// ...AND THE FLOOR IS RELEASED AGAIN ONCE THE FOG IS FAR PAST IT. UE can hold a hard floor because
// their fog colour is authored: nothing else in their frame has to agree with it. Ours is the sky,
// and THE SKY IS NEVER FOGGED -- it is already fog of infinite depth. A hard floor therefore leaves
// the water at the horizon holding (1 - maxOpacity) of its own colour against a sky holding none of
// it, and the two cannot meet: measured 9.9/255 of luminance step across the horizon row at 0.70.
// That made the entire range below 1.0 unusable on any open view, which is worse than the ceiling
// is useful.
//
// So the floor holds where it earns its keep -- distances around the depth at which it first bites,
// which is where "do not let the far hills vanish entirely" means something -- and lets go over the
// next factor of four, by which point the surface is thousands of times deeper into the fog than
// the ceiling ever described and the only honest answer is the sky. No new knob: the release is
// measured in multiples of the ceiling's OWN clip depth, so it scales with whatever the ceiling is
// set to.
float AtmosphereMinTransmittance(float opticalDepth, float maxOpacity)
{
    const float floorT = 1.0f - saturate(maxOpacity);
    // -log2(floorT) is the optical depth at which the ceiling first bites; below it the exponential
    // is above the floor anyway and this whole term is inert.
    const float clipDepth = -log2(max(floorT, 1.0e-6f));
    const float release = saturate((opticalDepth / max(clipDepth, 1.0e-4f) - 1.0f) / 3.0f);
    return floorT * (1.0f - release);
}

float AtmosphereTransmittance(float opticalDepth, float minTransmittance)
{
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
//
// AND THAT DEPARTURE IS WHY THE SUN LOBE IS FADED OUT BY `headroom`, WHERE UE LET THEIRS GROW
// WITHOUT LIMIT. Their non-directional colour is an authored constant that contains no sun, so the
// directional term is the only place the sun's forward scattering can come from and must survive to
// infinity. Ours is the sky itself, which ALREADY contains the sun's glow around it -- so a lobe
// that survives to infinity adds the sun a second time, and adds it only to geometry, never to the
// sky pixel next door. That is exactly the horizon seam the user reported: a warm band on the water
// stopping dead at the horizon line. Faded, the fog converges on the sky and the seam cannot exist;
// the lobe keeps doing its job over the distances where the base colour has NOT yet saturated,
// which is the only place it was ever describing something the sky sample was missing.
//
// Copying the multiplier without copying what cancels it is the same trap as
// [[transcription-half-a-pair]]: half a pair transcribed out of UE is not a transcription.
// `skyBackScatter` IS THE PHASE FUNCTION, AND IT IS WHY THERE IS NO SECOND DENSITY.
//
// A medium's density cannot depend on which way you look -- it is a property of the air, not of the
// camera. What IS directional is how much of the sun that air throws back at you: Mie scattering off
// haze is strongly forward-peaked, so looking into the sun the same air glows and looking away from
// it the same air is dim. That is why a backlit shore reads as thick haze and a front-lit one stays
// crisp, at identical density.
//
// Making density directional instead would break the thing density controls: EXTINCTION. Turn the
// camera and the far island would fade in and out of visibility -- not just change colour, actually
// change how much of itself survives -- which reads as the world breathing as you pan. Modulating
// the scattered colour leaves the island's contrast alone and only changes what the air adds.
//
// The near end is where this matters and the far end is where it must not exist: over a SHORT
// column what you see is mostly single-scattered sun, which is what the phase function describes;
// over an infinite one you are looking at the sky, whose own anisotropy is already baked into the
// sample. Hence `headroom` again -- the same fade the sun lobe and the sky blur use, and the same
// reason. 1.0 is the neutral value and reproduces the pre-phase image exactly.
float3 AtmosphereInscatter(float3 skyAlongView, float3 sunColor, float viewDotSun,
                           float opticalDepthShared, float distance, float headroom,
                           AtmosphereParams p)
{
    // 1 looking into the sun, 0 looking away. Squared because Mie is forward-PEAKED rather than
    // linear across the sphere: a view 90 degrees off the sun already sits much nearer the backward
    // figure than the forward one, which a straight lerp would not say.
    const float forward = saturate(0.5f + 0.5f * viewDotSun);
    const float phase = lerp(saturate(p.skyBackScatter), 1.0f, forward * forward);
    const float3 base = skyAlongView * lerp(1.0f, phase, saturate(headroom));

    const float lobe = pow(saturate(viewDotSun), max(p.sunScatterExponent, 1.0f));
    const float sunTravel = max(distance - p.sunScatterStartDistance, 0.0f);
    const float sunIntegral = opticalDepthShared * sunTravel;
    const float sunFactor = 1.0f - saturate(exp2(-sunIntegral));
    const float sun = lobe * max(p.sunScatterStrength, 0.0f) * sunFactor * saturate(headroom);
    return base + sunColor * sun;
}

#endif // ATMOSPHERE_HLSLI
