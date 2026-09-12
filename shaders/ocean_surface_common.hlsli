// THE OCEAN SURFACES' COMMON GROUND -- every function the surf-sim surface
// (ocean_surface_surf_sim.hlsli) and the run-up surface (ocean_surface_runup.hlsli) had
// textually identical, pulled out on 2026-09-12 (owner: "общие функции в общий инклуд"). Each
// surface includes this after its cbuffer, textures, samplers and structs, so the functions read
// the same names either way; the acceptance test of the move was byte-identical DXIL for all four
// permutations (VS/PS x surf-sim/run-up). A function that DIFFERS between the surfaces stays in its
// file; do not "fix" one copy here without checking that both surfaces want the change.
//
// The cloud shadow on the water (plan C3) lives here too: the map, and the read every sun term is
// scaled by (see CloudSunVisibility).
#ifndef OCEAN_SURFACE_COMMON_HLSLI
#define OCEAN_SURFACE_COMMON_HLSLI
#include "cloud_shadow_common.hlsli" // plan C3: the cloud shadow map's read, shared with the lighting and the fog

Texture2D<float4> CloudShadowMap : register(t21); // plan C3: the cloud shadow map (a dummy when no clouds; cloudShadowParams.x gates the read)

// Plan C3 on the water: the sun's visibility through the cloud column above this point, the SAME
// map and helper the deferred lighting and the fog multiply into the sun. The water had no sun
// shadow at all (shadowAttenuation was a constant 1), so under a closed deck the sea kept sparkling
// while the sand beside it went dark (owner, 2026-09-12). It fills the light's shadowAttenuation
// slot: the specular and the foam already multiply by it, and the body terms (GetOceanColor) light
// with the sun through the cloud PLUS the sky irradiance -- a first version scaled the sun colour
// itself, and the water, which had no sky term of its own, went black beside sand that keeps its
// sky irradiance ("дико тёмные").
float CloudSunVisibility(float3 worldPos)
{
    if (cloudShadowParams.x == 0.0f) { return 1.0f; }
    return CloudShadowTransmittance(worldPos, cloudShadowViewProj, cloudShadowParams.y, CloudShadowMap, LinearClampSampler);
}

Gradient CreateGradient(float4 src[kGradientMaxKeys], float2 params)
{
    Gradient g;
    [unroll]
    for (uint i = 0u; i < kGradientMaxKeys; ++i)
    {
        g.colors[i] = src[i];
    }
    g.colorsCount = (int)params.x;
    g.type = params.y > 0.5f;
    return g;
}

float3 SampleGradient(Gradient grad, float t)
{
    float3 color = grad.colors[0].rgb;
    [unroll]
    for (uint i = 1u; i < kGradientMaxKeys; ++i)
    {
        float prevPos = grad.colors[i - 1u].w;
        float nextPos = grad.colors[i].w;
        float denom = max(nextPos - prevPos, 1e-4f);
        float colorPos = saturate((t - prevPos) / denom);
        float active = step((float)i, (float)(grad.colorsCount - 1));
        colorPos *= active;
        float typeMask = grad.type ? 1.0f : 0.0f;
        float blendType = lerp(colorPos, step(0.01f, colorPos), typeMask);
        color = lerp(color, grad.colors[i].rgb, blendType);
    }
    return color;
}

float2 ComputeScreenUV(float4 clipPosition)
{
    float2 ndc = clipPosition.xy / max(clipPosition.w, 1e-5f);
    return ndc * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
}

float2 ScreenUVToNDC(float2 uv)
{
    float2 ndc;
    ndc.x = uv.x * 2.0f - 1.0f;
    ndc.y = 1.0f - uv.y * 2.0f;
    return ndc;
}

float3 ViewSpacePosition(float depthSample, float2 uv)
{
    float2 ndc = ScreenUVToNDC(uv);
    float4 clipPos = float4(ndc, depthSample, 1.0f);
    float4 viewPos = mul(clipPos, invProj);
    return viewPos.xyz / max(viewPos.w, 1e-6f);
}

float DepthToViewZ_Fast(float d)
{
    return depthParams.y / (d - depthParams.x);
}

float3 PositionWsFromDepth(float depthSample, float2 uv)
{
    float3 viewPos = ViewSpacePosition(depthSample, uv);
    float4 worldPos = mul(float4(viewPos, 1.0f), invView);
    float invW = rcp(max(worldPos.w, 1e-6f));
    return worldPos.xyz * invW;
}

float2 ShoreDepthUV(float2 baseXZ)
{
    float2 offsetXZ = baseXZ - shoreViewParams.xy;
    float invExtent = shoreViewParams.w;
    return float2(offsetXZ.x * invExtent + 0.5f, 0.5f - offsetXZ.y * invExtent);
}

// surf sim injection (debug): the modern surface's SDF UV mapping, duplicated for the debug view.
float2 ShoreSdfUV(float2 baseXZ)
{
    float2 offsetXZ = baseXZ - shoreSdfParams.xy;
    float invExtent = shoreSdfParams.z;
    return float2(offsetXZ.x * invExtent + 0.5f, 0.5f - offsetXZ.y * invExtent);
}

float ShoreViewDepth(float depthSample)
{
    return lerp(shoreDepthParams.x, shoreDepthParams.y, depthSample);
}

float ModifiedManhattanDistance(float3 a, float3 b)
{
    float3 v = a - b;
    return max(abs(v.x + v.z) + abs(v.x - v.z), abs(v.y)) * 0.5f;
}

float EaseInOutClamped(float x)
{
    x = saturate(x);
    return 3.0f * x * x - 2.0f * x * x * x;
}

float4 LodWeights(float viewDist, float lodScale)
{
    float4 length = max(cascadeLengthScales, float4(1e-3f, 1e-3f, 1e-3f, 1e-3f));
    float4 fade = max(length * lodScale, float4(1e-3f, 1e-3f, 1e-3f, 1e-3f));
    float4 x = (viewDist - fade) / fade;
    return float4(1.0f, 1.0f, 1.0f, 1.0f) - float4(
        EaseInOutClamped(x.x),
        EaseInOutClamped(x.y),
        EaseInOutClamped(x.z),
        EaseInOutClamped(x.w));
}

float3 ClipMapVertexInternal(float3 positionOS,
    float2 uv,
    float clipScale,
    float levelHalfSize,
    float3 viewerPosition)
{
    float3 morphOffset = float3(uv.x, 0.0f, uv.y);
    positionOS *= clipScale;
    float meshScale = positionOS.y;
    float step = max(meshScale * 4.0f, 1e-3f);

    float snappedX = floor(viewerPosition.x / step) * step;
    float snappedZ = floor(viewerPosition.z / step) * step;
    float3 worldPos = float3(snappedX + positionOS.x, 0.0f, snappedZ + positionOS.z);

    float morphStart = ((levelHalfSize + 1.0f) * 0.5f + 8.0f) * meshScale;
    float morphEnd = (levelHalfSize - 2.0f) * meshScale;

    float denom = max(1e-3f, morphEnd - morphStart);
    float t = saturate((ModifiedManhattanDistance(worldPos, viewerPosition) - morphStart) / denom);
    worldPos += morphOffset * meshScale * t;
    return worldPos;
}

float3 ClipMapVertex(float3 positionOS, float2 uv)
{
    return ClipMapVertexInternal(positionOS, uv, clipMapParams.x, clipMapParams.y, clipMapViewer.xyz);
}

float3 ClipMapVertexPrev(float3 positionOS, float2 uv)
{
    return ClipMapVertexInternal(positionOS, uv, prevClipMapParams.x, prevClipMapParams.y, prevClipMapViewer.xyz);
}

float2 ApplyClipMapWarp(float2 worldUV, float viewDistXzSquared, float warpDistance)
{
    float warpScale = min(1.0f, viewDistXzSquared / max(warpDistance * warpDistance * 100.0f, 1.0f));
    float2 warpOffset = sin(worldUV.yx / max(warpDistance, 1e-3f)) * warpDistance * 0.4f * windParams0.w;
    return worldUV + warpOffset * warpScale;
}

float3 SampleDisplacementCascadeTexture(Texture2DArray<float4> tex, float2 worldXZ, uint cascade)
{
    float lengthScale = max(cascadeLengthScales[cascade], 1e-3f);
    float3 uvw = float3(worldXZ / lengthScale, cascade * 2.0f);
    float4 sample = tex.SampleLevel(LinearWrapSampler, uvw, 0);
    return sample.xyz;
}

static const float kLodThreshold = 0.05f;

float3 SampleDisplacementTexture(Texture2DArray<float4> tex, float2 worldXZ, float4 weights, uint cascadesCount)
{
    float3 displacement = float3(0.0f, 0.0f, 0.0f);
    [unroll]
    for (uint cascade = 0; cascade < 4; ++cascade)
    {
        if (cascade >= cascadesCount)
        {
            break;
        }
        float w = weights[cascade];
        if (cascade == 0 || w > kLodThreshold)
        {
            displacement += w * SampleDisplacementCascadeTexture(tex, worldXZ, cascade);
        }
    }
    return displacement;
}

float3 SampleCurrentDisplacement(float2 worldXZ, float4 weights, uint cascadesCount)
{
    return SampleDisplacementTexture(DisplacementDerivatives, worldXZ, weights, cascadesCount);
}

float3 SamplePreviousDisplacement(float2 worldXZ, float4 weights, uint cascadesCount)
{
    return SampleDisplacementTexture(PrevDisplacementDerivatives, worldXZ, weights, cascadesCount);
}

float4 CombineDerivatives(DerivativesSet derivatives, float4 weights)
{
    float4 combined = float4(0.0f, 0.0f, 0.0f, 0.0f);
    [unroll]
    for (uint cascade = 0; cascade < 4; ++cascade)
    {
        combined += derivatives.cascades[cascade] * weights[cascade];
    }
    return combined;
}

static const float kNormalScale = 1.5f;

float3 NormalFromCombinedDerivatives(float4 derivatives)
{
    float denomX = max(1e-3f, 1.0f + derivatives.z);
    float denomZ = max(1e-3f, 1.0f + derivatives.w);
    float2 slope = float2(derivatives.x / denomX, derivatives.y / denomZ) * kNormalScale;
    return normalize(float3(-slope.x, 1.0f, -slope.y));
}

float3 NormalFromDerivatives(DerivativesSet derivatives, float4 normalWeights)
{
    float4 combined = CombineDerivatives(derivatives, normalWeights);
    return NormalFromCombinedDerivatives(combined);
}

float4 SampleFoamCascade(float2 worldXZ, uint cascade)
{
    float lengthScale = max(cascadeLengthScales[cascade], 1e-3f);
    float3 uvw = float3(worldXZ / lengthScale, cascade);
    return FoamTurbulence.Sample(LinearWrapSampler, uvw);
}

FoamTurbulenceSet SampleFoamTurbulence(float2 worldXZ, float4 weights, uint cascadesCount)
{
    FoamTurbulenceSet set;
    [unroll]
    for (uint cascade = 0; cascade < 4; ++cascade)
    {
        set.cascades[cascade] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        if (cascade >= cascadesCount)
        {
            continue;
        }

        float w = weights[cascade];
        if (cascade == 0 || w > kLodThreshold)
        {
            float lengthScale = max(cascadeLengthScales[cascade], 1e-3f);
            float3 uvw = float3(worldXZ / lengthScale, cascade);
            set.cascades[cascade] = FoamTurbulence.Sample(LinearWrapSampler, uvw) * w;
        }
    }
    return set;
}

float4 ActiveCascadesMask(uint cascadesCount)
{
    return float4(
        cascadesCount > 0 ? 1.0f : 0.0f,
        cascadesCount > 1 ? 1.0f : 0.0f,
        cascadesCount > 2 ? 1.0f : 0.0f,
        cascadesCount > 3 ? 1.0f : 0.0f);
}

float4 MixTurbulence(FoamTurbulenceSet turbulence, float4 foamWeights, float4 mixWeights)
{
    float4 accum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    [unroll]
    for (uint cascade = 0; cascade < 4; ++cascade)
    {
        accum += turbulence.cascades[cascade] * foamWeights[cascade];
    }
    float totalWeight = dot(foamWeights * mixWeights, float4(1.0f, 1.0f, 1.0f, 1.0f));
    return accum / max(totalWeight, 1e-3f);
}

float2 RotateUV(float2 uv, float2 center, float2 rotation, float sign)
{
    uv -= center;
    float s = rotation.y;
    float c = rotation.x;
    float2x2 rMatrix = float2x2(c, -sign * s, sign * s, c);
    rMatrix *= 0.5f;
    rMatrix += 0.5f;
    rMatrix = rMatrix * 2.0f - 1.0f;
    uv = mul(uv, rMatrix);
    uv += center;
    return uv;
}

float FoamTrailSample(float2 worldUV, float2 direction, float2 scale)
{
    float2 rotated = RotateUV(worldUV, float2(0.0f, 0.0f), direction, 1.0f);
    float2 safeScale = max(scale, float2(1e-3f, 1e-3f));
    return FoamTrailTex.SampleLevel(LinearWrapSampler, rotated / safeScale, 0).r;
}

float DeepFoam(float2 worldUV, float3 viewDir, float3 normal, float time)
{
    float denom = max(dot(normal, viewDir), 1e-3f);
    float2 parallaxDir = (viewDir.xz / denom + 0.5f * normal.xz);
    float2 uv = worldUV - parallaxDir * foamParams2.z - windParams1.xy * time;
    return FoamUnderwaterTex.SampleLevel(LinearWrapSampler, uv * 0.2f, 0).r;
}

float2 Coverage(FoamTurbulenceSet turbulence, float4 mixWeights, float2 worldUV, float deepFoam, float bias)
{
    float4 mixed = MixTurbulence(turbulence, foamCascadeWeights, mixWeights);
    float foamValueCurrent = lerp(mixed.y, mixed.x, foamParams0.z);
    float foamValuePersistent = 0.5f * (mixed.z + mixed.w);
    foamValueCurrent = lerp(foamValueCurrent, foamValuePersistent, foamParams0.w);
    foamValueCurrent -= 1.0f;
    foamValuePersistent -= 1.0f;

    float trail0 = FoamTrailSample(worldUV, foamTrailParams1.xy, foamTrailParams0.xy);
    float trailTexture = trail0;
    if (foamParams2.x > 0.0f)
    {
        float trail1 = FoamTrailSample(worldUV, foamTrailParams1.zw, foamTrailParams0.zw);
        trailTexture = lerp(trail0, trail1, saturate(foamParams2.x));
    }
    
    foamValuePersistent += saturate(foamValuePersistent + 1.0f) * trailTexture * foamParams1.y;
    float foamValue = max(foamValuePersistent + foamParams1.x * (1.0f - bias),
        foamValueCurrent + foamParams0.x * (1.0f - bias));

    float surfaceFoam = saturate(foamValue * foamParams0.y);
    float shallowUnderwaterFoam = saturate((foamValue + 0.1f * foamParams1.z) * foamParams0.y);
    float deepUnderwaterFoam = deepFoam * saturate((foamValue + foamParams1.z * 0.25f) * foamParams0.y * 0.8f);
    return float2(surfaceFoam, max(shallowUnderwaterFoam, deepUnderwaterFoam));
}

float Pow5(float x)
{
    float x2 = x * x;
    return x2 * x2 * x;
}

float SchlickFresnel(float cosTheta)
{
    const float baseReflectivity = 0.02f;
    float clamped = saturate(cosTheta);
    return baseReflectivity + (1.0f - baseReflectivity) * Pow5(1.0f - clamped);
}

float2 SlopeVarianceSquared(float windSpeed, float viewDist, float alignment, float scale)
{
    float upwind = 0.01f * sqrt(max(windSpeed, 0.0f)) * viewDist / max(viewDist + scale, 1e-3f);
    return float2(upwind, upwind * (1.0f - 0.3f * alignment));
}

float3 TransformToWind(float3 v)
{
    return mul(worldToWind, float4(v, 0.0f)).xyz;
}

float SampleDistantRoughness(float2 worldUV, float viewDist)
{
    float2 uv = worldUV * 0.001f * 0.01f;
    float roughness = DistantRoughnessMap.SampleLevel(LinearWrapSampler, uv, 0).r;
    float patchLength = max(simulationParams.x, 1.0f);
    roughness *= saturate((viewDist / patchLength) * 0.05f);
    return roughness;
}

float2 SubsurfaceScatteringFactor(const LightingInput li)
{
    float3 aligned = normalize(lerp(li.viewDir, li.normal, subsurfaceParams.w));
    float normalFactor = saturate(dot(aligned, li.viewDir));

    float heightOffset = li.referenceWaveHeight * (1.0f + heightFogParams.x);
    float heightFactor = saturate((li.positionWS.y + heightOffset) * 0.5f / max(0.5f, li.referenceWaveHeight));
    heightFactor = pow(abs(heightFactor), max(1.0f, li.referenceWaveHeight * 0.4f));

    float spread = max(subsurfaceParams.z, 1e-3f);
    float sunDot = saturate(dot(-li.mainLight.direction, -li.viewDir));
    float sunExponent = min(50.0f, 1.0f / spread);
    float sun = subsurfaceParams.x * normalFactor * heightFactor * pow(sunDot, sunExponent);

    float distFade = heightFogParams.y;
    float environment = subsurfaceParams.y * normalFactor * heightFactor * saturate(1.0f - li.viewDir.y);
    float fade = distFade / (distFade + li.viewDist + 1e-3f);
    return float2(sun, environment) * fade;
}

float meanFresnel(float cosThetaV, float sigmaV)
{
    return pow(abs(1.0f - cosThetaV), 5.0f * exp(-2.69f * sigmaV)) / (1.0f + 22.7f * pow(abs(sigmaV), 1.5f));
}

// V, N in wind space
float MeanFresnel(float3 V, float3 N, float2 sigmaSq)
{
    float2 v = V.xz; // view direction in wind space
    float2 t = v * v / (1.0f - V.y * V.y); // cos^2 and sin^2 of view direction
    float sigmaV2 = dot(t, sigmaSq); // slope variance in view direction
    return meanFresnel(dot(V, N), sqrt(sigmaV2));
}

float EffectiveFresnel(const LightingInput li, const BrunetonInputs bi)
{
    //(void)bi;
    //return saturate(SchlickFresnel(dot(li.viewDir, li.normal)));

    const float R = 0.02f;
    float fresnel = R + (1.0f - R) * MeanFresnel(
		bi.viewDirWind,
		bi.normalWind,
		bi.slopeVarianceSquared);
    return saturate(fresnel);
}

// Horizon pull for the ENVIRONMENT reflection ray (skyParams.w; 1 = identity, the shipped default).
//
// A wave crest swings the reflected ray between "just above the horizon" (bright) and "high into the
// zenith" (dark), and a clear-sky HDRI has a steep Rayleigh gradient between those two — so adjacent
// facets sample very different radiance, and at grazing angles Fresnel is ~1, which passes that
// contrast to the eye at full strength. It reads as hard dark streaks along the crests.
//
// The prefilter is supposed to soften exactly this, but IblClampToSharp (ibl_common.hlsli) bounds the
// blurred sample by the SHARP one in the same direction — a one-sided guard against the sun smearing
// through the lobe. Where the sharp direction lands in the dark zenith, the ceiling is "2x dark" and
// the blur is undone precisely where it was needed, so the streaks stay pixel-crisp at any roughness.
//
// Compressing the ray's Y before the sky lookup keeps the reflection in the band near the horizon,
// which is also where real water reflects from at these view angles. Sun glitter is unaffected while
// the sun is low (this scene's is ~3 degrees) because the pull moves rays TOWARD it; a high sun would
// have its specular handled by the direct term anyway. Only the ocean's env sample is touched --
// the planar reflection, the land IBL and the sky itself are untouched.
float3 OceanSkyReflectDir(float3 reflectDir)
{
    const float pull = (skyParams.w > 0.0f) ? skyParams.w : 1.0f;
    if (pull >= 0.999f) { return reflectDir; }
    return normalize(float3(reflectDir.x, reflectDir.y * pull, reflectDir.z));
}

float2 OceanReflectionUvOffset(const LightingInput li, float3 adjustedNormal)
{
    float3 flatReflectDir = reflect(-li.viewDir, float3(0.0f, 1.0f, 0.0f));
    float3 waveReflectDir = reflect(-li.viewDir, adjustedNormal);
    float2 reflectionDelta = waveReflectDir.xz - flatReflectDir.xz;

    float distanceFade = saturate(li.viewDist / max(specularParams.z, 1.0f));
    float grazing = saturate(1.0f - abs(waveReflectDir.y));
    float strength = lerp(0.08f, 0.025f, distanceFade) * lerp(0.45f, 1.0f, grazing) * 20;
    return reflectionDelta * strength;
}

float OceanReflectionEdgeFade(float2 uv)
{
    float2 edgeDist = min(uv, float2(1.0f, 1.0f) - uv);
    return saturate(min(edgeDist.x, edgeDist.y) * 64.0f);
}

float3 DeepScatterColor(float depthScale)
{
    return deepScatterColor.rgb;
}

float3 SssColor(float depthScale)
{
    return sssColor.rgb;
}

float3 DiffuseColor(float depthScale)
{
    return diffuseColor.rgb;
}

float3 AbsorptionTint(float attenuation)
{
    float4 colors[kGradientMaxKeys];
    [unroll]
    for (uint i = 0u; i < kGradientMaxKeys; ++i)
    {
        colors[i] = absorptionColors[i];
    }
    Gradient gradient = CreateGradient(colors, absorptionGradientParams.xy);
    return SampleGradient(gradient, attenuation);
}

float3 ColorThroughWater(float3 color, float3 volumeColor, float distThroughWater, float depth)
{
    distThroughWater = max(distThroughWater, 0.0f);
    depth = max(depth, 0.0f);

    float absorptionScale = max(refractionParams.z, 1.0f);
    float fogDensity = max(refractionParams.w, 0.0f);

    float attenuation = exp(-(distThroughWater + depth) / absorptionScale);
    float3 tinted = color * AbsorptionTint(attenuation);

    float fog = 1.0f - exp(-fogDensity * distThroughWater);
    return lerp(tinted, volumeColor, saturate(fog));
}

#endif // OCEAN_SURFACE_COMMON_HLSLI
