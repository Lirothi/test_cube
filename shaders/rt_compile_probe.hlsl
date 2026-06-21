// DXR inline-raytracing (RayQuery) compile probe — S2.
//
// Sole purpose: validate that the engine's DXC route builds a cs_6_5 compute
// shader that declares a RaytracingAccelerationStructure and traces an inline
// ray (TraceRayInline / Proceed). It is never bound or dispatched. The inline
// [RootSignature] mirrors the real RT passes (TLAS as an SRV + an output UAV) so
// this also exercises the embedded root-signature extraction path.
#pragma pack_matrix(row_major)

#define RT_PROBE_RS \
    "DescriptorTable(SRV(t0))," \
    "DescriptorTable(UAV(u0))"

RaytracingAccelerationStructure Scene : register(t0);
RWTexture2D<float4>             Output : register(u0);

[numthreads(8, 8, 1)]
[RootSignature(RT_PROBE_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    RayDesc ray;
    ray.Origin    = float3(0.0f, 0.0f, 0.0f);
    ray.Direction = float3(0.0f, 0.0f, 1.0f);
    ray.TMin      = 0.0f;
    ray.TMax      = 1e30f;

    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFFu, ray);
    while (q.Proceed()) {}

    float t = (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? q.CommittedRayT() : -1.0f;
    Output[dtid.xy] = float4(t, 0.0f, 0.0f, 1.0f);
}
