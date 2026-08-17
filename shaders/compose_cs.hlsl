#define COMPOSE_CS_RS "CBV(b0), DescriptorTable(SRV(t0, numDescriptors=12, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE))"
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
    float2 screenSize;
    float2 invScreenSize;
    float4 shoreWetnessWindow;     // xy: centre, z: 1 / half extent, w: darkening
    float4 shoreWetnessAppearance; // x: water-film reflection, yz: slope cutoff/full-wet normal Y, w: water level
    float4 shoreWetnessFallback;   // xy: height above/below water, z: normalized fade start
    float4 shoreWetnessBreakup;    // x: upper-edge strength, y: broad XZ scale in metres
}

static const float kEps = 1e-6;

// Roughness->mip scale for the skybox fallback. The cube has an 11-mip chain; this
// matches glass.hlsl (rough*5) so opaque and glass sky reflections blur identically.
// Legacy sky roughness ceiling, used ONLY when a sky has no F7 derivatives. It was always a guess:
// the display cube's mip chain is a box filter, so treating mip N as "roughness N/5" approximates a
// GGX lobe with a square one. F8's prefiltered cube is the real answer; this stays as the fallback
// for skies imported before it existed.
static const float kSkyRoughMaxMip = 5.0;

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0.xxx - F0) * pow(1.0 - cosTheta, 5.0);
}

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
        const bool useSplitSum = (skySpecMipCount > 0u);

        float3 skyCol = 0.0f.xxx;
        if (enableSkySpecular != 0u)
        {
            // F8: index the PREFILTERED cube by perceptual roughness across its real mip count.
            // Its last mip is roughness 1 by construction, so no constant has to be guessed.
            // Unreal's logarithmic mapping, so a mip always means the same roughness whatever the
            // mip count -- see ibl_common.hlsli. The bake uses the exact inverse.
            const float mip = useSplitSum
                ? IblMipFromRoughness(rough, (float)skySpecMipCount)
                : rough * kSkyRoughMaxMip;
            skyCol = useSplitSum
                ? SkySpecular.SampleLevel(gSmp, Rw, mip).rgb * skyboxIntensity
                : SkyboxTex.SampleLevel(gSmp, Rw, mip).rgb * skyboxIntensity;
        }

        // Skybox as fallback: (ssrColor*α + sky*(1-α)). Apply the material
        // control after the source/fallback blend so None, SkyOnly, SSR and RT
        // share the same response without changing analytic direct specular.
        float3 refl = reflectionRGB + skyCol * (1.0 - reflectionA);
        refl *= indirectSpecularScale;

        float3 spec;
        if (useSplitSum)
        {
            // Split sum: the LUT already integrates Fresnel and the geometry term over the lobe,
            // so multiplying by FresnelSchlick again would apply Fresnel twice. The old
            // `pow(gloss, 1)` fade goes with it -- it existed to hide the fact that a box-filtered
            // mip is not a rough lobe, and a real prefilter does not need hiding.
            const float2 ab = BrdfLut.SampleLevel(gSmp, float2(cosT, rough), 0).rg;
            spec = refl * (F0 * ab.x + ab.y);
        }
        else
        {
            float3 F = FresnelSchlick(cosT, F0);
            float gloss = saturate(1.0 - rough);
            spec = refl * F * pow(gloss, 1);
        }
        color += spec;

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

    SceneColor[dispatchThreadId.xy] = float4(color, 1.0);
}
