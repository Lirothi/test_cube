// Rung 2 page-cache depth clear. One instance per pool page, drawn under a FULL-pool viewport: for a
// DIRTY page (PerPageDirty[iid] != 0) emit a full-cell quad at z=1.0 (far) so the casters redraw over
// a fresh page; a CLEAN page emits an off-screen degenerate quad (no raster) so its cached depth from
// a previous frame survives. Replaces the whole-pool ClearDepthStencilView, which would have wiped the
// cached pages. Depth-only (no RT), depth func ALWAYS so z=1.0 overwrites whatever was there.
#include "vsm_addressing.hlsli"

#define VSM_PAGE_CLEAR_RS \
    "RootFlags(0), " \
    "DescriptorTable(SRV(t0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

StructuredBuffer<uint> PerPageDirty : register(t0); // per pool page: 1 = dirty (clear it), 0 = cached

struct VSOut { float4 H : SV_POSITION; };

[RootSignature(VSM_PAGE_CLEAR_RS)]
VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    VSOut o;
    if (PerPageDirty[iid] == 0u)
    {
        o.H = float4(-2.0, -2.0, 1.0, 1.0); // off NDC (all 4 verts) -> clipped, zero coverage
        return o;
    }
    // Pool cell (gx,gy) -> NDC rect under the full-pool viewport. NDC y is up; pool row 0 is the top.
    const uint  gx   = iid % VSM_POOL_PAGES_AXIS;
    const uint  gy   = iid / VSM_POOL_PAGES_AXIS;
    const float axis = (float)VSM_POOL_PAGES_AXIS;
    const float x0 = (float)gx        / axis * 2.0 - 1.0;
    const float x1 = (float)(gx + 1u) / axis * 2.0 - 1.0;
    const float y0 = 1.0 - (float)gy        / axis * 2.0; // top edge
    const float y1 = 1.0 - (float)(gy + 1u) / axis * 2.0; // bottom edge
    // Triangle strip over 4 verts: (x0,y0),(x1,y0),(x0,y1),(x1,y1).
    float2 pos;
    pos.x = (vid & 1u) ? x1 : x0;
    pos.y = (vid & 2u) ? y1 : y0;
    o.H = float4(pos, 1.0, 1.0); // z = 1.0 (far / cleared depth)
    return o;
}

[RootSignature(VSM_PAGE_CLEAR_RS)]
void PSMain(VSOut i) { }
