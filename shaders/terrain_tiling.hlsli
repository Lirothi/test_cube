#ifndef TERRAIN_TILING_HLSLI
#define TERRAIN_TILING_HLSLI

// Voronoi texture bombing for large, repeating terrain textures. The Voronoi grid is evaluated
// procedurally, so the material needs no authored zone map. Each zone owns a stable UV rotation
// and uniform scale; the three nearest zones are blended to hide cell boundaries.
struct TerrainZone
{
    float2 seed;
    float4 random;
    float distanceSq;
};

struct TerrainTileSample
{
    float2 uv;
    float2 gradientX;
    float2 gradientY;
    float2 rotation; // cos(angle), sin(angle)
    float weight;
    float scale;
    float contrast; // r in Equation 5 of the paper; 0.5 = no ramp
};

// beta in Equations 3 and 4: how far the per-sample metric is allowed to pull the geometric weight.
static const float kTerrainFallOffContrast = 0.6;
static const float3 kTerrainLumaWeights = float3(0.299, 0.587, 0.114);

inline TerrainTileSample TerrainIdentityTileSample(float2 uv, float weight)
{
    TerrainTileSample result;
    result.uv = uv;
    result.gradientX = ddx(uv);
    result.gradientY = ddy(uv);
    result.rotation = float2(1.0, 0.0);
    result.weight = weight;
    result.scale = 1.0;
    result.contrast = 0.5;
    return result;
}

inline float4 TerrainHash4(float2 cell)
{
    const float4 h = float4(
        dot(cell, float2(127.1, 311.7)),
        dot(cell, float2(269.5, 183.3)),
        dot(cell, float2(419.2, 371.9)),
        dot(cell, float2(95.7, 227.1)));
    return frac(sin(h) * 43758.5453);
}

inline float2 TerrainHash22(float2 cell)
{
    float3 p3 = frac(float3(cell.x, cell.y, cell.x) * float3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.xx + p3.yz) * p3.zy);
}

inline float2 TerrainValueNoise2(float2 position)
{
    const float2 cell = floor(position);
    float2 fraction = frac(position);
    fraction = fraction * fraction * (3.0 - 2.0 * fraction);

    const float2 noise00 = TerrainHash22(cell);
    const float2 noise10 = TerrainHash22(cell + float2(1.0, 0.0));
    const float2 noise01 = TerrainHash22(cell + float2(0.0, 1.0));
    const float2 noise11 = TerrainHash22(cell + 1.0.xx);
    return lerp(
        lerp(noise00, noise10, fraction.x),
        lerp(noise01, noise11, fraction.x),
        fraction.y);
}

// A continuous multi-frequency warp breaks the geometrically clean Voronoi arcs into torn,
// natural-looking borders. It moves only the zone-map lookup: source UVs and their gradients are
// left untouched, so edge breakup cannot disturb texture filtering or create swimming detail.
inline float2 TerrainWarpZoneMap(float2 zoneUv, float4 edgeParams)
{
    const float detail = max(edgeParams.y, 0.5);

    // The warped map must stay injective: once |J(warp)| passes ~1 the zone map folds, so a zone
    // reappears mirrored right next to itself and cell borders cusp into slivers -- the opposite of
    // what tiling is for. Measured on this noise, max|J| ~= 4.55 * breakup * detail, so cap the
    // amplitude against the frequency and keep a margin below the fold threshold.
    const float maxBreakup = 1.5 / (4.55 * detail);
    const float breakup = min(clamp(edgeParams.x, 0.0, 0.45), maxBreakup);

    const float2 octave0 =
        TerrainValueNoise2(zoneUv * detail + float2(17.13, 41.71)) * 2.0 - 1.0;
    const float2 octave1 =
        TerrainValueNoise2(zoneUv * (detail * 2.07) + float2(73.91, 11.27)) * 2.0 - 1.0;
    const float2 octave2 =
        TerrainValueNoise2(zoneUv * (detail * 4.19) + float2(29.47, 89.63)) * 2.0 - 1.0;
    return (octave0 + octave1 * 0.5 + octave2 * 0.25) * (breakup / 1.75);
}

inline float2 TerrainRotate(float2 value, float2 rotation)
{
    return float2(
        rotation.x * value.x - rotation.y * value.y,
        rotation.y * value.x + rotation.x * value.y);
}

inline float2 TerrainRotateInverse(float2 value, float2 rotation)
{
    return float2(
        rotation.x * value.x + rotation.y * value.y,
       -rotation.y * value.x + rotation.x * value.y);
}

inline void TerrainInsertZone(TerrainZone candidate,
                              inout TerrainZone nearest,
                              inout TerrainZone second,
                              inout TerrainZone third)
{
    if (candidate.distanceSq < nearest.distanceSq)
    {
        third = second;
        second = nearest;
        nearest = candidate;
    }
    else if (candidate.distanceSq < second.distanceSq)
    {
        third = second;
        second = candidate;
    }
    else if (candidate.distanceSq < third.distanceSq)
    {
        third = candidate;
    }
}

inline TerrainTileSample TerrainMakeTileSample(
    TerrainZone zone,
    float weight,
    float2 tiledUv,
    float2 tiledGradientX,
    float2 tiledGradientY,
    float zoneSize,
    float maxRotationRadians,
    float scaleVariation,
    float colorContrast)
{
    TerrainTileSample result;

    const float angle = (zone.random.z * 2.0 - 1.0) * maxRotationRadians;
    float sine;
    float cosine;
    sincos(angle, sine, cosine);
    result.rotation = float2(cosine, sine);

    const float minScale = max(1.0 - scaleVariation, 0.25);
    const float scale = lerp(minScale, 1.0 + scaleVariation, zone.random.w);
    const float2 pivot = zone.seed * zoneSize;

    result.uv = pivot + TerrainRotate(tiledUv - pivot, result.rotation) * scale;
    result.gradientX = TerrainRotate(tiledGradientX, result.rotation) * scale;
    result.gradientY = TerrainRotate(tiledGradientY, result.rotation) * scale;
    result.weight = weight;
    result.scale = scale;
    result.contrast = colorContrast;
    return result;
}

// params: x = zone size in source-texture repeats, y = max rotation in radians,
// z = scale variation around 1, w = edge blend (0 = hard cells, 1 = broad blend).
// edgeParams: x = zone-map breakup amplitude, y = breakup detail, zw reserved.
inline void TerrainBuildTileSamples(float2 tiledUv, float4 params, float4 edgeParams,
                                    out TerrainTileSample tile0,
                                    out TerrainTileSample tile1,
                                    out TerrainTileSample tile2)
{
    const float zoneSize = max(params.x, 0.25);
    const float2 zoneUv = tiledUv / zoneSize;
    const float2 warpedZoneUv = zoneUv + TerrainWarpZoneMap(zoneUv, edgeParams);
    const float2 baseCell = floor(warpedZoneUv);

    TerrainZone nearest;
    TerrainZone second;
    TerrainZone third;
    nearest.seed = second.seed = third.seed = 0.0.xx;
    nearest.random = second.random = third.random = 0.0.xxxx;
    nearest.distanceSq = second.distanceSq = third.distanceSq = 1.0e20;

    // Feature points stay away from their cell corners. Besides producing more even macro-zones,
    // this guarantees that the nearest candidates live in this 3x3 neighborhood.
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            TerrainZone candidate;
            const float2 cell = baseCell + float2(x, y);
            candidate.random = TerrainHash4(cell);
            candidate.seed = cell + lerp(0.2.xx, 0.8.xx, candidate.random.xy);
            const float2 delta = warpedZoneUv - candidate.seed;
            candidate.distanceSq = dot(delta, delta);
            TerrainInsertZone(candidate, nearest, second, third);
        }
    }

    float3 weights = float3(1.0, 0.0, 0.0);
    const float edgeBlend = saturate(params.w);
    if (edgeBlend > 1.0e-4)
    {
        const float sharpness = lerp(96.0, 6.0, edgeBlend);
        const float3 distances = float3(
            nearest.distanceSq, second.distanceSq, third.distanceSq);
        weights = exp2(-max(distances - nearest.distanceSq.xxx, 0.0.xxx) * sharpness);
        // A linear crossfade between strongly rotated, therefore weakly correlated texture
        // lookups loses high-frequency contrast. Keep the artist's Blend control, but shape the
        // geometric weights like the reference stochastic-tiling implementations (power weights).
        const float weightExponent = lerp(4.0, 1.0, edgeBlend);
        weights = pow(max(weights, 0.0.xxx), weightExponent);
        weights *= rcp(max(weights.x + weights.y + weights.z, 1.0e-5));
    }

    const float2 tiledGradientX = ddx(tiledUv);
    const float2 tiledGradientY = ddy(tiledUv);
    const float maxRotationRadians = clamp(params.y, 0.0, 3.14159265359);
    const float scaleVariation = clamp(params.z, 0.0, 0.75);
    // r for the color ramp (Equation 5). The paper recommends r in [0.65, 0.75] and notes that
    // r = 0.5 is a no-op, so the artist's Blend control drives it: hard cells keep their contrast,
    // a broad blend fades to the plain linear mix.
    const float colorContrast = lerp(0.85, 0.5, edgeBlend);

    tile0 = TerrainMakeTileSample(nearest, weights.x, tiledUv, tiledGradientX, tiledGradientY,
                                  zoneSize, maxRotationRadians, scaleVariation, colorContrast);
    tile1 = TerrainMakeTileSample(second, weights.y, tiledUv, tiledGradientX, tiledGradientY,
                                  zoneSize, maxRotationRadians, scaleVariation, colorContrast);
    tile2 = TerrainMakeTileSample(third, weights.z, tiledUv, tiledGradientX, tiledGradientY,
                                  zoneSize, maxRotationRadians, scaleVariation, colorContrast);
}

inline float4 TerrainSampleTexture(Texture2D textureMap, SamplerState textureSampler,
                                   TerrainTileSample tile0,
                                   TerrainTileSample tile1,
                                   TerrainTileSample tile2)
{
    return textureMap.SampleGrad(textureSampler, tile0.uv, tile0.gradientX, tile0.gradientY) * tile0.weight +
           textureMap.SampleGrad(textureSampler, tile1.uv, tile1.gradientX, tile1.gradientY) * tile1.weight +
           textureMap.SampleGrad(textureSampler, tile2.uv, tile2.gradientX, tile2.gradientY) * tile2.weight;
}

// Equation 5 / Listing 8: Perlin's S-curve on the blending weights, normalized. k = 1 (r = 0.5)
// leaves the weights untouched; r > 0.5 pushes them apart, which is what restores the contrast a
// linear blend of decorrelated tiles loses.
inline float3 TerrainGain3(float3 x, float r)
{
    const float k = log2(max(1.0 - r, 1.0e-4)) * rcp(log2(0.5));
    const float3 s = 2.0 * step(0.5.xxx, x);
    const float3 m = 2.0 * (1.0.xxx - s);
    const float3 result = 0.5 * s + 0.25 * m * pow(max(0.0.xxx, s + x * m), k);
    return result * rcp(max(result.x + result.y + result.z, 1.0e-5));
}

// Listing 4 (Equations 1, 4 and 5): the paper deliberately drops histogram preservation so the
// ORIGINAL texture can be sampled, and recovers the lost contrast by shaping the weights instead --
// luminance modulates them, then the S-curve ramp separates them. Restoring variance around a
// texture-wide mean (the Heitz-Neyret operator) is only valid in the Gaussianized domain; applied to
// raw color it overshoots and clamps. Used for color and data maps; alpha keeps the plain
// coverage-preserving blend below.
inline float4 TerrainSampleTextureColor(Texture2D textureMap,
                                        SamplerState textureSampler,
                                        TerrainTileSample tile0,
                                        TerrainTileSample tile1,
                                        TerrainTileSample tile2)
{
    const float4 sample0 = textureMap.SampleGrad(
        textureSampler, tile0.uv, tile0.gradientX, tile0.gradientY);
    const float4 sample1 = textureMap.SampleGrad(
        textureSampler, tile1.uv, tile1.gradientX, tile1.gradientY);
    const float4 sample2 = textureMap.SampleGrad(
        textureSampler, tile2.uv, tile2.gradientX, tile2.gradientY);

    const float3 luminance = float3(
        dot(sample0.rgb, kTerrainLumaWeights),
        dot(sample1.rgb, kTerrainLumaWeights),
        dot(sample2.rgb, kTerrainLumaWeights));

    float3 weights = float3(tile0.weight, tile1.weight, tile2.weight) *
                     lerp(1.0.xxx, luminance, kTerrainFallOffContrast);
    weights *= rcp(max(weights.x + weights.y + weights.z, 1.0e-5));
    weights = TerrainGain3(weights, tile0.contrast);

    return sample0 * weights.x + sample1 * weights.y + sample2 * weights.z;
}

// Listings 9 and 10 of "Practical Real-Time Hex-Tiling" treat a tangent-space normal map as
// partial derivatives of an implicit height field. Derivatives are linear and can be blended;
// normalized normals cannot. Keep the paper's bounded division for nearly vertical source normals.
inline float2 TerrainTangentNormalToDerivative(float3 tangentNormal)
{
    const float derivativeLimitScale = 1.0 / 128.0;
    const float3 absoluteNormal = abs(tangentNormal);
    const float denominator = max(
        absoluteNormal.z,
        derivativeLimitScale * max(absoluteNormal.x, absoluteNormal.y));
    return -tangentNormal.xy / max(denominator, 1.0e-6);
}

inline float3 TerrainDecodeTangentNormal(float4 packedNormal, bool normalIsRG)
{
    float3 tangentNormal = packedNormal.xyz * 2.0 - 1.0;
    if (normalIsRG)
    {
        tangentNormal.z = sqrt(saturate(1.0 - dot(tangentNormal.xy, tangentNormal.xy)));
    }
    return tangentNormal;
}

inline float2 TerrainSampleNormalDerivative(Texture2D normalMap,
                                            SamplerState textureSampler,
                                            TerrainTileSample tile,
                                            bool normalIsRG)
{
    const float4 packedNormal = normalMap.SampleGrad(
        textureSampler, tile.uv, tile.gradientX, tile.gradientY);
    const float2 derivative = TerrainTangentNormalToDerivative(
        TerrainDecodeTangentNormal(packedNormal, normalIsRG));

    // Listing 3: the lookup rotates by R, so the derivative rotates back by transpose(R).
    // The tile's UV scale is deliberately NOT applied here. Treating the normal map as a height
    // field fixed in UV would make a zone shrunk to 0.8 carry 1/0.8 steeper slopes, i.e. every zone
    // gets its own effective normal strength -- and because mean shading saturates with slope, each
    // Voronoi island then reads at its own brightness (~5% apart at normalStrength 5, which is
    // exactly the island artifact). The paper varies rotation and offset only, never scale.
    return TerrainRotateInverse(derivative, tile.rotation);
}

inline float2 TerrainBlendNormalDerivatives(Texture2D normalMap,
                                            SamplerState textureSampler,
                                            TerrainTileSample tile0,
                                            TerrainTileSample tile1,
                                            TerrainTileSample tile2,
                                            bool normalIsRG)
{
    const float2 derivative0 = TerrainSampleNormalDerivative(
        normalMap, textureSampler, tile0, normalIsRG);
    const float2 derivative1 = TerrainSampleNormalDerivative(
        normalMap, textureSampler, tile1, normalIsRG);
    const float2 derivative2 = TerrainSampleNormalDerivative(
        normalMap, textureSampler, tile2, normalIsRG);

    // Equation 3 / Listing 3: favor samples carrying an actual slope so unrelated opposing
    // derivatives do not turn a transition into a flat, visibly blurry patch. No ramp here --
    // the paper keeps r = 0.5 for normal maps.
    const float3 derivativeLengthSq = float3(
        dot(derivative0, derivative0),
        dot(derivative1, derivative1),
        dot(derivative2, derivative2));
    float3 slopeWeights = sqrt(
        derivativeLengthSq / (1.0.xxx + derivativeLengthSq));
    slopeWeights = lerp(1.0.xxx, slopeWeights, kTerrainFallOffContrast);

    float3 weights = float3(tile0.weight, tile1.weight, tile2.weight) * slopeWeights;
    weights *= rcp(max(weights.x + weights.y + weights.z, 1.0e-5));
    return derivative0 * weights.x + derivative1 * weights.y + derivative2 * weights.z;
}

#endif // TERRAIN_TILING_HLSLI
