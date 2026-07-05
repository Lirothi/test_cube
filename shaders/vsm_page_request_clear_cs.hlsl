// Rung 2 / Step 19: clear the VSM page-request bitfield before the request pass marks it.
// One thread per uint word.
#define VSM_PAGE_REQUEST_CLEAR_RS \
    "CBV(b0), " \
    "DescriptorTable(UAV(u0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer ClearCB : register(b0)
{
    uint gWordCount;
    uint3 _pad;
};

RWStructuredBuffer<uint> Request : register(u0);

[numthreads(8, 8, 1)]
[RootSignature(VSM_PAGE_REQUEST_CLEAR_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.y != 0u) { return; }
    if (dtid.x >= gWordCount) { return; }
    Request[dtid.x] = 0u;
}
