// Part E2c: single-workgroup bitonic sort of a sorted-index buffer for alpha (smoke) emitters,
// so premultiplied OVER blending draws back-to-front. Runs in the object-compute pass AFTER
// update+spawn, using the camera position cached from the previous frame's draw (a one-frame
// lag in sort order is imperceptible). Additive emitters (fire/sparks) never dispatch this.
//
// Fixed group size => sorted emitters are capped at SORT_N particles (enforced CPU-side).
#include "particle_common.hlsli"

#define SORT_N 1024

cbuffer SortParams : register(b0)
{
    float3 camPos;
    uint sortCount; // = maxParticles (<= SORT_N)
};

RWStructuredBuffer<Particle> gParts : register(u0);
RWStructuredBuffer<uint>     gSorted : register(u1);

groupshared float gKey[SORT_N];
groupshared uint  gVal[SORT_N];

[numthreads(SORT_N, 1, 1)]
[RootSignature("RootConstants(num32BitConstants=4, b0), DescriptorTable(UAV(u0, numDescriptors=2, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))")]
void main(uint3 dtid : SV_DispatchThreadID)
{
    const uint i = dtid.x;

    // Key = -distanceSq so an ASCENDING sort puts the farthest particle first (back-to-front).
    // Dead / out-of-range slots get +inf -> they sink to the tail (the VS skips them anyway).
    float key = 3.4e38;
    if (i < sortCount)
    {
        Particle p = gParts[i];
        if (p.age >= 0.0 && p.life > 0.0)
        {
            float3 d = p.pos - camPos;
            key = -dot(d, d);
        }
    }
    gKey[i] = key;
    gVal[i] = i;
    GroupMemoryBarrierWithGroupSync();

    // Batcher bitonic network (ascending). Each thread owns one element.
    [loop] for (uint k = 2u; k <= SORT_N; k <<= 1u)
    {
        [loop] for (uint j = k >> 1u; j > 0u; j >>= 1u)
        {
            uint ixj = i ^ j;
            if (ixj > i)
            {
                bool ascending = ((i & k) == 0u);
                bool swap = ascending ? (gKey[i] > gKey[ixj]) : (gKey[i] < gKey[ixj]);
                if (swap)
                {
                    float tk = gKey[i]; gKey[i] = gKey[ixj]; gKey[ixj] = tk;
                    uint tv = gVal[i]; gVal[i] = gVal[ixj]; gVal[ixj] = tv;
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }

    if (i < sortCount)
    {
        gSorted[i] = gVal[i];
    }
}
