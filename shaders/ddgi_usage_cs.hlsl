// Interior lighting plan D: which probes the SCREEN reads (`gi.sleepPeriod`). Recorded at the head of Main_GiTrace,
// after the G-buffer: every 4th pixel each way (a different one of its 4x4 block each frame, so in 16 frames every
// pixel), its surface point biased as DdgiIrradiance biases it, and the 8 probes around it in each cascade stamped
// with this frame. A probe no pixel has stamped for kDdgiAwakeFrames is IDLE: it updates one turn in sleepPeriod
// (ddgi_constants.hlsli DdgiProbeSkips) -- the probes behind the camera, under the ground, up in the air. The steady
// picture does not move (an idle probe still converges, more slowly; nothing on screen reads it), only how fast a
// probe the camera turns to catches a change of the light.
// The quarter-resolution grid is dense enough: 4 pixels at the near cascade's 8 m edge are 0.3 m (spacing 0.5), at the
// far one's 32 m 1.3 m (spacing 2), and each sample stamps a cube of 8.
#define DDGI_USAGE_RS \
    "CBV(b0)," \
    "DescriptorTable(SRV(t0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(UAV(u0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

#pragma pack_matrix(row_major)
#include "utils.hlsli"
#include "ddgi_common.hlsli"

// Mirrors DdgiUsageConstants (SceneRenderer_Gi.cpp).
cbuffer DdgiUsageCB : register(b0)
{
    float4x4 invView;    // the camera (jittered, as the depth it reads)
    float4x4 invProj;
    float4 cameraPos;    // xyz
    int4 nearBase;       // xyz the near cascade's first cell
    float4 nearParams;   // x spacing, y 1 = stamp it
    int4 farBase;
    float4 farParams;
    uint4 grid;          // xy the sample grid, zw this frame's pixel in each 4x4 block
    uint4 stamp;         // x this frame's stamp
};

Texture2D<float> SceneDepth : register(t0);
Texture2D<float4> SceneGb1 : register(t1);
RWStructuredBuffer<uint> UsageNear : register(u0);
RWStructuredBuffer<uint> UsageFar : register(u1);

void StampProbes(RWStructuredBuffer<uint> usage, int3 base, float spacing, float3 P, float3 N, float3 V)
{
    const float3 biased = P + (N * 0.2f + V * 0.8f) * (0.75f * spacing * 0.3f);
    const int3 cell0 = int3(floor(biased / spacing));
    if (any(cell0 < base) || any(cell0 + 1 > base + kDdgiDims - 1)) { return; }
    [unroll]
    for (uint i = 0u; i < 8u; ++i)
    {
        const int3 cell = cell0 + int3(i & 1u, (i >> 1) & 1u, (i >> 2) & 1u);
        usage[DdgiSlotIndex(DdgiSlotOfCell(cell))] = stamp.x;
    }
}

[numthreads(8, 8, 1)]
[RootSignature(DDGI_USAGE_RS)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= grid.x || id.y >= grid.y) { return; }
    uint dw, dh;
    SceneDepth.GetDimensions(dw, dh);
    const uint2 px = min(id.xy * 4u + grid.zw, uint2(dw, dh) - 1u);
    const float z = SceneDepth.Load(int3(px, 0));
    if (z <= 1.0e-6f) { return; } // the sky (reversed Z): no surface reads a probe
    const float2 uv = (float2(px) + 0.5f) / float2(dw, dh);
    const float3 P = ReconstructPosWS(uv, z, invProj, invView);
    const float3 N = normalize(SceneGb1.Load(int3(px, 0)).rgb * 2.0f - 1.0f);
    const float3 V = normalize(cameraPos.xyz - P);
    if (nearParams.y > 0.0f) { StampProbes(UsageNear, nearBase.xyz, nearParams.x, P, N, V); }
    if (farParams.y > 0.0f) { StampProbes(UsageFar, farBase.xyz, farParams.x, P, N, V); }
}
