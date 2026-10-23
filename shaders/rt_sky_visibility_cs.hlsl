// RT sky visibility (interior lighting plan S; photographic plan P9 item 2) -- the PRODUCER.
//
// UE's Distance Field AO bent normal (DistanceFieldAOShared.ush ComputeBentNormal): the mean of the
// unoccluded directions over the hemisphere, normalized so an open sky has length 1 -- length =
// how much sky the point sees, direction = where it sees it from. UE cones through mesh distance
// fields; this engine has no SDFs but has the TLAS, so it is traced with inline RayQuery: `rayCount`
// cosine-distributed rays per HALF-resolution pixel, each a shadow-style "anything in the way"
// query against the SHADOW casters (rt::kInstanceMaskShadow -- what casts no shadow hides no sky)
// out to `maxDistance`, the masked foliage alpha-tested like every other RT ray.
//
// The few rays are accumulated over frames IN THE SAME DISPATCH: the previous frame's result,
// reprojected through the G-buffer velocity, is blended in with `historyWeight` where it saw the
// same surface (device depth within 10 %); a disocclusion starts afresh. The consumers upsample it
// with UE's bilateral 2x2 (sky_visibility.hlsli), so .w carries the device depth the trace saw.
#define RT_SKYVIS_CS_RS \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "CBV(b0)," \
    "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP)," \
    "StaticSampler(s1, filter=FILTER_MIN_MAG_MIP_POINT, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP)"

#pragma pack_matrix(row_major)
#include "utils.hlsli"
#include "rt_geometry.hlsli"

cbuffer SkyVisibility : register(b0)
{
    float4x4 invView;
    float4x4 invProj;
    uint tlasIndex;     uint gb1Index;      uint depthIndex;    uint velocityIndex;
    uint outUavIndex;   uint historyIndex;  uint geomInfoIndex; uint alphaMode; // alphaMode: rt_reflect_common.hlsli's
    uint outWidth;      uint outHeight;     uint rayCount;      uint frameIndex;
    float maxDistance;  float historyWeight; uint historyValid; uint _skyVisPad;
}

SamplerState gSmp      : register(s0); // linear clamp
SamplerState gSmpPoint : register(s1); // point clamp

static const uint kSkyVisMaskShadow = 0x02u; // rt::kInstanceMaskShadow (rt_reflect_common.hlsli RT_MASK_SHADOW)

// rt_reflect_common.hlsli RtTraceShadow, as a yes/no: anything opaque (or an alpha-tested texel that
// keeps) between `origin` and `maxDistance` along `dir`.
bool SkyRayBlocked(RaytracingAccelerationStructure tlas, StructuredBuffer<GeometryInfo> geom, float3 origin, float3 dir)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 1.0e-3f;
    ray.TMax = maxDistance;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(tlas, alphaMode != 2u ? RAY_FLAG_FORCE_OPAQUE : RAY_FLAG_NONE, kSkyVisMaskShadow, ray);
    while (q.Proceed())
    {
        if (RtAlphaCandidatePasses(geom, gSmp, q.CandidateInstanceID(), q.CandidateGeometryIndex(),
                                   q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()))
        {
            q.CommitNonOpaqueTriangleHit();
        }
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) { return false; }
    // First-hit mode: the committed hit alone is alpha-tested (RtTraceShadow's cheap half).
    return !(alphaMode == 1u &&
             !RtAlphaCandidatePasses(geom, gSmp, q.CommittedInstanceID(), q.CommittedGeometryIndex(),
                                     q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics()));
}

[numthreads(8, 8, 1)]
[RootSignature(RT_SKYVIS_CS_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= outWidth || dtid.y >= outHeight)
    {
        return;
    }
    RWTexture2D<float4> outTex = ResourceDescriptorHeap[outUavIndex];
    Texture2D depthT = ResourceDescriptorHeap[depthIndex];
    Texture2D gb1 = ResourceDescriptorHeap[gb1Index];

    // The full-resolution pixel under this half-resolution texel's centre (GTAO's convention).
    const float2 uv = (float2(dtid.xy) + 0.5f) / float2(outWidth, outHeight);
    const float z = depthT.SampleLevel(gSmpPoint, uv, 0).r;
    if (z <= 1.0e-6f)
    {
        outTex[dtid.xy] = 0.0f.xxxx; // the sky itself: .w 0, never read by the upsample
        return;
    }
    const float3 N = normalize(gb1.SampleLevel(gSmpPoint, uv, 0).rgb * 2.0f - 1.0f);
    const float3 P = ReconstructPosWS(uv, z, invProj, invView);
    const float3 camPos = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), invView).xyz;
    // RtSetupReflectionRay's offset: off the surface by a share of the view distance, so a far
    // texel's depth error does not start the ray inside its own surface.
    const float3 origin = P + N * max(0.02f, length(P - camPos) * 0.002f);

    // Tangent frame (UE FindBestAxisVectors).
    const float3 up = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    const float3 T = normalize(cross(up, N));
    const float3 B = cross(N, T);

    RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[tlasIndex];
    StructuredBuffer<GeometryInfo> geom = ResourceDescriptorHeap[geomInfoIndex];
    // Per-pixel, per-frame rotation of an R2 sequence: the rays of successive frames fill the
    // hemisphere between each other, which is what the accumulation below integrates.
    const uint seed = RtWangHash(dtid.x * 1973u + dtid.y * 9277u + frameIndex * 26699u);
    const float2 rotation = float2(float(seed & 0xFFFFu), float(seed >> 16)) * (1.0f / 65536.0f);
    const uint rays = clamp(rayCount, 1u, 16u);
    float3 unoccluded = 0.0f.xxx;
    for (uint k = 0; k < rays; ++k)
    {
        const float u1 = frac(rotation.x + float(k) * 0.7548776662f);
        const float u2 = frac(rotation.y + float(k) * 0.5698402910f);
        // Cosine-distributed: the sky light's own weighting, so a plain average is its integral.
        const float r = sqrt(u1);
        const float phi = 2.0f * kPi * u2;
        const float3 d = normalize(T * (r * cos(phi)) + B * (r * sin(phi)) + N * sqrt(saturate(1.0f - u1)));
        if (!SkyRayBlocked(tlas, geom, origin, d))
        {
            unoccluded += d;
        }
    }
    // UE's BentNormalNormalizeFactor: 1 / |mean direction of an open hemisphere|, which for
    // cosine-distributed directions is 1 / (2/3).
    float3 bent = unoccluded * (1.5f / float(rays));
    const float bentLength = length(bent);
    if (bentLength > 1.0f) { bent /= bentLength; }

    if (historyValid != 0u)
    {
        Texture2D velocity = ResourceDescriptorHeap[velocityIndex];
        const float2 prevUv = uv - velocity.SampleLevel(gSmpPoint, uv, 0).rg; // motion = currUv - prevUv
        if (all(prevUv >= 0.0f) && all(prevUv <= 1.0f))
        {
            Texture2D<float4> history = ResourceDescriptorHeap[historyIndex];
            const float4 h = history.SampleLevel(gSmpPoint, prevUv, 0);
            // The same surface: relative step in reverse-Z depth = relative step in distance.
            if (h.w > 0.0f && abs(h.w - z) <= 0.1f * z)
            {
                bent = lerp(bent, h.xyz, saturate(historyWeight));
            }
        }
    }
    outTex[dtid.xy] = float4(bent, z);
}
