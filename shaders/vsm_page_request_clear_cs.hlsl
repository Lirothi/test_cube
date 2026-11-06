// Rung 2 / Step 19: clear the VSM page-request bitfield before the request pass marks it. By last frame's summary: a page table of
// millions of entries holds a few hundred marks, and one thread a word was a million threads a frame with the lamps on. The water's
// mask behind the bitfield is whole every frame. The first frame after the buffer's creation clears all of it (gFullWords), in two
// dimensions: past 65535 groups of 64 at full capacity.
#include "vsm_addressing.hlsli"

#define VSM_PAGE_REQUEST_CLEAR_RS \
    "CBV(b0), " \
    "DescriptorTable(UAV(u0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer ClearCB : register(b0)
{
    uint gFullWords;    // not 0: every word of the buffer, nothing else
    uint gSummaryBase;
    uint gSummaryWords;
    uint gCoarseBase;
    uint gWaterBase;
    uint gWaterWords;
    uint2 _pad;
};

RWStructuredBuffer<uint> Request : register(u0);

[numthreads(64, 1, 1)]
[RootSignature(VSM_PAGE_REQUEST_CLEAR_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (gFullWords != 0u)
    {
        const uint i = dtid.x + dtid.y * VSM_CLEAR_ROW_THREADS;
        if (i < gFullWords) { Request[i] = 0u; }
        return;
    }
    const uint t = dtid.x;
    if (t < gWaterWords) { Request[gWaterBase + t] = 0u; }
    if (t >= gSummaryWords) { return; }
    uint summary = Request[gSummaryBase + t];
    if (summary == 0u) { return; }
    Request[gSummaryBase + t] = 0u;
    while (summary != 0u)
    {
        const uint word = t * 32u + firstbitlow(summary);
        summary &= summary - 1u;
        Request[word] = 0u;
        Request[gCoarseBase + word] = 0u;
    }
}
