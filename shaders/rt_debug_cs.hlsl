// RT bindless hit-attribute debug visualization (S6 + S9).
//
// cs_6_6 inline RayQuery with SM6.6 dynamic resources (ResourceDescriptorHeap[]).
// From each GBuffer surface, trace the reflection ray; on a hit, use the TLAS
// InstanceID to index a bindless geometry table, fetch the hit triangle's vertex
// normals from the raw VB/IB, interpolate by barycentrics, and write the world-
// space hit normal (as color) into the SSR target for inspection (TextureDebug
// Viewer -> Ssr). This proves bindless per-hit geometry access (S9). All
// resources are accessed via ResourceDescriptorHeap; indices arrive in b0.
#define RT_DEBUG_CS_RS \
    "RootFlags(CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "CBV(b0)," \
    "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP)," \
    "StaticSampler(s1, filter=FILTER_MIN_MAG_MIP_POINT, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP)"

#pragma pack_matrix(row_major)
#include "utils.hlsl"

// Mirrors rt::GeometryInfoGPU (3x 16B rows).
struct GeometryInfo
{
    uint   vbIndex;
    uint   ibIndex;
    uint   indexIs32;
    uint   albedoTexIndex;
    float  roughness;
    float  metalness;
    uint   mrTexIndex;
    uint   _pad1;
    float4 baseColor;
};

cbuffer Probe : register(b0)
{
    float4x4 invView;
    float4x4 invProj;
    uint tlasIndex;
    uint gb1Index;
    uint depthIndex;
    uint ssrUavIndex;
    uint geomInfoIndex;
    uint outWidth;
    uint outHeight;
    uint _pad;
}

SamplerState gSmp      : register(s0); // linear clamp
SamplerState gSmpPoint : register(s1); // point clamp

// VertexPNTUV: position@0 (float3), normal@12 (float3), tangent@24 (float4), uv@40 (float2).
static const uint kVertexStride = 48u;
static const uint kNormalOffset = 12u;

uint LoadIndex16(ByteAddressBuffer ib, uint i)
{
    const uint byteOff = i * 2u;
    const uint word = ib.Load(byteOff & ~3u);
    return ((byteOff & 2u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
}

uint3 LoadTriangle(ByteAddressBuffer ib, uint prim, uint is32)
{
    if (is32 != 0u)
    {
        return ib.Load3(prim * 12u); // three 32-bit indices
    }
    const uint b = prim * 3u;
    return uint3(LoadIndex16(ib, b), LoadIndex16(ib, b + 1u), LoadIndex16(ib, b + 2u));
}

float3 LoadNormal(ByteAddressBuffer vb, uint vertex)
{
    return asfloat(vb.Load3(vertex * kVertexStride + kNormalOffset));
}

[numthreads(8, 8, 1)]
[RootSignature(RT_DEBUG_CS_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= outWidth || dtid.y >= outHeight)
    {
        return;
    }

    RWTexture2D<float4> outTex = ResourceDescriptorHeap[ssrUavIndex];
    Texture2D depthT = ResourceDescriptorHeap[depthIndex];
    Texture2D gb1    = ResourceDescriptorHeap[gb1Index];

    float2 uv = (float2(dtid.xy) + 0.5f) / float2(outWidth, outHeight);
    float depth = depthT.SampleLevel(gSmpPoint, uv, 0).r;

    float4 result = float4(0.0f, 0.0f, 0.0f, 1.0f); // background / sky: black
    if (depth > 1e-6f)
    {
        float3 N = normalize(gb1.SampleLevel(gSmp, uv, 0).rgb * 2.0f - 1.0f);
        float3 P = ReconstructPosWS(uv, depth, invProj, invView);
        float3 camPos = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), invView).xyz;
        float3 R = reflect(normalize(P - camPos), N);

        RayDesc ray;
        ray.Origin    = P + N * 0.02f;
        ray.Direction = R;
        ray.TMin      = 0.0f;
        ray.TMax      = 1e4f;

        RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[tlasIndex];
        RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
        q.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFFu, ray);
        while (q.Proceed()) {}

        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            // Bindless per-hit geometry fetch (S9): InstanceID -> geometry record
            // -> raw VB/IB -> interpolate the hit triangle's vertex normals.
            StructuredBuffer<GeometryInfo> geom = ResourceDescriptorHeap[geomInfoIndex];
            GeometryInfo g = geom[q.CommittedInstanceID()];
            ByteAddressBuffer vb = ResourceDescriptorHeap[g.vbIndex];
            ByteAddressBuffer ib = ResourceDescriptorHeap[g.ibIndex];

            uint3 tri = LoadTriangle(ib, q.CommittedPrimitiveIndex(), g.indexIs32);
            float3 n0 = LoadNormal(vb, tri.x);
            float3 n1 = LoadNormal(vb, tri.y);
            float3 n2 = LoadNormal(vb, tri.z);

            float2 bary = q.CommittedTriangleBarycentrics();
            float  w = 1.0f - bary.x - bary.y;
            float3 nObj = normalize(n0 * w + n1 * bary.x + n2 * bary.y);

            float3x4 o2w = q.CommittedObjectToWorld3x4();
            float3 nWorld = normalize(mul((float3x3)o2w, nObj));

            result = float4(nWorld * 0.5f + 0.5f, 1.0f) * g.baseColor; // hit normal as color
        }
        else
        {
            result = float4(0.10f, 0.20f, 0.60f, 1.0f); // miss = reflects sky
        }
    }

    outTex[dtid.xy] = result;
}
