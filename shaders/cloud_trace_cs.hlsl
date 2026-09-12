// Volumetric clouds, plan C2: the view ray march, at HALF the render resolution, one ray per texel.
// Transcription of UE VolumetricCloud.usf MainCommon (:436-1740) minus the material system: the layer
// intersection (:459-497), the depth-buffer clamp (:534-593), the sample distribution (:637-647),
// the participating-media octaves and two-lobe phase (:368-424, :329-337), the x^2 shadow march to
// the sun (:1101-1131), the Frostbite integration (:1352-1364), the aerial perspective applied once
// at the transmittance-weighted mean depth (:1503-1541), the output (:1710-1737). The DENSITY is ours
// (cloud_common.hlsli). Deltas from UE, all deliberate:
//   * blue noise -> interleaved gradient noise rotated by the frame index (no blue-noise asset here);
//   * the height fog is NOT applied here (:1548-1567): compose fogs the clouded pixel afterwards
//     exactly as it fogs the sky, so the fog sits over sky and cloud alike with one model;
//   * no ground contribution, no local lights, no second light, no per-sample atmosphere
//     transmittance -- all off in UE's defaults too;
//   * the half-res texel reads the FURTHEST of its four full-res depths (UE's min/max mode does
//     the same at the tracing resolution), so a cloud behind a thin silhouette is still traced.
//
// b0 CloudCB (cloud_common.hlsli)
// t0 CloudBaseNoise  t1 CloudDetailNoise  t2 CloudWeather  t3 Depth (render res)
// t4 SkyAerialVolume (B3)  t5 DistantSkyLight (B5, raw luminance)
// u0 CloudColor (rgb pre-exposed luminance, a transmittance)  u1 CloudDepth (km, the front)
// s0 linear wrap (noise)  s1 linear clamp (aerial volume)
#pragma pack_matrix(row_major)
#define CLOUD_DENSITY
#define CLOUD_T_BASE t0
#define CLOUD_T_DETAIL t1
#define CLOUD_T_WEATHER t2
#define CLOUD_S_WRAP s0
#include "cloud_common.hlsli"
#include "cloud_march.hlsli"
#include "sky_aerial_common.hlsli"

#define CLOUD_TRACE_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=6, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(Sampler(s0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE))"

Texture2D<float> DepthT : register(t3);
Texture3D<float4> SkyAerialVolume : register(t4);
Texture2D<float4> DistantSkyLight : register(t5);
RWTexture2D<float4> CloudColor : register(u0);
RWTexture2D<float> CloudDepth : register(u1);
SamplerState gSmpLinearClamp : register(s1);

// The value a texel with no cloud in front of anything carries: far, and inside FP16.
static const float kCloudNoDepthKm = 60000.0f;

// The start offset's noise, per pixel and per frame. UE use a blue-noise texture indexed by
// StateFrameIndexMod8; we have no blue-noise asset. This is the canonical TEMPORAL interleaved
// gradient noise: a FIXED spatial IGN phase plus the golden ratio times the frame index. Two
// versions came before it and both were measured wrong (owner, 2026-09-12):
//   * IGN TRANSLATED by a fixed diagonal vector every frame -- IGN is a hatch, its values run in
//     lines, and a long history averaged the moving hatch into broad diagonal bands across every
//     thick cloud;
//   * white noise from an integer hash -- no bands, but three times the temporal sigma (0.14 ->
//     0.48 codes): a random sequence per pixel converges as 1/sqrt(N).
// The golden-ratio sequence per pixel is the best-distributed 1D sequence there is (error ~ 1/N),
// so the accumulation converges fastest, and nothing moves spatially, so there is no direction
// for the resolve to smear the hatch along -- it fades out with the history instead.
float CloudStartNoise(uint2 pixel, float frame)
{
    const float ign = frac(52.9829189f * frac(dot(float2(pixel), float2(0.06711056f, 0.00583715f))));
    return frac(ign + 0.61803398875f * frame);
}

[numthreads(8, 8, 1)]
[RootSignature(CLOUD_TRACE_RS)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint2 outSize = uint2(cloudOutput.xy);
    if (any(id.xy >= outSize)) { return; }

    const float2 uv = (float2(id.xy) + 0.5f) / cloudOutput.xy;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float3 camPos = cloudCamera.xyz;
    // Any clip z in front of the camera gives a point on the pixel's ray (0.5 under reversed Z).
    const float4 onRay = mul(float4(ndc, 0.5f, 1.0f), cloudInvViewProjNoJitter);
    const float3 rayDir = normalize(onRay.xyz / onRay.w - camPos);
    const float3 originKm = CloudWorldToPlanetKm(camPos);
    const float preExposure = cloudOutput.z;
    const uint debugView = (uint)cloudAerial.w;

    float4 outColor = float4(0.0f, 0.0f, 0.0f, 1.0f);
    float outDepth = kCloudNoDepthKm;

    float tMin, tMax;
    float2 tBottom;
    bool trace = CloudLayerSegment(originKm, rayDir, tMin, tMax, tBottom);
    // usf:520-524: nothing to trace, or the layer starts too far away to be worth it.
    trace = trace && tMax > tMin && tMin <= cloudWind.w;

    // usf:534-593: the depth buffer. The FURTHEST of the four full-res texels under this one; the
    // sky is device z 0 and is left at the layer's far end.
    float tDepthKm = kCloudNoDepthKm;
    if (trace)
    {
        const uint2 depthSize = uint2(cloudDepth.xy);
        const uint2 base = min(id.xy * 2u, depthSize - 1u);
        const uint2 next = min(base + 1u, depthSize - 1u);
        const float z = min(min(DepthT.Load(int3(base, 0)), DepthT.Load(int3(next.x, base.y, 0))),
                            min(DepthT.Load(int3(base.x, next.y, 0)), DepthT.Load(int3(next, 0))));
        if (z > 1.0e-7f)
        {
            const float4 P = mul(float4(ndc, z, 1.0f), cloudInvViewProjNoJitter);
            tDepthKm = length(P.xyz / P.w - camPos) / CloudMetresPerKm;
            if (tDepthKm < tMin) { trace = false; } // geometry in front of the layer's near face
            tMax = min(tMax, tDepthKm);
        }
    }

    if (!trace)
    {
        CloudColor[id.xy] = outColor;
        CloudDepth[id.xy] = outDepth;
        return;
    }

    // usf:621-627, TracingMaxDistanceMode 0: a bounded distance past the layer's entry point.
    tMax = tMin + min(cloudShadowTrace.w, tMax - tMin);
    outDepth = tMax; // usf:1735 NoCloudDepth: the far end of what was traced, for the reprojection

    // The march itself is cloud_march.hlsli, shared with the environment capture (C4): the sample
    // distribution, the shadow march, the octaves and the integration live there.
    CloudMarchInputs mi;
    mi.originMetres = camPos;
    mi.rayDir = rayDir;
    mi.tMinKm = tMin;
    mi.tMaxKm = tMax;
    mi.startOffset = CloudStartNoise(id.xy, cloudTemporal.w);
    mi.sampleCountMax = cloudTrace.x;
    mi.sampleCountMin = cloudTrace.y;
    mi.invDistanceToMax = cloudTrace.z;
    mi.distantSkyLight = cloudAerial.z > 0.0f ? DistantSkyLight.Load(int3(0, 0, 0)).rgb : 0.0f.xxx;
    const CloudMarchResult march = CloudMarch(mi);
    const float3 luminance = march.luminance;
    const float transmittance = march.transmittance;
    const uint stepsTaken = march.stepsTaken;
    const float debugCoverage = march.debugCoverage;

    // usf:1503-1541: aerial perspective OVER the cloud, once, at tAP. The volume is stored
    // pre-exposed with this frame's exposure, so the luminance is exposed first and blended as is.
    const float tAP = march.tApKm;
    float3 exposed = luminance * preExposure;
    if (cloudAerial.x > 0.0f && march.sawCloud)
    {
        const float3 P = camPos + rayDir * (tAP * CloudMetresPerKm);
        const float4 clip = mul(float4(P, 1.0f), cloudViewProjNoJitter);
        const float viewDepthKm = clip.w / CloudMetresPerKm;
        if (clip.w > 0.0f && viewDepthKm > cloudAerial.y)
        {
            const float startKm = cloudAerial.y * tAP / max(viewDepthKm, 1.0e-4f);
            const float2 apUv = (clip.xy / clip.w) * float2(0.5f, -0.5f) + 0.5f;
            const float4 ap = SampleSkyAerial(SkyAerialVolume, gSmpLinearClamp, saturate(apUv), tAP, startKm, 1.0f);
            exposed = ap.rgb * (1.0f - transmittance) + ap.a * exposed;
        }
    }

    // usf:1710-1737.
    const float grayT = transmittance < cloudTrace.w ? 0.0f : transmittance;
    outColor = float4(exposed, grayT);
    outDepth = grayT > 0.99f ? tMax : tAP;

    // Debug views, DISPLAY-LINEAR gray in .rgb, alpha 0 so compose shows them as the pixel
    // (see the fog's rule: a gray written as radiance is black under a daylight EV).
    if (debugView == 1u) { outColor = float4(debugCoverage.xxx, 0.0f); }
    else if (debugView == 2u) { outColor = float4(grayT.xxx, 0.0f); }
    else if (debugView == 3u) { outColor = float4(((float)stepsTaken / max(cloudTrace.x, 1.0f)).xxx, 0.0f); }

    CloudColor[id.xy] = outColor;
    CloudDepth[id.xy] = outDepth;
}
