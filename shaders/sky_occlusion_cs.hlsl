// The sky visibility WITHOUT ray tracing -- plan S's fallback (`skyVis.fallback`, SkyOcclusionMap.h). Writes the very
// target the RT trace writes (rt_sky_visibility_cs.hlsl): UE DFAO's bent normal, its length the share of the sky a
// point sees, .w the device depth -- so lighting, compose, glass and the RT resolve read it through the same
// functions (sky_visibility.hlsli) and nothing downstream knows which of the two made it.
//
// What it traces against is the world seen from above: the scene's casters drawn top-down twice around the camera
// (their highest surface from above, their lowest from below) and the terrain once (SceneRenderer_SkyOcclusion.cpp),
// reduced here into two levels -- 1 m and 4 m a texel by default (skyVis.fallbackTexel) -- of (coverage, coverage x
// top, coverage x bottom, ground).
// A texel is a slab of air from its bottom to its top whose vertical transmittance is 1 - coverage x opacity (a crown
// of fronds, a roof, a rock), standing on the ground. CSTrace then marches the same cosine-distributed directions the
// rays take through that 2.5D world: a segment inside a slab loses exp(-sigma x its length in it), one under the
// ground ends the direction. It sees what the RT trace sees at the scale of a crown or a roof -- the grove's floor
// under its canopy, a crown's lower fronds under its upper ones, a trunk under its crown, the room under its roof --
// and nothing finer (GTAO's contact radius has that), nor walls (a vertical surface has no area from above).
//
// Premultiplied heights, so a bilinear read between a slab and the open air keeps the slab's height and halves its
// coverage instead of inventing a slab at half its height. Deterministic: the same directions at every pixel and
// frame, no noise, no history.
#pragma pack_matrix(row_major)
#include "utils.hlsli"

#define SKYOCC_REDUCE_RS \
    "CBV(b0)," \
    "DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(UAV(u0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"
#define SKYOCC_FAR_RS \
    "CBV(b0)," \
    "DescriptorTable(SRV(t0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(UAV(u0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"
#define SKYOCC_TRACE_RS \
    "CBV(b0)," \
    "DescriptorTable(SRV(t0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(UAV(u0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP)"

// Mirrors SkyOcclusionConstants (SceneRenderer_SkyOcclusion.cpp).
cbuffer SkyOcclusionCB : register(b0)
{
    float4x4 invView;        // CSTrace: the camera (jittered, as the depth it reads)
    float4x4 invProj;
    float4x4 topViewProj;    // CSReduce: world -> the top-down renders' clip (the objects' top and the terrain)
    float4x4 bottomViewProj; // ...and the bottom-up one
    float4 mapParams;        // x the map's min world x, y its min world z, z 1 / its width (m), w the near level's texel (m)
    float4 heightParams;     // x the top camera's height, y the bottom camera's, z their near plane, w far - near
    uint4 sizes;             // x, y CSTrace's output (half resolution); z the depth renders' size; w the level written
    float4 traceParams;      // x reach (m), y opacity (a slab's coverage -> its opacity), z where the slabs start (m),
                             // w the receiver's lift off its surface (m)
    float4 fadeParams;       // the map's edge (SkyOcclusionMap::FadeParams): x, y the camera's world x, z; z the horizontal
                             // distance from it where the fade starts (m); w 1 / the fade's width (m)
};

// What a texel holds where nothing stands on the ground: no ground (the sea) -- a direction going down goes on.
static const float kNoGround = -1000.0f;
// The depth renders' clear: nothing drawn.
static const float kEmptyDepth = 0.99999f;

// One entry point per material (SKYOCC_PASS 0 / 1 / 2 = CSReduce / CSReduceFar / CSTrace): each table starts at t0 / u0,
// which is how RecordComputeDispatch's tables are found (Material::RootParameterInfo), so their resources would overlap.
#ifndef SKYOCC_PASS
#define SKYOCC_PASS 2
#endif

#if SKYOCC_PASS == 0
// ---- CSReduce: the three depth renders -> the near level ----------------------------------------------------------

Texture2D<float> TopDepth : register(t0);
Texture2D<float> BottomDepth : register(t1);
Texture2D<float> TerrainDepth : register(t2);
RWTexture2D<float4> LevelOut : register(u0);

int2 DepthTexel(float4x4 viewProj, float2 xz)
{
    // Orthographic: w = 1. Row 0 of the render is its top edge (ndc y +1).
    const float4 clip = mul(float4(xz.x, 0.0f, xz.y, 1.0f), viewProj);
    const float2 uv = float2(clip.x * 0.5f + 0.5f, 0.5f - clip.y * 0.5f);
    return clamp(int2(floor(uv * float(sizes.z))), int2(0, 0), int2(sizes.z, sizes.z) - 1);
}

[numthreads(8, 8, 1)]
[RootSignature(SKYOCC_REDUCE_RS)]
void CSReduce(uint3 dtid : SV_DispatchThreadID)
{
    uint levelSize, levelSizeY;
    LevelOut.GetDimensions(levelSize, levelSizeY);
    if (dtid.x >= levelSize || dtid.y >= levelSizeY)
    {
        return;
    }
    const uint sub = max(sizes.z / levelSize, 1u);
    const float fine = 1.0f / (mapParams.z * float(sizes.z)); // a depth texel, in metres
    float covered = 0.0f;
    float top = -1.0e9f;
    float bottom = 1.0e9f;
    float groundSum = 0.0f;
    float groundCount = 0.0f;
    for (uint j = 0; j < sub; ++j)
    {
        for (uint i = 0; i < sub; ++i)
        {
            // Texel (i, j) of this level's texel, in the level's convention: x east, z north, row 0 at the min z.
            const float2 xz = mapParams.xy + (float2(dtid.xy * sub + uint2(i, j)) + 0.5f) * fine;
            const float dTer = TerrainDepth.Load(int3(DepthTexel(topViewProj, xz), 0));
            float ground = kNoGround;
            if (dTer < kEmptyDepth)
            {
                ground = heightParams.x - (heightParams.z + dTer * heightParams.w);
                groundSum += ground;
                groundCount += 1.0f;
            }
            const float dTop = TopDepth.Load(int3(DepthTexel(topViewProj, xz), 0));
            if (dTop >= kEmptyDepth)
            {
                continue;
            }
            const float hTop = heightParams.x - (heightParams.z + dTop * heightParams.w);
            const float dBottom = BottomDepth.Load(int3(DepthTexel(bottomViewProj, xz), 0));
            // Its lowest surface seen from below; none (a closed mesh standing on the ground, a trunk, a single-sided
            // roof's top) = solid down to the ground, or a 1 m slab over no ground at all.
            float hBottom = dBottom < kEmptyDepth ? heightParams.y + (heightParams.z + dBottom * heightParams.w)
                                                  : (ground > kNoGround ? ground : hTop - 1.0f);
            hBottom = min(max(hBottom, ground), hTop);
            covered += 1.0f;
            top = max(top, hTop);
            bottom = min(bottom, hBottom);
        }
    }
    const float c = covered / float(sub * sub);
    LevelOut[dtid.xy] = float4(c, covered > 0.0f ? c * top : 0.0f, covered > 0.0f ? c * bottom : 0.0f,
                               groundCount > 0.0f ? groundSum / groundCount : kNoGround);
}

#elif SKYOCC_PASS == 1
// ---- CSReduceFar: the near level -> the far one (4 x 4 each) --------------------------------------------------------

Texture2D<float4> NearLevel : register(t0);
RWTexture2D<float4> LevelOut : register(u0);

[numthreads(8, 8, 1)]
[RootSignature(SKYOCC_FAR_RS)]
void CSReduceFar(uint3 dtid : SV_DispatchThreadID)
{
    uint levelSize, levelSizeY;
    LevelOut.GetDimensions(levelSize, levelSizeY);
    if (dtid.x >= levelSize || dtid.y >= levelSizeY)
    {
        return;
    }
    uint nearSize, nearSizeY;
    NearLevel.GetDimensions(nearSize, nearSizeY);
    const uint sub = max(nearSize / levelSize, 1u);
    float3 sum = 0.0f.xxx; // coverage and the premultiplied heights average as they are
    float groundSum = 0.0f;
    float groundCount = 0.0f;
    for (uint j = 0; j < sub; ++j)
    {
        for (uint i = 0; i < sub; ++i)
        {
            const float4 t = NearLevel.Load(int3(dtid.xy * sub + uint2(i, j), 0));
            sum += t.xyz;
            if (t.w > kNoGround * 0.5f)
            {
                groundSum += t.w;
                groundCount += 1.0f;
            }
        }
    }
    LevelOut[dtid.xy] = float4(sum / float(sub * sub), groundCount > 0.0f ? groundSum / groundCount : kNoGround);
}

#else
// ---- CSTrace: the sky visibility at half resolution -----------------------------------------------------------------

SamplerState gSmp : register(s0);
Texture2D<float> SceneDepth : register(t0);
Texture2D<float4> SceneGb1 : register(t1);
Texture2D<float4> MapNear : register(t2);
Texture2D<float4> MapFar : register(t3);
RWTexture2D<float4> SkyVisOut : register(u0);

// The directions: cosine-distributed about the normal, the same set at every pixel (an R2 sequence), so the answer is
// a smooth function of the point and needs no history. The steps: quadratic in distance, the first at reach / 64.
static const uint kDirections = 12u;
static const uint kSteps = 8u;
// From this step on the march reads the far level (4 m texels): where its footprint has outgrown the near one's.
static const uint kFarFromStep = 5u;
// Below this a slab's thickness is not trusted (a roof drawn as one sheet has top = bottom).
static const float kMinSlab = 0.25f;

[numthreads(8, 8, 1)]
[RootSignature(SKYOCC_TRACE_RS)]
void CSTrace(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= sizes.x || dtid.y >= sizes.y)
    {
        return;
    }
    uint dw, dh;
    SceneDepth.GetDimensions(dw, dh);
    // The full-resolution pixel under this half-resolution texel's centre (CSTrace's / GTAO's convention).
    const float2 uv = (float2(dtid.xy) + 0.5f) / float2(sizes.xy);
    const int2 px = min(int2(uv * float2(dw, dh)), int2(dw, dh) - 1);
    const float z = SceneDepth.Load(int3(px, 0));
    if (z <= 1.0e-6f)
    {
        SkyVisOut[dtid.xy] = 0.0f.xxxx; // the sky itself: .w 0
        return;
    }
    const float3 N = normalize(SceneGb1.Load(int3(px, 0)).rgb * 2.0f - 1.0f);
    const float3 P = ReconstructPosWS(uv, z, invProj, invView);
    const float3 origin = P + N * traceParams.w;

    const float3 up = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    const float3 T = normalize(cross(up, N));
    const float3 B = cross(N, T);

    const float reach = traceParams.x;
    const float opacity = traceParams.y;
    const float slabStart = traceParams.z;
    // The map's edge: the occlusion fades out with the horizontal distance from the camera, the open sky beyond -- a
    // gradient round the camera instead of the map's square in the world. Inside the fade's end every step of every
    // direction lies on the map (FadeParams), so the march's own "off the map" below is only reached past it.
    const float edge = 1.0f - smoothstep(0.0f, 1.0f, (length(P.xz - fadeParams.xy) - fadeParams.z) * fadeParams.w);
    float3 unoccluded = 0.0f.xxx;
    [loop]
    for (uint k = 0; k < kDirections; ++k)
    {
        const float u1 = frac(0.5f + float(k) * 0.7548776662f);
        const float u2 = frac(0.5f + float(k) * 0.5698402910f);
        const float r = sqrt(u1);
        const float phi = 2.0f * kPi * u2;
        const float3 d = normalize(T * (r * cos(phi)) + B * (r * sin(phi)) + N * sqrt(saturate(1.0f - u1)));

        float transmittance = 1.0f;
        float tPrev = 0.0f;
        [loop]
        for (uint s = 1u; s <= kSteps && edge > 0.0f; ++s)
        {
            const float f = float(s) / float(kSteps);
            const float t = reach * f * f;
            const float3 a = origin + d * tPrev;
            const float3 b = origin + d * t;
            const float3 m = 0.5f * (a + b);
            const float dt = t - tPrev;
            tPrev = t;
            const float2 mapUv = (m.xz - mapParams.xy) * mapParams.z;
            if (any(mapUv < 0.0f) || any(mapUv > 1.0f))
            {
                break; // off the map: the open sky beyond it
            }
            const float4 cell = (s >= kFarFromStep) ? MapFar.SampleLevel(gSmp, mapUv, 0) : MapNear.SampleLevel(gSmp, mapUv, 0);
            const float lo = min(a.y, b.y);
            const float hi = max(a.y, b.y);
            const float span = max(hi - lo, 1.0e-3f);
            // The ground: the share of the segment's height range under it ends that much of the direction -- a ramp,
            // not a step, so the answer stays smooth as the point moves.
            if (cell.w > kNoGround * 0.5f)
            {
                transmittance *= 1.0f - saturate((cell.w - lo) / span);
            }
            // A slab: exp(-sigma x the segment's length inside it), sigma such that straight down through it passes
            // 1 - coverage x opacity. Not before `slabStart`: nearer than that the receiver's own texel is a slab around
            // it (a trunk's column, a tent's), which GTAO's contact radius answers instead.
            if (t > slabStart && cell.x > 1.0e-3f)
            {
                const float c = cell.x;
                const float slabTop = cell.y / c;
                const float slabBottom = cell.z / c;
                const float inside = (hi - lo > 1.0e-3f) ? saturate((min(hi, slabTop) - max(lo, slabBottom)) / span)
                                                        : ((m.y > slabBottom && m.y < slabTop) ? 1.0f : 0.0f);
                if (inside > 0.0f)
                {
                    const float sigma = -log(1.0f - min(c * opacity, 0.98f)) / max(slabTop - slabBottom, kMinSlab);
                    transmittance *= exp(-sigma * dt * inside);
                }
            }
            if (transmittance < 1.0e-3f)
            {
                transmittance = 0.0f;
                break;
            }
        }
        unoccluded += d * lerp(1.0f, transmittance, edge);
    }
    // UE's BentNormalNormalizeFactor for cosine-distributed directions (rt_sky_visibility_cs.hlsl CSTrace).
    float3 bent = unoccluded * (1.5f / float(kDirections));
    const float bentLength = length(bent);
    if (bentLength > 1.0f) { bent /= bentLength; }
    SkyVisOut[dtid.xy] = float4(bent, z);
}
#endif
