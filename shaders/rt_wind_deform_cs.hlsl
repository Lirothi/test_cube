// RW (RT reflections subset): wind-deform a caster's vertices for its per-instance BLAS.
//
// The BLAS is built in OBJECT space and the TLAS applies the instance transform, while the wind
// arc is defined in WORLD space about the object's origin (see wind.hlsli). So this pass computes
// the vertex's world position, applies THE SAME WindOffset the gbuffer VS applies -- same header,
// same argument sourcing; a re-implementation drifts and the reflection detaches from the tree
// exactly the way a drifting shadow would -- and transforms the swayed position BACK to object
// space. Output is a tightly packed float3 position stream: the BLAS needs nothing else, and hit
// shading (normals/UVs/alpha) keeps reading the STATIC vertex buffer through the bindless table --
// indices are unchanged by deformation and UVs do not move with sway; the slightly stale normals
// of a bent frond are invisible at reflection resolution.
//
// Root CBV + descriptor tables: the engine's binding model supports exactly that and nothing
// else (Material.cpp asserts on root SRV/UAV descriptors), so the AS pass binds the frame heap
// for these dispatches and stages one SRV table + one UAV table per caster.
#define RT_WIND_DEFORM_RS \
    "CBV(b0)," \
    "DescriptorTable(SRV(t0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(UAV(u0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

#pragma pack_matrix(row_major)
#include "wind.hlsli"

cbuffer Deform : register(b0)
{
    float4x4 world;     // object -> world (row-vector), origin in _41.._43
    float4x4 invWorld;  // world -> object, to bring the swayed position back for the BLAS
    float2 windDirXZ;   // WindState.windDirXZ (already negated to "blows toward")
    float  swayAmp;     // WindState.swayAmplitude (metres at full strength)
    float  swayFreq;
    float  gustMul;     // current gust envelope
    float  timeSec;     // WindState.time -- the CURRENT pose; RT has no motion vectors to feed
    float  windStrength; // EffectiveWindStrength: W8 distance fade already applied
    float  trunkStiff;
    float  leafScale;   // GetWindLeafScaleWorld
    float  foliage;     // max over the object's slots -- only scales the leaf term, which the
                        // baked COLOR_0.b already gates to leaf vertices (0 on wood)
    uint   vertexCount;
    uint   vertexStride; // bytes; COLOR_0 (the W7.1 wind bake) sits at offset 48 of VertexPNTUV
};

ByteAddressBuffer   SrcVB : register(t0);        // the mesh's static vertex buffer
RWByteAddressBuffer DstPositions : register(u0); // float3 per vertex, stride 12

[numthreads(64, 1, 1)]
[RootSignature(RT_WIND_DEFORM_RS)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= vertexCount)
    {
        return;
    }

    const uint base = id.x * vertexStride;
    const float3 objPos = asfloat(SrcVB.Load3(base));
    const uint packed = SrcVB.Load(base + 48u); // COLOR_0, R8G8B8A8_UNORM (R low byte)
    const float4 windWeights = float4((packed >> 0u) & 0xFFu, (packed >> 8u) & 0xFFu,
                                      (packed >> 16u) & 0xFFu, (packed >> 24u) & 0xFFu) / 255.0f;

    const float3 posWS = mul(float4(objPos, 1.0f), world).xyz;
    const float3 offset = WindOffset(objPos, posWS,
                                     float3(world._41, world._42, world._43), windStrength,
                                     windWeights, foliage, trunkStiff, leafScale,
                                     windDirXZ, swayAmp, swayFreq, gustMul, timeSec);
    const float3 deformedObj = mul(float4(posWS + offset, 1.0f), invWorld).xyz;
    DstPositions.Store3(id.x * 12u, asuint(deformedObj));
}
