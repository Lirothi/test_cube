// Rung 2 / Step 19: screen-space page-request pass. One thread per screen pixel: reconstruct
// the pixel's world position from the camera depth, project it into every active shadow view's
// virtual shadow space, and mark the virtual page that view needs at that receiver. Produces
// the page-request set (a bitfield, 1 bit per virtual page); NOTHING consumes it yet (Step 20
// allocates physical pages from it). Single virtual level for now — mip/clipmap LOD is a later
// refinement (Step 24).
#pragma pack_matrix(row_major)
#include "utils.hlsl"

#define VSM_PAGE_REQUEST_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

// Must match vsm::kMaxVirtualViews / kVirtualPagesPerAxis in VirtualShadowMap.h.
static const uint kMaxViews = 36u;
static const uint kVPagesAxis = 64u;   // kVirtualPagesPerAxis (kVirtualRes / kPageSize)
static const uint kVPagesView = kVPagesAxis * kVPagesAxis; // 4096

struct ViewProjEntry
{
    float4x4 viewProj;
    float4   params; // x = valid (0/1), y = zNear, z = zFar
};

cbuffer PageRequestCB : register(b0)
{
    float4x4 invView;
    float4x4 invProj;
    float4   camPosWS;
    float4   screen;   // x=w, y=h, z=1/w, w=1/h
    uint     numViews;
    uint3    _pad;
    ViewProjEntry views[kMaxViews];
};

Texture2D                DepthT  : register(t0);
RWStructuredBuffer<uint> Request : register(u0);

[numthreads(8, 8, 1)]
[RootSignature(VSM_PAGE_REQUEST_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    const uint w = (uint)screen.x;
    const uint h = (uint)screen.y;
    if (dtid.x >= w || dtid.y >= h) { return; }

    const float2 uv = (float2(dtid.xy) + 0.5f) * screen.zw;
    const float z = DepthT.Load(int3(int2(dtid.xy), 0)).r;
    if (z <= kEpsilon) { return; } // background / no geometry (reverse-Z: 0 = far/cleared)

    const float3 P = ReconstructPosWS(uv, z, invProj, invView);

    const uint count = min(numViews, kMaxViews);
    for (uint v = 0; v < count; ++v)
    {
        if (views[v].params.x < 0.5f) { continue; } // inactive view slot

        float4 clip = mul(float4(P, 1.0f), views[v].viewProj);
        if (clip.w <= 0.0f) { continue; }
        float3 ndc = clip.xyz / clip.w;
        if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f) { continue; }
        if (ndc.z < 0.0f || ndc.z > 1.0f) { continue; }

        // Shadow UV (D3D clip -> UV, y flipped), then the virtual page it lands in.
        float2 suv = float2(0.5f * ndc.x + 0.5f, 0.5f - 0.5f * ndc.y);
        uint px = min((uint)(suv.x * kVPagesAxis), kVPagesAxis - 1u);
        uint py = min((uint)(suv.y * kVPagesAxis), kVPagesAxis - 1u);
        uint page = v * kVPagesView + py * kVPagesAxis + px;

        uint word = page >> 5u;
        uint bit = page & 31u;
        uint prev;
        InterlockedOr(Request[word], 1u << bit, prev);
    }
}
