// Tier-2 hardware ray-traced reflections (S7 + S9 + S10).
//
// cs_6_6 inline RayQuery + SM6.6 dynamic resources. Per GBuffer surface, trace
// one reflection ray against the TLAS. On a hit:
//   - if it reprojects onto the visible surface on screen, sample the lit HDR
//     buffer (Tier-1 fast path: exact first-bounce color);
//   - otherwise SHADE the hit directly (Tier-2): bindless-fetch the interpolated
//     normal + albedo (texture), then sun diffuse + ambient with a shadow ray.
//     This lets OFF-SCREEN geometry reflect instead of falling back to skybox.
// On miss: coverage 0 -> compose's skybox fallback. Writes premultiplied
// (rgb, coverage) into the SSR target; blur + compose are unchanged.
//
// Glossy (S10+S11): the reflection direction is jittered within a roughness-
// scaled cone using a per-frame-varying seed, so the single noisy ray integrates
// to a clean glossy reflection once the S11 temporal denoise accumulates it over
// frames. Mirror surfaces (roughness ~0) trace a sharp ray.
#define RT_REFLECT_CS_RS \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "CBV(b0)," \
    "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP)," \
    "StaticSampler(s1, filter=FILTER_MIN_MAG_MIP_POINT, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP)"

#pragma pack_matrix(row_major)
#include "utils.hlsl"

// Mirrors rt::GeometryInfoGPU.
struct GeometryInfo
{
    uint   vbIndex;
    uint   ibIndex;
    uint   indexIs32;
    uint   albedoTexIndex; // 0xFFFFFFFF = no texture (use baseColor)
    float4 baseColor;
};

cbuffer Probe : register(b0)
{
    float4x4 view;
    float4x4 proj;
    float4x4 invView;
    float4x4 invProj;
    float3 sunDirWS;     float ambientIntensity;
    float3 lightRgb;     float exposure;
    float depthA;        float depthB;        uint outWidth;     uint outHeight;
    uint tlasIndex;      uint lightIndex;     uint gb1Index;     uint depthIndex;
    uint ssrUavIndex;    uint geomInfoIndex;  uint gb0Index;     uint frameSeed;
}

SamplerState gSmp      : register(s0);
SamplerState gSmpPoint : register(s1);

static const uint kVertexStride = 48u; // VertexPNTUV
static const uint kNormalOffset = 12u;
static const uint kUVOffset     = 40u;

uint LoadIndex16(ByteAddressBuffer ib, uint i)
{
    const uint byteOff = i * 2u;
    const uint word = ib.Load(byteOff & ~3u);
    return ((byteOff & 2u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
}
uint3 LoadTriangle(ByteAddressBuffer ib, uint prim, uint is32)
{
    if (is32 != 0u) { return ib.Load3(prim * 12u); }
    const uint b = prim * 3u;
    return uint3(LoadIndex16(ib, b), LoadIndex16(ib, b + 1u), LoadIndex16(ib, b + 2u));
}
float3 LoadNormal(ByteAddressBuffer vb, uint vertex) { return asfloat(vb.Load3(vertex * kVertexStride + kNormalOffset)); }
float2 LoadUV(ByteAddressBuffer vb, uint vertex)     { return asfloat(vb.Load2(vertex * kVertexStride + kUVOffset)); }

// Per-pixel+frame 2D hash for the glossy jitter (varies each frame so temporal
// accumulation integrates many samples).
float2 Hash22(uint2 p, uint s)
{
    uint3 v = uint3(p, s);
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x;
    return float2(v.x & 0xFFFFu, v.y & 0xFFFFu) * (1.0f / 65535.0f);
}

// Perturb the mirror reflection R within a roughness-scaled cone.
float3 JitterReflection(float3 R, float3 N, float rough, uint2 px, uint seed)
{
    float2 u = Hash22(px, seed);
    float3 up = abs(R.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 T = normalize(cross(up, R));
    float3 B = cross(R, T);
    float a = rough * rough;
    float phi = 6.2831853f * u.x;
    float rad = a * sqrt(u.y);
    float3 J = normalize(R + (rad * cos(phi)) * T + (rad * sin(phi)) * B);
    return (dot(J, N) < 0.0f) ? R : J;
}

// Trace one reflection ray; on hit, return its radiance (screen color where the
// hit is visible, else shaded). Returns false on miss.
bool TraceReflection(float3 origin, float3 dir, out float3 radiance)
{
    radiance = float3(0.0f, 0.0f, 0.0f);

    RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[tlasIndex];
    RayDesc ray; ray.Origin = origin; ray.Direction = dir; ray.TMin = 0.0f; ray.TMax = 1e4f;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFFu, ray);
    while (q.Proceed()) {}
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) { return false; }

    float3 hitWS = origin + dir * q.CommittedRayT();
    Texture2D depthT = ResourceDescriptorHeap[depthIndex];

    // Fast path: the hit is the visible surface at some screen pixel -> exact color.
    float4 hv = mul(float4(hitWS, 1.0f), view);
    float4 hc = mul(hv, proj);
    if (hc.w > 1e-6f)
    {
        float2 huv = (hc.xy / hc.w) * float2(0.5f, -0.5f) + 0.5f;
        if (all(huv >= 0.0f) && all(huv <= 1.0f))
        {
            float sd = depthT.SampleLevel(gSmpPoint, huv, 0).r;
            if (sd > 1e-6f)
            {
                float visVZ = depthB / (sd - depthA);
                if (abs(hv.z - visVZ) / max(visVZ, 1e-3f) < 0.05f)
                {
                    Texture2D lightT = ResourceDescriptorHeap[lightIndex];
                    radiance = lightT.SampleLevel(gSmp, huv, 0).rgb;
                    return true;
                }
            }
        }
    }

    // Tier-2 shaded path: bindless geometry + material, sun diffuse + ambient + shadow.
    StructuredBuffer<GeometryInfo> geom = ResourceDescriptorHeap[geomInfoIndex];
    GeometryInfo g = geom[q.CommittedInstanceID()];
    ByteAddressBuffer vb = ResourceDescriptorHeap[g.vbIndex];
    ByteAddressBuffer ib = ResourceDescriptorHeap[g.ibIndex];

    uint3 tri = LoadTriangle(ib, q.CommittedPrimitiveIndex(), g.indexIs32);
    float2 bary = q.CommittedTriangleBarycentrics();
    float  bw = 1.0f - bary.x - bary.y;
    float3 nObj = normalize(LoadNormal(vb, tri.x) * bw + LoadNormal(vb, tri.y) * bary.x + LoadNormal(vb, tri.z) * bary.y);
    float3x4 o2w = q.CommittedObjectToWorld3x4();
    float3 Nw = normalize(mul((float3x3)o2w, nObj));
    if (dot(Nw, dir) > 0.0f) { Nw = -Nw; }

    float3 albedo = g.baseColor.rgb;
    if (g.albedoTexIndex != 0xFFFFFFFFu)
    {
        float2 uvHit = LoadUV(vb, tri.x) * bw + LoadUV(vb, tri.y) * bary.x + LoadUV(vb, tri.z) * bary.y;
        Texture2D albedoTex = ResourceDescriptorHeap[g.albedoTexIndex];
        albedo *= albedoTex.SampleLevel(gSmp, uvHit, 0).rgb;
    }

    float3 L = normalize(-sunDirWS);
    float ndl = saturate(dot(Nw, L));
    float shadow = 1.0f;
    if (ndl > 0.0f)
    {
        RayDesc sray; sray.Origin = hitWS + Nw * 0.02f; sray.Direction = L; sray.TMin = 0.0f; sray.TMax = 1e4f;
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> sq;
        sq.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFFu, sray);
        sq.Proceed();
        if (sq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) { shadow = 0.0f; }
    }
    radiance = (albedo * ambientIntensity + albedo * ndl * shadow) * lightRgb * exposure;
    return true;
}

[numthreads(8, 8, 1)]
[RootSignature(RT_REFLECT_CS_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= outWidth || dtid.y >= outHeight)
    {
        return;
    }

    RWTexture2D<float4> outTex = ResourceDescriptorHeap[ssrUavIndex];
    Texture2D depthT = ResourceDescriptorHeap[depthIndex];
    Texture2D gb1    = ResourceDescriptorHeap[gb1Index];
    Texture2D gb0    = ResourceDescriptorHeap[gb0Index];

    float2 uv = (float2(dtid.xy) + 0.5f) / float2(outWidth, outHeight);
    float depth = depthT.SampleLevel(gSmpPoint, uv, 0).r;

    float4 result = float4(0.0f, 0.0f, 0.0f, 0.0f); // no surface / miss -> skybox via compose
    if (depth > 1e-6f)
    {
        float3 N = normalize(gb1.SampleLevel(gSmp, uv, 0).rgb * 2.0f - 1.0f);
        float3 P = ReconstructPosWS(uv, depth, invProj, invView);
        float3 camPos = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), invView).xyz;
        float3 R = reflect(normalize(P - camPos), N);

        // Sharp mirror ray (clean, stable). Stochastic roughness-jittered glossy is
        // disabled: at 1 sample/pixel it needs a real denoiser (DLSS Ray
        // Reconstruction) to not look noisy/"dance"; gb0/frameSeed/JitterReflection
        // are kept for when that lands.

        float3 radiance;
        if (TraceReflection(P + N * 0.02f, R, radiance))
        {
            result = float4(radiance, 1.0f); // premultiplied, full coverage
        }
    }

    outTex[dtid.xy] = result;
}
