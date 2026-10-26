#define COMPOSE_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=21, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE))"
// t0: LightTarget (HDR)
// t1: GB2 (DefaultLit emissive or foliage subsurface/transmission payload)
// t2: GB0 (Albedo+Metal encoded in A)
// t3: GB1 (Normal01 + Rough encoded in A)
// t4: Depth (R32F SRV created from the DSV)
// t5: Skybox cubemap
// t6: Filtered reflection (premultiplied)
// t7: GBAux (AO, indirect specular scale, shading model)
// t8: persistent world-space shore wetness (R32_UINT, 0..65535)
// t9:  F8 GGX-prefiltered sky radiance (mip m <-> roughness m/(mips-1))
// t10: F8 cosine-convolved sky irradiance (already divided by PI)
// t11: F8 split-sum environment BRDF (RG16F, x = scale on F0, y = bias)
// u0: Scene color (HDR)

#pragma pack_matrix(row_major)

#include "utils.hlsli"
#include "ibl_common.hlsli"
#include "height_fog.hlsli"
#include "fog_common.hlsli"
#include "sky_aerial_common.hlsli"
#include "sky_view_mapping.hlsli"
#include "sky_visibility.hlsli" // plan S: the sky specular's occlusion, lighting_cs's own line

Texture2D LightTarget : register(t0);
Texture2D GB2 : register(t1);
Texture2D GB0 : register(t2);
Texture2D GB1 : register(t3);
Texture2D DepthT : register(t4);
TextureCube SkyboxTex : register(t5);
Texture2D ReflectionTexture : register(t6);
Texture2D GBAux : register(t7);
Texture2D<uint> ShoreWetness : register(t8);
TextureCube SkySpecular : register(t9);
TextureCube SkyIrradiance : register(t10);
Texture2D BrdfLut : register(t11);
// P6B item 7: dynamic screen-space AO at RENDER resolution, gated by `gtaoEnabled`.
Texture2D GtaoTex : register(t12);
// Volumetric fog: the integrated froxel volume (light, transmittance) per slice, sampled by view
// depth. A placeholder on frames without the volume (never read then: fogVolumeParams.x == 0).
Texture3D<float4> FogVolume : register(t13);
Texture3D<float4> SkyAerialVolume : register(t14);
// t15: the SkyView LUT, read ONLY for sky pixels and ONLY to make the fog's in-scattering colour
// the same value the skybox drew. Empty in HDRI mode -- `skyViewPlanet.z` gates it.
Texture2D<float4> SkyViewLut : register(t15);
// Plan C2: the resolved clouds at half resolution (rgb pre-exposed luminance, a transmittance) and
// the cloud's front depth in km, gated by `cloudParams.x`; dummies keep the range populated otherwise.
Texture2D<float4> CloudTex : register(t16);
Texture2D<float> CloudDepthTex : register(t17);
// t18: the FURTHEST HZB (mip 0 = half the render resolution), read only by trace mode 0's bilateral
// upsampling (cloudParams.w 2) -- the scene depth each half-res cloud texel was traced against.
Texture2D<float> CloudHzbTex : register(t18);
// t19: the sky's transmittance LUT, for the sun's disc (gated by `sunDiscDir.w`).
Texture2D<float4> SunTransmittanceLut : register(t19);
// t20: plan S, the RT sky visibility (lighting_cs t17, the same texture), gated by skyVisParams.x.
Texture2D<float4> SkyVisTex : register(t20);

RWTexture2D<float4> SceneColor : register(u0);

SamplerState gSmp : register(s0); // LinearClamp (color)
SamplerState gSmpPoint : register(s1); // PointClamp (depth)

cbuffer PerFrame : register(b0)
{
    float4x4 invView; // view  -> world
    float4x4 invProj; // clip  -> view
    float skyboxIntensity; // 1.0
    float3 camPosWS;
    uint enableSkySpecular;
    // F8. 0 = this sky has no prefiltered derivatives, so the legacy mip-chain path runs and the
    // image is unchanged. > 0 = the real mip count of the prefiltered cube, which is what replaces
    // the guessed `kSkyRoughMaxMip` below.
    uint skySpecMipCount;
    // P6B items 6-7: the same pair lighting_cs takes. See CombinedAo there for the rule.
    uint gtaoEnabled;
    float gtaoStrength;
    float2 screenSize;
    float2 invScreenSize;
    float4 shoreWetnessWindow;     // xy: centre, z: 1 / half extent, w: darkening
    float4 shoreWetnessAppearance; // x: water-film reflection, yz: slope cutoff/full-wet normal Y, w: water level
    float4 shoreWetnessFallback;   // xy: height above/below water, z: normalized fade start
    float4 shoreWetnessBreakup;    // x: upper-edge strength, y: broad XZ scale in metres
    // P7 EXPONENTIAL HEIGHT FOG -- not aerial perspective, which is the sky's own volume and lives
    // in `aerialParams` below. `fogParams0.x` = 0 disables the fog, and the whole block is then
    // skipped -- which is the interface contract's "screenshot-equivalent to M2".
    float4 fogParams0;   // x: density, y: height falloff, z: reference height, w: start distance
    float4 fogParams1;   // x: max opacity, y: sun scatter strength, z: sun scatter exponent, w: sun scatter start
    float4 fogParams2;   // x: sky blur (roughness at the lightly-fogged end), yzw reserved
    float4 fogSunDir;    // xyz: direction TO the sun (world)
    float4 fogSunColor;  // rgb: the sun's effective colour
    // P7 item 8. 0 = normal, 1 = transmittance, 2 = in-scattering. Rides the fog block so a view
    // costs nothing when the fog is off -- and shows nothing either, which is the honest answer.
    // Volumetric fog (plan part A) adds 3 = the froxel slice grid, 4 = the volume's own
    // in-scatter, 5 = the volume's own transmittance.
    uint fogDebugView;
    // Volumetric fog: x = 1 when the froxel volume was built this frame (0 = the analytic model
    // alone, exactly the pre-plan image), y = the volume's far plane in view depth (metres),
    // z = 1 / preExposure (the volume is stored pre-exposed, UE HeightFogCommon.ush:448),
    // w = the grid's slice count. `fogVolumeZParams` = (B, O, S) of fog_common.hlsli, w = the volume's start.
    float4 fogVolumeParams;
    float4 fogVolumeZParams;
    // P16.1: everything this pass writes is scaled by the exposure the tonemap is about to apply,
    // so the FP16 target holds numbers near 1 instead of raw radiance. 1.0 = not pre-exposed.
    float preExposure;
    float4 aerialParams; // enabled, start view depth in metres, 1/preExposure, reserved
    float4x4 aerialViewProj; // non-jittered world-to-clip
    // B6.2: x = view height (km), y = planet bottom radius (km), z = procedural sky on,
    // w = 1 / the LUT's storage pre-exposure. Mirrors what Skybox.cpp hands the sky pass.
    float4 skyViewPlanet;
    // Plan C2: x = the cloud pair was built this frame, y = 1 / preExposure (the cloud's luminance
    // is stored pre-exposed, `lit` is raw), z = the cloud debug view (replaces the frame), w = render
    // pixels per resolved cloud pixel (1; 2 in trace mode 0 = the bilateral upsampling).
    float4 cloudParams;
    // Plan C2: xy = this frame's camera jitter in uv. The clouds are reconstructed UNJITTERED; the
    // jittered pixel at uv looks where the unjittered frame's uv - xy does, so they are read there
    // (SceneRenderer_Lighting.cpp has the why). zw = 0.
    float4 cloudJitter;
    // THE SUN'S DISC, drawn here pre-exposed (SunDiscRaw below): xyz the direction TO the sun in the sky
    // LUT's local frame (Z up), w its angular radius (rad; 0 = no disc). Only over the procedural sky.
    float4 sunDiscDir;
    // rgb the disc's outer-space luminance -- UE GetLightDiskLuminance, illuminance / solid angle
    // (SkyAtmosphereRendering.cpp:577-582) -- w the atmosphere's top radius (km).
    float4 sunDiscLuminance;
    // Plan S: lighting_cb.hlsli's skyVisParams, the same values -- x on, y diffuse, z specular strength.
    float4 skyVisParams;
}

static const float kEps = 1e-6;

// Plan C2: the resolved clouds at the jittered pixel's position, CATMULL-ROM (Jimenez's 5-tap form).
// The clouds sit on the unjittered grid and every frame is read half a pixel off it in a new
// direction; a bilinear read is a one-pixel tent averaged over those offsets, i.e. a blur the
// upscaler then faithfully accumulated -- DLSS Quality measured no sharper than Balanced, both
// well short of native. A cubic read is a near-lossless shift. The negative lobes can overshoot:
// colour floored at 0, transmittance clamped to [0, 1].
float4 SampleCloudCatmullRom(float2 uv)
{
    float2 size;
    CloudTex.GetDimensions(size.x, size.y);
    const float2 samplePos = uv * size;
    const float2 texPos1 = floor(samplePos - 0.5f) + 0.5f;
    const float2 f = samplePos - texPos1;
    const float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    const float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    const float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    const float2 w3 = f * f * (-0.5f + 0.5f * f);
    const float2 w12 = w1 + w2;
    const float2 texPos0 = (texPos1 - 1.0f) / size;
    const float2 texPos3 = (texPos1 + 2.0f) / size;
    const float2 texPos12 = (texPos1 + w2 / w12) / size;
    float4 result = CloudTex.SampleLevel(gSmp, float2(texPos12.x, texPos0.y), 0) * (w12.x * w0.y);
    result += CloudTex.SampleLevel(gSmp, float2(texPos0.x, texPos12.y), 0) * (w0.x * w12.y);
    result += CloudTex.SampleLevel(gSmp, float2(texPos12.x, texPos12.y), 0) * (w12.x * w12.y);
    result += CloudTex.SampleLevel(gSmp, float2(texPos3.x, texPos12.y), 0) * (w3.x * w12.y);
    result += CloudTex.SampleLevel(gSmp, float2(texPos12.x, texPos3.y), 0) * (w12.x * w3.y);
    result /= (w12.x * w0.y) + (w0.x * w12.y) + (w12.x * w12.y) + (w3.x * w12.y) + (w12.x * w3.y);
    return float4(max(result.rgb, 0.0f), saturate(result.a));
}

// TRACE MODE 0 (UE's r.VolumetricRenderTarget.Mode 0; cloudParams.w = 2): the clouds are resolved at HALF
// the render resolution, and this is UE's BILATERAL UPSAMPLING (VolumetricRenderTarget.usf
// ComposeVolumetricRTOverScenePS, UpsamplingMode 4 without the min/max depth trace, :1106-1219): the
// 2x2 resolved texels around the pixel, each with the scene depth it was traced against; where one of
// them is more than 10 % off this pixel's depth, each is weighted 1 / (|difference| in metres + 1), and
// elsewhere the soft filter. Ours, deliberately:
//   * the traced depth is not stored but re-derived: the trace took the FURTHEST of each half-res
//     pixel's four render pixels (cloud_trace_cs.hlsl), and that is the furthest HZB's mip 0, texel
//     for texel (hzb_build_cs.hlsl: the same clamped 2x2, on the same half-res grid) -- so one Gather
//     of it gives all four (four Gathers of the depth buffer measured ~20 us more on the owner's view);
//   * the soft filter is the Catmull-Rom above (UE: one bilinear tap), for the reason given there;
//   * the front depth the caller tests against is the heaviest texel's.
static const float kCloudSkyKm = 1.0e6f; // a sky pixel's distance (UE: device z clamped to 1e-12)
float CloudSceneKm(float z, float rayScale)
{
    if (z <= kEps) { return kCloudSkyKm; }
    // View z from device z: under a perspective projection (jittered or not) view.z and view.w depend
    // on clip z and w alone.
    const float viewZ = (z * invProj._33 + invProj._43) / (z * invProj._34 + invProj._44);
    return viewZ * rayScale / 1000.0f;
}
void SampleCloudBilateral(float2 uv, float2 cloudUv, float z, out float4 cloud, out float frontKm)
{
    float2 size;
    CloudTex.GetDimensions(size.x, size.y);
    const int2 maxTexel = int2(size) - 1;
    const int2 c0 = int2(floor(cloudUv * size - 0.5f));
    const int2 t0 = clamp(c0, 0, maxTexel);
    const int2 t1 = clamp(c0 + int2(1, 0), 0, maxTexel);
    const int2 t2 = clamp(c0 + int2(0, 1), 0, maxTexel);
    const int2 t3 = clamp(c0 + int2(1, 1), 0, maxTexel);
    // Distance along the ray per unit of view z, this pixel's (the texels are a pixel or two away).
    const float4 ray = mul(float4(UVtoNDC(uv), 0.5f, 1.0f), invProj);
    const float3 rayView = ray.xyz / ray.w;
    const float rayScale = length(rayView) / max(rayView.z, kEps);
    // The four texels' traced depths in ONE Gather at their shared corner (order w (0,0), z (1,0),
    // x (0,1), y (1,1); the sampler's clamp is the texel clamp above).
    const float4 hzb = CloudHzbTex.GatherRed(gSmpPoint, (float2(c0) + 1.0f) / size);
    const float4 traced = float4(CloudSceneKm(hzb.w, rayScale), CloudSceneKm(hzb.z, rayScale),
                                 CloudSceneKm(hzb.x, rayScale), CloudSceneKm(hzb.y, rayScale));
    // usf:1116: clamped to the furthest traced depth, so a pixel beyond all four still finds its match.
    const float pixelKm = min(CloudSceneKm(z, rayScale), max(max(traced.x, traced.y), max(traced.z, traced.w)));
    const float4 diff = abs(pixelKm - traced);
    if (max(max(diff.x, diff.y), max(diff.z, diff.w)) > pixelKm * 0.1f)
    {
        float4 w = 1.0f / (diff * 1000.0f + 1.0f);
        w /= dot(w, 1.0f);
        cloud = CloudTex.Load(int3(t0, 0)) * w.x + CloudTex.Load(int3(t1, 0)) * w.y
              + CloudTex.Load(int3(t2, 0)) * w.z + CloudTex.Load(int3(t3, 0)) * w.w;
        cloud = float4(max(cloud.rgb, 0.0f), saturate(cloud.a));
        int2 heaviest = t0;
        float heaviestWeight = w.x;
        if (w.y > heaviestWeight) { heaviest = t1; heaviestWeight = w.y; }
        if (w.z > heaviestWeight) { heaviest = t2; heaviestWeight = w.z; }
        if (w.w > heaviestWeight) { heaviest = t3; }
        frontKm = CloudDepthTex.Load(int3(heaviest, 0));
        return;
    }
    cloud = SampleCloudCatmullRom(cloudUv);
    frontKm = CloudDepthTex.SampleLevel(gSmpPoint, cloudUv, 0);
}

// THE SUN'S DISC, as UE draw it (SkyAtmosphere.usf:357-377 + SkyAtmosphereCommon.ush:269-287): the
// transmittance toward the sun times the disc's outer-space luminance times a soft edge, EXPOSED, then
// clamped to 64000 per channel ("exposed 64000 luminance is already crazy high, solar system sun is
// 1.6x10^9. Also this removes +inf float and helps TAA"). Drawn HERE and not in the sky pass because
// that pass writes RAW radiance into an FP16 target -- this project's disc is ~7e7 raw against a
// 65504 ceiling, and clamping it there left the sun ~1000x under the physics (only ~10x the sky), so
// nothing downstream could tell it from a bright cloud. Compose is where our frame becomes
// pre-exposed (it multiplies everything it writes by `preExposure`), i.e. UE's OutputPreExposure, so
// this returns the clamped value back in raw units for that one multiply. Full transmittance per
// channel -- UE's hue: a white core and a coloured falloff from the tonemap's clipping, red at sunset.
// The direction is built in VIEW space and only rotated into the world: the sky's usual probe point
// (z 0.5, 2 cm out) loses the 0.27-degree disc to float rounding once the camera is km from the origin.
float3 SunDiscRaw(float2 uv)
{
    if (skyViewPlanet.z <= 0.0f || sunDiscDir.w <= 0.0f) { return 0.0f.xxx; }
    const float4 pv = mul(float4(UVtoNDC(uv), 0.5f, 1.0f), invProj);
    const float3 worldDir = normalize(mul(float4(pv.xyz / pv.w, 0.0f), invView).xyz);
    const float3 dir = worldDir.xzy; // the LUT's local frame, Z up (skybox.hlsl does the same)
    const float radius = sunDiscDir.w;
    const float cosHalf = cos(radius);
    const float viewDotLight = dot(dir, sunDiscDir.xyz);
    const float height = skyViewPlanet.x, bottom = skyViewPlanet.y, top = sunDiscLuminance.w;
    if (viewDotLight <= cosHalf || SkyViewIntersectsGround(dir, height, bottom)) { return 0.0f.xxx; }
    // 2*sin(radius/2)^2 = 1 - cos(radius) without the cancellation, for a disc this small.
    const float oneMinusCos = 2.0f * pow(sin(0.5f * radius), 2.0f);
    const float softEdge = saturate(2.0f * (viewDotLight - cosHalf) / max(oneMinusCos, 1.e-12f));
    const float3 tr = SkyTransmittanceToSpace(SunTransmittanceLut, gSmp, dir, height, bottom, top);
    const float3 exposed = min(tr * sunDiscLuminance.rgb * softEdge * preExposure, 64000.0f.xxx);
    return exposed / max(preExposure, 1.0e-30f);
}

// How far a SKY pixel is shaded at, standing in for UE's ConvertFromDeviceZ(0) -- the far plane the
// depth buffer's 0 decodes to. Only near-horizontal rays get anywhere near it: the height falloff
// makes every other direction reach its asymptote long before, and the clamp beside its use keeps a
// steeply-angled ray from feeding a huge height delta into an exponential.
static const float kSkyFogDistance = 1.0e5f;

// Roughness->mip scale for the skybox fallback. The cube has an 11-mip chain; this
// matches glass.hlsl (rough*5) so opaque and glass sky reflections blur identically.
// `kSkyRoughMaxMip` (the legacy roughness ceiling for a sky with no F7 derivatives) and
// `FresnelSchlick` moved to ibl_common.hlsli when the lighting pass started needing them too.

inline float ReadDepth(float2 uv)
{
    return DepthT.SampleLevel(gSmpPoint, uv, 0).r; // Always sample LOD0, no bilinear
}

float SampleShoreWetness(float2 worldXZ, out float localCoverage)
{
    localCoverage = 0.0f;
    if (shoreWetnessWindow.z <= 0.0f)
    {
        return 0.0f;
    }

    const float2 uv =
        (worldXZ - shoreWetnessWindow.xy) * (shoreWetnessWindow.z * 0.5f) + 0.5f;
    if (any(uv < 0.0f) || any(uv >= 1.0f))
    {
        return 0.0f;
    }

    uint width;
    uint height;
    ShoreWetness.GetDimensions(width, height);
    const float2 edgeTexels = min(uv, 1.0f - uv) * float2(width, height);
    const float edgeFade = smoothstep(0.0f, 24.0f, min(edgeTexels.x, edgeTexels.y));
    localCoverage = edgeFade;
    const float2 texel = uv * float2(width, height) - 0.5f;
    // Shift the spline cell by half a texel so an exact texel centre gets symmetric weights.
    const float2 splineTexel = texel - 0.5f;
    const int2 base = int2(floor(splineTexel));
    const float2 f = frac(splineTexel);
    const int2 maximum = int2(width - 1u, height - 1u);
    // Continuous quadratic B-spline: nine real texel fetches, unlike the previous four-tap
    // bilinear read. This removes the 0.4 m history texels without introducing centre snapping.
    const float3 weightsX = float3(
        0.5f * (1.0f - f.x) * (1.0f - f.x),
        0.75f - (f.x - 0.5f) * (f.x - 0.5f),
        0.5f * f.x * f.x);
    const float3 weightsY = float3(
        0.5f * (1.0f - f.y) * (1.0f - f.y),
        0.75f - (f.y - 0.5f) * (f.y - 0.5f),
        0.5f * f.y * f.y);
    float filteredWetness = 0.0f;
    [unroll]
    for (int y = 0; y < 3; ++y)
    {
        [unroll]
        for (int x = 0; x < 3; ++x)
        {
            const int2 coord = clamp(base + int2(x, y), int2(0, 0), maximum);
            filteredWetness +=
                ShoreWetness.Load(int3(coord, 0)) * (1.0f / 65535.0f) *
                weightsX[x] * weightsY[y];
        }
    }
    return filteredWetness;
}

float WetnessBreakupHash(float2 cell)
{
    float3 p = frac(float3(cell.x, cell.y, cell.x) * 0.1031f);
    p += dot(p, p.yzx + 33.33f);
    return frac((p.x + p.y) * p.z);
}

float WetnessBreakupNoise(float2 position)
{
    const float2 cell = floor(position);
    const float2 local = frac(position);
    const float2 blend = local * local * (3.0f - 2.0f * local);
    const float n00 = WetnessBreakupHash(cell);
    const float n10 = WetnessBreakupHash(cell + float2(1.0f, 0.0f));
    const float n01 = WetnessBreakupHash(cell + float2(0.0f, 1.0f));
    const float n11 = WetnessBreakupHash(cell + float2(1.0f, 1.0f));
    return lerp(lerp(n00, n10, blend.x), lerp(n01, n11, blend.x), blend.y);
}

float SampleDistantShoreWetness(float3 worldPosition)
{
    const float aboveWater = max(shoreWetnessFallback.x, 0.0f);
    const float belowWater = max(shoreWetnessFallback.y, 0.0f);
    if (aboveWater + belowWater <= kEps)
    {
        return 0.0f;
    }

    const float signedHeight = worldPosition.y - shoreWetnessAppearance.w;
    if (abs(signedHeight) <= kEps)
    {
        return 1.0f;
    }

    const float extent = signedHeight > 0.0f ? aboveWater : belowWater;
    if (extent <= kEps)
    {
        return 0.0f;
    }

    const float absoluteHeight = abs(signedHeight);
    if (absoluteHeight >= extent)
    {
        return 0.0f;
    }

    float effectiveExtent = extent;
    const float breakupStrength = saturate(shoreWetnessBreakup.x);
    if (signedHeight > 0.0f && breakupStrength > kEps)
    {
        const float breakupScale = max(shoreWetnessBreakup.y, 0.1f);
        const float2 breakupUv = worldPosition.xz / breakupScale;
        const float broadNoise = WetnessBreakupNoise(breakupUv);
        const float detailNoise = WetnessBreakupNoise(
            breakupUv * 2.07f + float2(19.37f, -7.11f));
        // A broad non-repeating field chooses how much of the authored ABOVE-water reach is
        // removed at this XZ. Remapping leaves occasional full-length lobes, while the second
        // octave prevents the outer contour from reading as a single smooth sine wave.
        const float breakup = saturate((broadNoise * 0.72f + detailNoise * 0.28f - 0.2f) * 1.25f);
        effectiveExtent *= 1.0f - breakupStrength * breakup;
    }

    const float normalizedHeight = absoluteHeight / max(effectiveExtent, kEps);
    const float fadeStart = saturate(shoreWetnessFallback.z);
    // Above/Below are exact outer limits. Stay fully wet through Fade Start, then linearly reach
    // zero at normalizedHeight == 1; never inflate the authored height interval.
    return saturate((1.0f - normalizedHeight) / max(1.0f - fadeStart, kEps));
}

// P6B item 6 -- MUST match lighting_cs::CombinedAo. Product of the material's own cavity term
// and the screen-space estimate (UE's DiffuseIndirectComposite.usf:371), scaled by
// `gtaoStrength` (their AmbientOcclusionStaticFraction). Bounded, monotonic, identity at 1.
float CombinedAo(float materialAo, float2 uv)
{
    if (gtaoEnabled == 0u)
    {
        return materialAo;
    }
    // `strength` scales the DYNAMIC term only. UE's AmbientOcclusionStaticFraction damps the whole
    // product, but here the material term already shipped in F9 and is not this step's to switch
    // off: at strength 0 this must be an EXACT no-op against the pre-P6B build, and the sweep
    // level's AO row is what proves it (damping the product moved it by 177/255).
    //
    // P16.4: two scales in the target, combined with a MIN. The reasoning is in lighting_cs -- this
    // line only has to stay byte-identical to it, because compose SUBTRACTS the sky specular that
    // lighting added and the two must remain each other's exact inverse.
    const float2 dynamicAo = saturate(GtaoTex.SampleLevel(gSmpPoint, uv, 0).rg);
    return saturate(materialAo * lerp(1.0f, min(dynamicAo.x, dynamicAo.y), saturate(gtaoStrength)));
}

[numthreads(8,8,1)]
[RootSignature(COMPOSE_CS_RS)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width = (uint)screenSize.x;
    uint height = (uint)screenSize.y;
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
    {
        return;
    }

    float2 uv = (float2(dispatchThreadId.xy) + 0.5f) * invScreenSize;

    // The light target is PRE-EXPOSED (lighting_cb.hlsli lightPreExposure, UE's View.PreExposure) so a
    // glint above 65504 cd/m2 survives FP16; back to radiance here, in fp32, and everything below is as
    // it was -- this pass applies `preExposure` once, to what it writes.
    float3 lit = LightTarget.SampleLevel(gSmp, uv, 0).rgb / max(preExposure, 1.0e-30f);
    float3 payload = GB2.SampleLevel(gSmp, uv, 0).rgb;
    float4 gbAux = GBAux.SampleLevel(gSmpPoint, uv, 0);
    float indirectSpecularScale = saturate(gbAux.g);
    // F9: scalar material AO, written since F3, consumed from here on.
    const float materialAo = saturate(gbAux.r);
    uint shadingModel = DecodeShadingModel(gbAux.b);
    float3 color = lit;
    if (shadingModel != kShadingModelTwoSidedFoliage)
    {
        color += payload;
    }

    float z = ReadDepth(uv);

    // The sun's disc (SunDiscRaw): behind the clouds below, then into `color` and NOT `lit` -- the fog
    // takes a sky pixel's in-scattering colour from `lit` (the identity blend further down), so a disc in
    // `lit` would be its own fog colour and never dim; in `color` it is attenuated like any background.
    float3 sunDisc = !(z > kEps) ? SunDiscRaw(uv) : 0.0f.xxx;

    // Plan C2: THE CLOUDS, composed over what the sky pass drew and under everything this pass
    // adds -- the height fog, the froxel volume and the aerial perspective all sit on top, exactly
    // as they sit on the sky (UE ComposeVolumetricRTOverScene runs before their fog for the same
    // reason). `lit` is raw radiance, the cloud's luminance is pre-exposed: one scale reconciles them.
    // On a GEOMETRY pixel the cloud is applied only where the surface lies BEHIND the cloud's front
    // (the trace already stopped at the depth buffer; this keeps a half-res texel that straddles a
    // silhouette from bleeding cloud onto the near side of it).
    if (cloudParams.x > 0.0f)
    {
        const float2 cloudUv = uv - cloudJitter.xy;
        float4 cloud;
        float cloudFrontKm;
        if (cloudParams.w > 1.5f)
        {
            SampleCloudBilateral(uv, cloudUv, z, cloud, cloudFrontKm);
        }
        else
        {
            cloud = SampleCloudCatmullRom(cloudUv);
            cloudFrontKm = CloudDepthTex.SampleLevel(gSmpPoint, cloudUv, 0);
        }
        bool applyCloud = !(z > kEps);
        if (!applyCloud)
        {
            const float3 cloudPos = ReconstructPosWS(uv, z, invProj, invView);
            const float surfaceKm = length(cloudPos - camPosWS) / 1000.0f;
            applyCloud = surfaceKm > cloudFrontKm;
        }
        if (applyCloud)
        {
            const float3 delta = lit * (cloud.a - 1.0f) + cloud.rgb * cloudParams.y;
            lit += delta;
            color += delta;
            sunDisc *= cloud.a;
        }
    }
    color += sunDisc;

    if (z > kEps)
    {
        float4 gb0 = GB0.SampleLevel(gSmp, uv, 0);
        float4 gb1 = GB1.SampleLevel(gSmp, uv, 0);

        float3 albedo = gb0.rgb;
        float2 rm = UnpackRM(gb0.a);
        float rough = saturate(rm.x);
        float metal = saturate(rm.y);

        float3 N_ws = normalize(gb1.rgb * 2.0 - 1.0);
        float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metal);

        float3 Pw = ReconstructPosWS(uv, z, invProj, invView);
        float3 Vw = NormalizeSafe(camPosWS - Pw, float3(0.0, 0.0, 1.0));
        float3 Rw = NormalizeSafe(reflect(-Vw, N_ws), N_ws);

        float4 reflectionT = ReflectionTexture.SampleLevel(gSmp, uv, 0); // premultiplied
        float reflectionA = reflectionT.a;
        float3 reflectionRGB = reflectionT.rgb;

        // Q1: blur the sky fallback with roughness too — otherwise a rough surface shows a
        // mirror-sharp horizon next to the roughness-blurred SSR/RT reflection. The `gloss`
        // term below fades the reflection out as rough->1, so the very blurry upper mips are
        // only lightly weighted.
        const float cosT = saturate(dot(N_ws, Vw));

        float3 skyCol = 0.0f.xxx;
        if (enableSkySpecular != 0u)
        {
            skyCol = IblSkyRadiance(SkySpecular, SkyboxTex, gSmp, Rw, rough,
                                    skySpecMipCount, skyboxIntensity);
        }

        // F9: occlude the FALLBACK SKY only. An RT or SSR hit already knows what it saw -- it
        // traced the geometry that AO is a stand-in for -- so darkening it would double-count the
        // occlusion. The plan is explicit that widening this to all indirect methods needs its own
        // A/B, and this is the conservative half.
        skyCol *= IblSpecularOcclusion(cosT, CombinedAo(materialAo, uv), rough);
        // Plan S: lighting_cs's own line, on the same texel -- or the subtraction below stops
        // cancelling the sky that lighting added.
        if (skyVisParams.x != 0.0f)
        {
            const float4 skyVisBent = SkyVisUpsample(SkyVisTex, uv, z);
            if (skyVisBent.w > 0.0f)
            {
                skyCol *= SkyVisSpecular(skyVisBent.xyz, Rw, rough, skyVisParams.z);
            }
        }

        // ONLY THE DIFFERENCE THE REFLECTION MAKES. The sky term is already in the light target --
        // the lighting pass adds it now, so that the screen-space reflection pass, which samples
        // that target, sees a metal with its environment on it instead of a black disc. The blend
        // is unchanged in total: lighting contributed sky*weight, this contributes
        // (hit - sky*alpha)*weight, and the two sum to the old (hit + sky*(1-alpha))*weight.
        // Where a reflection found nothing (alpha 0) this is exactly zero, which is what makes
        // None/SkyOnly/RT screenshot-identical to the build before the move.
        float3 refl = reflectionRGB - skyCol * reflectionA;
        refl *= indirectSpecularScale;
        color += refl * IblSpecularWeight(BrdfLut, gSmp, F0, cosT, rough, skySpecMipCount);

        if (shadingModel == kShadingModelTerrain)
        {
            const float slopeWeight = smoothstep(
                shoreWetnessAppearance.y,
                max(shoreWetnessAppearance.z, shoreWetnessAppearance.y + 1e-3f),
                saturate(N_ws.y));
            float localCoverage = 0.0f;
            const float localWetness = saturate(SampleShoreWetness(Pw.xz, localCoverage));
            const float distantWetness = SampleDistantShoreWetness(Pw);
            const float historyHalfExtent = shoreWetnessWindow.z > kEps
                ? rcp(shoreWetnessWindow.z)
                : 0.0f;
            const float3 cameraDelta = camPosWS - Pw;
            const float distanceSq = dot(cameraDelta, cameraDelta);
            const float fallbackStart = historyHalfExtent * 0.75f;
            const float distanceFallback = historyHalfExtent > kEps
                ? smoothstep(
                    fallbackStart * fallbackStart,
                    historyHalfExtent * historyHalfExtent,
                    distanceSq)
                : 1.0f;
            // The history owns the near field. At the edge of its camera-centred window, or when
            // an elevated camera puts the surface beyond the history's useful world-space range,
            // crossfade into the height-only fallback. This keeps both the XZ border and a high
            // aerial view from exposing the finite 206 m history field.
            const float localAuthority = localCoverage * (1.0f - distanceFallback);
            const float wetness =
                saturate(lerp(distantWetness, localWetness, localAuthority)) * slopeWeight;
            color *= 1.0f - wetness * saturate(shoreWetnessWindow.w);

            // The darkening carries the dominant wet-sand read. A smaller, grazing-angle film
            // reflection restores the wet highlight without changing the terrain material or its
            // GBuffer layout.
            const float3 filmF = FresnelSchlick(cosT, 0.02f.xxx);
            color += refl * filmF * wetness * max(shoreWetnessAppearance.x, 0.0f);
        }
    }

    // P7: the height fog. Aerial perspective is a separate layer further down. Background pixels
    // hold the skybox, which IS the horizon colour -- fogging them would blend the sky towards
    // itself and, with the sun lobe added, quietly brighten the whole sky (plan item 6).
    // A debug view must not leave the pixels it does NOT describe showing the ordinary image: sky
    // and water are not opaque at compose time and so never enter the block below, and left as the
    // normal scene they read as "no fog here" instead of "not measured here". Black says the
    // second one.
    // Volumetric fog (plan part A): the froxel volume covers the view ray from the camera to
    // `fogVolumeParams.y` metres of view depth; the analytic model below continues from there to
    // the surface. UE's CombineVolumetricFog (HeightFogCommon.ush:430-460): the volume's own
    // (light, transmittance) sampled by view depth, the analytic fog evaluated with its start
    // pushed out to the volume's far plane along the ray (their ExcludeDistance = MaxDistance *
    // InvCosAngle), composed as `rgb = Vol.rgb + Analytic.rgb * Vol.a; a = Vol.a * Analytic.a`.
    // The SKY is inside the volume too (rays over the horizon) -- it samples the last slice -- and
    // stays free of the analytic term, which is already the sky itself.
    const bool volumeOn = fogVolumeParams.x > 0.5f;
    float4 vol = float4(0.0f, 0.0f, 0.0f, 1.0f);
    float excludeDistance = 0.0f;
    float surfaceViewDepth = 0.0f;
    if (volumeOn)
    {
        const float2 ndc = (uv * 2.0f - 1.0f) * float2(1.0f, -1.0f);
        float viewDepth = fogVolumeParams.y;
        if (z > kEps)
        {
            const float4 Pv = mul(float4(ndc, z, 1.0f), invProj);
            surfaceViewDepth = Pv.z / Pv.w;
            viewDepth = min(surfaceViewDepth, fogVolumeParams.y);
        }
        vol = FogVolume.SampleLevel(gSmp, FogVolumeUV(uv, viewDepth, fogVolumeZParams.xyz, (uint)fogVolumeParams.w), 0);
        vol.rgb *= fogVolumeParams.z; // stored pre-exposed
        if (FogVolumeInFront(viewDepth, fogVolumeZParams)) { vol = float4(0.0f, 0.0f, 0.0f, 1.0f); } // nearer than its start
        if (fogDebugView == 3u)
        {
            // The slice grid: a stripe per slice, so the depth distribution can be read off the image.
            const float slice = FogSliceFromDepth(fogVolumeZParams.xyz, viewDepth);
            color = (0.25f + 0.5f * frac(slice)).xxx;
            SceneColor[dispatchThreadId.xy] = float4(color, 1.0); // debug: display-linear, see below
            return;
        }
        // 4 = the volume's in-scattered light as radiance (exposed like the scene), 5 = its
        // transmittance as a gray level (display-linear, see the debug note below).
        if (fogDebugView == 4u) { color = vol.rgb; SceneColor[dispatchThreadId.xy] = float4(color * preExposure, 1.0); return; }
        if (fogDebugView == 5u) { color = vol.aaa; SceneColor[dispatchThreadId.xy] = float4(color, 1.0); return; }
    }

    float transmittance = 1.0f;
    float3 fogColor = 0.0f.xxx; // UE's FogColor: what the air adds, already weighted (HeightFogColor)
    // THE SKY IS FOGGED TOO, and that is UE's behaviour rather than an embellishment.
    // HeightFogPixelShader.usf reads the depth buffer, notes `bIsRendered = (DeviceZ != 0.0)`, and
    // the only thing that spares an unrendered pixel is `bOnlyOnRenderedOpaque` -- initialised to
    // false in SceneRendering.cpp:909 and set true nowhere but a scene capture asking for alpha
    // hold-out (SceneCaptureRendering.cpp:1397). Every ordinary view runs the same CalculateHeightFog
    // over the sky, at the distance ConvertFromDeviceZ(0) reports for the far plane.
    //
    // Skipping the sky is what drew a line across it. A distant silhouette has fogged geometry on one
    // side and untouched sky on the other, and the fog does not stop at the silhouette in the world,
    // so stopping there in the shader draws the seam. Fogging both sides does not merely soften it:
    // a ray that grazes the far edge of the terrain and one that just misses it share almost the same
    // height profile, so their line integrals agree to the grazing angle -- the two sides meet because
    // they are the same integral, not because a blend was tuned.
    //
    // The comment that used to sit here argued the opposite: that the sky IS the horizon colour, so
    // fogging it would only blend it towards itself. That would hold if the inscattering colour were
    // the sky in this exact direction. It is the sky BLURRED, with a sun lobe added -- and the
    // difference between those two is exactly the step that was showing.
    if (fogParams0.x > 0.0f)
    {
        // Sky or surface: decided once, used by the sun lobe's gate and by the ray below.
        const bool isSky = !(z > kEps);

        HeightFogParams fog;
        fog.density = fogParams0.x;
        fog.heightFalloff = fogParams0.y;
        fog.referenceHeight = fogParams0.z;
        fog.startDistance = fogParams0.w;
        fog.maxOpacity = fogParams1.x;
        // NO SUN LOBE ON THE SKY. UE's directional in-scattering exists because their fog colour
        // is an authored constant with no sun in it; ours is the sky along the ray, which already
        // contains the sun's glow, so on a SKY pixel the lobe adds it a second time -- and adds it
        // raw: `sunColor * sunScatterStrength` is 85000 lux times 0.5 here, dumped in a halo around
        // the sun. That is what started tearing the bloom apart. On GEOMETRY the base has no sun in
        // it and the lobe is doing exactly UE's job, so it stays.
        //
        // This is the gate the lobe should have had all along. It used to ride `headroom`, which
        // faded the lobe out as the fog saturated -- that hid the double count at the horizon but
        // made the term non-monotonic in distance, which drew its own dark line along the water.
        fog.sunScatterStrength = isSky ? 0.0f : fogParams1.y;
        fog.sunScatterExponent = fogParams1.z;
        fog.sunScatterStartDistance = fogParams1.w;

        // The pixel's ray. A sky pixel has no surface, so reconstruct at an arbitrary interior depth
        // -- the direction and the angle to the view axis are the same at every depth -- and carry
        // the distance separately.
        const float probeZ = isSky ? 0.5f : z;
        const float3 Pref = ReconstructPosWS(uv, probeZ, invProj, invView);
        const float3 toRef = Pref - camPosWS;
        const float refDist = max(length(toRef), kEps);
        const float3 viewDir = toRef / refDist;
        // Past a height delta of 127 / heightFalloff the model's own exp2 argument has reached UE's
        // clamp (HeightFogLineIntegral) and another metre changes nothing, so stopping the sky ray
        // there costs no accuracy and keeps a steeply-angled ray from carrying tens of kilometres of
        // delta into an exponential. A near-horizontal ray is unaffected and runs the full distance.
        const float skyReach = min(kSkyFogDistance,
                                   127.0f / (max(fog.heightFalloff, 1.0e-4f) * max(abs(viewDir.y), 1.0e-6f)));
        const float dist = isSky ? skyReach : refDist;
        const float3 Pw = camPosWS + viewDir * dist;

        if (volumeOn)
        {
            // The volume already holds this ray up to its far plane: the analytic integral runs
            // from there (along the ray, so a slanted ray excludes the right length). UE's
            // InvCosAngle, taken off the reconstructed ray so the sky has one too.
            const float2 ndcRay = (uv * 2.0f - 1.0f) * float2(1.0f, -1.0f);
            const float4 PvRef = mul(float4(ndcRay, probeZ, 1.0f), invProj);
            const float invCosAngle = refDist / max(PvRef.z / PvRef.w, kEps);
            excludeDistance = fogVolumeParams.y * invCosAngle;
            fog.startDistance = max(fog.startDistance, excludeDistance);
            fog.sunScatterStartDistance = max(fog.sunScatterStartDistance, excludeDistance);
        }

        const float fogShared = HeightFogSharedIntegral(dist, camPosWS.y, Pw.y, fog);
        const float tau = HeightFogOpticalDepth(fogShared, dist, fog);
        const float minT = HeightFogMinTransmittance(tau, fog.maxOpacity);
        transmittance = HeightFogTransmittance(tau, minT);

        // The sky sampled ALONG THE VIEW RAY, out of the whole-sphere PICTURE of the sky and never
        // out of a lighting probe -- FogSkyAlongView carries the reasoning and UE's distance fade.
        // Same shape as UE's InscatteringColorCubemap, generated rather than authored. Both the blur
        // and the sun lobe fade out as the fog saturates -- see HeightFogHeadroom.
        const float headroom = HeightFogHeadroom(transmittance, minT);
        // A SKY pixel's in-scattering colour IS THE PIXEL. Do not resample the sky for it: the
        // fog over a background pixel is the same light the sky already carries along that exact
        // ray, so taking `lit` -- what the sky pass wrote here -- makes the blend an identity by
        // construction, `color*T + color*(1-T)`, in BOTH sky modes.
        //
        // Resampling was the bug. Reading the cube (HDRI) or the LUT (procedural) at the
        // reconstructed ray is ALMOST the same direction the sky pass used, and almost is fatal
        // next to a sun of ~1e5: at the sun the fog lifted the sky by 11.9 levels against 2.7
        // everywhere else, and the bloom turned that into a flare. Continuity at a distant
        // silhouette does not need this -- it comes from the GEOMETRY side converging on the sky,
        // and geometry still samples the cube below.
        const float3 skyAlongView = isSky
            ? lit
            : FogSkyAlongView(SkyboxTex, gSmp, viewDir, dist,
                              HeightFogSkyRoughness(headroom, fogParams2.x), fogParams2.zw, skyboxIntensity);
        fogColor = HeightFogColor(skyAlongView, fogSunColor.rgb, dot(viewDir, fogSunDir.xyz), fogShared, dist,
                                  transmittance, fog);
    }

    // A debug view must not leave the pixels it does NOT describe showing "no fog": the sky is
    // never in the analytic model, so without the volume it is black ("not measured"); with the
    // volume it IS measured (the ray to the volume's far plane) and shows like everything else.
    // This used to be assigned BEFORE the two branches below and was overwritten by them, so the
    // sky read as T = 1 in the transmittance view -- and against the volume's honest value it
    // looked like a 40 % disagreement in the palm crowns.
    // Now that the analytic model covers the sky as well, "not measured" means only that neither
    // model is running at all -- it is no longer a statement about this pixel's depth.
    const bool debugUnmeasured = !(fogParams0.x > 0.0f) && !volumeOn;
    if (fogDebugView == 1u)
    {
        // What the SURFACE keeps. White = the air is doing nothing here, black = fully hidden.
        color = debugUnmeasured ? 0.0.xxx : (transmittance * vol.a).xxx;
    }
    else if (fogDebugView == 2u)
    {
        // What the AIR adds, already weighted by coverage -- i.e. the term actually summed
        // into the image, not the raw scattering colour. Reading the unweighted one would say
        // the fog is bright everywhere including where it contributes nothing.
        color = debugUnmeasured ? 0.0.xxx : fogColor * vol.a + vol.rgb;
    }
    else if (fogDebugView == 6u || fogDebugView == 7u)
    {
        // What compose actually reads out of the depth buffer. STRICTLY GRAY, one quantity per
        // view: the first version of this packed three numbers into R, G and B and the display
        // chain -- which is not a per-channel curve -- mixed them, so the mask read as "sky" on
        // pixels that carry depth. A colour is not a number here; a gray level is.
        //   6 = where compose calls the pixel geometry (`z > kEps`): white yes, black no.
        //   7 = log2(z) over 24 stops, which resolves the tiny reverse-Z values a linear scale
        //       would flatten to zero (20 km at a 0.1 m near plane is z = 5e-6).
        color = fogDebugView == 6u
            ? (z > kEps ? 1.0f : 0.0f).xxx
            : (z > 0.0f ? saturate((log2(z) + 24.0f) / 24.0f) : 0.0f).xxx;
    }
    else if (fogDebugView == 8u || fogDebugView == 9u)
    {
        // THE ENVIRONMENT CUBES, so the thing reflections actually read can be LOOKED AT instead of
        // inferred. Both are already bound here for the fog and the IBL, so this costs no binding.
        //   8 = SkyboxTex  -- the RADIANCE capture, i.e. exactly what sky_ibl_capture_cs wrote.
        //   9 = SkySpecular mip 1 -- the first mip the prefilter actually convolves (mip 0 of that
        //       cube is a verbatim copy of 8, see sky_ibl_filter_cs).
        //
        // EQUIRECTANGULAR, not a face cross: the vertical axis is then elevation, which is the axis
        // the SkyView LUT's parameterisation compresses, so banding and blotching separate at a
        // glance instead of being cut across six tiles.
        const float2 t = (float2(dispatchThreadId.xy) + 0.5f) / max(screenSize, 1.0f.xx);
        const float phi = (t.x * 2.0f - 1.0f) * 3.14159265f;
        const float theta = (0.5f - t.y) * 3.14159265f;
        const float3 d = float3(cos(theta) * sin(phi), sin(theta), cos(theta) * cos(phi));
        const float3 raw = fogDebugView == 8u
            ? SkyboxTex.SampleLevel(gSmp, d, 0.0f).rgb
            : SkySpecular.SampleLevel(gSmp, d, 1.0f).rgb;
        // LOG, not Reinhard. The cube holds RAW radiance and the sky sits far above 1, so Reinhard
        // pinned the whole upper hemisphere to white and showed nothing -- 16 stops of log is what
        // makes structure visible across that range. Structure is what this view is for; read
        // numbers with the gray views, not this one.
        color = saturate((log2(max(raw, 1.0e-8f.xxx)) + 20.0f) / 40.0f);
    }
    else
    {
        // B3 + P7, composed in UE's ORDER. Theirs runs RenderSkyAtmosphere (aerial perspective)
        // first and RenderFog second, both blending One/SourceAlpha, and the single-pass form spells
        // out why (SkyAtmosphereCommon.ush:148-151):
        //     "Apply any other fog OVER aerial perspective because AP is usually optically thiner."
        //     FinalFog.rgb = FogToApplyOver.rgb + AP.rgb * FogToApplyOver.a;
        //     FinalFog.a   = FogToApplyOver.a * AP.a;
        //
        // We had it inverted -- aerial perspective laid on top of the already-fogged colour -- which
        // throws away the one safeguard the ordering provides: where the other fog is dense (alpha
        // -> 0) UE multiply the aerial term away, while we added it at full strength. At a distant
        // silhouette that is the second half of the line the sky was being cut with: below it,
        // saturated fog PLUS a full-strength aerial term; above it, sky with neither. With the order
        // right the aerial term fades out exactly as the fog closes, so both sides land together.
        //
        // It matters more here than it does for UE, because THEIR height fog is an authored colour
        // and OURS is the sky sampled along the view ray -- already a cheap aerial perspective.
        // Stacking the two the wrong way round counts the same air twice.
        //
        // Aerial perspective first, so it is the layer everything else sits on top of. It stays off
        // the sky: our SkyView LUT, like UE's, is already the atmosphere integrated to infinity.
        float4 ap = float4(0, 0, 0, 1);
        if (aerialParams.x > 0.0f && z > kEps)
        {
            const float3 worldPos = ReconstructPosWS(uv, z, invProj, invView);
            const float4 clip = mul(float4(worldPos, 1), aerialViewProj);
            const float viewDepth = clip.w; // perspective w = view Z
            if (viewDepth > aerialParams.y)
            {
                const float distanceKm = length(worldPos - camPosWS) / 1000.0f;
                const float startKm = aerialParams.y * distanceKm / max(viewDepth, 1.e-4f);
                const float2 apUv = (clip.xy / clip.w) * float2(0.5f, -0.5f) + 0.5f;
                ap = SampleSkyAerial(SkyAerialVolume, gSmp, apUv, distanceKm, startKm, aerialParams.z);
            }
        }

        // The analytic model and the froxel volume, gathered into ONE "fog to apply over" pair. With
        // the analytic model off this is the volume alone; with both off it is the identity.
        float3 overRgb = fogColor;
        float overA = transmittance;
        overRgb = vol.rgb + overRgb * vol.a; // the volume sits over the analytic model, as before
        overA *= vol.a;

        // ...and the pair sits over aerial perspective, which is UE's line above.
        color = color * (overA * ap.a) + (overRgb + ap.rgb * overA);
    }

    // The debug views 1 / 3 / 5 are GRAY LEVELS in [0, 1], not radiance: written without the
    // pre-exposure they land in the tonemap as display-linear values (its multiplier is 1.0 while
    // pre-exposure is active) and read as the number they are. Multiplied by the pre-exposure they
    // were exposed like sunlight -- a transmittance of 1.0 came out BLACK under a daylight EV --
    // which is how the volumetric fog's parity check first "passed" on two black images. Views 2
    // and 4 are radiance (what the air adds) and stay exposed like the scene.
    // Plan C2: a cloud debug view is a display-linear gray written by the trace (coverage,
    // transmittance, samples); it replaces the frame, under the same ramp rule as the fog's grays.
    const bool cloudDebug = cloudParams.x > 0.0f && cloudParams.z != 0.0f;
    if (cloudDebug) { color = CloudTex.SampleLevel(gSmp, uv, 0).rgb; }
    const bool debugGray = cloudDebug || fogDebugView == 1u || fogDebugView == 6u || fogDebugView == 7u;
    // 8/9 are COLOUR views but must skip the pre-exposure the same way: they already carry their own
    // Reinhard, and multiplying raw cube radiance by the frame's pre-exposure would only flatten the
    // structure they exist to show. They get no calibration ramp -- they are not read as numbers.
    const bool debugRawColor = fogDebugView == 8u || fogDebugView == 9u;
    // EVERY gray view carries the calibration ramp, not just the ones that were written with it.
    // "Display-linear" is not the same as "readable": the tonemapper still runs, its shoulder
    // compresses the top of the range, and its input scale moves with auto-exposure -- so a ramp
    // measured in one shot cannot decode another. Reading a transmittance view without the ramp
    // beside it turned a smooth gradient into a flat plateau and sent me looking for a bug in the
    // fog that the display had invented. The ramp costs eight rows and makes the shot self-decoding.
    if (debugGray && dispatchThreadId.y < 96u)
    {
        color = (((float)dispatchThreadId.x + 0.5f) / max(screenSize.x, 1.0f)).xxx;
    }
    SceneColor[dispatchThreadId.xy] =
        float4((debugGray || debugRawColor) ? color : color * preExposure, 1.0);
}
