// Which pages of the sun's clipmap a caster can shadow: one bit per (level, page), behind the page-request bitfield, for the
// water's marks (vsm_page_request_cs.hlsl). The opaque scene asks for a page under a surface it sees; the water asked for one
// under every pixel of the sea out to the horizon, ~30 pages a level, 83 % of the pool's use on an open-sea view, and those
// pages draw empty. A caster's box in the light's frame IS the column it can shadow, whatever its height, so a page none of
// them touches has nothing to read: this is vsm_page_scatter_cs's own test (the same box, the same page grid) asked ahead of
// the allocation, on every level.
#pragma pack_matrix(row_major)
#include "utils.hlsli"
#include "vsm_addressing.hlsli"

#define VSM_WATER_MASK_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=3, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), " \
    "DescriptorTable(UAV(u0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer WaterMaskCB : register(b0)
{
    uint  gNumCasters;
    uint  gNumViews;
    uint  gMaskBase; // the mask's first word: the bitfield's length
    float gMargin;   // m round every box (vsm::kWaterMaskMargin)
};

struct CasterBounds { float4 center; float4 halfExtents; };

StructuredBuffer<CasterBounds> Bounds     : register(t0); // per-caster world AABB (unified buffer)
StructuredBuffer<uint>         CasterMeta : register(t1); // bits1+ = slot count on the FIRST slot
StructuredBuffer<VsmViewData>  Views      : register(t2); // this frame's request slot
RWStructuredBuffer<uint>       Request    : register(u0);

[numthreads(64, 1, 1)]
[RootSignature(VSM_WATER_MASK_RS)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    const uint c = dtid.x;
    if (c >= gNumCasters) { return; }
    if ((CasterMeta[c] >> 1u) == 0u) { return; } // a continuation slot: its object was handled by its first slot
    const CasterBounds b = Bounds[c];
    if (b.halfExtents.w != 0.0f) { return; } // UE Cast Shadow off: no page takes it
    const float3 ext = b.halfExtents.xyz + gMargin;
    // Coarse to fine: the levels are nested, so a box outside one is outside every finer one. Most casters are far from
    // the camera and are done after a few levels.
    const uint end = min(gNumViews, VSM_CLIPMAP_END);
    for (uint cv = end; cv-- > VSM_CLIPMAP_FIRST; )
    {
        if (Views[cv].params.x < 0.5f) { continue; }
        // An orthographic level: w = 1, and the box's reach along each NDC axis is the row's absolute dot.
        const float4x4 vp = Views[cv].viewProj;
        const float2 ctr = mul(float4(b.center.xyz, 1.0f), vp).xy;
        const float2 r = abs(vp[0].xy) * ext.x + abs(vp[1].xy) * ext.y + abs(vp[2].xy) * ext.z;
        const float2 lo = ctr - r;
        const float2 hi = ctr + r;
        if (hi.x < -1.0f || lo.x > 1.0f || hi.y < -1.0f || lo.y > 1.0f) { break; } // outside this level, and the finer ones
        // NDC -> the level's page grid, y flipped: vsm_page_scatter_cs's rect.
        const int axis = (int)VSM_WATER_MASK_AXIS;
        const uint px0 = (uint)clamp((int)floor((lo.x * 0.5f + 0.5f) * axis), 0, axis - 1);
        const uint px1 = (uint)clamp((int)floor((hi.x * 0.5f + 0.5f) * axis), 0, axis - 1);
        const uint py0 = (uint)clamp((int)floor((0.5f - hi.y * 0.5f) * axis), 0, axis - 1);
        const uint py1 = (uint)clamp((int)floor((0.5f - lo.y * 0.5f) * axis), 0, axis - 1);
        const uint rowBits = (0xFFFFu >> (15u - (px1 - px0))) << px0;
        for (uint py = py0; py <= py1; ++py)
        {
            const uint word = gMaskBase + (cv - VSM_CLIPMAP_FIRST) * 8u + (py >> 1u);
            const uint bits = rowBits << ((py & 1u) * 16u);
            // Read first, as MarkPage does: the coarse levels' few words would take an atomic from nearly every caster.
            if ((Request[word] & bits) == bits) { continue; }
            uint prev;
            InterlockedOr(Request[word], bits, prev);
        }
    }
}
