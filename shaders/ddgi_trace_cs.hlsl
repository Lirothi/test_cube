// Interior lighting plan D: the probe volume's TRACE (DDGI; RTXGI's ProbeTraceRGS as inline RayQuery).
// One group per probe updated this frame, one thread per ray: the closest hit along the ray, shaded
// diffusely as the light it sends back toward the probe -- the sun with an RT shadow ray, every spot
// and point light in reach with a shadow ray, the sky through ONE cosine ray of visibility (the probe
// average does the integral), and the volume's own previous irradiance at the hit (the infinite
// bounce). A miss brings nothing: the probes hold the bounce, the sky is plan S's (ddgi_common.hlsli).
// A hit on the back of a one-sided surface stores a negative, shortened distance (RTXGI): the update
// counts the FIXED rays' to switch off probes inside geometry, and the depth sees a near occluder there.
// A fixed ray (ddgi_common.hlsli kDdgiFixedRays) stops at its hit: it classifies, it does not light.
#define DDGI_TRACE_RS \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "CBV(b0)," \
    "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP)," \
    "StaticSampler(s1, filter=FILTER_MIN_MAG_MIP_POINT, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP)"

#pragma pack_matrix(row_major)
#include "utils.hlsli"
#include "rt_geometry.hlsli"
#include "rt_lights.hlsli"
#include "ddgi_common.hlsli"
#include "ddgi_constants.hlsli"

SamplerState gSmp      : register(s0); // linear clamp
SamplerState gSmpPoint : register(s1); // point clamp

static const uint kDdgiMaskShadow = kRtMaskShadow; // rt::kInstanceMaskShadow: what casts no shadow blocks no light
// The albedo is read a few mips down: a probe averages over a sphere, and a mip-0 texel per ray is noise.
static const float kDdgiAlbedoMip = 3.0f;

// rt_sky_visibility_cs.hlsl SkyRayTransmittance: the share of the light that gets through -- 0 behind the geometry,
// 1 - opacity through a tree's leaf card (rt_geometry.hlsli RtVisibilityTransmittance), 1 open.
float DdgiTransmittance(RaytracingAccelerationStructure tlas, float3 origin, float3 dir, float maxDistance)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 1.0e-3f;
    ray.TMax = maxDistance;
    return RtVisibilityTransmittance(tlas, ray, kDdgiMaskShadow, meshOpacity);
}

[numthreads(96, 1, 1)] // kDdgiRays: the 32 fixed rays (one wave) + the 64 blended
[RootSignature(DDGI_TRACE_RS)]
void CSMain(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    const uint probeIndex = gid.x * max(schedule.x, 1u) + schedule.y;
    if (gid.x >= schedule.z || probeIndex >= kDdgiProbeCount || gi >= kDdgiRays)
    {
        return;
    }
    RWTexture2D<float4> rays = ResourceDescriptorHeap[rayUavIndex];
    const uint2 rayTexel = uint2(gi, gid.x);
    const DdgiVolumeDesc volume = DdgiVolumeFromConstants();
    const int3 cell = DdgiCellOfSlot(DdgiSlotFromIndex(probeIndex), volume.base);
    // From where the probe stands (ddgi_update_cs relocates it): its offset while the slot was traced for this cell
    // in this generation, its grid point otherwise -- the update reads the same state before it writes.
    StructuredBuffer<uint4> probeState = ResourceDescriptorHeap[stateIndex];
    const uint4 ownState = probeState[DdgiStateIndex(probeIndex)];
    const bool placed = all(int3(asint(ownState.x), asint(ownState.y), asint(ownState.z)) == cell) &&
                        (ownState.w >> 8) == volume.generation && (ownState.w & kDdgiStateValid) != 0u;
    const float3 origin = float3(cell) * volume.spacing +
                          (placed ? asfloat(probeState[DdgiOffsetIndex(probeIndex)].xyz) : 0.0f.xxx);
    const float3 dir = DdgiRayDirection(gi);

    RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[tlasIndex];
    StructuredBuffer<GeometryInfo> geom = ResourceDescriptorHeap[geomInfoIndex];
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.0f;
    ray.TMax = volumeParams.z;
    // The closest GEOMETRY, no alpha test (that is the reflections' alone, rt_geometry.hlsli). A tree's leaf card stops
    // only its mesh's opacity of the ray (RtCutoutTransmittance); the rest would bring what lies behind it, which is not
    // traced -- the ray goes no further -- and counts as a miss, no bounce: the hit's radiance is scaled by the card's share.
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFFu, ray);
    q.Proceed();
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        rays[rayTexel] = float4(0.0f, 0.0f, 0.0f, 1.0e4f); // nothing in reach: no bounce from here
        return;
    }
    const float t = q.CommittedRayT();
    const float cardShare = meshOpacity != 0u
        ? 1.0f - RtCutoutTransmittance(q.CommittedInstanceContributionToHitGroupIndex(), q.CommittedGeometryIndex())
        : 1.0f;

    const GeometryInfo g = geom[GeometryRecordIndex(q.CommittedInstanceID(), q.CommittedGeometryIndex())];
    ByteAddressBuffer vb = ResourceDescriptorHeap[g.vbIndex];
    ByteAddressBuffer ib = ResourceDescriptorHeap[g.ibIndex];
    const uint3 tri = LoadTriangle(ib, g.firstTri + q.CommittedPrimitiveIndex(), g.indexIs32);
    const float2 bary = q.CommittedTriangleBarycentrics();
    const float bw = 1.0f - bary.x - bary.y;
    const float3 nObj = LoadNormal(vb, tri.x, g.vertexStride) * bw + LoadNormal(vb, tri.y, g.vertexStride) * bary.x +
                        LoadNormal(vb, tri.z, g.vertexStride) * bary.y;
    float3 N = normalize(mul((float3x3)q.CommittedObjectToWorld3x4(), nObj));
    // A masked surface (a leaf card) or a two-sided material (the tent's fabric, glTF doubleSided) shows its other face
    // from behind; anything else seen from behind is the inside of a solid. The two-sided material was read as a solid:
    // a probe inside the tent saw the fabric's back faces all round, counted itself inside geometry, and was switched
    // off or relocated OUT through the fabric -- the tent's floor then took the sunlit outside's bounce in 0.5 m cells,
    // the owner's "squares inside the tent" (2026-10-27).
    if (dot(N, dir) > 0.0f)
    {
        if (g.alphaCutoff < 0.0f && ((g.flags & kGeomFlagTwoSided) == 0u || twoSidedSheets == 0u))
        {
            rays[rayTexel] = float4(0.0f, 0.0f, 0.0f, -0.2f * t);
            return;
        }
        N = -N;
    }
    // A fixed ray only classifies the probe (ddgi_update_cs): its distance and side, no light.
    if (gi < kDdgiFixedRays)
    {
        rays[rayTexel] = float4(0.0f, 0.0f, 0.0f, t);
        return;
    }

    const float2 uv = LoadUV(vb, tri.x, g.vertexStride) * bw + LoadUV(vb, tri.y, g.vertexStride) * bary.x +
                      LoadUV(vb, tri.z, g.vertexStride) * bary.y;
    float3 albedo = g.baseColor.rgb;
    if (g.albedoTexIndex != 0xFFFFFFFFu)
    {
        Texture2D albedoTex = ResourceDescriptorHeap[g.albedoTexIndex];
        albedo *= albedoTex.SampleLevel(gSmp, uv, kDdgiAlbedoMip).rgb;
    }
    float metal = saturate(g.metalness);
    if (g.mrTexIndex != 0xFFFFFFFFu)
    {
        Texture2D mrTex = ResourceDescriptorHeap[g.mrTexIndex];
        const float m = mrTex.SampleLevel(gSmp, uv, kDdgiAlbedoMip).r;
        metal = saturate(g.mrMultiply != 0u ? m * metal : m);
    }
    // lighting_cs's Lambert half: (1 - metal) * albedo / pi per unit of irradiance.
    const float3 diffuse = albedo * (1.0f - metal);

    const float3 P = origin + dir * t;
    const float3 shadowOrigin = P + N * 0.02f;
    float3 irradiance = 0.0f.xxx; // E at the hit; the outgoing radiance is diffuse / pi times it

    // The sun (or the moon, whichever holds the light): lighting_cs's lightRgb, an RT shadow ray.
    const float3 L = normalize(-sunDirWS);
    const float sunNdotL = dot(N, L);
    if (sunNdotL > 0.0f && any(lightRgb > 0.0f))
    {
        irradiance += lightRgb * (sunNdotL * DdgiTransmittance(tlas, shadowOrigin, L, 1.0e4f));
    }
    // The lamps in reach, each with its shadow ray (a candle behind a wall lights nothing here).
    if (spotCount > 0u)
    {
        StructuredBuffer<SpotLightData> spots = ResourceDescriptorHeap[spotLightIndex];
        for (uint si = 0u; si < spotCount; ++si)
        {
            float3 sl; float sdist;
            const float3 rad = RtEvalSpotLight(spots[si], P, sl, sdist);
            const float ndl = dot(N, sl);
            if (ndl <= 0.0f || dot(rad, rad) <= 0.0f) { continue; }
            irradiance += rad * (ndl * DdgiTransmittance(tlas, shadowOrigin, sl, max(sdist - 0.04f, 0.0f)));
        }
    }
    if (pointCount > 0u)
    {
        StructuredBuffer<PointLightData> points = ResourceDescriptorHeap[pointLightIndex];
        for (uint pi = 0u; pi < pointCount; ++pi)
        {
            float3 pl; float pdist;
            const float3 rad = RtEvalPointLight(points[pi], P, pl, pdist);
            const float ndl = dot(N, pl);
            if (ndl <= 0.0f || dot(rad, rad) <= 0.0f) { continue; }
            irradiance += rad * (ndl * DdgiTransmittance(tlas, shadowOrigin, pl, max(pdist - 0.04f, 0.0f)));
        }
    }
    float3 radiance = diffuse * kInvPi * irradiance;

    // The sky at the hit, seen through one cosine ray (plan S's reach): the probe's 64 blended rays and its
    // hysteresis average it, as SkyVis averages its four over frames.
    if (skyIrradianceIndex != 0u)
    {
        const uint seed = RtWangHash(probeIndex * 9781u + gi * 6271u + schedule.w * 26699u);
        const float u1 = float(seed & 0xFFFFu) * (1.0f / 65536.0f);
        const float u2 = float(seed >> 16) * (1.0f / 65536.0f);
        const float3 up = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
        const float3 T = normalize(cross(up, N));
        const float3 B = cross(N, T);
        const float r = sqrt(u1);
        const float phi = 2.0f * kPi * u2;
        const float3 skyDir = normalize(T * (r * cos(phi)) + B * (r * sin(phi)) + N * sqrt(saturate(1.0f - u1)));
        const float skyThrough = DdgiTransmittance(tlas, shadowOrigin, skyDir, skyRayDistance);
        if (skyThrough > 0.0f)
        {
            TextureCube skyIrradiance = ResourceDescriptorHeap[skyIrradianceIndex];
            radiance += diffuse * skyIrradiance.SampleLevel(gSmp, N, 0).rgb * (skyIrradianceScale * skyThrough);
        }
    }
    else
    {
        radiance += diffuse * ambientIntensity * lightRgb; // lighting_cs's flat fill without a sky IBL
    }

    // The infinite bounce: the volume's own previous irradiance arriving at the hit.
    if (bounceOn != 0u)
    {
        Texture2D irradianceAtlas = ResourceDescriptorHeap[irradianceIndex];
        Texture2D depthAtlas = ResourceDescriptorHeap[depthIndex];
        float w;
        const float3 bounce = DdgiIrradiance(irradianceAtlas, depthAtlas, probeState, gSmp, volume, P, N, -dir, 1.0f, w);
        radiance += diffuse * bounce * w;
    }
    rays[rayTexel] = float4(radiance * cardShare, t);
}
