// Volumetric fog, pass 1 of 2: per froxel cell, the medium and the light scattered towards the
// camera -- UE's MaterialSetupCS folded into LightScatteringCS (VolumetricFog.usf:122-140 and
// :766-1000). Plan part A (docs/volumetric_fog_sky_clouds_ssgi_plan.md).
//
// Per cell (jittered inside it, UE's Halton offsets + optional history):
//   sigma  = extinction of the analytic height fog at the cell's height (fog_common.hlsli)
//   S      = albedo * sigma
//   L      = sunColor * shadow(P) * HG(g, dot(toSun, -V)) * sunScatter          (:888)
//          + skyIrradiance(-V) * skyScatter * skyVisibility(P)                  (:1013-1039, SH -> our cube)
//   out    = (preExposure * L * S, sigma), lerped with the reprojected history (:1027-1033)
// The sun's shadow is the lighting pass's own sampler (lighting_cb.hlsli maps the same b0): CSM
// atlas or the VSM clipmap, single tap, no receiver biases beyond the normal offset towards the
// sun and no contact shadows -- a volume has no surface.
//
// b0 = the lighting cbuffer (lighting_cb.hlsli, the same allocation the lighting pass reads)
// b1 = FogCB (fog_common.hlsli)
// t0 ShadowAtlas  t1 VsmPageTable  t2 VsmPool  t3 SkyIrradiance (cube)  t4 FogHistory (3D, prev slot)
// t5 HzbFurthest (this frame)  t6 PrevHzbFurthest (last frame) -- conservative depth (plan A3)
// t7 SpotLights  t8 PointLights  t9 SpotShadowAtlas  t10 PointShadowAtlas -- local lights (plan A4)
// t15 the scene TLAS -- the sky's visibility (FOG_SKY_OCCLUSION, compiled on RT hardware only)
// u0 FogScatter (3D)
// s0 point clamp  s1 comparison linear  s2 linear wrap  s3 linear clamp (the lighting pass's set)
#pragma pack_matrix(row_major)
#include "utils.hlsli"
#include "vsm_sample.hlsli"
#include "csm_sample.hlsli"
#include "fog_common.hlsli"
#include "rt_lights.hlsli" // SpotLightData / PointLightData (the light passes' layout)
#include "local_shadow_bias.hlsli" // plan S1: the point light's receiver bias, the light pass's own
#include "local_shadow_filter.hlsli" // plan S4: a light's square of its atlas slice, the point faces' addressing
#include "cloud_shadow_common.hlsli" // plan C3: the cloud's shadow on the sun in the air

#define FOG_SCATTER_RS \
    "CBV(b0), CBV(b1), " \
    "DescriptorTable(SRV(t0, numDescriptors=16, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(Sampler(s0, numDescriptors=4, flags=DESCRIPTORS_VOLATILE))"

FOG_CB(b1)

Texture2D               ShadowAtlas   : register(t0);
StructuredBuffer<uint>  VsmPageTable  : register(t1);
Texture2D               VsmPool       : register(t2);
TextureCube             SkyIrradiance : register(t3);
Texture3D<float4>       FogHistory    : register(t4);
Texture2D<float>        HzbFurthest   : register(t5); // this frame's furthest depth pyramid (min reverse-Z)
Texture2D<float>        PrevHzbFurthest : register(t6); // last frame's (the history's conservative depth)
StructuredBuffer<SpotLightData>  SpotLights      : register(t7);  // plan A4: the light passes' buffers
StructuredBuffer<PointLightData> PointLights     : register(t8);
Texture2DArray                   SpotShadowAtlas : register(t9);  // Legacy local shadows
Texture2DArray                   PointShadowAtlas : register(t10); // 6 faces per light, a 2D array since plan S4
Texture2D<float4>       DistantSkyLight : register(t11); // B5 raw isotropic luminance at 6 km
Texture2D<float4>       CloudShadowMap  : register(t12); // C3 cloud shadow map (dummy without clouds)
// t13: S15, this frame's SDSM partitions (placeholder SRV when the mode is not SDSM).
StructuredBuffer<SdsmPartition> SdsmPartitions : register(t13);
// Plan S7, the direct read: the local shadow cache's pool -- a light whose atlas page is negative is read there (page
// -page - 1, the atlas layout). A Legacy atlas stands in while there is no pool.
Texture2DArray LocalShadowPool : register(t14);
#ifndef FOG_SKY_OCCLUSION
#define FOG_SKY_OCCLUSION 0
#endif
#if FOG_SKY_OCCLUSION
#include "rt_geometry.hlsli" // RtVisibilityBlocked, RtWangHash
RaytracingAccelerationStructure FogTlas : register(t15); // a placeholder while fogLocal.w is 0 (never traced)
#endif
RWTexture3D<float4>     FogScatter    : register(u0);

// S15: AFTER the resource declarations, not before. MakeCsmParams reads SdsmPartitions, so the
// resource has to exist by the time this header is expanded -- which is the order lighting_cs.hlsl
// has always had, and the only reason this include sat above was that nothing forced it down.
#include "lighting_cb.hlsli"

SamplerState            gSmpPoint       : register(s0);
SamplerComparisonState  gSmpLinear      : register(s1);
SamplerState            gSmpLinearWrap  : register(s2);
SamplerState            gSmpLinearClamp : register(s3);

static const uint kFogHistoryValid = 1u;
static const uint kFogJitterOn = 2u;
static const uint kFogConservativeDepth = 4u;
static const uint kFogTemporalOn = 8u;
static const uint kFogDepthConstraint = 32u; // FogApplyDepthConstraint (bit 4 = the distant sky ambient)
// R3 low-discrepancy sequence (Roberts): the extra cell offsets of a history-miss supersample.
static const float3 kFogR3 = float3(0.8191725134f, 0.6710436067f, 0.5497004779f);

// Plan S9e: the group's local lights. The group's 4x4 cells x 4 slices share one world-space box -- the AABB of its
// frustum chunk's eight corners, which holds every sample of every cell (the offsets lie in [0, 1]) -- and a light
// whose range sphere misses the box would `continue` at the loops' range test in every cell of it. Walking only the
// lights that touch it, in index order, sums the same terms in the same order; past kFogTileLights lights the loops
// walk all of them, as before. The light passes cull per 8x8 pixel tile the same way (pointlight_cs.hlsl).
static const uint kFogTileLights = 1024u;
static const uint kFogTileWords = kFogTileLights / 32u;
static const uint kFogLightWalkCeiling = 65536u; // literal loop bound (engine rule); the counts only shorten it
groupshared uint gsFogSpotMask[kFogTileWords];
groupshared uint gsFogPointMask[kFogTileWords];
groupshared float3 gsFogTileMin;
groupshared float3 gsFogTileMax;

bool FogSphereTouchesTile(float3 centre, float radius)
{
    // A hair of slack over the radius: the corners and the samples are the same math on different offsets.
    const float reach = radius * 1.001f + 1e-3f;
    const float3 dd = centre - clamp(centre, gsFogTileMin, gsFogTileMax);
    return radius > 1e-6f && dot(dd, dd) <= reach * reach;
}

// Every thread of the group calls it (barriers), before any of them returns.
void BuildFogTileLights(uint groupIndex, uint3 groupId)
{
    if (groupIndex < kFogTileWords)
    {
        gsFogSpotMask[groupIndex] = 0u;
        gsFogPointMask[groupIndex] = 0u;
    }
    if (groupIndex == 0u)
    {
        const uint3 base = groupId * 4u;
        float3 lo = 1e30f.xxx;
        float3 hi = -1e30f.xxx;
        [unroll]
        for (uint c = 0u; c < 8u; ++c)
        {
            const uint3 corner = base + uint3((c & 1u) ? 4u : 0u, (c & 2u) ? 4u : 0u, (c & 4u) ? 4u : 0u);
            const float3 P = FogCellWorldPosition(corner, 0.0f.xxx, fogGridSize, fogGridZParams, fogProjZ, fogInvViewProjNoJitter);
            lo = min(lo, P);
            hi = max(hi, P);
        }
        gsFogTileMin = lo;
        gsFogTileMax = hi;
    }
    GroupMemoryBarrierWithGroupSync();
    [loop]
    for (uint si = groupIndex; si < kFogTileLights; si += 64u)
    {
        if (si >= fogLocal.x) { break; }
        if (FogSphereTouchesTile(SpotLights[si].positionRange.xyz, SpotLights[si].positionRange.w))
        {
            InterlockedOr(gsFogSpotMask[si >> 5u], 1u << (si & 31u));
        }
    }
    [loop]
    for (uint pi = groupIndex; pi < kFogTileLights; pi += 64u)
    {
        if (pi >= fogLocal.y) { break; }
        if (FogSphereTouchesTile(PointLights[pi].position, PointLights[pi].radius))
        {
            InterlockedOr(gsFogPointMask[pi >> 5u], 1u << (pi & 31u));
        }
    }
    GroupMemoryBarrierWithGroupSync();
}

// The k-th light of a loop: k itself when the loop walks every light, else the next set bit of the group's mask
// (`word` / `bits` carry the walk). False = no more lights.
bool FogNextTileLight(bool spots, bool walkAll, uint count, uint k, inout uint word, inout uint bits, out uint index)
{
    index = k;
    if (walkAll) { return k < count; }
    [loop]
    while (bits == 0u && word + 1u < kFogTileWords)
    {
        ++word;
        bits = spots ? gsFogSpotMask[word] : gsFogPointMask[word];
    }
    if (bits == 0u) { return false; }
    index = word * 32u + firstbitlow(bits);
    bits &= bits - 1u;
    return true;
}

// Sun visibility at a point IN THE AIR: the lighting pass's CSM / VSM path, minus everything that
// belongs to a receiver surface (receiver-plane bias, contact shadows, SMRT). N = the direction
// to the sun and ndl = 1 make the normal-offset terms a small lift towards the light -- harmless
// in a volume, and it keeps one sampler for both consumers.
float FogSunShadow(float3 P)
{
    // Plan C3: the cloud's shadow on the air (UE VolumetricFog.usf:874-880 multiply the same
    // GetCloudVolumetricShadow into the directional light's scattering).
    const float cloud = cloudShadowParams.x != 0.0f
        ? CloudShadowTransmittance(P, cloudShadowViewProj, cloudShadowParams.y, CloudShadowMap, gSmpLinearClamp)
        : 1.0f;
    const float3 dirToLight = normalize(-sunDirWS);
    if (useVsm != 0u)
    {
        VsmSmrtParams smrt = (VsmSmrtParams)0; // rayCount 0 = the single-tap SampleCmp path
        return cloud * VsmClipmapShadow(P, dirToLight, camPosWS, clipmapNormalBias, vsmDepthBias,
                                        clipmapDepthBiasDecay, clipmapDepthBiasFloorNdc, clipmapBlendWidth,
                                        invProj._11, clipmapUvNormal, dirToLight, smrt, 0.0f,
                                        clipmapViewProj, VsmPageTable, VsmPool, gSmpLinear);
    }
    int cascade;
    return cloud * CsmSampleShadow(MakeCsmParams(), ShadowAtlas, gSmpLinear, gSmpPoint, P, dirToLight, 1.0f, cascade);
}

// The light passes' LightDistanceAttenuation (utils.hlsli) with UE's volumetric distance bias in
// place of its fixed +1: the same smooth range window, 1 / (d^2 + bias^2) inside it.
float FogLocalDistanceAttenuation(float dist, float range, float distanceBiasSqr)
{
    const float invRange = (range > 1e-6f) ? (1.0f / range) : 0.0f;
    const float t = saturate(dist * dist * invRange * invRange);
    const float w = saturate(1.0f - t * t);
    return (w * w) / (dist * dist + distanceBiasSqr);
}

// Local-light shadows at a point IN THE AIR (plan A4): the light passes' samplers minus the
// receiver-normal terms (a volume has no surface). The sample point is pushed ALONG the light ray
// by two shadow texels at the camera-selected VSM level (the light passes' slope-scaled depth push
// with the slope at 1), which is what keeps a cell next to a caster from shadowing itself.
float FogSpotShadow(SpotLightData light, float3 P, float3 toL, float distToLight)
{
    if (light.shadowParams.y < 0.0f) { return 1.0f; } // no shadow slot this frame
    if (fogLocal.z != 0u)
    {
        const uint  lvl        = VsmSelectLevel(length(P - camPosWS), fogLocalParams.w, VSM_MAX_LEVEL);
        const float cosOuter   = max(light.directionCosOuter.w, 1e-3f);
        const float tanOuter   = sqrt(saturate(1.0f - cosOuter * cosOuter)) / cosOuter;
        const float texelWorld = (2.0f * distToLight * tanOuter / VSM_VIRTUAL_RES) * exp2((float)lvl);
        const float3 Pv = P + toL * (texelWorld * 2.0f);
        return VsmSpotShadow((uint)light.shadowParams4.z, light.viewProj, Pv, camPosWS, fogLocalParams.w, 0.0f, // S9e: its view
                             VsmPageTable, VsmPool, gSmpLinear);
    }
    float4 clip = mul(float4(P, 1.0f), light.viewProj);
    // The TEXEL bias (local_shadow_bias.hlsli): no surface here, so the cell moves toward the light only, by the normal
    // and the push texels together -- the VSM branch above does the same with its own two texels.
    if (light.shadowParams2.w > 0.0f)
    {
        const float cosOuter = max(light.directionCosOuter.w, 1e-3f);
        const float tanOuter = sqrt(saturate(1.0f - cosOuter * cosOuter)) / cosOuter;
        const float res = max(light.shadowAtlas.z / max(fogLocalParams.y, 1e-9f), 1.0f);
        const float texelWorld = 2.0f * max(clip.w, 0.0f) * tanOuter / res;
        clip = mul(float4(P + toL * (texelWorld * (light.shadowParams2.w + light.shadowParams2.x)), 1.0f), light.viewProj);
    }
    const float3 ndc = clip.xyz / max(clip.w, 1e-6f);
    const float2 uv = ndc.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
    const float depth = SpotReceiverDepth(clip, light.shadowParams3.x, light.shadowParams4.y); // plan S4: the atlas's encoding
    if (any(uv < 0.0f) || any(uv > 1.0f) || depth <= 0.0f || depth >= 1.0f) { return 1.0f; }
    // Plan S9: its square of an atlas page; fogLocalParams.y is one page texel. Plan S7: a negative page is the pool's.
    const float2 uvRect = LocalShadowAtlasUV(uv, light.shadowAtlas.xy, light.shadowAtlas.zz, fogLocalParams.yy, 0.5f);
    const bool fromPool = light.shadowAtlas.w < 0.0f;
    const float3 tap = float3(uvRect, fromPool ? -light.shadowAtlas.w - 1.0f : light.shadowAtlas.w);
    const float lit = fromPool ? LocalShadowPool.SampleCmpLevelZero(gSmpLinear, tap, depth - light.shadowParams.w)
                               : SpotShadowAtlas.SampleCmpLevelZero(gSmpLinear, tap, depth - light.shadowParams.w);
    return lerp(1.0f, lit, light.shadowParams2.z); // plan S3: UE's shadow fade, as the spot pass's
}

float FogPointShadow(PointLightData light, float3 P, float3 toL, float distToLight)
{
    if (light.shadowParams.x < 0.0f) { return 1.0f; }
    if (fogLocal.z != 0u)
    {
        const uint  lvl        = VsmSelectLevel(length(P - camPosWS), fogLocalParams.w, VSM_MAX_LEVEL);
        const float texelWorld = (2.0f * distToLight / VSM_VIRTUAL_RES) * exp2((float)lvl);
        const float3 Pv = P + toL * (texelWorld * 2.0f);
        return VsmPointShadow((uint)light.shadowParams4.z, Pv, light.position, light.shadowParams.z, // S9e: first view
                              light.shadowParams.w, camPosWS, fogLocalParams.w, 0.0f,
                              VsmPageTable, VsmPool, gSmpLinear);
    }
    // pointlight_cs.hlsl's cube compare: the face's view-space depth with its bias (no surface: NoL = 1).
    float3 d = P - light.position;
    if (light.shadowFade.w > 0.0f) // the TEXEL bias: toward the light by the normal + push texels (no surface here)
    {
        const float texelWorld = 2.0f * max(abs(d.x), max(abs(d.y), abs(d.z))) * light.shadowFade.z;
        d += toL * (texelWorld * (light.shadowFade.w + light.volumetric.y));
    }
    const float m = max(abs(d.x), max(abs(d.y), abs(d.z)));
    const float nearP = light.shadowParams.z;
    const float farP = max(light.shadowParams.w, nearP + 1e-3f);
    const float mBiased = PointShadowCompareDistance(m, light.shadowParams, light.volumetric.y, 1.0f);
    const float zc = PointShadowCompareDepth(mBiased, nearP, farP, light.shadowParams4.y); // plan S4
    // Plan S9: the light's face in its 3f x 2f block of an atlas page (PointShadowCubeUV); a page texel = one face / res.
    // Plan S7: a negative page is the block's in the cache pool.
    const bool fromPool = light.shadowFade.y < 0.0f;
    const float page = fromPool ? -light.shadowFade.y - 1.0f : light.shadowFade.y;
    const float3 tap = PointShadowCubeUV(d, light.shadowAtlas, page, light.shadowAtlas.zw * light.shadowFade.z);
    const float lit = fromPool ? LocalShadowPool.SampleCmpLevelZero(gSmpLinear, tap, zc)
                               : PointShadowAtlas.SampleCmpLevelZero(gSmpLinear, tap, zc);
    return lerp(1.0f, lit, light.shadowFade.x); // plan S3: UE's shadow fade, as the point pass's
}

// The tile's farthest depth (min reverse-Z) over the 3x3 tile neighbourhood: the integrated volume is
// read with a trilinear filter, so a screen ray in column c blends columns c-1..c+1 -- a column culled
// against ITS OWN tile alone stops accumulating at its floor and the sky pixels next to it read a
// dark, flickering halo (docs/bug_fog_spot_flicker.md, defect 1). Culling only against the farthest
// surface of the whole footprint keeps every column a filtered read can touch fully lit. Beyond the
// pyramid Load returns 0 = the far plane, which never culls.
float FogTileFarthest(Texture2D<float> pyramid, int2 tile, uint mip)
{
    float farthest = 1.0f;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            farthest = min(farthest, pyramid.Load(int3(tile + int2(dx, dy), (int)mip)).r);
        }
    }
    return farthest;
}

// How much of the sky a point in the air sees: UE ComputeSkyVisibility (VolumetricFog.usf:713-735) traces "one 45 degree
// cone straight up" against the global distance field and multiplies the skylight's scattering by it (:1038-1039); with
// Lumen the translucency volume carries the same occlusion. Here the cone is `fogLocal.w` rays spread uniformly over its
// solid angle against the TLAS -- the shadow casters, a leaf card solid, as every visibility ray (rt_geometry.hlsli) --
// out to `fogLocalParams2.z` metres (the sky visibility's reach, skyVis.maxDistance), and the share that gets through.
// Rotated per cell, sample and frame: the fog's history (fogJitter.w) is what averages the few rays.
// Without it the air of a tunnel glowed with the whole open sky: a lit box of haze from the fog's start distance on,
// in front of walls the sky never reaches (2026-10-26, owner: "what is this rectangle in the centre").
float FogSkyVisibility(float3 P, uint3 coord, uint sampleIndex)
{
#if FOG_SKY_OCCLUSION
    const uint rays = min(fogLocal.w, 8u);
    if (rays == 0u) { return 1.0f; }
    const uint seed = RtWangHash(coord.x * 1973u + coord.y * 9277u + coord.z * 26699u + fogMisc.z * 7919u + sampleIndex * 104729u);
    const float2 rotation = float2(float(seed & 0xFFFFu), float(seed >> 16)) * (1.0f / 65536.0f);
    const float cosHalfAngle = 0.70710678f; // UE: tan(PI / 4), a 45-degree half angle
    RayDesc ray;
    ray.Origin = P;
    ray.TMin = 0.0f;
    ray.TMax = max(fogLocalParams2.z, 0.5f);
    uint open = 0u;
    [loop]
    for (uint k = 0u; k < 8u; ++k)
    {
        if (k >= rays) { break; }
        const float u1 = frac(rotation.x + float(k) * 0.7548776662f);
        const float u2 = frac(rotation.y + float(k) * 0.5698402910f);
        const float cosTheta = 1.0f - u1 * (1.0f - cosHalfAngle); // uniform over the cone's solid angle
        const float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
        const float phi = 2.0f * kPi * u2;
        ray.Direction = float3(sinTheta * cos(phi), cosTheta, sinTheta * sin(phi)); // +Y is up
        if (!RtVisibilityBlocked(FogTlas, ray, 0x02u)) { ++open; } // rt::kInstanceMaskShadow
    }
    return float(open) / float(rays);
#else
    return 1.0f;
#endif
}

// UE ApplyDepthConstraintsToOffset (VolumetricFog.usf:87-114; r.VolumetricFog.GridCenterOffsetFromDepthBuffer 0.5,
// r.VolumetricFog.OffsetThresholdToAcceptDepthBufferOffset 1): a sample less than `fogDepthParams.y` slices behind the
// surface its own pixel sees is moved to `fogDepthParams.x` slices in front of it; a sample farther behind stays (pulling
// every hidden sample forward would light the edges of meshes wrongly, their comment -- ours does pull every one by
// default, fog.depthThreshold 128: SceneFrameData.h fogDepthThreshold says why and what it measured). UE applies it to the medium
// and to every lighting sample, as here. The surface is this frame's furthest pyramid at mip 0 (the farthest of a
// 2x2 of render pixels; UE reads the scene depth itself) -- the same on a wall, a hair farther at a silhouette.
// Without it a cell straddling a thin wall carried the light of the air BEHIND the wall -- the open sky, the sun past
// the roof -- into the air in front of it, and in a tunnel each slice's cells along the walls lit a ring: concentric
// bands, one per slice (2026-10-26, once the sky term was occluded and the uniform haze no longer hid them).
void FogApplyDepthConstraint(uint3 coord, inout float3 cellOffset)
{
    if ((fogFlags & kFogDepthConstraint) == 0u) { return; }
    uint w, h, mips;
    HzbFurthest.GetDimensions(0, w, h, mips);
    const float2 uv = (float2(coord.xy) + cellOffset.xy) / float2(fogGridSize.xy); // the cell's uv IS its screen uv
    const int2 texel = clamp(int2(uv * float2(w, h)), int2(0, 0), int2(w, h) - 1);
    const float surfaceZ = HzbFurthest.Load(int3(texel, 0)).r;
    if (surfaceZ <= 0.0f) { return; } // the sky: nothing to stay in front of
    const float surfaceSlice = FogSliceFromDepth(fogGridZParams, FogViewDepthFromDeviceZ(fogProjZ, surfaceZ)) - fogDepthParams.x;
    const float toSurface = surfaceSlice - (float(coord.z) + cellOffset.z); // < 0: the sample is behind it
    if (toSurface < 0.0f && -toSurface < fogDepthParams.y)
    {
        cellOffset.z += toSurface;
    }
}

// One lighting sample of a cell at `cellOffset` inside it: (preExposure * L * scattering, sigma).
float4 FogSampleCell(uint3 coord, float3 cellOffset, float3 distantAmbient, uint sampleIndex)
{
    float viewDepth;
    const float3 P = FogCellWorldPosition(coord, cellOffset, fogGridSize, fogGridZParams,
                                          fogProjZ, fogInvViewProjNoJitter, viewDepth);
    const float3 toCell = P - camPosWS;
    const float dist = length(toCell);
    const float3 V = dist > 1.0e-4f ? toCell / dist : float3(0.0f, 0.0f, 1.0f); // camera -> cell

    // The medium (UE MaterialSetupCS): extinction from the analytic height fog at this height,
    // scattering = albedo * extinction.
    const float sigma = FogExtinctionAt(P.y, fogMedium0, fogMedium1.y);
    const float3 scattering = fogMedium1.x * sigma;
    const float g = fogMedium1.z;

    float3 L = 0.0f.xxx;
    // Sun: colour * shadow * phase (UE :888, PhaseFunction(g, dot(L, -CameraVector)) with L TO the
    // light and CameraVector = camera -> cell). Their HenyeyGreensteinPhase is written with
    // +2g*cos (ParticipatingMediaCommon.ush:96), so their forward peak sits at cos = -1; ours is
    // the textbook -2g*cos form (peak at +1), hence the argument is negated: dot(toSun, V) is +1
    // when the camera looks INTO the sun, which is where forward scattering (g > 0) must peak.
    // No sun (a closed cloud deck), no shadow lookup: its passes did not run (lighting_cs.hlsl).
    if (fogMedium1.w > 0.0f && any(lightRgb > 0.0f))
    {
        const float3 toSun = normalize(-sunDirWS);
        const float shadow = FogSunShadow(P);
        L += lightRgb * fogMedium1.w * shadow * FogPhaseHG(g, dot(toSun, V));
    }
    // Sky: UE evaluate the skylight SH at -V * g; the irradiance cube in the direction the air
    // is looked THROUGH is the same term without the SH's directional damping (delta, plan Â§3).
    // Times the share of the sky the cell sees (FogSkyVisibility, UE :1038-1039).
    if (fogMedium2.x > 0.0f && (fogFlags & 16u) != 0u)
    {
        // B5 already integrates the full sphere with uniform phase: no extra PI/HG or HDRI scale.
        L += distantAmbient * FogSkyVisibility(P, coord, sampleIndex);
    }
    else if (fogMedium2.x > 0.0f && skyIrradianceEnabled != 0u)
    {
        L += SkyIrradiance.SampleLevel(gSmpLinearClamp, -V, 0).rgb * skyIrradianceScale * fogMedium2.x *
             FogSkyVisibility(P, coord, sampleIndex);
    }
    // Local lights (plan A4, UE :944-1030): every spot / point whose range reaches the cell, with
    // its shadow map -- of the lights whose range touches the cell's group (plan S9e, the masks
    // above), and the range test is the per-cell cull. The bias in
    // LightDistanceAttenuation's denominator is the singularity bias next to the source (UE
    // InverseSquaredLightDistanceBiasScale); the cone's soft edge is the light passes' squared
    // ramp (UE additionally soften it by the cell radius, LightSoftFading -- not done). Literal
    // loop bounds; the counts only shorten them.
    if (fogLocalParams.x > 0.0f)
    {
        // UE :951-956: the cell radii (to the diagonal neighbour in 3D and within the slice) size the
        // cone soft edge (LightSoftFading * the 2D radius) and the inverse-square bias
        // (max(radius * scale, 1)^2 added to d^2), so a cell straddling the cone edge or containing
        // the source is a blend, not a coin flip -- the stepped, flickering edge the user saw. UE's
        // floor of 1 is 1 cm (its units): 0.01 m here, as LightDistanceAttenuation's kLightDistanceBiasSqr
        // (it was 1 m, which dimmed every lamp's glow near the bulb).
        // From the UNJITTERED cell centre: a per-frame jitter in the bias / soft-fade sizes was one more
        // thing making a cell's light differ frame to frame (docs/bug_fog_spot_flicker.md, defect 2).
        const float3 Pc0 = FogCellWorldPosition(coord, 0.5f.xxx, fogGridSize, fogGridZParams, fogProjZ, fogInvViewProjNoJitter);
        const float3 Pn3 = FogCellWorldPosition(coord + uint3(1u, 1u, 1u), 0.5f.xxx, fogGridSize, fogGridZParams, fogProjZ, fogInvViewProjNoJitter);
        const float3 Pn2 = FogCellWorldPosition(coord + uint3(1u, 1u, 0u), 0.5f.xxx, fogGridSize, fogGridZParams, fogProjZ, fogInvViewProjNoJitter);
        const float cellRadius = length(Pc0 - Pn3);
        const float softFadeDistance = fogLocalParams2.x * length(Pc0 - Pn2);
        float distanceBiasSqr = max(cellRadius * fogLocalParams2.y, 0.01f);
        distanceBiasSqr *= distanceBiasSqr;
        const bool spotAll = fogLocal.x > kFogTileLights;
        uint spotWord = 0u;
        uint spotBits = gsFogSpotMask[0];
        [loop]
        for (uint sk = 0u; sk < kFogLightWalkCeiling; ++sk)
        {
            uint si;
            if (!FogNextTileLight(true, spotAll, fogLocal.x, sk, spotWord, spotBits, si)) { break; }
            const SpotLightData light = SpotLights[si];
            const float3 Lvec = light.positionRange.xyz - P;
            const float d = length(Lvec);
            if (d >= light.positionRange.w || light.positionRange.w <= 1e-6f) { continue; }
            const float3 Ldir = Lvec / max(d, 1e-6f);
            const float spotCos = dot(-Ldir, light.directionCosOuter.xyz);
            if (spotCos <= light.directionCosOuter.w) { continue; }
            float angleAtten = saturate((spotCos - light.directionCosOuter.w) * light.shadowParams.z);
            angleAtten *= angleAtten;
            // UE GetSpotLightVolumetricSoftFading (DeferredLightingCommon.ush:583): how far inside the
            // cone aperture the cell sits, faded over the soft distance.
            float softFade = 1.0f;
            if (softFadeDistance > 0.0f)
            {
                const float cosOuter = light.directionCosOuter.w;
                const float tanOuter = sqrt(saturate(1.0f - cosOuter * cosOuter)) / max(cosOuter, 1e-3f);
                const float3 toCell = -Lvec; // light -> cell
                const float along = dot(light.directionCosOuter.xyz, toCell);
                const float aperture = along * tanOuter;
                const float fromAxis = length(toCell - light.directionCosOuter.xyz * along);
                softFade = saturate((aperture - fromAxis) / softFadeDistance);
            }
            const float atten = FogLocalDistanceAttenuation(d, light.positionRange.w, distanceBiasSqr) * angleAtten * softFade;
            const float shadow = FogSpotShadow(light, P, Ldir, d);
            // shadowParams2.y = this light's VolumetricScatteringIntensity (plan A4d), times the global knob.
            L += light.colorIntensity.xyz * light.colorIntensity.w * atten * shadow * fogLocalParams.x * light.shadowParams2.y
                 * FogPhaseHG(g, dot(Ldir, V));
        }
        const bool pointAll = fogLocal.y > kFogTileLights;
        uint pointWord = 0u;
        uint pointBits = gsFogPointMask[0];
        [loop]
        for (uint pk = 0u; pk < kFogLightWalkCeiling; ++pk)
        {
            uint pi;
            if (!FogNextTileLight(false, pointAll, fogLocal.y, pk, pointWord, pointBits, pi)) { break; }
            const PointLightData light = PointLights[pi];
            const float3 Lvec = light.position - P;
            const float d = length(Lvec);
            if (d >= light.radius || light.radius <= 1e-6f) { continue; }
            const float3 Ldir = Lvec / max(d, 1e-6f);
            const float atten = FogLocalDistanceAttenuation(d, light.radius, distanceBiasSqr);
            const float shadow = FogPointShadow(light, P, Ldir, d);
            L += light.color * light.intensity * atten * shadow * fogLocalParams.x * light.volumetric.x * FogPhaseHG(g, dot(Ldir, V));
        }
    }
    return float4(fogMedium2.y * L * scattering, sigma); // pre-exposed, UE :1024
}

[numthreads(4, 4, 4)]
[RootSignature(FOG_SCATTER_RS)]
void CSMain(uint3 coord : SV_DispatchThreadID, uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    // Plan S9e: the group's light masks, before any thread returns (the builder's barriers). fogLocalParams.x is the
    // dispatch's constant, so the whole group takes the same side.
    if (fogLocalParams.x > 0.0f)
    {
        BuildFogTileLights(groupIndex, groupId);
    }
    if (any(coord >= fogGridSize)) { return; }

    // Conservative depth (UE :785-800): the furthest pyramid's texel over this cell's 16x16 pixels is
    // the FARTHEST surface any of them sees (min reverse-Z). If even that is in front of the cell's
    // near face -- half a voxel towards the camera, so a bilinear read of the cell still lands on
    // lit data -- nothing on screen looks through this cell: it stores nothing and the sampling
    // (the shadow lookups above all) is skipped. Beyond the pyramid (a grid column past its last
    // texel) Load returns 0 = the far plane, which never culls.
    const bool conservative = (fogFlags & kFogConservativeDepth) != 0u;
    if (conservative)
    {
        const float tileFar = FogTileFarthest(HzbFurthest, (int2)coord.xy, fogMisc.x);
        const float nearFaceDepth = FogDepthFromSlice(fogGridZParams, max((float)coord.z - 0.5f, 0.0f));
        const float cellNearZ = FogDeviceZFromViewDepth(fogProjZ, nearFaceDepth);
        if (tileFar > cellNearZ)
        {
            FogScatter[coord] = 0.0f.xxxx;
            return;
        }
    }

    // Temporal history (UE :1027-1033): the previous slot's volume at this cell's position under
    // last frame's camera, re-exposed to this frame; off screen -> no history. With conservative
    // depth the history cell must also have been in FRONT of last frame's geometry (UE
    // FixupHistoryUV, :699-765, reduced to the one tap the grid is read at): a cell that was
    // behind a surface last frame stored nothing and must not be blended from.
    float historyWeight = 0.0f;
    float3 historyUv = 0.0f.xxx;
    if ((fogFlags & kFogHistoryValid) != 0u && fogJitter.w > 0.0f)
    {
        const float3 Pc = FogCellWorldPosition(coord, 0.5f.xxx, fogGridSize, fogGridZParams,
                                               fogProjZ, fogInvViewProjNoJitter);
        const float4 prevClip = mul(float4(Pc, 1.0f), fogPrevViewProjNoJitter);
        if (prevClip.w > 1.0e-4f)
        {
            const float2 prevUv = (prevClip.xy / prevClip.w) * float2(0.5f, -0.5f) + 0.5f;
            historyUv = FogVolumeUV(prevUv, prevClip.w, fogGridZParams, fogGridSize.z);
            if (all(historyUv.xy >= 0.0f) && all(historyUv.xy < 1.0f))
            {
                historyWeight = fogJitter.w;
                if (conservative)
                {
                    const float3 Pn = FogCellWorldPosition(coord, float3(0.5f, 0.5f, -0.5f), fogGridSize,
                                                           fogGridZParams, fogProjZ, fogInvViewProjNoJitter);
                    const float4 prevNearClip = mul(float4(Pn, 1.0f), fogPrevViewProjNoJitter);
                    const float prevFar = FogTileFarthest(PrevHzbFurthest, (int2)(historyUv.xy * (float2)fogGridSize.xy), fogMisc.x);
                    if (prevNearClip.w > 1.0e-4f && prevFar > prevNearClip.z / prevNearClip.w)
                    {
                        historyWeight = 0.0f;
                    }
                }
            }
        }
    }

    // History-miss supersampling (UE :822-829, HistoryMissSupersampleCount): a cell that has nothing
    // to accumulate into takes several jittered samples at once, so the first frame after a cut or
    // the frame's newly uncovered edge is not a single noisy sample. Literal loop bound, the count
    // from the CB only shortens it (engine rule).
    // ... and the EVERY-frame sample count (fogMisc.w, knob `samplesPerCell`): a single jittered
    // sample per frame leaves a residual flicker after the 0.9 history wherever the light changes a
    // lot inside one cell (a spot's tip, docs/bug_fog_spot_flicker.md defect 2). Two samples are the
    // antithetic pair (offset, 1 - offset): a linear gradient across the cell cancels exactly. Three
    // and four continue with the R3 sequence.
    const bool temporalOn = (fogFlags & kFogTemporalOn) != 0u;
    const uint perFrame = clamp(fogMisc.w, 1u, 4u);
    const uint superCount = (temporalOn && historyWeight <= 0.001f) ? max(max(fogMisc.y, 1u), perFrame) : perFrame;
    // B5 is isotropic and constant across this cell's supersamples. Load once, outside
    // the history-miss loop (also avoids multiplying descriptor checks under GPU validation).
    float3 distantAmbient = 0.0f.xxx;
    if (fogMedium2.x > 0.0f && (fogFlags & 16u) != 0u)
        distantAmbient = DistantSkyLight.Load(int3(0,0,0)).rgb * fogMedium2.x;
    float4 result = 0.0f.xxxx;
    [loop]
    for (uint si = 0u; si < 4u; ++si)
    {
        if (si >= superCount) { break; }
        float3 cellOffset = (si == 0u) ? fogJitter.xyz
                          : (si == 1u) ? (1.0f.xxx - fogJitter.xyz)
                          : frac(fogJitter.xyz + kFogR3 * (float)si);
        FogApplyDepthConstraint(coord, cellOffset);
        result += FogSampleCell(coord, cellOffset, distantAmbient, si);
    }
    result /= (float)superCount;

    if (historyWeight > 0.0f)
    {
        float4 history = FogHistory.SampleLevel(gSmpLinearClamp, historyUv, 0);
        history.rgb *= fogMedium2.z * fogMedium2.y; // previous pre-exposure -> this frame's
        result = lerp(result, history, historyWeight);
    }

    // UE MakePositiveFinite: a NaN in one cell would smear through the integration and the history.
    result = max(result, 0.0f.xxxx);
    if (!all(isfinite(result))) { result = 0.0f.xxxx; }
    FogScatter[coord] = result;
}
