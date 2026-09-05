#define COMPOSE_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=14, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE))"
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
#include "atmosphere.hlsli"
#include "fog_common.hlsli"

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
    // P7 aerial perspective. `fogParams0.x` = 0 disables it, and the whole block below is then
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
    // w = the grid's slice count. `fogVolumeZParams` = (B, O, S) of fog_common.hlsli.
    float4 fogVolumeParams;
    float4 fogVolumeZParams;
    // P16.1: everything this pass writes is scaled by the exposure the tonemap is about to apply,
    // so the FP16 target holds numbers near 1 instead of raw radiance. 1.0 = not pre-exposed.
    float preExposure;
}

static const float kEps = 1e-6;

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

    float3 lit = LightTarget.SampleLevel(gSmp, uv, 0).rgb;
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

    // P7: aerial perspective, applied LAST and only to real geometry. Background pixels already
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
    float3 inscatter = 0.0f.xxx;
    if (z > kEps && fogParams0.x > 0.0f)
    {
        AtmosphereParams fog;
        fog.density = fogParams0.x;
        fog.heightFalloff = fogParams0.y;
        fog.referenceHeight = fogParams0.z;
        fog.startDistance = fogParams0.w;
        fog.maxOpacity = fogParams1.x;
        fog.sunScatterStrength = fogParams1.y;
        fog.sunScatterExponent = fogParams1.z;
        fog.sunScatterStartDistance = fogParams1.w;
        fog.skyBackScatter = fogParams2.y;

        const float3 Pw = ReconstructPosWS(uv, z, invProj, invView);
        const float3 toPoint = Pw - camPosWS;
        const float dist = length(toPoint);
        const float3 viewDir = dist > kEps ? toPoint / dist : float3(0.0, 0.0, 1.0);

        if (volumeOn)
        {
            // The volume already holds this ray up to its far plane: the analytic integral runs
            // from there (along the ray, so a slanted ray excludes the right length).
            excludeDistance = fogVolumeParams.y * (dist / max(surfaceViewDepth, kEps));
            fog.startDistance = max(fog.startDistance, excludeDistance);
            fog.sunScatterStartDistance = max(fog.sunScatterStartDistance, excludeDistance);
        }

        const float fogShared = AtmosphereSharedIntegral(dist, camPosWS.y, Pw.y, fog);
        const float tau = AtmosphereOpticalDepth(fogShared, dist, fog);
        const float minT = AtmosphereMinTransmittance(tau, fog.maxOpacity);
        transmittance = AtmosphereTransmittance(tau, minT);

        // The sky sampled ALONG THE VIEW RAY: this is the fog's own colour, so a distant surface
        // converges on the sky it sits against instead of on an authored constant as in UE. Both
        // the blur and the sun lobe fade out as the fog saturates -- see AtmosphereHeadroom.
        const float headroom = AtmosphereHeadroom(transmittance, minT);
        const float3 skyAlongView = AtmosphereClampSkySample(
            IblSkyRadiance(SkySpecular, SkyboxTex, gSmp, viewDir,
                           AtmosphereSkyRoughness(headroom, fogParams2.x),
                           skySpecMipCount, skyboxIntensity),
            IblSkyRadiance(SkySpecular, SkyboxTex, gSmp, viewDir,
                           0.0f, skySpecMipCount, skyboxIntensity));
        inscatter = AtmosphereInscatter(skyAlongView, fogSunColor.rgb,
                                        dot(viewDir, fogSunDir.xyz), fogShared, dist,
                                        headroom, fog);
    }

    // A debug view must not leave the pixels it does NOT describe showing "no fog": the sky is
    // never in the analytic model, so without the volume it is black ("not measured"); with the
    // volume it IS measured (the ray to the volume's far plane) and shows like everything else.
    // This used to be assigned BEFORE the two branches below and was overwritten by them, so the
    // sky read as T = 1 in the transmittance view -- and against the volume's honest value it
    // looked like a 40 % disagreement in the palm crowns.
    const bool debugUnmeasured = !(z > kEps && fogParams0.x > 0.0f) && !volumeOn;
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
        color = debugUnmeasured ? 0.0.xxx : inscatter * (1.0 - transmittance) * vol.a + vol.rgb;
    }
    else if (z > kEps && fogParams0.x > 0.0f)
    {
        color = (color * transmittance + inscatter * (1.0 - transmittance)) * vol.a + vol.rgb;
    }
    else if (volumeOn)
    {
        color = color * vol.a + vol.rgb; // the sky and anything the analytic model leaves alone
    }

    // The debug views 1 / 3 / 5 are GRAY LEVELS in [0, 1], not radiance: written without the
    // pre-exposure they land in the tonemap as display-linear values (its multiplier is 1.0 while
    // pre-exposure is active) and read as the number they are. Multiplied by the pre-exposure they
    // were exposed like sunlight -- a transmittance of 1.0 came out BLACK under a daylight EV --
    // which is how the volumetric fog's parity check first "passed" on two black images. Views 2
    // and 4 are radiance (what the air adds) and stay exposed like the scene.
    const bool debugGray = fogDebugView == 1u;
    SceneColor[dispatchThreadId.xy] = float4(debugGray ? color : color * preExposure, 1.0);
}
