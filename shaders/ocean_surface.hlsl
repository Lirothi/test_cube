// TWO WHOLE SURFACES IN ONE MATERIAL, chosen by OCEAN_SHORE_RUNUP:
//   1 (default)                    — the modern shore stack below: run-up sheet with a travelling
//                                    front, anchored swash, contact foam with the torn dither
//                                    edge, the SDF, the sink, all of it.
//   0 ("--ocean-classic-shore")    — ocean_surface_legacy.hlsli, the surface VERBATIM from commit
//                                    3e54d5d (2026-06-22), the last state before the shore rework:
//                                    classic depth-map damping and the old contact foam. Kept as a
//                                    byte-faithful baseline for looks and perf, not re-created.
#ifndef OCEAN_SHORE_RUNUP
#define OCEAN_SHORE_RUNUP 1
#endif

#if !OCEAN_SHORE_RUNUP
#include "ocean_surface_legacy.hlsli"
#else

#define OCEAN_SURFACE_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0), DescriptorTable(SRV(t0, numDescriptors=16, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE))"
#pragma pack_matrix(row_major)

#include "utils.hlsli"

cbuffer OceanCB : register(b0)
{
    float4x4 model;
    float4x4 view;
    float4x4 proj;
    float4x4 prevModel;
    float4x4 viewProj;
    float4x4 viewProjNoJitter;
    float4x4 prevViewProjNoJitter;
    float4x4 invView;
    float4x4 invProj;
    float4 shoreViewParams;            // x: center x, y: center z, z: height, w: inv extent (1 / 500)
    float4 shoreSdfParams;             // x: centre x, y: centre z, z: inv extent (1 / 2000), w: texel world size
    float4 shoreDepthParams;           // x: zNear, y: zFar
    float4x4 worldToWind;
    float4 simulationParams;           // x: patch length, y: inv patch length, z: time, w: cascades count
    float4 viewerParams;               // x: viewer x, y: viewer z, z: amplitude, w: fade distance
    float4 cascadeLengthScales;        // length scales per cascade
    float4 inverseCascadeLengthScales; // inv length scales per cascade
    float4 clipMapParams;              // x: scale, y: level half size, z: vertex density, w: fade distance
    float4 clipMapViewer;              // xyz: viewer position
    float4 prevClipMapParams;          // previous frame clipmap params
    float4 prevClipMapViewer;          // previous frame viewer position
    float4 foamParams0;                // x: coverage, y: density, z: sharpness, w: persistence
    float4 foamParams1;                // x: trail, y: trail strength, z: underwater intensity, w: normal strength
    float4 foamCascadeWeights;         // per-cascade foam weighting
    float4 specularParams;             // x: spec strength, y: roughness scale, z: roughness distance, w: horizon fog strength
    float4 refractionParams;           // x: surface refraction strength, y: underwater refraction strength, z: absorption depth scale, w: fog density
    float4 subsurfaceParams;           // x: sun scatter strength, y: sky scatter strength, z: scatter spread, w: view alignment strength
    float4 heightFogParams;            // x: SSS height bias, y: SSS fade distance, z: horizon fog distance scale, w: reflection normal strength
    float4 normalSamplingParams;       // x: detail normal mip bias, y: active macro normal mip bias
    float4 shoreBehaviorParams0;       // x: vertical fade depth, y: horizontal minimum, z: horizontal fade depth, w: normal fade depth
    float4 shoreBehaviorParams1;       // x: run-up depth, y: run-up strength, z: max wave height, w: bottom clearance
    float4 shoreNormalMinWeights;      // minimum normal/foam weight for each cascade at the shoreline
    float4 shoreFoamGeometryParams;    // x: main width, y: breakup length, z: geometry-edge refraction fade, w: opacity
    float4 shoreFoamPatternParams;     // x: pattern scale, y: density, z: scroll speed, w: signed-depth warp strength
    float4 shoreFoamBreakupParams;     // x: breakup-length variation, y: variation scale, z: contact normal strength, w: unused
    float4 shoreFoamWindParams;        // x: wind force 0..1, y: wind force below which there is no contact foam, z: wind force at which it is at full strength, w: unused
    float4 shoreFoamAlbedoParams;      // x: shore albedo scale, y: shore albedo scroll speed, z: signed-depth warp range, w: warp scale
    float4 shoreSlopeParams;           // xy: run-up slope fade gradient thresholds, z: edge soft depth, w: geometry fade distance
    float4 shoreSwashParams;           // x: swash amplitude, y: run-up slope smoothing baseline (shore-map texels), z: reference wave height at "full at wind"
    float4 shoreSamplingParams;        // xy: shore-depth texel size, zw: shore-depth texel world size
    float4 sunDirAmbient;              // xyz: sun direction, w: ambient intensity
    float4 sunColorExposure;           // xyz: sun color, w: exposure multiplier
    float4 deepScatterColor;           // xyz: deep scatter tint, w: unused
    float4 sssColor;                   // xyz: subsurface scattering tint, w: unused
    float4 diffuseColor;               // xyz: diffuse tint, w: unused
    float4 absorptionGradientParams;   // x: color count, y: gradient type (0 = linear, 1 = curved)
    float4 absorptionColors[8];        // gradient color keys (rgb) and position in w
    float4 windParams0;                // x: wind speed, y: waves scale, z: alignment, w: uv warp strength
    float4 windParams1;                // xy: wind direction, z: reference wave height, w: padding
    float4 foamTrailParams0;           // xy: trail size 0, zw: trail size 1
    float4 foamTrailParams1;           // xy: trail dir 0, zw: trail dir 1
    float4 foamParams2;                // x: trail blend, y: unused, z: underwater parallax, w: padding
    float4 foamTint;                   // xyz: foam tint, w: unused
    float4 depthTextureSize;           // xy: texel size, zw: texture size
    float2 depthParams;                // x: zNear / (zNear - zFar) y :(zNear * zFar) / (zFar - zNear)
};

Texture2DArray<float4> DisplacementDerivatives : register(t0);
Texture2DArray<float4> PrevDisplacementDerivatives : register(t1);
Texture2DArray<float4> FoamTurbulence : register(t2);
Texture2D SceneColorTexture : register(t3);
TextureCube SkyboxTexture : register(t4);
Texture2D DistantRoughnessMap : register(t5);
Texture2D FoamDetailMap : register(t6);
Texture2D FoamAlbedoTex : register(t7);
Texture2D FoamUnderwaterTex : register(t8);
Texture2D FoamTrailTex : register(t9);
Texture2D ShoreFoamBreakupMaskTex : register(t10);
Texture2D ShoreFoamAlbedoTex : register(t11);
Texture2D SceneDepthTexture : register(t12);
Texture2D ShoreDepthTexture : register(t13);
// Shore SDF: plan-view distance to the waterline over the WHOLE level (2 km at ~1.95 m per texel),
// built once per load by ocean_shore_sdf.hlsl. Negative inland. It answers exactly one question —
// how far is land — which is what the wave's vertical damping needs, and unlike a camera-following
// window it has an answer everywhere. Nothing reads it for foam, run-up or normals: those want
// DEPTH, which is what the shore map above is for.
Texture2D ShoreSdfTexture : register(t14);
Texture2D OceanReflectionTexture : register(t15);
SamplerState LinearWrapSampler : register(s0);
SamplerState LinearClampSampler : register(s1);
SamplerState PointClampSampler : register(s2);
SamplerState AnisotropicWrapSampler : register(s3);

struct VSInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float2 baseXZ : TEXCOORD1;
    float4 positionNDC : TEXCOORD2;
    float viewDepth : TEXCOORD3;
    float4 prevPositionNDC : TEXCOORD4;
    float4 positionNDCJitter : TEXCOORD5;
    // w carries |grad depth| (metres of depth per metre of world XZ), the SMOOTH half of the foam
    // strip's antialiasing floor — see the depthFeather comment in PSMain. Interpolators are
    // allocated in float4 slots anyway, so this component was already being paid for.
    float4 shoreData : TEXCOORD6;      // x: predicted depth, y: source depth, z: shore-effect weight
};

struct DerivativesSet
{
    float4 cascades[4];
};

struct FoamInput
{
    DerivativesSet derivatives;
    float2 worldUV;
    float viewDist;
    float4 lodWeights;
    float4 shoreWeights;
    float time;
    float3 viewDir;
    float3 normal;
    float shoreDepth;
    // Screen-space depth change per pixel, taken from the VERTEX-interpolated water depth rather
    // than from fwidth() of the shore-depth TEXTURE. A bilinear fetch is C0 but not C1: its
    // derivative is piecewise constant and jumps at every texel boundary, so fwidth() of it paints
    // that map's texel grid into whatever consumes it — the regular stripes across the strip.
    float depthFeather;
    // Metres of world per screen pixel at this fragment, from the flat base grid. Both the strip's
    // minimum width and the tear pattern's frequency ceiling are expressed in pixels through it.
    float pixelWorldSize;
    float fallbackShoreDepth;
    float fallbackShoreWeight;
    float shoreFieldWeight;
    float shoreEffectWeight;
};

struct FoamData
{
    float2 coverage;
    float3 normal;
    float3 albedo;
};

struct FoamTurbulenceSet
{
    float4 cascades[4];
};

struct LightData
{
    float3 direction;
    float3 color;
    float shadowAttenuation;
};

struct LightingInput
{
    float3 normal;
    float3 viewDir;
    float viewDist;
    float roughnessMap;
    float3 positionWS;
    float2 screenUV;
    float4 shore;
    float4 positionNDC;
    float viewDepth;
    float3 cameraPos;
    float height;
    float referenceWaveHeight;
    float slopeFactor;
    LightData mainLight;
    float ambient;
};

struct BrunetonInputs
{
    float3 viewDirWind;
    float3 normalWind;
    float2 slopeVarianceSquared;
};

static const float3 kSkyColor = float3(0.24f, 0.38f, 0.55f);
static const float kSpecularMinPower = 64.0f;
static const float kSpecularMaxPower = 512.0f;
static const float kSkyRoughMaxMip = 5.0f;
static const float kLodThreshold = 0.05f;

static const uint kGradientMaxKeys = 8u;

struct Gradient
{
    float4 colors[kGradientMaxKeys];
    int colorsCount;
    bool type;
};

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

float SampleSceneDepth(float2 uv)
{
    return SceneDepthTexture.SampleLevel(PointClampSampler, uv, 0).r;
}

// Filtered read, for FOAM only. Foam uses the scene depth as a soft mask, so it wants the values
// interpolated; point sampling hands it the depth buffer's pixel steps and the contact strip
// inherits them as a staircase. Refraction and world-position reconstruction keep the point read
// above on purpose — averaging depth across a silhouette invents a position on neither surface.
float SampleSceneDepthFiltered(float2 uv)
{
    return SceneDepthTexture.SampleLevel(LinearClampSampler, uv, 0).r;
}

// The main depth buffer is reverse-Z and is cleared to exactly 0. Treating 1 as the empty value
// reconstructs the clear as a far-plane point; on the horizon that point crosses sea level and the
// out-of-field shore fallback mistakes the whole row for a coastline.
bool HasSceneGeometryDepth(float depthSample)
{
    return depthSample > 0.0f;
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

// Bilinear done BY HAND, because the hardware's is not smooth enough for this field.
//
// D3D only guarantees 8 bits of subtexel precision in the texture address, so a hardware-filtered
// tap is not a ramp between texels — it is a 256-step staircase across each one. On the shore map
// that texel is about a metre wide, which puts a step every few millimetres of ground. Anywhere
// else that is invisible; at the waterline it is not, because the camera stands half a metre above
// the water and looks along it, so a few millimetres of beach cover something like ten screen
// pixels. Every contour of the depth field then comes out as a regular sawtooth, and the contact
// foam - whose whole shape is one contour of this field, authored millimetres wide - rides it. That
// is the rectangular staircase along the foam edge.
//
// Gather returns the four texels untouched and the weights are computed here in float, so the ramp
// is exact. The gather is taken at the CENTRE of the 2x2 block (half a texel from any boundary),
// which is 128 quantization steps of margin - the hardware cannot pick a different footprint than
// the one floor() picked. Same cost as the Sample it replaces.
float SampleShoreDepth(float2 uv)
{
    const float2 texelUV = max(shoreSamplingParams.xy, float2(1e-6f, 1e-6f));
    const float2 st = uv / texelUV - 0.5f;
    const float2 texelIndex = floor(st);
    const float2 w = st - texelIndex;
    // GatherRed order is (0,1) (1,1) (1,0) (0,0) relative to the block's upper-left texel.
    const float4 taps = ShoreDepthTexture.GatherRed(
        LinearClampSampler, (texelIndex + 1.0f) * texelUV);
    const float top = lerp(taps.w, taps.z, w.x);
    const float bottom = lerp(taps.x, taps.y, w.x);
    return lerp(top, bottom, w.y);
}

float2 ShoreDepthUV(float2 baseXZ)
{
    float2 offsetXZ = baseXZ - shoreViewParams.xy;
    float invExtent = shoreViewParams.w;
    return float2(offsetXZ.x * invExtent + 0.5f, 0.5f - offsetXZ.y * invExtent);
}

float2 ShoreSdfUV(float2 baseXZ)
{
    float2 offsetXZ = baseXZ - shoreSdfParams.xy;
    float invExtent = shoreSdfParams.z;
    return float2(offsetXZ.x * invExtent + 0.5f, 0.5f - offsetXZ.y * invExtent);
}

// x: metres to the waterline, negative inland. y: water depth (the bed's height below sea level).
// Off the edge of the map it reports open ocean — the map covers the level, so anything outside it
// has neither a shore nor a bottom in reach.
float2 SampleShoreField(float2 baseXZ)
{
    const float2 uv = ShoreSdfUV(baseXZ);
    if (any(uv < 0.0f) || any(uv > 1.0f))
    {
        return float2(10000.0f, 1000.0f);
    }
    const float2 field = ShoreSdfTexture.SampleLevel(LinearClampSampler, uv, 0).rg;
    return float2(field.x, -field.y);
}

float ShoreViewDepth(float depthSample)
{
    return lerp(shoreDepthParams.x, shoreDepthParams.y, depthSample);
}

struct ShoreData
{
    float waterDepth;
    float2 depthGradient;
    float fieldWeight;
};

float ShoreWaterDepth(float2 shoreUV)
{
    return ShoreViewDepth(SampleShoreDepth(shoreUV)) - shoreViewParams.z;
}

float ShoreFieldWeight(float2 shoreUV)
{
    float2 texelUV = max(shoreSamplingParams.xy, float2(1e-6f, 1e-6f));
    float2 edgeDistanceUV = min(shoreUV, 1.0f - shoreUV);
    float2 edgeDistanceTexels = edgeDistanceUV / texelUV;
    float nearestEdgeTexels = min(edgeDistanceTexels.x, edgeDistanceTexels.y);
    return smoothstep(2.0f, 12.0f, nearestEdgeTexels);
}

float ShoreEffectDepthWeight(float waterDepth)
{
    return 1.0f - smoothstep(4.0f, 5.0f, max(waterDepth, 0.0f));
}

ShoreData GetShoreData(float2 worldXZ)
{
    ShoreData shore;
    shore.waterDepth = 1000.0f;
    shore.depthGradient = float2(0.0f, 0.0f);
    shore.fieldWeight = 0.0f;

    float2 texelUV = max(shoreSamplingParams.xy, float2(1e-6f, 1e-6f));

    float2 shoreUV = ShoreDepthUV(worldXZ);
    if (all(shoreUV >= texelUV) && all(shoreUV <= 1.0f - texelUV))
    {
        float centerDepth = ShoreWaterDepth(shoreUV);

        // Gradient over a WIDE baseline, centred on the vertex — Run-up Slope Smoothing (texels).
        //
        // It used to be a forward difference to the NEXT texel, and single-texel differences of
        // this map are noise: the run-up slope gate compares length(gradient) against a threshold,
        // so that noise became a frozen zigzag waterline — metre-scale teeth wherever the beach's
        // slope straddled the gate. The wider centred difference reads the slope of the BEACH, not
        // of one texel; the teeth become bays and tongues. Everything downstream benefits the same
        // way: the shoreward push direction stops flipping texel to texel, and the depth advection
        // (predictedDepth) stops amplifying single-texel steps. Costs two extra taps per vertex.
        //
        // The baseline is a slider because it IS the tooth-count control: the number of teeth is
        // the number of times the slope field crosses the gate window along the beach, and this
        // sets which spatial frequency of the seabed that field still contains.
        const float kGradientBaselineTexels = max(shoreSwashParams.y, 0.5f);
        float xPos = ShoreWaterDepth(shoreUV + float2(texelUV.x * kGradientBaselineTexels, 0.0f));
        float xNeg = ShoreWaterDepth(shoreUV - float2(texelUV.x * kGradientBaselineTexels, 0.0f));
        // Shore UV runs opposite world Z.
        float zPos = ShoreWaterDepth(shoreUV - float2(0.0f, texelUV.y * kGradientBaselineTexels));
        float zNeg = ShoreWaterDepth(shoreUV + float2(0.0f, texelUV.y * kGradientBaselineTexels));
        float2 texelWorld = max(shoreSamplingParams.zw, float2(1e-3f, 1e-3f));

        shore.waterDepth = centerDepth;
        shore.fieldWeight =
            ShoreFieldWeight(shoreUV) *
            ShoreEffectDepthWeight(centerDepth);
        const float invBaseline = 0.5f / kGradientBaselineTexels;
        shore.depthGradient = float2(
            (xPos - xNeg) * invBaseline / texelWorld.x,
            (zPos - zNeg) * invBaseline / texelWorld.y);

    }
    return shore;
}

// How much of the wave's VERTICAL motion survives at this vertex.
//
// Keyed on PROXIMITY TO LAND, never on distance to the camera. Damping by camera distance is what  
// flattens the open ocean and eats detail that should still be there kilometres out — the sea does
// not care where the viewer is. Deep water reads 1 at any range.
//
//   inside the near window  -> 1; the existing fade / run-up / sink own the vertical there
//   inside the far cascade  -> settles as the bed rises, flat over land
//   beyond both             -> 1, i.e. open ocean. A shore out there is past the far cascade's
//                              reach, and at that range it is sub-pixel anyway.
//
// `margin` is the conservative radius the query is widened by. A distant clipmap quad is tens of
// metres across, so asking about one corner lets an island slip between samples; taking the minimum
// over a cross of that radius makes LAND win, which is the safe direction to be wrong in.
#if OCEAN_VS_DEPTH_PROBE
// A/B ALTERNATIVE to ShoreVerticalDamp, compiled only under --ocean-vs-depth-probe. Same job —
// quiet the wave near land outside the shore-depth window — but asking the SCREEN-SPACE depth
// buffer instead of the world-space SDF.
//
// Kept so the two can be compared directly. Its failure modes are structural, not tuning:
//   * a vertex projects to a pixel that, at a grazing angle, shows a DIFFERENT surface than the
//     one under the vertex, so water gets pushed down along beaches it is nowhere near;
//   * a vertex off-screen has no probe at all, so the damping pops as the camera turns;
//   * the buffer holds palms, props and anything else drawn, not just terrain;
//   * the depth GAP goes to zero at a grazing angle over deep water, so looking along the surface
//     damps the open sea. That is inherent to comparing along the view ray rather than in world
//     space, and it is the clearest thing to watch for against the SDF.
float ShoreVerticalDampFromDepthBuffer(float3 probeWorld, float nearFieldWeight)
{
    const float near01 = saturate(nearFieldWeight);
    [branch]
    if (near01 >= 1.0f - 1e-3f)
    {
        return 1.0f;
    }

    const float4 probeClip = mul(float4(probeWorld, 1.0f), viewProj);
    [branch]
    if (probeClip.w <= 1e-3f)
    {
        return 1.0f;
    }

    const float2 probeNdc = probeClip.xy / probeClip.w;
    const float2 probeUv = float2(probeNdc.x * 0.5f + 0.5f, 0.5f - probeNdc.y * 0.5f);
    [branch]
    if (any(probeUv != saturate(probeUv)))
    {
        return 1.0f; // off-screen: no data, hence no damping. This is the interesting failure.
    }

    const float solidDepth = SceneDepthTexture.SampleLevel(PointClampSampler, probeUv, 0).r;
    [branch]
    if (!HasSceneGeometryDepth(solidDepth))
    {
        return 1.0f;
    }

    // Plain gap between the two depths — no world-space reconstruction, no height. probeClip.w IS
    // the vertex's view-space z under a standard perspective projection, so nothing else is needed.
    //
    //   gap  > 0 : solid geometry sits BEHIND the water; the more of it, the freer the wave
    //   gap ~= 0 : the two surfaces are on top of each other, so hold the wave down
    //   gap  < 0 : the water is behind the geometry, hidden, and damping it costs nothing
    //
    // Metres of VIEW depth; tune by editing this line and restarting.
    const float kProbeGapRange = 1.0f;
    const float gap = DepthToViewZ_Fast(solidDepth) - probeClip.w;
    const float settle = smoothstep(0.0f, kProbeGapRange, gap);
//return 1;

    return lerp(settle, 1.0f, near01);
}
#endif

float ShoreVerticalDamp(float2 field, float margin, float nearFieldWeight)
{
    const float near01 = saturate(nearFieldWeight);
    [branch]
    if (near01 >= 1.0f - 1e-3f)
    {
        return 1.0f;
    }

    // Two answers from the one field sample, and BOTH are needed.
    //
    //   distance — off a steep bank the water is deep right against the rock, so a depth test
    //              alone reads "open ocean" there and lets the swell hit the wall at full height.
    //   depth    — the middle of a wide lagoon is far from every shore and still knee deep, so a
    //              distance test alone leaves a full swell running over the shallows.
    //
    // Whichever says "settle down" wins.
    const float fadeDepth = max(shoreBehaviorParams0.x, 0.01f);

    // The margin absorbs the two ways this vertex does not represent a point: its quad spans up to
    // half a diagonal around it, and choppiness slides the surface further still.
    const float fadeWidth = max(fadeDepth * 8.0f, margin);
    const float distanceSettle = smoothstep(margin, margin + fadeWidth, field.x);
    const float depthSettle = smoothstep(0.0f, fadeDepth, field.y);
    // MEASURED on the atoll: with beaches this gentle, depth alone gives all but the same picture —
    // the distance term earns its keep on STEEP shores (deep water hard against a wall, where depth
    // says "open ocean") and, more concretely, by feeding the sink an inland distance that used to
    // be a division by a noisy gradient. Cheap enough to keep for the case the content grows one.
    const float settle = min(distanceSettle, depthSettle);
    return lerp(settle, 1.0f, near01);
}

float2 ShorewardDirection(float2 depthGradient)
{
    float lengthSquared = dot(depthGradient, depthGradient);
    return lengthSquared > 1e-8f
        ? -depthGradient * rsqrt(lengthSquared)
        : float2(0.0f, 0.0f);
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

// World size of one grid cell at this vertex's clipmap level. Needed because a distant quad can be
// tens of metres across: whatever decides "is there land here" has to answer for the whole quad,
// not for the point at its corner.
float ClipMapCellSize(float3 positionOS)
{
    return max(positionOS.y * clipMapParams.x, 1e-3f);
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

float4 SampleDerivativesCascade(float2 worldXZ, uint cascade, float mipBias)
{
    float lengthScale = max(cascadeLengthScales[cascade], 1e-3f);
    float3 uvw = float3(worldXZ / lengthScale, cascade * 2.0f + 1.0f);
    float4 sample = DisplacementDerivatives.SampleBias(AnisotropicWrapSampler, uvw, mipBias);
    return sample;
}

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

DerivativesSet SampleDerivatives(float2 worldXZ, float4 weights, uint cascadesCount, float mipBias)
{
    DerivativesSet derivatives;
    [unroll]
    for (uint cascade = 0; cascade < 4; ++cascade)
    {
        derivatives.cascades[cascade] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    [unroll]
    for (uint cascade = 0; cascade < 4; ++cascade)
    {
        if (cascade >= cascadesCount)
        {
            break;
        }

        float w = weights[cascade];
        if (w > kLodThreshold)
        {
            derivatives.cascades[cascade] = SampleDerivativesCascade(worldXZ, cascade, mipBias) * w;
        }
    }
    return derivatives;
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

// Wind scale for the whole contact strip: 0 = no foam at all, 1 = the tuned widths.
//
// `.y` is the wind force BELOW WHICH there is no contact foam, and `.z` the force at which it
// reaches full strength — a ramp between two wind speeds.
//
// It used to be `lerp(calmAmount, 1, smoothstep(0, fullWind, windForce))`, i.e. `.y` was the
// multiplier AT ZERO WIND — a floor. That made foam impossible to remove with wind: at calm 0.1 the
// widths never fell below 10% however still the water got. The value at `.z` is unchanged by this
// rewrite (both forms return exactly 1 there), so a setup tuned at full wind looks identical.
float ContactFoamWindAmount()
{
    const float windForce = saturate(shoreFoamWindParams.x);
    const float calmWind = saturate(shoreFoamWindParams.y);
    // Guard the degenerate ordering: smoothstep needs edge0 < edge1.
    const float fullWind = max(shoreFoamWindParams.z, calmWind + 1e-3f);
    // LINEAR between the two, not smoothstep: the slider should read as "how much foam", so half
    // way between the thresholds must give half the widths. smoothstep's S-curve made the middle
    // of the range behave nothing like the number on screen.
    return saturate((windForce - calmWind) / (fullWind - calmWind));
}

[RootSignature(OCEAN_SURFACE_RS)]
VSOutput VSMain(VSInput input)
{
    VSOutput output;

    uint cascadesCount = max((uint)simulationParams.w, 1u);

    float3 baseWorld = ClipMapVertex(input.position.xyz, input.uv);
    //float3 prevBaseWorld = ClipMapVertexPrev(input.position.xyz, input.uv);

    ShoreData shore = GetShoreData(baseWorld.xz);
    float2 shoreDirection = ShorewardDirection(shore.depthGradient);
    // One SDF read per vertex, shared by the wave damping and the sink below.
    const float2 shoreField = SampleShoreField(baseWorld.xz);

    float2 worldUV = baseWorld.xz;
    //float2 prevWorldUV = prevBaseWorld.xz;

    float3 viewVector = baseWorld - clipMapViewer.xyz;
    //float3 prevViewVector = prevBaseWorld - prevClipMapViewer.xyz;
    float viewDist = length(viewVector);
    //float prevViewDist = length(prevViewVector);
    float viewDistXzSquared = dot(viewVector.xz, viewVector.xz);
    //float prevViewDistXzSquared = dot(prevViewVector.xz, prevViewVector.xz);

    float warpDistance = max(cascadeLengthScales.x, 1.0f) * 0.5f;
    worldUV = ApplyClipMapWarp(worldUV, viewDistXzSquared, warpDistance);
    //prevWorldUV = ApplyClipMapWarp(prevWorldUV, prevViewDistXzSquared, warpDistance);

    float4 weights = LodWeights(viewDist, clipMapParams.w);
    //float4 prevWeights = LodWeights(prevViewDist, prevClipMapParams.w);

    float3 displacement = SampleCurrentDisplacement(worldUV, weights, cascadesCount);
    //float3 prevDisplacement = SamplePreviousDisplacement(prevWorldUV, prevWeights, cascadesCount);
    float geometryFadeDistance = max(shoreSlopeParams.w, 1.0f);
    float geometryWaveWeight = 1.0f - smoothstep(
        geometryFadeDistance * 0.65f,
        geometryFadeDistance,
        viewDist);

    float waterDepth = shore.waterDepth;
    float shoreFieldWeight = shore.fieldWeight;
    // Distance damping of the wave, and it applies to the VERTICAL component ONLY, EVERYWHERE.
    //
    // Two changes from what this used to be, both deliberate:
    //
    // 1. Y only. It used to scale the whole displacement vector, and that is what made the contact
    //    strip die off with distance: the strip's depth is predictedDepth = waterDepth +
    //    dot(gradient, horizontalDisplacement), so damping XZ damped the foam along with the
    //    geometry. Only Y can lift a quad through a beach — XZ slides the surface sideways at sea
    //    level, where the depth test keeps it under the sand. So the wave's SHAPE keeps feeding the
    //    foam at any distance while its Y motion is taken out. The foam is off the geometry now.
    //
    // 2. Still weighted by shoreFieldWeight, i.e. it only acts near a KNOWN shore. Far open water
    //    must keep its Y motion — flattening everything past the fade distance turned the horizon
    //    into a mirror with a few stray swells on it. Out-of-range shores are handled below,
    //    against the depth buffer, instead of by killing the whole ocean's vertical motion.
    //
    // Near the camera the weight is 1 and nothing changes.
    displacement.y *= lerp(1.0f, geometryWaveWeight, saturate(shoreFieldWeight));

    // Second term, and the one that keeps a distant island intact: damping by PROXIMITY TO LAND.
    //
    // The query is widened by half a quad's diagonal plus the horizontal displacement, because a
    // far clipmap quad is tens of metres across and its choppiness drags the corners further still
    // — asking only about the vertex's own position lets an island slip between samples and the
    // wave saws through the beach. Widening makes land win, which is the safe way to be wrong.
    const float cellSize = ClipMapCellSize(input.position.xyz);
    const float shoreQueryMargin = cellSize * 0.7071f + length(displacement.xz);
#if OCEAN_VS_DEPTH_PROBE
    // A/B: the screen-space probe REPLACES the SDF here, so a run with and without the flag differs
    // in exactly one thing — where the "is there land" answer came from.
    displacement.y *= ShoreVerticalDampFromDepthBuffer(
        float3(baseWorld.x + displacement.x, displacement.y, baseWorld.z + displacement.z),
        shoreFieldWeight);
#else
    displacement.y *= ShoreVerticalDamp(shoreField, shoreQueryMargin, shoreFieldWeight);
#endif
    float positiveDepth = max(waterDepth, 0.0f);
    // Water is never completely still.
    //
    // Floor under the VERTICAL fade only — the horizontal one is floored by Shallow XZ Strength
    // itself (see below), which is what that slider means.
    //
    // Right at the waterline the vertical fade goes to zero and those vertices stop dead. Frozen
    // water beside a wall reads as glass, which is worse than slightly wrong water.
    //
    // Deliberately a constant and not a slider: shaders compile at runtime here, so it can be
    // tuned by editing this line and restarting. Say the word and it becomes a proper parameter.
    const float kShoreMinMotion = 0.15f;

    float shoreVerticalFade = max(smoothstep(
        0.0f,
        max(shoreBehaviorParams0.x, 0.01f),
        positiveDepth), kShoreMinMotion);
    float terrainSlope = length(shore.depthGradient);
    // WIND ENTERS THE RUN-UP HERE, through the slope thresholds.
    //
    // As the wind falls toward the calm threshold the swell has less appetite for climbing, so the
    // beach has to be flatter before the sheet will run up it — the thresholds tighten and the
    // sheet stops reaching. That is the honest place for wind to act: it changes how far the water
    // climbs, not how the shoreline is shaped.
    //
    // It used to be done by scaling BOTTOM CLEARANCE with wind instead, which shrank the run-up
    // sheet's standing height so the foam sank with it. That worked by side effect and fought the
    // clearance's other job (the floor keeping water out of the seabed), so it is gone.
    //
    // At the calm end the thresholds are 0.9 of the authored values — a light touch on purpose,
    // since the widths already scale with wind and this must not double up on them.
    //
    // 0.9 IS LOAD-BEARING for Bottom Clearance. This was hand-tuned down to 0.1 at some point,
    // and that quietly killed the clearance slider at low wind: 0.1 shrinks the slope window
    // tenfold (start 1 degree reads as 0.1), no real beach passes, the run-up sheet — the only
    // thing the clearance's standing-height job feeds — stops existing, and the slider goes dead
    // (measured: a 5x clearance change moved 0.09% of the frame at wind 0.25). If calm needs to
    // suppress the shore HARDER than the width scaling already does, that wants its own knob, not
    // this one — anything below ~0.5 here disconnects a slider that gives no hint why.
    const float kRunupSlopeCalmScale = 0.1f;

    // THE SWASH WAVE IS SAMPLED AT THE WATERLINE, NOT AT THIS VERTEX.
    //
    // Driving the run-up from the wave at the vertex itself made the wet edge a map of the local
    // FFT field: a short-crested sea lifts one PATCH of beach for a moment, so the line mostly sat
    // still and occasionally spat a narrow tongue where a crest happened to land — a flame lick
    // that detached and vanished, not a wave arriving. Static captures looked fine; in motion it
    // read as exactly that.
    //
    // A real swash is the wave AT THE WATERLINE lending the sheet its energy, and the wet edge
    // follows it IN PHASE all the way up the beach. So every vertex asks for the wave at its own
    // nearest point of the waterline — the SDF gives the distance inland, the smoothed depth
    // gradient the direction — and the whole profile from the sea to the sheet's tip breathes as
    // one body: it runs up, it pulls back. Costs one displacement fetch, paid only inside the
    // shore field; in open water the anchor degenerates to the vertex itself, so the field is
    // continuous across the waterline.
    //
    // Clamped by Run-up Max Wave — this same value also drives the shoreward push below, so that
    // slider caps the swash driver as a whole.
    float runupWave = 0.0f;
    [branch]
    if (shoreFieldWeight > 1e-3f)
    {
        const float2 waterlineAnchor =
            baseWorld.xz - shoreDirection * max(-shoreField.x, 0.0f);
        // THE SHORE'S WAVE DRIVE STOPS GROWING AT "FULL AT WIND".
        //
        // Every other nearshore term saturates there (ContactFoamWindAmount caps the gate, the
        // reach, the foam widths), but this is the raw wave in metres and the FFT keeps growing
        // all the way to wind 1 — so past full the push (Run-up Strength times this) and
        // everything advected by it kept inflating into absurd surf. The ratio of reference wave
        // heights (the one full-at-wind WOULD give over the one the sea actually has) rescales
        // the anchored wave back to its full-at-wind size; at or below full the ratio clamps to
        // one and nothing changes. The open sea is untouched — this only shrinks what the RUN-UP
        // is allowed to feel.
        const float shoreWaveScale = min(
            1.0f, max(shoreSwashParams.z, 1e-3f) / max(windParams1.z, 1e-3f));
        const float swashWave =
            SampleCurrentDisplacement(waterlineAnchor, weights, cascadesCount).y *
            geometryWaveWeight * shoreWaveScale;
        runupWave = clamp(
            swashWave,
            -max(shoreBehaviorParams1.z, 0.0f),
            max(shoreBehaviorParams1.z, 0.0f));
    }

    // THE SWASH IS A MATERIAL MOTION, NOT A MASK.
    //
    // Two earlier shapes of this both animated a THRESHOLD over standing water — first the sheet's
    // height, then its reach — and both read the same on screen: a shutter opening and closing
    // over water that never went anywhere. The water has to MOVE: the splash-zone vertices shuttle
    // horizontally along the shore direction with the wave phase, like cloth pulled up the beach
    // and back, and the foam and ripples ride along because the geometry itself is what travels.
    //
    // The travel is signed metres of ground: a crest carries the whole zone up the beach, the
    // drain pulls it back past its base position. No rectification and no rest pose — the sea
    // never stops, so the wet edge never dwells anywhere. The phase is the anchored wave, so one
    // profile of beach moves as one body; the slope gate and shore band weight it exactly like
    // the wave-height push it joins below.
    //
    // NORMALISED PHASE, CONFINED TO A LOW-WIND WINDOW. Both halves are load-bearing, and each was
    // once shipped alone and failed:
    //
    //   - Normalised alone (wave / reference height, a plus-minus-one signal at any sea): the
    //     excursion detached from the weather. A storm stacked metres of travel on top of a push
    //     that is already metres — the swash visibly AMPLIFIED the waves — and a dead-calm ripple
    //     swung the line as far as a gale, stretching the near-shore quads into long smeared-
    //     specular ribbons.
    //   - Proportional to the wave in metres alone: the storm end tamed itself, but the low-wind
    //     end starved — centimetres of wave times any sane gain is nothing, and measured at wind
    //     0.3 the whole term sat at run-noise level. That is precisely the range this device
    //     exists for: the authored push is wave-height too, so light weather leaves BOTH dead.
    //
    // So: the normalised phase gives light seas a real excursion, and the window hands the shore
    // back to the push before the sea is big enough to speak for itself — ramping in above the
    // calm threshold (below it there is no surf at all) and fading out QUADRATICALLY toward full
    // wind. The result peaks in the low-to-mid band and is gone at both extremes.
    const float kSwashTravelMeters = 3.0f;
    const float swashPhase = clamp(runupWave / max(windParams1.z, 1e-2f), -1.0f, 1.0f);
    const float swashWindAmount = ContactFoamWindAmount();
    const float swashWindow =
        saturate(swashWindAmount * 4.0f) *
        (1.0f - swashWindAmount) * (1.0f - swashWindAmount);
    const float swashTravel =
        swashPhase * saturate(shoreSwashParams.x) * kSwashTravelMeters * swashWindow;

    // How far inland this vertex is, in MATERIAL coordinates (undisplaced baseXZ) — computed here
    // because the wet-edge floor below needs it, and reused by the reach fade and the sheet's
    // inland die-off. It needs BOTH sources.
    //
    // The SDF is the accurate one in metres, but it is 1.95 m per texel against the near depth
    // map's 0.98 m, so within a texel of the waterline the two disagree: the near map (which the
    // FOAM reads) says land while the SDF still says water. In that gap nothing pushed the sheet
    // down — it stood proud of the beach with solid foam on it, since a negative depth reads as
    // fully inside the strip. Flooring the SDF with an estimate from the near map closes it.
    //
    // The estimate divides by a FIXED slope, never the measured gradient: dividing by a difference
    // of neighbouring texels is what used to send this to infinity and tear the mesh into spikes.
    const float kInlandAssumedSlope = 0.05f;
    const float inlandFadeDistance = max(max(-shoreField.x, 0.0f),
                                         max(-waterDepth, 0.0f) / kInlandAssumedSlope);

    // Wind enters the gate as a static scale: a calm sea only climbs a gentler beach.
    const float runupSlopeGate = lerp(kRunupSlopeCalmScale, 1.0f, ContactFoamWindAmount());
    float runupSlopeWeight = 1.0f - smoothstep(
        shoreSlopeParams.x * runupSlopeGate,
        max(shoreSlopeParams.y * runupSlopeGate,
            shoreSlopeParams.x * runupSlopeGate + 1e-4f),
        terrainSlope);
    runupSlopeWeight *= shoreFieldWeight * geometryWaveWeight;

    // NO wet-edge floor here, and that is a measured decision, twice over.
    //
    // A waterline-hugging strip where the sheet stands regardless of the gate was tried (to give
    // Bottom Clearance a consumer at calm) and it drew a JAGGED WHITE FENCE: a clearance-tall step
    // of water at the waterline reads as a wall at a grazing angle, and the strip's sub-metre
    // boundary rides the SDF's 1.95 m texels, so its edge was sawtooth noise. If the calm
    // shoreline ever needs a standing film, it has to be millimetres thick and cut by something
    // finer than the SDF.
    float horizontalDepthWeight = smoothstep(
        0.0f,
        max(shoreBehaviorParams0.z, 0.01f),
        positiveDepth);
    // The floor here IS Shallow XZ Strength, not a hidden constant.
    //
    // A floor is needed because `runupSlopeWeight` goes to zero on a steep face, and multiplying by
    // it alone stops the water dead — frozen water beside a wall reads as glass, which is worse
    // than slightly wrong water. But using a constant for it silently overrode the slider: with
    // kShoreMinMotion at 0.15, every authored value below that behaved identically and the first
    // fifth of the slider's travel did nothing. The slider already means "how much lateral motion
    // survives in the shallows", so it is exactly the right number to floor with — and now 0 really
    // does still the water at the waterline while 1 leaves the chop untouched.
    const float shallowXzStrength = saturate(shoreBehaviorParams0.y);
    float shoreHorizontalFade = max(lerp(
        shallowXzStrength * runupSlopeWeight,
        1.0f,
        horizontalDepthWeight), shallowXzStrength);
    float horizontalFade = lerp(1.0f, shoreHorizontalFade, shoreFieldWeight);
    float shoreBand = 1.0f -
        smoothstep(0.0f, max(shoreBehaviorParams1.x, 0.01f), positiveDepth);
    shoreBand *= shoreFieldWeight;

    float2 horizontalDisplacement = displacement.xz * horizontalFade;
    // The shoreward shuttle: the authored wave-height push (Run-up Strength) plus the swash
    // travel, one coherent motion. The cut criteria downstream (reach fade, sheet height) read
    // the UNDISPLACED baseXZ, so they are material coordinates: the water's edge is carried by
    // this displacement instead of the water sliding through a world-anchored edge.
    float2 runupPush =
        shoreDirection *
        (runupWave * max(shoreBehaviorParams1.y, 0.0f) + swashTravel) *
        shoreBand * runupSlopeWeight;
    // THE PUSH STOPS AT GROUND THE SHEET CANNOT CLIMB.
    //
    // At storm strength the push is metres, and on a steep face it carried water vertices INTO
    // the hillside: the landing point's ground stands far above anything the sheet can stand on,
    // and the crest poked out of the dune as a white shred. The advected depth cannot catch this
    // — it is a first-order extrapolation clamped to +-1 m of depth shift, blind exactly on the
    // steep face where it matters. So the landing is checked with a REAL tap of the shore map at
    // the displaced position, and the push fades out as the ground there rises past what the
    // run-up sheet could legitimately climb. A gentle beach never triggers it (metres of push
    // gain centimetres of ground); a cliff stops the wave at its foot, which is what a cliff does.
    [branch]
    if (dot(runupPush, runupPush) > 1e-6f)
    {
        const float landingDepth = ShoreWaterDepth(
            ShoreDepthUV(baseWorld.xz + horizontalDisplacement + runupPush));
        const float maxClimb = max(shoreBehaviorParams1.w, 0.0f) + 0.3f;
        const float kClimbFade = 0.5f;
        runupPush *= 1.0f - smoothstep(maxClimb, maxClimb + kClimbFade, -landingDepth);
    }
    horizontalDisplacement += runupPush;

    // The ground under the vertex is read AT THE DISPLACED POSITION, with a real tap.
    //
    // It used to be a first-order extrapolation (depth at base plus gradient times step, clamped
    // to +-1 m), and every vertical decision below — the seabed floor, the sheet height, the foam
    // advection — was made as if the vertex still stood at its material base. On a gentle beach
    // the extrapolation is nearly exact; on a steep face it is blind by construction, so a vertex
    // carried metres by the chop and the push kept the height of the water it CAME from and poked
    // out of the hillside as a white shred. Asking the map where the vertex actually LANDS makes
    // the whole vertical stack agree with the ground it is drawn over; outside the shore field the
    // old clamped extrapolation stays as the fallback (there is no map to ask there, and its +-1 m
    // clamp is what kept an underwater scarp from painting a foam ribbon across open water).
    float predictedDepth = waterDepth + clamp(
        dot(shore.depthGradient, horizontalDisplacement), -1.0f, 1.0f);
    [branch]
    if (shoreFieldWeight > 1e-3f && dot(horizontalDisplacement, horizontalDisplacement) > 1e-6f)
    {
        predictedDepth = ShoreWaterDepth(
            ShoreDepthUV(baseWorld.xz + horizontalDisplacement));
    }

    float shoreVerticalDisplacement = displacement.y * shoreVerticalFade;
    // Bottom clearance has TWO jobs and NEITHER follows the wind any more.
    //
    // As a floor under the surface it is a geometric guard keeping the water out of the seabed.
    // As the height the run-up sheet stands above the waterline it is a reach onto dry sand.
    // Both are properties of the SHORE, not of the weather, so both stay at what was authored.
    //
    // It used to be scaled by wind so that the sheet — and the foam sitting on it — faded out as
    // the wind approached the calm threshold. That is now done where it belongs, by tightening the
    // run-up SLOPE thresholds (see runupSlopeGate above): a calm sea climbs a gentler beach.
    const float bottomClearance = max(shoreBehaviorParams1.w, 0.0f);
    // The sheet's standing height is STATIC — the swash moves its FRONT, not its altitude.
    //
    // Scaling the clearance with the wave phase was the previous shape of the swash and it drained
    // WRONG: near the shore the raised sheet is nearly parallel to the sand, so lowering it drops
    // the whole surface THROUGH the beach at once instead of pulling the wet edge back — the water
    // vanished in place, and what remained was the bare still-water isoline cutting the terrain in
    // a dead-straight, dead-static line under the edge fade. A sheet that always stands a full
    // clearance above the ground keeps a crisp intersection at any phase; what advances and
    // retreats is how far inland that sheet extends (the reach fade below).
    const float runupClearance = bottomClearance;

    float bottomLimit = -max(predictedDepth - bottomClearance, 0.0f);
    shoreVerticalDisplacement = max(shoreVerticalDisplacement, bottomLimit);
    // The sheet must DIE OFF INLAND, and nothing above did that.
    //
    // shoreBand fades it by DEPTH, but on dry land the depth term is pinned at zero, so the fade
    // never engaged: `-predictedDepth` grows as the ground rises, the sheet climbed with it, and
    // what was left standing on the beach was a raised blister of water — covered in foam, because
    // a negative depth reads as solidly inside the strip. `inlandFadeDistance` (computed above,
    // next to the wet edge) is what cuts it.

    // The sheet's extent in MATERIAL coordinates — deliberately constant.
    //
    // `inlandFadeDistance` is measured at the UNDISPLACED baseXZ, so this cut travels with the
    // shuttle above: the vertex that carries the water's edge is decided once, in the water's own
    // frame, and wherever the swash slides it, the edge goes along. Animating this threshold
    // instead (the reach, earlier the height) was the mask mistake — a world-anchored edge with
    // water sliding through it.
    //
    // The fade is a PLATEAU WITH A CLIFF, not a straight ramp. The ramp thinned the sheet all the
    // way from the waterline, so its far end approached the sand at a grazing angle and the
    // intersection smeared into metres of near-coplanar mush; the plateau holds the full standing
    // height and drops over the last half of the reach, so the water/sand cut stays crisp.
    //
    // THE WIND LIVES HERE, in the reach — not in the slope gate. Choking the gate at calm (the
    // 0.1 scale this shader carried for a while) removes the sheet everywhere, and with it every
    // visible effect of Bottom Clearance; leaving the gate open with a full reach parks a
    // foam-covered 3 m sheet on a dead-calm beach. What calm actually does is shorten how far the
    // water gets: full storm keeps the whole runway, calm keeps a narrow wet hem at the edge whose
    // thickness the clearance still authors — so the slider works in any weather.
    const float kRunupReach = 3.0f;
    const float kCalmReachScale = 0.15f;
    const float runupReach =
        kRunupReach * lerp(kCalmReachScale, 1.0f, ContactFoamWindAmount());
    const float runupReachFade =
        1.0f - smoothstep(runupReach * 0.55f, runupReach, inlandFadeDistance);
    // The sheet DIES on ground it could not have climbed. With the honest displaced-ground depth
    // above, a vertex carried into a hillside reports metres of negative depth, and without this
    // kill the sheet formula would happily stand clearance above THAT — a wall of water up the
    // slope. Same ceiling the push limiter uses: a little above the standing clearance.
    const float maxSheetClimb = runupClearance + 0.3f;
    const float sheetClimbKill =
        1.0f - smoothstep(maxSheetClimb, maxSheetClimb + 0.5f, -predictedDepth);
    float runupSheetHeight =
        max(-predictedDepth + runupClearance, 0.0f) *
        shoreBand * runupSlopeWeight * runupReachFade * sheetClimbKill;
    shoreVerticalDisplacement = max(shoreVerticalDisplacement, runupSheetHeight);

    float verticalDisplacement = lerp(
        displacement.y,
        shoreVerticalDisplacement,
        shoreFieldWeight);

#if OCEAN_SHORE_SINK
    // Dry land: instead of letting the pixel shader discard this, tuck it UNDER the terrain and let
    // the depth test draw the waterline — per pixel, which is what the fwidth-based clip bought,
    // and without the discard that cost the whole draw its early-Z.
    //
    // The sheet dips at a FIXED ANGLE past the waterline and then settles onto a shelf. It does not
    // follow the terrain: mirroring it meant the water dived as hard as the land climbed, which is
    // both odd to look at and pointless, since a steep face already hides whatever is behind it.
    // What actually needs burying is the long shallow stretch, where sand and water are nearly
    // coplanar and neither wins the depth test cleanly.
    //
    // How far inland this vertex is comes straight from the SDF — that is the quantity it stores.
    //
    // It used to be ESTIMATED as depth/slope, and that estimate was the shakiest arithmetic in this
    // shader: the gradient is a difference of neighbouring texels, so it is noisy and can come out
    // near zero, and dividing by it sent the distance to infinity for individual vertices and tore
    // the mesh into spikes. A floor of 0.05 on the slope held that together and quietly distorted
    // the dip on every gentle beach, which is exactly where the sheet needs burying most.
    {
        const float kSinkSlope = 0.05f;     // tangent of the dip angle (0.5 = about 27 degrees)
        const float kSinkMaxDepth = 2.0f;  // the shelf the sheet settles onto, metres below water

        const float kSinkFlatten = 2.0f;  // metres inland over which the buried sheet goes still

        // Distance past the waterline, straight from the SDF and NOTHING ELSE.
        //
        // Specifically NOT the floored `inlandFadeDistance` the fades use. That floor is derived
        // from depth, so feeding it here would make the dip steepen wherever the terrain climbs
        // faster — the water mirroring the land, which is both odd to look at and pointless, since
        // a steep face already hides whatever is behind it. The dip has to be a FIXED ANGLE walked
        // out from the waterline, and only a horizontal distance can give that.
        const float inlandDistance = max(-shoreField.x, 0.0f);

        // Buried water is still water. Nothing under the terrain is ever seen, so leaving the wave
        // running down there only pays for displacement that can poke back through the sand — and
        // in wireframe it is the sheet thrashing under the beach. Fade the wave out as the sheet
        // goes under and let the dip carry it down as a smooth plane.
        float buried = saturate(inlandDistance / kSinkFlatten);
        verticalDisplacement = lerp(verticalDisplacement, 0.0f, buried);
        horizontalDisplacement *= 1.0f - buried;

        verticalDisplacement -=
            min(inlandDistance * kSinkSlope, kSinkMaxDepth);
    }
#endif

    // Shores beyond the depth window are handled by the SDF above, in world space, before the wave
    // is even built. A screen-space probe of the depth buffer was tried here instead and removed:
    // a vertex projects into a pixel that at a grazing angle shows a different surface, vertices
    // off-screen get nothing at all, and the buffer holds props as
    // well as terrain. Tight enough to stop the leak, it ate the shoreline.
    //prevDisplacement *= attenuation;

    float3 world = float3(
        baseWorld.x + horizontalDisplacement.x,
        verticalDisplacement,
        baseWorld.z + horizontalDisplacement.y);
    //float3 prevWorldPos = float3(prevBaseWorld.x + prevDisplacement.x, prevDisplacement.y, prevBaseWorld.z + prevDisplacement.z);
    float3 prevWorldPos = world;
    output.worldPos = world;

    output.baseXZ = worldUV;
    // `terrainSlope` is already length(shore.depthGradient) from the run-up block above.
    output.shoreData = float4(predictedDepth, waterDepth, shoreFieldWeight, terrainSlope);

    float4 local = float4(world, 1.0f);
    float4 worldH = mul(local, model);
    float4 viewPos = mul(worldH, view);
    output.viewDepth = viewPos.z;
    float4 clipPos = mul(worldH, viewProj);
    output.position = clipPos;
    output.positionNDC = mul(worldH, viewProjNoJitter);
    output.positionNDCJitter = mul(worldH, viewProj);
    float4 prevLocal = float4(prevWorldPos, 1.0f);
    float4 prevWorld = mul(prevLocal, prevModel);
    output.prevPositionNDC = mul(prevWorld, prevViewProjNoJitter);
    return output;
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

// =============================== Contact foam ===============================
//
// The whole strip is ONE dissolve — a single threshold sweep, no cut lines:
//
//   t : position through the strip. Negative inside the solid band, 0 where
//       tearing starts, 1 at the far end of the longest possible tail.
//   n : tear noise in [0,1] (fine octave of the breakup mask).
//   a = smoothstep(t - f, t + f, n)
//
// For t <= -f this is 1 for ANY n — the solid band falls out of the same
// expression, which is what replaces the old hard
// `if (depth <= breakupStart - edgeFeather) return solid` boundary: fingers
// grow continuously out of the solid mass and there is no line to hide. For
// t >= 1 + f it is 0 — the strip ends through the same feather instead of a
// separate terminal envelope. In between, pixels whose noise beats the sweep
// survive as fingers, and every torn edge is `f` wide — the dithered hand-off
// the hard branch used to fake.
//
// The tail LENGTH is modulated by a second, low-frequency octave of the same
// mask before the sweep (long fingers here, short there). That replaces the
// old SampleBias(+3) "breakup length variation" — which is where the stripes
// came from: +3 bias put its 20 m field onto a mip whose texels are ~31 cm in
// world space (bilinear kinks at every texel), and the anisotropic sampler
// stretched those kinks into streaks along the view axis. Here both octaves
// are plain linear+mips, and the macro one is rotated ~37 degrees so the two
// periods never line up — no common tile, no creases, no streaking.
//
// Cost: 2 linear samples + 1 smoothstep (was: 1 aniso Sample, up to 2 aniso
// SampleBias, three smoothsteps and the threshold plumbing between them).
//
// The Signed Depth Warp is back, and it is not optional decoration: it is the
// only thing here that moves the ISOBATH. Tearing acts inside the tail, so at
// low wind — where every width is scaled down — the outer edge collapses onto a
// contour of the seabed, which is smooth and does not move. See its block below.
#if OCEAN_FOAM_DEBUG
// Contact-foam diagnostics. Compiled ONLY into the --ocean-foam-debug variant,
// so the shipping shader carries none of it (a runtime `if` would still cost
// the writes below on every water pixel). The VIEW is picked at runtime from
// shoreFoamBreakupParams.w, so switching views needs no rebuild and no PSO
// churn — only entering/leaving debug does.
//   xyzw = sweep t, feather, tear noise, tail length (metres of depth)
static float4 g_foamDbg = float4(0.0f, 0.0f, 0.0f, 0.0f);
//   xy = final contact mask (alpha, coverage)
static float2 g_foamDbgMask = float2(0.0f, 0.0f);

// Blue -> cyan -> green -> yellow -> red over 0..1, so a staircase in a scalar
// field is obvious in a way a grayscale ramp never is.
float3 DebugHeat(float x)
{
    x = saturate(x);
    return saturate(float3(
        x * 4.0f - 2.0f,
        x < 0.5f ? x * 4.0f : 4.0f - x * 4.0f,
        2.0f - x * 4.0f));
}
#endif

float2 ContactFoamMask(
    float2 baseXZ,
    float shoreDepth,
    float depthFeather,
    float pixelWorldSize,
    float time)
{
    // Softness of every torn edge, in sweep units (0..1 spans the whole tail).
    // Tune by editing + restarting — shaders compile at runtime.
    const float kTearSoftness = 0.10f;

    const float windAmount = ContactFoamWindAmount();
    const float mainWidth = max(shoreFoamGeometryParams.x, 0.0f) * windAmount;
    const float opacity = saturate(shoreFoamGeometryParams.w);
    // Uniform values only — this never diverges within a quad.
    [branch]
    if (mainWidth <= 1e-4f || opacity <= 1e-4f)
    {
        return float2(0.0f, 0.0f);
    }

    float tailBase = min(max(shoreFoamGeometryParams.y, 0.0f) * windAmount, mainWidth);
    const float solidWidth = mainWidth - tailBase;

    // Shared scrolling domain: world XZ drifting along the wind.
    float2 scrollDirection = windParams1.xy;
    const float dirLenSq = dot(scrollDirection, scrollDirection);
    scrollDirection = dirLenSq > 1e-8f ? scrollDirection * rsqrt(dirLenSq)
                                       : float2(1.0f, 0.0f);
    const float2 p = baseXZ - scrollDirection * time * max(shoreFoamPatternParams.z, 0.0f);

    // Macro octave: same texture, rotated, at the variation frequency. Left at the authored scale —
    // it shapes the coastline's rhythm, which stays readable at any range.
    const float2 pMacro = float2(p.x * 0.8f - p.y * 0.6f, p.x * 0.6f + p.y * 0.8f);
    const float macro = ShoreFoamBreakupMaskTex.Sample(LinearWrapSampler,
        pMacro * max(shoreFoamBreakupParams.y, 1e-4f)).r;

    // Tail length: base +- variation (same depth units as the slider), floored
    // at a quarter of the base so it thins locally but never collapses into an
    // on/off flicker — the old max(..., edgeFeather) collapse was exactly that.
    // Variation is an ABSOLUTE depth, and deliberately NOT capped against the tail.
    //
    // Its job is to throw long fingers far out from a narrow band — the tail has to be able to run
    // many times the main width, otherwise the strip is a ribbon of constant thickness. Two other
    // shapes were tried and both broke that: clamping it to the tail pinned the slider at about
    // 0.006 (everything above the first few percent did nothing), and making it a FRACTION of the
    // tail capped the reach at twice the base, which is the same ribbon with a wobble.
    //
    // The cost is that this slider, not Main Width, sets how far the foam reaches: a large value
    // against a narrow band means long tails and therefore a lot of foam. That is the intent —
    // shorten the reach by lowering THIS, not by fighting it elsewhere.
    float variation = max(shoreFoamBreakupParams.x, 0.0f) * windAmount;

    // MINIMUM WIDTH IN PIXELS, applied as a SCALE on both the base and its variation.
    //
    // Far away a strip authored in centimetres of depth is thinner than a pixel and reads as a
    // drawn outline around the island. Floor its width — but flooring the RESULT was wrong: once
    // the floor exceeded the variation, every stretch of coast came out the same width and the
    // band went perfectly even, which is the opposite of what the distance was supposed to fix.
    // Scaling both terms keeps the ratio between them, so the far band is wide enough to see AND
    // still breathes along the shore.
    //
    // Sized from the pixel's world footprint and a NOMINAL slope rather than from `depthFeather`:
    // on the fallback path that feather is the screen derivative of the scene depth, which at a few
    // hundred metres is enormous and blew the tail up to metres of depth.
    const float kMinStripPixels = 7.0f;
    const float kStripSlopeGuess = 0.05f;
    // Scaled by wind like every other width. Without it the floor was weather-blind: at range it
    // inflated the band to the same size in a dead calm as in a gale, so a light wind that should
    // have shown a thread showed a full white rim around the island.
    const float minStripDepth =
        pixelWorldSize * kMinStripPixels * kStripSlopeGuess * windAmount;
    // Normalised against MAIN WIDTH, never against the tail. The tail is legitimately zero when
    // Breakup Length is zero, and dividing by it sent this multiplier into the tens: a variation of
    // 0.09 came out as metres of tail, so a band authored with NO breakup grew enormous fingers.
    // Main width is guaranteed non-zero here (the function returns early otherwise).
    const float stripScale = max(1.0f, minStripDepth / max(mainWidth, 1e-4f));
    tailBase *= stripScale;
    variation *= stripScale;

    const float tailLen = max(tailBase + (macro * 2.0f - 1.0f) * variation,
                              max(tailBase * 0.25f, 1e-4f));

    // SIGNED DEPTH WARP — the thing that stops the outer edge reading as a contour line.
    //
    // Everything else here only decides where the edge tears WITHIN the tail. When the tail is thin
    // — light wind shrinks every width by windAmount — that leaves the outer edge sitting on a
    // clean isobath, and an isobath is smooth and, since the seabed does not move, motionless. No
    // amount of tearing fixes that, because tearing has no room to act.
    //
    // So warp the DEPTH the sweep reads, which moves the contour itself. Its own octave and its own
    // frequency (Depth Warp Scale), because it wants to be much coarser than the tear pattern:
    // this is the shape of the waterline, not its texture. Amplitude is in metres of depth, so on a
    // beach of slope s it shifts the line by strength/s metres of ground — 2 cm on a 3% slope is
    // about 70 cm of wander, which is what makes it stop looking drawn with a ruler.
    //
    // Fades out with depth over Depth Warp Range: bending the line matters at the waterline, and
    // leaving it on out in deeper water only smears the far side of the strip.
    float warpedDepth = shoreDepth;
    const float warpStrength = max(shoreFoamPatternParams.w, 0.0f);
    [branch]
    if (warpStrength > 1e-5f)
    {
        // Sampled from the WIND GUSTS texture (bound as FoamDetailMap), not from the breakup mask.
        // The warp wants a soft, low-frequency field — the shape a waterline wanders in — while the
        // breakup mask is high contrast and torn, which is its texture. Sharing one texture put
        // every bend of the line on top of a tear, and that is what read as artificial.
        const float warpNoise = FoamDetailMap.Sample(LinearWrapSampler,
            p * max(shoreFoamAlbedoParams.w, 1e-4f)).r * 2.0f - 1.0f;
        // Range is measured against THE STRIP, not against absolute depth. Set to 0.1 m while the
        // strip reaches 0.24 m deep, an absolute fade switched the warp off exactly where it was
        // wanted — on the outer edge, the one that reads as a drawn contour. Floored at the strip's
        // own extent so the whole edge always gets bent; raising the slider pushes it further out.
        const float warpRange = max(shoreFoamAlbedoParams.z, mainWidth + tailLen);
        const float warpFade = 1.0f - smoothstep(warpRange, warpRange * 2.0f, max(shoreDepth, 0.0f));
        // Amplitude is RELATIVE to the strip, not an absolute depth.
        //
        // As a raw metre value the slider was unusable: it had to be clamped against the tail (a
        // warp wider than the strip shoves the whole thing off the beach instead of bending it),
        // and with millimetre-wide strips that clamp bit at about 0.006 — so everything above the
        // first few percent of the slider's travel did exactly the same thing, which reads as an
        // on/off switch. Scaling by the strip instead means 1.0 = "displace the line by its own
        // width", at any wind and any authored width, and the whole range does something.
        const float kWarpStripScale = 1.5f;
        const float warpAmp = warpStrength * kWarpStripScale *
                              max(mainWidth, tailLen);
        warpedDepth += warpNoise * warpAmp * warpFade;
    }

    // Fine octave: the tear pattern itself. Histogram stretched exactly like
    // the old threshold mapping did, so existing masks read the same.
    //
    // TWO OCTAVES AT FIXED WORLD SCALES. Nothing here may depend on where the camera is.
    //
    // The dither tears the band by comparing this noise against the depth sweep, so it only reads
    // as torn if several periods fit ACROSS the band. At range the authored period is wider than
    // the band, the band fills in solid, and the island gets a drawn white outline. The old cure
    // was to RAISE the frequency as the band got thin on screen, and that was a mistake twice over.
    //
    // First, the frequency became a continuous function of view distance, so every camera move
    // rescaled the noise and the pattern crawled over ground that was standing still. Measured at
    // two camera heights over the same shore it sat at 3.7x and 3.9x the authored scale, so Pattern
    // Scale was barely in charge anywhere and the crawl was permanent. With Breakup Length
    // Variation at zero the band is otherwise perfectly steady, which is exactly when it shows.
    //
    // Second, it could not work anyway. Measured at four distances, the boost asked for periods
    // under two pixels as soon as the band got genuinely thin — past the sampling limit, which is
    // what the old frequency cap was there to catch. Detail cannot be added to something that is
    // already too small to resolve; a band that thin is the job of the minimum-width floor above,
    // not of the dither.
    //
    // So: one octave at the authored scale, one at a fixed multiple of it, both pinned to the
    // world. The only distance term left is the detail octave bowing out once its own period
    // approaches the sampling limit — the same thing a mip chain would do, which is a fade the eye
    // reads as detail settling, not as texture sliding.
    const float kDetailOctave = 4.0f;
    const float kDetailMix = 0.35f;
    const float authoredFine = max(shoreFoamPatternParams.x, 1e-3f);
    const float detailPeriodPixels =
        1.0f / max(authoredFine * kDetailOctave * pixelWorldSize, 1e-6f);
    const float detailWeight = kDetailMix * smoothstep(2.0f, 5.0f, detailPeriodPixels);

    float fine = ShoreFoamBreakupMaskTex.Sample(LinearWrapSampler, p * authoredFine).r;
    [branch]
    if (detailWeight > 1e-3f)
    {
        const float detail = ShoreFoamBreakupMaskTex.Sample(
            LinearWrapSampler, p * (authoredFine * kDetailOctave)).r;
        fine = lerp(fine, detail, detailWeight);
    }
    fine = lerp(0.02f, 0.98f, fine);

    // The sweep. Density shifts how much of the tail stays filled.
    float t = (max(warpedDepth, 0.0f) - solidWidth) / tailLen;
    t -= (saturate(shoreFoamPatternParams.y) - 0.5f) * 0.5f;

    // Edge softness plus a screen-space AA floor. CLAMPED AT BOTH ENDS, and the
    // ceiling is not cosmetic:
    //
    //   coverage = smoothstep(t - f, t + f, fine),  fine in [0.02, 0.98]
    //
    // so once f grows past the sweep, BOTH edges of the smoothstep straddle the
    // whole noise range and every pixel returns ~0.5 regardless of t. The strip
    // stops obeying depth entirely — it just goes uniformly half-white. That is
    // the foam line along the far edge of the shore-depth field: `depthFeather`
    // is fwidth() of the vertex depth, which jumps to 1000 (the out-of-field
    // sentinel in GetShoreData) across the field boundary, so f explodes there.
    // With f <= kMaxFeather, t - f >= 0.98 is reachable again and deep water is
    // guaranteed foam-free.
    //
    // The floor also matters for a second artifact: fwidth() of a vertex
    // interpolant is discontinuous across every triangle edge, and the water
    // clipmap's edges are a world-axis-aligned grid — that discontinuity is the
    // rectangular staircase along the foam edge. Clamping keeps f inside a
    // narrow band so the steps cannot swing the edge visibly.
    const float kMaxFeather = 0.25f;
    const float feather = clamp(depthFeather * 1.5f / tailLen, kTearSoftness, kMaxFeather);

    float coverage = smoothstep(t - feather, t + feather, fine);

    // Kill the mask INLAND. `t` uses max(depth, 0), so everywhere the ground is above water the
    // sweep sits at a constant negative value and the mask returns a solid 1 — the whole buried
    // sheet, all the way under the beach, is painted with foam. Under the sand nobody sees it, but
    // on a coarse distant LOD the sunken vertices smear that solid white across the shore and it
    // reads as a pale blotch on the island. Fading it out over the first stretch of dry ground costs
    // nothing where it matters (the strip lives at depth >= 0) and removes the blotch entirely.
    //
    // The fade SCALES WITH WIND. It used to be a flat 0.2 m of negative depth, and every other
    // width in this mask already breathes with the weather — which went unnoticed only while the
    // calm shore had no run-up sheet to paint. The moment a calm sea kept its wet hem, that hem
    // came out solid white: anything shallower than 0.2 m of dry ground is inside a constant fade
    // that the wind never touches. Scaled, a storm still foams the whole swash sheet while a calm
    // sea keeps a thin lace right at the waterline.
    const float kInlandFoamFade = // metres of NEGATIVE depth over which the mask dies
        lerp(0.2f, 0.5f, windAmount);
    coverage *= 1.0f - smoothstep(0.0f, kInlandFoamFade, max(-shoreDepth, 0.0f));
#if OCEAN_FOAM_DEBUG
    g_foamDbg = float4(t, feather, fine, tailLen);
#endif
    return float2(coverage * opacity, coverage);
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

FoamData GetFoamData(FoamInput input, uint cascadesCount)
{
    FoamData data;
    data.coverage = float2(0.0f, 0.0f);
    data.normal = input.normal;
    data.albedo = float3(1.0f, 1.0f, 1.0f);

    float4 activeCascades = ActiveCascadesMask(cascadesCount);
    FoamTurbulenceSet turbulence = SampleFoamTurbulence(input.worldUV, input.lodWeights * input.shoreWeights, cascadesCount);
    float4 mixWeights = input.lodWeights * activeCascades;

    float biasSample = FoamDetailMap.SampleLevel(LinearWrapSampler, input.worldUV * 0.01f * 0.01f, 0).r;
    float bias = biasSample * saturate(input.viewDist / max(simulationParams.x, 1.0f) * 0.5f);
    
    //data.coverage.x = bias;
    //return data;

    float deepFoam = DeepFoam(input.worldUV, input.viewDir, input.normal, input.time);
    data.coverage = Coverage(turbulence, mixWeights, input.worldUV, deepFoam, bias);

    // Coverage() contains the simulated crest foam as well as the explicit
    // contact mask below. Suppress its near-shore contribution with the same
    // wind response, otherwise it survives as a detached breakup tail when
    // contact foam reaches zero. The padded, unwarped envelope keeps this
    // attenuation local to the contact strip and leaves open-ocean crests
    // untouched.
    float contactFoamWindAmount = ContactFoamWindAmount();
    [branch]
    if (contactFoamWindAmount < 1.0f - 1e-4f)
    {
        float contactFoamWindExtent =
            max(shoreFoamGeometryParams.x, 0.0f) +
            max(shoreFoamBreakupParams.x, 0.0f) +
            max(shoreFoamPatternParams.w, 0.0f);
        float fieldFeather =
            max(fwidth(input.shoreDepth) * 2.0f, 1e-3f);
        float fallbackFeather =
            max(fwidth(input.fallbackShoreDepth) * 2.0f, 1e-3f);
        float fieldWindZone = 0.0f;
        if (input.shoreFieldWeight > 1e-3f)
        {
            fieldWindZone =
                (1.0f - smoothstep(
                    contactFoamWindExtent,
                    contactFoamWindExtent + fieldFeather,
                    max(input.shoreDepth, 0.0f))) *
                saturate(input.shoreEffectWeight);
        }

        float fallbackWindZone = 0.0f;
        if (input.shoreFieldWeight < 1.0f - 1e-3f)
        {
            fallbackWindZone =
                (1.0f - smoothstep(
                    contactFoamWindExtent,
                    contactFoamWindExtent + fallbackFeather,
                    max(input.fallbackShoreDepth, 0.0f))) *
                saturate(input.fallbackShoreWeight);
        }

        float shoreWindZone = lerp(
            fallbackWindZone,
            fieldWindZone,
            saturate(input.shoreFieldWeight));
        data.coverage *= lerp(
            1.0f,
            contactFoamWindAmount,
            saturate(shoreWindZone));
    }

    float shoreAlbedoWeight = 0.0f;
    float3 shoreFoamAlbedo = float3(1.0f, 1.0f, 1.0f);
    if (shoreFoamGeometryParams.w > 0.0f)
    {
        float fieldWeight = saturate(input.shoreFieldWeight);
        float2 fieldContactFoam = float2(0.0f, 0.0f);
        float2 fallbackContactFoam = float2(0.0f, 0.0f);
        [branch]
        if (fieldWeight > 1e-3f)
        {
            fieldContactFoam = ContactFoamMask(
                input.worldUV,
                input.shoreDepth,
                input.depthFeather,
                input.pixelWorldSize,
                input.time);
            fieldContactFoam *= saturate(input.shoreEffectWeight);
        }
#if OCEAN_FOAM_DEBUG
        // The mask runs TWICE (field then fallback) and both write the statics,
        // so the field terms are parked here and blended back below with the
        // same weight the masks themselves are blended with.
        const float4 dbgField = g_foamDbg;
#endif
        [branch]
        if (fieldWeight < 1.0f - 1e-3f)
        {
            fallbackContactFoam = ContactFoamMask(
                input.worldUV,
                input.fallbackShoreDepth,
                input.depthFeather,
                input.pixelWorldSize,
                input.time);
            fallbackContactFoam *= saturate(input.fallbackShoreWeight);
        }
        float2 contactFoamMask = lerp(
            fallbackContactFoam,
            fieldContactFoam,
            fieldWeight);
#if OCEAN_FOAM_DEBUG
        g_foamDbg = lerp(g_foamDbg, dbgField, fieldWeight);
        g_foamDbgMask = contactFoamMask;
#endif
        [branch]
        if (contactFoamMask.x > 1e-3f)
        {
            float2 scrollDirection = windParams1.xy;
            float directionLengthSquared = dot(scrollDirection, scrollDirection);
            scrollDirection = directionLengthSquared > 1e-8f
                ? scrollDirection * rsqrt(directionLengthSquared)
                : float2(1.0f, 0.0f);
            float2 shoreAlbedoUV = (input.worldUV - scrollDirection * input.time * max(shoreFoamAlbedoParams.y, 0.0f)) * max(shoreFoamAlbedoParams.x, 1e-3f);
            shoreFoamAlbedo = ShoreFoamAlbedoTex.Sample(AnisotropicWrapSampler, shoreAlbedoUV).rgb;

            // Treat the dark bubble interiors as actual holes in contact
            // foam coverage instead of merely tinting opaque foam gray.
            float shoreFoamLuminance = dot(
                shoreFoamAlbedo,
                float3(0.2126f, 0.7152f, 0.0722f));
            float bubbleCoverage = max(smoothstep(0.30f, 0.68f, shoreFoamLuminance), 0.15f);
            contactFoamMask *= bubbleCoverage;
        }
        data.coverage.x = max(data.coverage.x, contactFoamMask.x);
        shoreAlbedoWeight = contactFoamMask.y;

        //shoreFoamAlbedo = float3(1,0,0);
    }

    float4 foamNormalWeights = saturate(float4(1.0f, 0.66f, 0.33f, 0.0f) + foamParams1.w) * activeCascades;
    float3 foamNormal = NormalFromDerivatives(input.derivatives, foamNormalWeights);
    float contactNormalReduction =
        shoreAlbedoWeight *
        (1.0f - saturate(shoreFoamBreakupParams.z));
    data.normal = normalize(lerp(
        foamNormal,
        input.normal,
        contactNormalReduction));

    float2 uv = input.worldUV * 1.0f;
    float3 peakFoamAlbedo =
        FoamAlbedoTex.SampleLevel(LinearWrapSampler, uv, 0).rgb;
    data.albedo = peakFoamAlbedo;
    [branch]
    if (shoreAlbedoWeight > 1e-3f)
    {
        data.albedo = lerp(peakFoamAlbedo, shoreFoamAlbedo, shoreAlbedoWeight);
    }
    return data;
}

float3 LitFoamColor(const LightingInput li, const FoamData foamData)
{
    float ndotl = (0.2f + 0.8f * saturate(dot(foamData.normal, -li.mainLight.direction)))
        * li.mainLight.shadowAttenuation;
    float3 skyAmbient = kSkyColor * (li.ambient + 0.3f * (1.0f - foamData.normal.y));
    return foamData.albedo * foamTint.rgb * (ndotl * li.mainLight.color + skyAmbient);
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

BrunetonInputs BuildBrunetonInputs(const LightingInput li)
{
    // Only viewDirWind / normalWind / slopeVarianceSquared are ever read (by EffectiveFresnel).
    // The wind-space light direction and the tangent frame were written and never used.
    BrunetonInputs bi;
    bi.viewDirWind = TransformToWind(li.viewDir);
    bi.normalWind = TransformToWind(li.normal);

    float windSpeed = max(windParams0.x, 0.0f);
    float wavesScale = max(windParams0.y, 0.0f);
    float alignment = windParams0.z;
    float roughScale = max(specularParams.y, 0.0f);
    float2 slopeVariance = roughScale * (1.0f + li.roughnessMap * 0.3f)
        * SlopeVarianceSquared(windSpeed * wavesScale, li.viewDist, alignment, max(specularParams.z, 1.0f));
    bi.slopeVarianceSquared = slopeVariance;
    return bi;
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

float OceanSurfaceRoughness(const LightingInput li)
{
    return saturate(specularParams.y * (1.0f + li.roughnessMap * 0.3f));
}

float3 Specular(const LightingInput li, const BrunetonInputs bi, float roughness)
{
    //(void)bi;
    float3 halfDir = normalize(-li.mainLight.direction + li.viewDir);
    float specPower = lerp(kSpecularMinPower, kSpecularMaxPower, 1.0f - roughness);
    float spec = pow(saturate(dot(li.normal, halfDir)), specPower);
    spec *= specularParams.x * li.mainLight.shadowAttenuation;
    return spec * li.mainLight.color;
}

float2 OceanReflectionUvOffset(const LightingInput li, float3 adjustedNormal)
{
    float3 flatReflectDir = reflect(-li.viewDir, float3(0.0f, 1.0f, 0.0f));
    float3 waveReflectDir = reflect(-li.viewDir, adjustedNormal);
    float2 reflectionDelta = waveReflectDir.xz - flatReflectDir.xz;

    float distanceFade = saturate(li.viewDist / max(specularParams.z, 1.0f));
    float grazing = saturate(1.0f - abs(waveReflectDir.y));
    float strength = lerp(0.08f, 0.025f, distanceFade) * lerp(0.45f, 1.0f, grazing) * 2;
    return reflectionDelta * strength;
}

float OceanReflectionEdgeFade(float2 uv)
{
    float2 edgeDist = min(uv, float2(1.0f, 1.0f) - uv);
    return saturate(min(edgeDist.x, edgeDist.y) * 64.0f);
}

float3 Reflection(const LightingInput li, float roughness)
{
    float reflectionNormalStrength = saturate(heightFogParams.w);
    float3 adjustedNormal = normalize(lerp(li.normal, float3(0.0f, 1.0f, 0.0f), reflectionNormalStrength));
    float3 reflectDir = reflect(-li.viewDir, adjustedNormal);

    float skyMip = roughness * kSkyRoughMaxMip;
    float3 skySample = SkyboxTexture.SampleLevel(LinearClampSampler, reflectDir, skyMip).rgb;
    float2 reflectionUV = li.screenUV + OceanReflectionUvOffset(li, adjustedNormal);
    float edgeFade = OceanReflectionEdgeFade(reflectionUV);
    float4 oceanReflection = OceanReflectionTexture.SampleLevel(LinearClampSampler, saturate(reflectionUV), 0);
    float visibility = saturate(oceanReflection.a) * edgeFade;
    return oceanReflection.rgb * edgeFade + skySample * (1.0f - visibility);
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

float3 RefractionCoords(float refractionStrength, float4 positionNDC, float viewDepth, float3 normal)
{
    float2 uvOffset = normal.xz * refractionStrength;
    uvOffset.y *= depthTextureSize.z * abs(depthTextureSize.y);

    float2 refractedUV = ((positionNDC.xy + uvOffset) / positionNDC.w);
    refractedUV = saturate(refractedUV * float2(0.5f, -0.5f) + float2(0.5f, 0.5f));
    
    float depthSample = SampleSceneDepth(refractedUV);
    float refractedDepth = DepthToViewZ_Fast(depthSample);

    float depthDiff = refractedDepth - viewDepth;
    uvOffset *= saturate(depthDiff);

    refractedUV = ((positionNDC.xy + uvOffset) / positionNDC.w);
    refractedUV = saturate(refractedUV * float2(0.5f, -0.5f) + float2(0.5f, 0.5f));

    depthSample = SampleSceneDepth(refractedUV);
    return float3(refractedUV, depthSample);
}

float3 Refraction(const LightingInput li, const FoamData foamData, float2 sss, float3 foamColor)
{
    float depthScale = 0.0f;
    float3 color = DeepScatterColor(depthScale);
    
    float3 sssColor = SssColor(depthScale);
    color += sssColor * saturate(sss.x + sss.y);
    
    //return color;

    float ndotl = saturate(dot(li.normal, -li.mainLight.direction));
    color += (ndotl * 0.8f + 0.2f) * li.mainLight.color * DiffuseColor(depthScale);
    
    //return color;

    float3 refractionCoords = RefractionCoords(refractionParams.x, li.positionNDC, li.viewDepth, li.normal);
    float3 backgroundColor = SceneColorTexture.SampleLevel(LinearClampSampler, refractionCoords.xy, 0).rgb;

    //return backgroundColor;

    float3 backgroundPositionWS = PositionWsFromDepth(refractionCoords.z, refractionCoords.xy);
    float backgroundDistance = length(backgroundPositionWS - li.cameraPos) - li.viewDist;
    color = ColorThroughWater(backgroundColor, color, backgroundDistance, -backgroundPositionWS.y);

    //return color;

    // AbsorptionTint builds and evaluates a gradient; skip it where the lerp weight is already
    // zero, which is all of the open ocean.
    float underwaterFoamAmount =
        foamData.coverage.y * (20.0f / (20.0f + li.viewDist));
    [branch]
    if (underwaterFoamAmount > 1e-3f)
    {
        float3 tint = AbsorptionTint(0.8f);
        color = lerp(color, foamColor * tint * tint, underwaterFoamAmount);
    }
    return color;
}

float4 HorizonBlend(const LightingInput li)
{
    float horizonFog = max(specularParams.w, 0.01f);
    float distanceScale = 100.0f + 7.0f * abs(li.cameraPos.y);
    float exponent = -5.0f / horizonFog * (abs(li.viewDir.y) + distanceScale / (li.viewDist + distanceScale));
    float blend = saturate(exp(exponent));

    // The caller does lerp(color, horizon.rgb, horizon.a), so a zero alpha makes this a no-op —
    // and everything except the horizon band has blend ~ 0. Computing blend is pure ALU; the
    // cubemap fetch it now guards is not, and it used to be unconditional.
    // SampleLevel is an EXPLICIT-LOD fetch, so moving it behind a branch is safe; an implicit-LOD
    // Sample needs derivatives and could not be moved here.
    [branch]
    if (blend <= 1e-3f)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    float3 dir = -float3(li.viewDir.x, 0.0f, li.viewDir.z);
    float3 horizonColor = SkyboxTexture.SampleLevel(LinearClampSampler, dir, 0).rgb;
    return float4(horizonColor, blend);
}

float3 GetOceanColor(const LightingInput li, const LightingInput macroLi, const FoamData foamData)
{
    BrunetonInputs bi = BuildBrunetonInputs(macroLi);
    float2 sss = SubsurfaceScatteringFactor(macroLi);
    float3 foamLitColor = LitFoamColor(li, foamData);
    float roughness = OceanSurfaceRoughness(li);

    float fresnel = EffectiveFresnel(macroLi, bi);
    float3 specular = Specular(li, bi, roughness) * Pow5(1.0f - saturate(foamData.coverage.y));
    float3 reflected = Reflection(macroLi, roughness);
    //return reflected;
    float3 refracted = Refraction(macroLi, foamData, sss, foamLitColor);
    //return refracted;
    float4 horizon = HorizonBlend(macroLi);
    //return horizon.aaa;

    float3 color = specular + lerp(refracted, reflected, fresnel);
    //color = fresnel.xxx;
    color = lerp(color, foamLitColor, foamData.coverage.x);
    color = lerp(color, horizon.rgb, horizon.a);
    return color;
}

struct PSOut
{
    float4 color : SV_Target0;
    float2 velocity : SV_Target1;
};

[RootSignature(OCEAN_SURFACE_RS)]
PSOut PSMain(VSOutput input)
{
    uint cascadesCount = max((uint)simulationParams.w, 1u);

    float sourceWaterDepth = input.shoreData.y;
#if !OCEAN_SHORE_SINK
    // The discard costs the WHOLE draw its early-Z, not just these pixels — see
    // ocean::g_shoreSinkCut. OCEAN_SHORE_SINK removes it from the compiled shader entirely and
    // moves the cut into the vertex shader.
    float shoreClipWidth = max(fwidth(sourceWaterDepth), 1e-4f);
    clip(sourceWaterDepth + shoreClipWidth * 0.5f);
#endif

    float3 baseWorld = float3(input.baseXZ.x, 0.0f, input.baseXZ.y);
    float3 viewVector = baseWorld - clipMapViewer.xyz;
    float viewDist = length(viewVector);
    float2 screenUV = ComputeScreenUV(input.positionNDCJitter);
    float2 shoreUV = ShoreDepthUV(input.worldPos.xz);
    float shoreFieldWeight = ShoreFieldWeight(shoreUV);
    // Contact-foam depth comes from the VERTEX (shoreData.x), NOT from a texture lookup here.
    //
    // The lookup used to be ShoreWaterDepth(ShoreDepthUV(input.worldPos.xz)), and worldPos is
    // baseXZ plus the wave's horizontal displacement — a nonlinear field sampled per vertex and
    // interpolated linearly, so the screen->world mapping kinks at every triangle edge. Reading a
    // steep depth field through those kinks quantized the strip into rectangular facets the size
    // of a clipmap quad, world-aligned and static (which is why the breakup texture visibly
    // scrolled OVER them). Raising the field to 2048^2 changed nothing — it was never the field.
    //
    // shoreData.x is the same depth with the displacement applied ANALYTICALLY in the VS
    // (predictedDepth = depth + dot(gradient, displacement)), so it is smooth, it still follows the
    // run-up, and it costs one texture sample LESS in the pixel shader. Diagnostic view 10 keeps
    // the old path for comparison.
    float contactShoreDepth = 1000.0f;
    float shoreEffectWeight = 0.0f;
    [branch]
    if (shoreFieldWeight > 1e-3f)
    {
        // HYBRID: the depth itself is fetched HERE, per pixel, at the vertex's BASE position; only
        // the wave's advection comes interpolated from the vertex.
        //
        // Taking the whole thing from the vertex (shoreData.x) tied the isobath to the clipmap's
        // vertex spacing. On a coarse distant LOD the field is only sampled every few metres and
        // linearly interpolated between, so the waterline the foam draws drifts away from the real
        // one — the strip visibly peels off the beach — and it slides as the clipmap morphs its
        // vertices around. Fetching per pixel pins it to the map instead of to the mesh.
        //
        // Sampled at baseXZ, NOT at worldPos: worldPos carries the wave's horizontal displacement,
        // a nonlinear field interpolated linearly, and reading a steep depth map through those
        // kinks is what quantized the strip into world-aligned facets. baseXZ is the flat y=0 grid,
        // so neighbouring triangles are exactly coplanar and the fetch is smooth.
        //
        // The advection is added back as the vertex's own correction (predicted minus source): it
        // is small and smooth, so interpolating THAT costs nothing.
        const float2 baseShoreUV = ShoreDepthUV(input.baseXZ);
        const float waveAdvection = input.shoreData.x - input.shoreData.y;
        contactShoreDepth = ShoreWaterDepth(baseShoreUV) + waveAdvection;
        shoreEffectWeight =
            ShoreEffectDepthWeight(contactShoreDepth);
    }

    // Both blocks below want the scene depth AT THIS PIXEL, and each used to fetch it itself —
    // two identical samples whenever both conditions held. Fetched once under the OR of the two
    // conditions, so a pixel that needs neither still pays nothing.
    const bool needsFallbackShore =
        shoreFieldWeight < 1.0f - 1e-3f && shoreFoamGeometryParams.w > 0.0f;
    const bool needsRefractionSoftEdge =
        shoreSlopeParams.z > 0.0f &&
        sourceWaterDepth < max(shoreBehaviorParams1.x, 0.01f);
    float sceneDepthAtPixel = 0.0f; // reverse-Z clear: no opaque geometry
    [branch]
    if (needsFallbackShore || needsRefractionSoftEdge)
    {
        sceneDepthAtPixel = SampleSceneDepthFiltered(screenUV);
    }
    const bool hasSceneGeometryAtPixel =
        HasSceneGeometryDepth(sceneDepthAtPixel);

    float fallbackShoreDepth = 1000.0f;
    float fallbackShoreWeight = 0.0f;
    [branch]
    if (needsFallbackShore)
    {
        float sceneDepthSample = sceneDepthAtPixel;
        if (hasSceneGeometryAtPixel)
        {
            // Filtered: this depth ends up in the contact-foam mask (fallbackShoreDepth), which
            // must not carry the depth buffer's steps. A fully clear footprint remains exactly 0
            // and was rejected above; mixed geometry/clear samples retain the intended soft edge.
            float3 scenePositionWS =
                PositionWsFromDepth(sceneDepthSample, screenUV);
            fallbackShoreDepth = -scenePositionWS.y;

            // Weighted by WATER DEPTH, the same quantity the strip beside the near shore is
            // measured with, so an out-of-field shoreline gets a band of comparable width.
            //
            // It used to be weighted by the along-ray gap between water and geometry against
            // `Refraction Soft Edge Distance` — two centimetres, and a parameter that has nothing
            // to do with foam. At a few hundred metres and a grazing angle only a sliver of the
            // beach falls inside two centimetres of view depth, so the band collapsed to an even
            // hairline no matter how wide the foam was authored.
            fallbackShoreWeight = ShoreEffectDepthWeight(fallbackShoreDepth);
        }
    }

    float refractionSoftEdge = 1.0f;
    [branch]
    if (needsRefractionSoftEdge && hasSceneGeometryAtPixel)
    {
        float sceneViewDepth = DepthToViewZ_Fast(sceneDepthAtPixel);
        float geometryDepthSeparation = max(sceneViewDepth - input.viewDepth, 0.0f);
        refractionSoftEdge = smoothstep(
            0.0f,
            shoreSlopeParams.z,
            geometryDepthSeparation);
    }

    float4 weights = LodWeights(viewDist, clipMapParams.w);
    DerivativesSet macroDeriv = SampleDerivatives(
        input.baseXZ, weights, cascadesCount, normalSamplingParams.y);
    DerivativesSet deriv = macroDeriv;
    if (weights.x > kLodThreshold)
    {
        deriv.cascades[0] =
            SampleDerivativesCascade(input.baseXZ, 0u, normalSamplingParams.x) * weights.x;
    }

    float normalFade = lerp(
        1.0f,
        smoothstep(
            0.0f,
            max(shoreBehaviorParams0.w, 0.01f),
            max(input.shoreData.x, 0.0f)),
        saturate(input.shoreData.z));
    float4 normalWeights = lerp(saturate(shoreNormalMinWeights), 1.0f.xxxx, normalFade);
    float4 combinedDerivatives = CombineDerivatives(deriv, normalWeights);
    float4 macroCombinedDerivatives = CombineDerivatives(macroDeriv, normalWeights);
    float3 normal = NormalFromCombinedDerivatives(combinedDerivatives);
    float3 macroNormal = NormalFromCombinedDerivatives(macroCombinedDerivatives);
    //return float4(normal, 1);

    float3 viewDir = normalize(clipMapViewer.xyz - input.worldPos);
    float3 lightDir = normalize(sunDirAmbient.xyz);

    float slopeFactor = saturate(1.0f - normal.y);
    float height = input.worldPos.y;

    FoamInput foamInput;
    foamInput.derivatives = deriv;
    foamInput.worldUV = input.baseXZ;
    foamInput.viewDist = viewDist;
    foamInput.lodWeights = weights;
    foamInput.shoreWeights = normalWeights;
    foamInput.time = simulationParams.z;
    foamInput.viewDir = viewDir;
    foamInput.normal = normal;
    foamInput.shoreDepth = contactShoreDepth;
    // Pixel footprint in world metres, from the view DISTANCE — never from ddx/ddy of an
    // interpolant.
    //
    // The derivative version is exact but it is CONSTANT INSIDE A TRIANGLE and jumps at every edge,
    // and the water clipmap's triangles are a world-aligned grid. Anything sized by it inherits
    // that: the antialiasing floor came out per-triangle and drew the rectangular staircase along
    // the foam edge, and the dither's frequency came out as a mosaic of triangles with moire
    // hatching inside each. Distance is interpolated, so this is continuous everywhere.
    //
    // Approximate on purpose. It only feeds thresholds — an AA floor and two width/frequency
    // clamps — so being off by a factor at a grazing angle costs nothing, while a discontinuity
    // costs a visible artifact. Cheaper too: no derivative instructions.
    const float pixelWorldSize =
        max(input.viewDepth * 2.0f / max(proj._22 * depthTextureSize.w, 1e-3f), 1e-4f);
    // Metres of water DEPTH one pixel spans: the field's own gradient (interpolated, smooth) times
    // that footprint.
    foamInput.depthFeather = max(input.shoreData.w * pixelWorldSize, 1e-4f);
    foamInput.pixelWorldSize = pixelWorldSize;
    foamInput.fallbackShoreDepth = fallbackShoreDepth;
    foamInput.fallbackShoreWeight = fallbackShoreWeight;
    foamInput.shoreFieldWeight = shoreFieldWeight;
    foamInput.shoreEffectWeight = shoreEffectWeight;

    FoamData foamData = GetFoamData(foamInput, cascadesCount);
    //return float4(foamData.coverage.xxx, 1);

    float roughnessMap = SampleDistantRoughness(input.baseXZ, viewDist);
    //return float4(roughnessMap.xxx, 1);
    
    LightData light;
    light.direction = lightDir;
    light.color = sunColorExposure.xyz * sunColorExposure.w;
    light.shadowAttenuation = 1.0f;

    LightingInput li;
    li.normal = normal;
    li.viewDir = viewDir;
    li.viewDist = viewDist;
    li.roughnessMap = roughnessMap;
    li.positionWS = input.worldPos;
    li.screenUV = screenUV;
    li.shore = float4(0.0f, 0.0f, 0.0f, 0.0f);
    li.viewDepth = input.viewDepth;
    li.cameraPos = clipMapViewer.xyz;
    li.height = height;
    li.positionNDC = input.positionNDCJitter;
    li.referenceWaveHeight = windParams1.z;
    li.slopeFactor = slopeFactor;
    li.mainLight = light;
    li.ambient = sunDirAmbient.w;

    LightingInput macroLi = li;
    macroLi.normal = macroNormal;
    macroLi.slopeFactor = saturate(1.0f - macroNormal.y);

    float3 color = GetOceanColor(li, macroLi, foamData);
    
    float refractionEdgeWeight = refractionSoftEdge;
    [branch]
    if (refractionEdgeWeight < 0.95f)
    {
        float3 softEdgeRefraction = SceneColorTexture.SampleLevel(LinearClampSampler, screenUV, 0).rgb;
        color = lerp(softEdgeRefraction, color, refractionEdgeWeight);
    }
    
    float4 outColor = float4(saturate(color), 1.0f);

    float2 currUv = ClipToUV(input.positionNDC);
    float2 prevUv = ClipToUV(input.prevPositionNDC);
    float2 motion = currUv - prevUv;

    //outColor = float4(normalWeights.xyz, 1.0f);

#if OCEAN_FOAM_DEBUG
    // View id comes from shoreFoamBreakupParams.w (ocean::g_foamDebugView); the
    // list lives next to the combo in OceanControlsWindow. The fwidth() is taken
    // OUTSIDE the branch on purpose — a gradient op inside varying flow control
    // is undefined even when the condition happens to be uniform.
    const float dbgRawDepthFeather = max(fwidth(sourceWaterDepth), 1e-4f);
    const int foamDebugView = (int)(shoreFoamBreakupParams.w + 0.5f);
    [branch]
    if (foamDebugView > 0)
    {
        float3 dbg = float3(0.0f, 0.0f, 0.0f);
        if (foamDebugView == 1)
        {
            // Sweep t. Green = the solid band (t < 0), heat ramp across the
            // tail, magenta past its end. Any staircase HERE is the depth field.
            dbg = g_foamDbg.x < 0.0f
                ? float3(0.0f, 0.6f, 0.0f)
                : (g_foamDbg.x > 1.0f ? float3(0.5f, 0.0f, 0.5f)
                                      : DebugHeat(g_foamDbg.x));
        }
        else if (foamDebugView == 2)
        {
            // Feather actually used. Red = pinned at kMaxFeather.
            dbg = DebugHeat(g_foamDbg.y / 0.3f);
        }
        else if (foamDebugView == 3)
        {
            // RAW fwidth(sourceWaterDepth) — the unclamped source. This is the
            // one that shows the clipmap-quad steps and the field-edge blowup.
            dbg = DebugHeat(dbgRawDepthFeather / 0.3f);
        }
        else if (foamDebugView == 4)
        {
            dbg = g_foamDbg.z.xxx;                       // tear noise
        }
        else if (foamDebugView == 5)
        {
            dbg = g_foamDbgMask.y.xxx;                   // final contact coverage
        }
        else if (foamDebugView == 6)
        {
            dbg = float3(shoreFieldWeight, fallbackShoreWeight, shoreEffectWeight);
        }
        else if (foamDebugView == 7)
        {
            // Shore depth in 10 cm contour bands; blue above the waterline.
            const float d = contactShoreDepth;
            const float band = frac(abs(d) * 10.0f) < 0.5f ? 0.70f : 0.30f;
            dbg = d < 0.0f ? float3(0.12f, 0.12f, band)
                           : float3(band, band * 0.75f, 0.12f);
        }
        else if (foamDebugView == 8)
        {
            dbg = DebugHeat(g_foamDbg.w * 0.5f);         // tail length, 0..2 m
        }
        else if (foamDebugView == 13)
        {
            // Shore SDF as the pixel shader sees it: green = water (distance out), red = inland
            // (negative), banded every 2 m. Black means the field reports nothing here at all.
            const float2 field = SampleShoreField(input.baseXZ);
            const float band = frac(abs(field.x) * 0.5f) < 0.5f ? 1.0f : 0.55f;
            dbg = field.x >= 0.0f ? float3(0.0f, band, 0.2f * band)
                                  : float3(band, 0.0f, 0.0f);
        }
        else if (foamDebugView == 12)
        {
            // Where the out-of-range (depth-buffer) shore handling is in charge.
            // G = shore field weight, R = its complement, i.e. the depth-buffer regime.
            dbg = float3(1.0f - saturate(shoreFieldWeight), saturate(shoreFieldWeight), 0.0f);
        }
        else if (foamDebugView == 10)
        {
            // The OLD path, kept for A/B: depth fetched in the pixel shader through worldPos.
            // The rectangular facets in this view are the artifact the vertex depth removed.
            const float oldShoreDepth = shoreFieldWeight > 1e-3f
                ? ShoreWaterDepth(shoreUV)
                : 1000.0f;
            const float2 oldMask = ContactFoamMask(
                input.baseXZ, oldShoreDepth, foamInput.depthFeather,
                foamInput.pixelWorldSize, foamInput.time);
            dbg = oldMask.y.xxx;
        }
        else if (foamDebugView == 14)
        {
            // The isobaths the foam edge actually rides, in 1 MM bands. View 7 draws the same
            // field at 10 cm, which is far too coarse to see anything wrong with it: the strip is
            // authored millimetres wide, so that is the scale the field has to be judged at. Every
            // contour here must be a smooth curve. A regular sawtooth on ALL of them at once means
            // the field is quantized somewhere upstream, not that the foam pattern is noisy — that
            // is how the sampler's 8-bit subtexel snap was found (see SampleShoreDepth).
            const float band = frac(abs(contactShoreDepth) * 1000.0f) < 0.5f ? 1.0f : 0.35f;
            dbg = contactShoreDepth < 0.0f ? float3(0.1f, 0.1f, band) : float3(band, band, 0.1f);
        }
        else if (foamDebugView == 9)
        {
            // Sweep t with the shore-depth field's TEXEL GRID laid over it: one checker cell is
            // one texel of the 512^2 map. If the facets in the sweep line up with the cells, the
            // strip is simply resolving finer than the field it reads.
            const float2 shoreTexel = shoreUV / max(shoreSamplingParams.xy, 1e-6f.xx);
            const float checker = fmod(floor(shoreTexel.x) + floor(shoreTexel.y), 2.0f);
            dbg = DebugHeat(g_foamDbg.x) * lerp(0.55f, 1.0f, checker);
        }
        outColor = float4(dbg, 1.0f);
    }
#endif

    PSOut o;
    o.color = outColor;
    o.velocity = motion;
    return o;
}

#endif // OCEAN_SHORE_RUNUP
