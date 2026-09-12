// Volumetric clouds: the view-ray march shared by the screen trace (cloud_trace_cs.hlsl, plan C2)
// and the environment capture (cloud_capture_cs.hlsl, plan C4). Transcription of UE
// VolumetricCloud.usf MainCommon: the sample distribution (:637-647), the participating-media
// octaves and two-lobe phase (:368-424, :329-337), the x^2 shadow march to the sun (:1101-1131),
// the Frostbite integration (:1352-1364), the transmittance-weighted mean depth (:1223-1229). The
// DENSITY is ours (cloud_common.hlsli, CLOUD_DENSITY). The caller owns the layer intersection, the
// depth clamp, the start offset, the aerial perspective, the exposure and the output.
//
// Requires cloud_common.hlsli (with CLOUD_DENSITY) before it.
#ifndef CLOUD_MARCH_HLSLI
#define CLOUD_MARCH_HLSLI

struct CloudMarchInputs
{
    float3 originMetres;    // world
    float3 rayDir;          // unit, world
    float tMinKm;           // the segment to march, along rayDir from the origin
    float tMaxKm;
    float startOffset;      // 0..1 of the first step: the trace's temporal noise, 0.5 for a deterministic march
    float sampleCountMax;   // steps over distanceToSampleCountMax (cloudTrace.x, or the capture's cap)
    float sampleCountMin;   // cloudTrace.y
    float invDistanceToMax; // cloudTrace.z, 1/km
    float3 distantSkyLight; // B5, raw luminance (0 when off)
};

struct CloudMarchResult
{
    float3 luminance;       // raw, NOT pre-exposed
    float transmittance;    // along the segment
    float tApKm;            // transmittance-weighted mean depth of the medium; tMaxKm when none was met
    bool sawCloud;          // any sample with extinction > 0
    uint stepsTaken;
    float debugCoverage;    // the first sample's weather coverage (debug view 1)
};

CloudMarchResult CloudMarch(CloudMarchInputs mi)
{
    const float3 camPos = mi.originMetres;
    const float3 rayDir = mi.rayDir;
    const float tMin = mi.tMinKm;
    const float tMax = mi.tMaxKm;

    // usf:637-647 grow the sample COUNT with the traced distance up to 15 km, as an integer, and
    // divide the distance by it -- so the step LENGTH jumps every time the traced length crosses a
    // multiple of 15 km / SampleCountMax. At UE's 768 that is 19.5 m and invisible; at a budget
    // count it is a whole cloud-feature and the jump draws CONCENTRIC RINGS around the zenith,
    // where the path through the shell is shortest (seen 2026-09-12 at high coverage). So the step
    // length is held CONSTANT at 15 km / SampleCountMax and the count follows the length (ceil, the
    // last segment partial) -- the same samples as UE's below the cap, without the quantisation --
    // and only past the cap does the step stretch, smoothly, as UE's does.
    const float lengthKm = tMax - tMin;
    const float sampleMax = max(mi.sampleCountMax, 1.0f);
    float stepT = max(1.0f / max(mi.invDistanceToMax * sampleMax, 1.0e-6f), lengthKm / sampleMax); // km
    uint stepCount = (uint)ceil(lengthKm / stepT);
    if (stepCount < (uint)mi.sampleCountMin) { stepCount = (uint)mi.sampleCountMin; stepT = lengthKm / (float)stepCount; }
    float t = tMin + mi.startOffset * stepT;

    const float cosTheta = dot(cloudSun.xyz, rayDir);
    const float basePhase = CloudPhase(cosTheta);
    const float3 sunIlluminance = cloudSunColor.rgb;
    const float3 distantSkyLight = mi.distantSkyLight;
    const float albedo = saturate(cloudMedium.y);
    const float invLayerHeight = cloudLayer.z;
    const float shadowLengthKm = cloudShadowTrace.y;
    const float shadowSteps = max(cloudShadowTrace.x, 1.0f);
    const float invShadowSteps = 1.0f / shadowSteps;

    float3 luminance = 0.0f;
    float transmittance = 1.0f;
    float tApWeighted = 0.0f, tApWeights = 0.0f;
    uint stepsTaken = 0u;
    float debugCoverage = 0.0f;

    [loop] for (uint i = 0; i < stepCount; ++i)
    {
        if (t >= tMax) { break; } // the jittered start can push the last sample past the end
        const float dtMetres = min(stepT, tMax - t) * CloudMetresPerKm; // the last segment is partial
        const float3 P = camPos + rayDir * (t * CloudMetresPerKm);
        const float normAlt = (length(CloudWorldToPlanetKm(P)) - cloudLayer.x) * invLayerHeight;
        if (normAlt <= 0.0f || normAlt >= 1.0f) { t += stepT; continue; } // usf:800-806
        ++stepsTaken;
        const CloudSample s = CloudSampleAt(P, normAlt);
        if (i == 0u || debugCoverage == 0.0f) { debugCoverage = s.coverage; }
        if (s.extinction <= 0.0f) { t += stepT; continue; } // usf:781-787 conservative density

        CloudMedia m = CloudSetupMedia(albedo, s.extinction, basePhase);

        // usf:864-869: the sky's light fades out towards the bottom of the layer.
        const float3 ambient = distantSkyLight * saturate(cloudShadowTrace.z + normAlt);

        // usf:1101-1131: the shadow march towards the sun, x^2 sample distribution, no jitter
        // ("this one cannot be hidden well by TAA"), stopping when the ray leaves the layer.
        //
        // THE MARCH IS BOUNDED BY THE LAYER'S EXIT ALONG THE SUN (ours). UE spread their 80 samples
        // over ShadowTracingDistance (15 km) and let the loop break at the layer's edge; with the
        // budget count (6) the same rule put the FIRST sample 208 m out, past every lump that could
        // shadow this point, and weighted it as 417 m of medium: any lump 200 m sunward cast a black
        // shadow a kilometre long, and a thin layer read as combed streaks converging on the sun's
        // azimuth (owner, 2026-09-12). Bounding the length by the exit spends the x^2 budget inside
        // the layer -- for a 0.5 km layer at a 25-degree sun the first sample lands 16 m out.
        float shadowExitKm = shadowLengthKm;
        {
            const float3 planetP = CloudWorldToPlanetKm(P);
            float2 tTopSun, tBottomSun;
            if (CloudRaySphere(planetP, cloudSun.xyz, 0.0f.xxx, cloudLayer.y, tTopSun) && tTopSun.y > 0.0f)
            {
                shadowExitKm = min(shadowExitKm, tTopSun.y);
            }
            if (CloudRaySphere(planetP, cloudSun.xyz, 0.0f.xxx, cloudLayer.x, tBottomSun) && tBottomSun.x > 0.0f)
            {
                shadowExitKm = min(shadowExitKm, tBottomSun.x); // a sun below the layer's top: out through the bottom
            }
        }
        const float shadowMarchKm = max(shadowExitKm, 1.0e-3f);
        // A FAR SHADOW SAMPLE DOES NOT RESOLVE THE DETAIL. With a point sun and opaque detail lumps
        // the single-scatter answer is a hard shadow column behind every lump, as long as the sun's
        // path through the layer; a thin layer reads as a comb converging on the sun's image (owner,
        // 2026-09-12: 24 shadow samples sharpened it, 2 softened it, the march off removed it; not
        // the history, the start jitter, the step, the noise textures or the base tile). Nothing in
        // the octaves fills an opaque column (every octave's transmittance is a power of the same
        // zero); what fills it in a real cloud is light diffused from the lit lumps around it, and
        // the mean of the detail over the neighbourhood is the density that light sees. So beyond
        // two detail features (a feature = a Worley cell = a quarter of the detail tile) a shadow
        // sample erodes by the detail's mean instead of by the point value (CloudSampleAt); inside
        // that distance the full detail stays, so a lump still shadows its own flank. A modelling
        // choice, ours: UE hands the distance to the material as ShadowSampleDistance (usf:1096,
        // MaterialTemplate.ush:2568) and leaves the same decision to the material author. By
        // distance, not by segment length, so that more samples refine the shadow instead of
        // bringing the comb back.
        const float detailFeatureKm = 0.25f / max(cloudShape.y, 1.0e-6f);
        float extinctionAcc[CLOUD_MS_COUNT];
        [unroll] for (int ms = 0; ms < CLOUD_MS_COUNT; ++ms) { extinctionAcc[ms] = 0.0f; }
        float previousNormT = 0.0f;
        [loop] for (float shadowT = invShadowSteps; shadowT <= 1.00001f; shadowT += invShadowSteps)
        {
            const float currentNormT = shadowT * shadowT;
            const float deltaNormT = currentNormT - previousNormT;
            const float sampleDistanceKm = shadowMarchKm * (previousNormT + deltaNormT * 0.5f);
            previousNormT = currentNormT;
            const float3 Ps = P + cloudSun.xyz * (sampleDistanceKm * CloudMetresPerKm);
            const float shadowAlt = (length(CloudWorldToPlanetKm(Ps)) - cloudLayer.x) * invLayerHeight;
            if (shadowAlt <= 0.0f || shadowAlt >= 1.0f) { break; }
            const float detailWeight = saturate(2.0f - sampleDistanceKm / detailFeatureKm);
            const CloudSample ss = CloudSampleAt(Ps, shadowAlt, detailWeight);
            float octaveExtinction = ss.extinction;
            float msE = saturate(cloudPhase.z);
            [unroll] for (int ms2 = 0; ms2 < CLOUD_MS_COUNT; ++ms2)
            {
                extinctionAcc[ms2] += octaveExtinction * deltaNormT;
                octaveExtinction *= msE; msE *= msE;
            }
        }
        [unroll] for (int ms3 = 0; ms3 < CLOUD_MS_COUNT; ++ms3)
        {
            m.transmittanceToSun[ms3] = exp(-extinctionAcc[ms3] * shadowMarchKm * CloudMetresPerKm);
        }

        // usf:1223-1229: where the aerial perspective is evaluated -- the transmittance-weighted mean depth.
        tApWeighted += t * transmittance;
        tApWeights += transmittance;

        // usf:1322-1372: scattered luminance towards the camera, octave by octave, the view
        // transmittance advanced by octave 0 only (Frostbite's analytic segment integral).
        [unroll] for (int ms4 = CLOUD_MS_COUNT - 1; ms4 >= 0; --ms4)
        {
            float3 sunSky = m.transmittanceToSun[ms4] * sunIlluminance * m.phase[ms4];
            // The distant sky light is kept out of the multi-scattering approximation (usf:1339).
            sunSky += ms4 == 0 ? ambient : 0.0f.xxx;
            const float3 scattered = sunSky * m.scattering[ms4];
            const float safeExtinction = max(1.0e-6f, m.extinction[ms4]);
            const float segmentT = exp(-safeExtinction * dtMetres);
            luminance += transmittance * (scattered - scattered * segmentT) / safeExtinction;
            if (ms4 == 0) { transmittance *= segmentT; }
        }
        // usf:1376: stop once nothing more can be seen through the cloud.
        if (transmittance < cloudTrace.w) { break; }
        t += stepT;
    }

    CloudMarchResult r;
    r.luminance = luminance;
    r.transmittance = transmittance;
    r.tApKm = tApWeights == 0.0f ? tMax : tApWeighted / max(1.0e-10f, tApWeights);
    r.sawCloud = tApWeights > 0.0f;
    r.stepsTaken = stepsTaken;
    r.debugCoverage = debugCoverage;
    return r;
}

#endif // CLOUD_MARCH_HLSLI
