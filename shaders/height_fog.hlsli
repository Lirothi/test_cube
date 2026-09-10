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
float HeightFogSkyRoughness(float headroom, float skyBlur)
{
    return max(skyBlur, 0.0f) * saturate(headroom);
}

// How far this pixel still is from the fog's own ceiling: 1 where the air has done nothing at all,
// 0 where it has taken over as completely as the ceiling allows. NOT the same as transmittance --
// it is measured against the floor currently in force, so it reaches 0 exactly where the fog stops
// growing. Takes the EFFECTIVE floor (HeightFogMinTransmittance), not the authored maxOpacity: the
// floor is released with depth, and a headroom computed against the authored value would never
// reach 0 for a released pixel -- leaving the sky blur and the sun lobe alive at the horizon, which
// is the seam this pair exists to prevent.
//
// Three things ride on it and all need the SAME zero: the sky blur, the sun lobe, and the phase.
float HeightFogHeadroom(float transmittance, float minTransmittance)
{
    return saturate((transmittance - minTransmittance) / max(1.0f - minTransmittance, 1.0e-4f));
}

struct HeightFogParams
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
float HeightFogLineIntegral(float heightFalloff, float rayDirectionY, float rayOriginTerms)
{
    const float falloff = max(-127.0f, heightFalloff * rayDirectionY);
    const float lineIntegral = (1.0f - exp2(-falloff)) / falloff;
    const float kLn2 = 0.69314718f;
    const float taylor = kLn2 - (0.5f * kLn2 * kLn2) * falloff;
    return rayOriginTerms * (abs(falloff) > 1.0e-6f ? lineIntegral : taylor);
}

// The per-unit line integral of the exponential profile along the view ray -- UE's
// GetExponentialHeightFog (HeightFogCommon.ush:213-299), two things of which matter here:
//
// (1) RayDirectionZ IS THE RAY'S FULL HEIGHT DELTA (:251, :283), not a slope. The line integral
//     is per unit of the [0,1] parameter along the segment and the segment's length multiplies
//     back in (HeightFogOpticalDepth). The first port divided the delta by the length, which
//     collapsed the height term to its Taylor limit for every ray: the whole descent from a high
//     camera integrated at the camera's own thin density. The froxel volume (plan part A), which
//     integrates the same profile numerically, exposed it as a step at the volume's far plane.
// (2) THE RAY IS RE-BASED AT THE START DISTANCE (:270-289). The fog-free band (and the volume's
//     exclude distance, which rides the same field) is not "the same average over a shorter
//     length": the origin term becomes the density at the height where the fog begins, and the
//     delta and the length are the remaining ones. That is what makes the seam between the volume
//     and the analytic continuation exact rather than an average.
//
// UE compute the shared per-unit integral ONCE and multiply it by two different lengths -- the
// view ray's, and the sun lobe's own shorter one. Splitting it the same way here is not tidiness:
// recomputing it per term would let the two drift apart under edits.
float HeightFogSharedIntegral(float distance, float cameraHeight, float pointHeight,
                               HeightFogParams p)
{
    if (p.density <= 0.0f)
    {
        return 0.0f;
    }

    const float rayLength = max(distance, 1.0e-4f);
    const float heightDelta = pointHeight - cameraHeight;
    // The exclusion point along the ray, in [0, 1] of its length (UE ExcludeIntersectionTime).
    const float excludeTime = saturate(max(p.startDistance, 0.0f) / rayLength);
    const float originHeight = cameraHeight + excludeTime * heightDelta;
    const float remainingDelta = (1.0f - excludeTime) * heightDelta;

    // UE's PreComputeFogOriginFactor at the re-based origin, with their IEEE exponent clamp.
    const float originPower = clamp(-p.heightFalloff * (originHeight - p.referenceHeight),
                                    -125.0f, 126.0f);
    const float rayOriginTerms = p.density * exp2(originPower);
    return HeightFogLineIntegral(p.heightFalloff, remainingDelta, rayOriginTerms);
}

// The view ray's own optical depth: the shared term over the fog-free start distance.
float HeightFogOpticalDepth(float sharedIntegral, float distance, HeightFogParams p)
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
float HeightFogMinTransmittance(float opticalDepth, float maxOpacity)
{
    const float floorT = 1.0f - saturate(maxOpacity);
    // -log2(floorT) is the optical depth at which the ceiling first bites; below it the exponential
    // is above the floor anyway and this whole term is inert.
    const float clipDepth = -log2(max(floorT, 1.0e-6f));
    // Released by HOW FAR THE FLOOR HAS OUTLIVED THE TRUTH, not by a multiple of clipDepth. The
    // difference matters at the horizon and only there. Measured on wind_test, camera 22.8 m, HDRI,
    // density 0.001, maxOpacity 0.9: clipDepth is 3.32, the optical depth at the water's last row is
    // about 6.6, so the old form (full release at 4x clipDepth = 13.3) had let go of barely a third.
    // The floor was still holding 6.7% of the water's own colour while its HONEST transmittance was
    // exp2(-6.6) = 1% -- propping a surface six times higher than it had any claim to. Against a sky
    // that is not fogged at all, that left a 32-level step along the horizon row, and it ran right
    // through the distant clouds.
    //
    // `opticalDepth - clipDepth` is that overshoot in stops: 0 where the ceiling first bites, 2 where
    // the surface is four times deeper into the fog than the ceiling ever described. Two stops is the
    // whole release. Still no new knob, and still scaled by the authored ceiling through clipDepth --
    // what changes is that the scale is now ABSOLUTE stops past the clip rather than a multiple of a
    // number that itself grows as the ceiling tightens.
    const float release = saturate((opticalDepth - clipDepth) * 0.5f);
    return floorT * (1.0f - release);
}

float HeightFogTransmittance(float opticalDepth, float minTransmittance)
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
// THE PHASE FUNCTION IS GONE, and `skyBackScatter` with it. It was `lerp(b, 1, forward*forward)`
// multiplying the base haze, sold as "how bright the haze is with the sun behind you". That is
// not a phase function: a phase function redistributes energy and integrates to 1 over the
// sphere, while this one is <= 1 in EVERY direction whenever b < 1, so it could only ever
// destroy light -- (1 + 2b)/3 of it on average, 40% at the value the atoll level shipped.
//
// It destroyed most where the fog was thickest, which is the horizon, so it drew a dark bar
// along the waterline: the sky fell to 117 there against 175 with the fog off, and the worst
// row-to-row step was 7.3 against a 0.28 no-fog control. Gating it by `headroom` (its first
// form) only moved the artifact -- the haze then went sky, darker, sky again as the gate
// released it, which is the same bar with a bright strip under it.
//
// UE carry no phase on their fog colour either. The directional part of scattering lives in
// the additive sun lobe below -- their DirectionalInscatteringColor -- which is the correct
// decomposition and not a concession: in-scattered radiance is the phase integrated against
// light from the whole sphere, and `L_sky(view) * phase(view . sun)` is not that integral.
// With the base isotropic and the lobe directional, the sky at the horizon matches the no-fog
// control to 0.31 of a level per row.
//
float3 HeightFogClampSkySample(float3 blurred, float3 unblurred)
{
    // One implementation, in ibl_common.hlsli. The fog and the water hit the same wall and must not
    // drift apart: if they disagreed, the sea and the air above it would answer to the same sky
    // differently and meet at the horizon in different weather.
    return IblClampToSharp(blurred, unblurred);
}

float3 HeightFogInscatter(float3 skyAlongView, float3 sunColor, float viewDotSun,
                           float opticalDepthShared, float distance, float headroom,
                           HeightFogParams p)
{
    // 1 looking into the sun, 0 looking away. Squared because Mie is forward-PEAKED rather than
    // linear across the sphere: a view 90 degrees off the sun already sits much nearer the backward
    // figure than the forward one, which a straight lerp would not say.
    // THE PHASE IS NOT GATED BY HEADROOM. It used to be `lerp(1, phase, headroom)`, on the argument
    // that multiple scattering isotropises a thick medium -- true in itself, but it made the haze
    // NON-MONOTONIC in optical depth: sky where there is no fog, darkened by the phase where the fog
    // is thin, and back to sky again where it saturates and the gate released the phase. On an open
    // view that last stretch is the degree above the horizon, and it read as a bright strip pinned
    // to the horizon with a darker sky above it. Measured on wind_test, procedural sky, the level's
    // skyBackScatter of 0.4: the fog's contribution ran -2.0, -3.2, -5.3, -8.0, -6.0, -0.9 down to
    // the waterline, and the worst row-to-row step was 2.56 against a 0.29 no-fog control.
    //
    // ...and applying it flat was no better, only differently wrong: `lerp(b, 1, f*f)` is <= 1 in
    // EVERY direction whenever b < 1, so it is not a phase function at all -- it is a dimmer. Its
    // average over the sphere is (1 + 2b)/3, which at the level's b = 0.4 means the model quietly
    // destroys 40% of the in-scattered energy, hardest where the fog is thickest. Measured on
    // wind_test looking away from the sun: the sky fell to 117 at the horizon against 158 with the
    // fog off -- a dark bar pinned to the waterline.
    //
    // So the base carries NO phase, which is UE's structure and the correct decomposition rather
    // than a concession. In-scattered radiance is the integral of the phase against the light
    // arriving from the whole sphere; `L_sky(view) * phase(view . sun)` is not that integral -- it
    // uses the sky along the VIEW as though it were light arriving from the SUN. UE split the two
    // instead: an isotropic base (their FogInscatteringColor / InscatteringColorCubemap, which
    // carries no directional term) plus the additive directional lobe below, which is where a phase
    // legitimately belongs. `skyBackScatter` therefore has no consumer any more.
    const float3 base = skyAlongView;

    const float lobe = pow(saturate(viewDotSun), max(p.sunScatterExponent, 1.0f));
    const float sunTravel = max(distance - p.sunScatterStartDistance, 0.0f);
    const float sunIntegral = opticalDepthShared * sunTravel;
    const float sunFactor = 1.0f - saturate(exp2(-sunIntegral));
    // NO `headroom` GATE ON THE LOBE EITHER, and the reason it had one has expired. It was added
    // when the SKY WAS NOT FOGGED: the lobe then reached only geometry, never the sky pixel next to
    // it, so at the horizon a warm band on the water stopped dead against an untouched sky. Fading
    // the lobe out as the fog saturated hid that seam -- and bought a worse one, because a term that
    // grows with distance and then vanishes is not monotonic. At a 2.8 degree sun it drew a dark
    // line along the horizon: the fog's contribution ran +0.2, +1.7, +2.4, -1.1, -2.2 over the last
    // dozen rows, worst step 1.92 against a 0.36 no-fog control.
    //
    // Compose fogs the sky now (its analytic branch lost the `z > kEps` gate, as UE's does not have
    // one either), so BOTH sides of that horizon get the lobe and the seam it was hiding cannot
    // form. What is left is UE's own term, unmodified: DirectionalInscatteringColor weighted by
    // DirectionalInscatteringExponent and held off the near field by its own start distance.
    const float sun = lobe * max(p.sunScatterStrength, 0.0f) * sunFactor;
    return base + sunColor * sun;
}

#endif // ATMOSPHERE_HLSLI
