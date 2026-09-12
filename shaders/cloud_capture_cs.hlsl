// Volumetric clouds, plan C4: the clouds in the sky environment (B4). THIS IS UE'S REAL-TIME SKY
// LIGHT CAPTURE ROLE, not a march per reflection ray: UE render the cloud into the sky light's
// capture (VolumetricCloudRendering.cpp:2257 IsReflectionRendering; :2266-2280 -- SampleCountMax =
// min(96 * ReflectionViewSampleCountScale, r.VolumetricCloud.ReflectionRaySampleMaxCount 80), the
// shadow march min(10 * scale, 24)) and every reflection -- ray-traced misses and hits, water,
// glass -- reads that cubemap. Here the sky's radiance cube is composited IN PLACE, mip by mip,
// between the SkyView capture and the probe filter (SkyAtmosphere::BuildEnvironment), so the
// specular and irradiance probes, compose's RT fallback, the ocean's off-screen reflection, the
// glass and the fog's sky picture all see one sky with one set of clouds:
//     radiance = radiance * T + L,   L = the cloud's luminance with the aerial perspective over it.
// Deltas from UE, deliberate:
//   * the probe is sea level under the CAMERA (the sky's probe is global; a global cloud probe put
//     the world origin's clouds over the player's head);
//   * the march is deterministic (start offset 0.5, no jitter, no history): the cube refreshes every
//     few frames and must not flicker between refreshes;
//   * the aerial perspective to the cloud front is a bounded integration of the LUTs the aerial
//     volume is built from, from the probe (there is no camera volume for a cube) -- the cube's
//     horizon row is the fog's far colour (B6): a far cloud without its air in front is a dark band;
//   * no depth buffer: the planet is the ground.
//
// b0 CloudCB with the sky's SkyAtmosphereCB appended (CLOUD_WITH_SKY_CB, cloud_common.hlsli)
// t0 CloudBaseNoise  t1 CloudDetailNoise  t2 CloudWeather  t3 DistantSkyLight (B5, raw luminance)
// t4 Transmittance LUT  t5 MultiScatter LUT
// u0 the radiance mip, six faces in the array     s0 linear wrap (noise)  s1 linear clamp (LUTs)
#pragma pack_matrix(row_major)
#define CLOUD_DENSITY
#define CLOUD_WITH_SKY_CB
#define CLOUD_T_BASE t0
#define CLOUD_T_DETAIL t1
#define CLOUD_T_WEATHER t2
#define CLOUD_S_WRAP s0
#include "cloud_common.hlsli"
#define SKY_VIEW
#define SKY_CB_EXTERNAL
#include "sky_atmosphere.hlsli"
#include "cloud_march.hlsli"
#include "sky_ibl_common.hlsli"

#define CLOUD_CAPTURE_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=6, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(Sampler(s0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE))"

Texture2D<float4> DistantSkyLight : register(t3);
Texture2D<float4> TransmittanceLut : register(t4);
Texture2D<float4> MultiScatterLut : register(t5);
RWTexture2DArray<float4> Radiance : register(u0);
SamplerState gSmpLinearClamp : register(s1);

// UE r.VolumetricCloud.ReflectionRaySampleMaxCount (80) at the default scale, independent of the
// main view's budget. The shadow march keeps the level's count (UE: min(10 * scale, 24)) so the
// reflected self-shadow is the one on screen. The aerial segment: eight steps, the volume's own
// count for its first four slices.
static const float kCaptureSampleCountMax = 80.0f;
static const uint kCaptureAerialSamples = 8u;

[numthreads(8, 8, 1)]
[RootSignature(CLOUD_CAPTURE_RS)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint size = (uint)cloudOutput.x;
    const uint face = id.y / size;
    const uint2 pixel = uint2(id.x, id.y % size);
    if (pixel.x >= size || face >= 6u) { return; }
    const float2 uv = (float2(pixel) + 0.5f) / (float)size;
    const float3 dir = SkyCubeDirection(uv, face); // world Y-up, the frame the cube is read in
    const float3 originM = cloudCamera.xyz;        // the probe: sea level under the camera (C++)
    const float3 originKm = CloudWorldToPlanetKm(originM);

    float tMin, tMax;
    float2 tBottom;
    bool trace = CloudLayerSegment(originKm, dir, tMin, tMax, tBottom);
    trace = trace && tMax > tMin && tMin <= cloudWind.w; // usf:520-524
    // No depth buffer here: the planet is the ground. For a ray into it the segment the intersection
    // finds is the one beyond the planet (usf:459-497 clip it the same way).
    float2 tPlanet;
    if (trace && CloudRaySphere(originKm, dir, 0.0f.xxx, cloudCamera.w, tPlanet) && tPlanet.x > 0.0f)
    {
        if (tPlanet.x < tMin) { trace = false; } else { tMax = min(tMax, tPlanet.x); }
    }
    if (!trace) { return; } // the sky's own texel stands
    tMax = tMin + min(cloudShadowTrace.w, tMax - tMin); // usf:621-627

    CloudMarchInputs mi;
    mi.originMetres = originM;
    mi.rayDir = dir;
    mi.tMinKm = tMin;
    mi.tMaxKm = tMax;
    mi.startOffset = 0.5f;
    mi.sampleCountMax = kCaptureSampleCountMax;
    mi.sampleCountMin = cloudTrace.y;
    mi.invDistanceToMax = cloudTrace.z;
    mi.distantSkyLight = cloudAerial.z > 0.0f ? DistantSkyLight.Load(int3(0, 0, 0)).rgb : 0.0f.xxx;
    const CloudMarchResult march = CloudMarch(mi);
    if (!march.sawCloud) { return; }

    // usf:1503-1541: the aerial perspective over the cloud, once, at the transmittance-weighted
    // depth -- here the segment integral from the probe to that depth, in the LUT's frame
    // (planet-centred km, local Z-up: world .xzy, as the sky capture maps its directions). The
    // atmosphere is spherically symmetric, so the probe's XZ does not enter.
    float3 apL = 0.0f.xxx, apT = 1.0f.xxx;
    if (cloudAerial.x > 0.0f)
    {
        const float3 p = float3(0.0f, 0.0f, SkyPlanet.x);
        SkyIntegrateSegment(p, dir.xzy, march.tApKm, kCaptureAerialSamples, 1.0f,
                            TransmittanceLut, MultiScatterLut, gSmpLinearClamp, apL, apT);
        apL *= SkyIlluminance.rgb * SkyIlluminance.w; // as the volume scales it (sky_lut_aerial_cs.hlsl)
    }
    const float apMeanT = saturate(dot(apT, 1.0f / 3.0f)); // the volume stores the mean; the trace blends by it
    const float3 cloudL = apL * (1.0f - march.transmittance) + apMeanT * march.luminance;
    const uint3 texel = uint3(pixel, face);
    const float3 sky = Radiance[texel].rgb;
    Radiance[texel] = float4(min(max(sky * march.transmittance + cloudL, 0.0f), 65504.0f), 1.0f);
}
