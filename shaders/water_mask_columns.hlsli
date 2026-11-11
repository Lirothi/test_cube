// A hull's PLAN (ocean/WaterMask.h): its LOD 0 rasterized from above on a grid in the model's xz, per cell a float4 whose w is
// the lowest surface over the cell wherever the mesh touches it (+big where it does not). A particle with
// EmitterDesc::collideHulls dies anywhere over the plan, from its lowest surface up (particle_common.hlsli InsideHull, a
// sprite's pixel: particles.hlsl); ocean.waterMask:3 paints the water still drawn there. The water itself is kept out by the
// hulls' lids (water_mask_lid.hlsl), not by this.
// A hull is five float4 rows (ocean::WaterMaskFrame): the world-to-model rows (the model's x y z = dot(row.xyz, world) +
// row.w), the grid (origin x, origin z, 1 / cell, asuint its first cell in `columns`), the span (the mesh's lowest and
// highest y, asuint nx, asuint nz).
#ifndef WATER_MASK_COLUMNS_HLSLI
#define WATER_MASK_COLUMNS_HLSLI

// The column under (or over) a point given in the hull's model space, whatever its height: (+big, -big, -big, +big)
// off the grid.
float4 HullColumnOver(float2 modelXZ, float4 grid, float4 span, StructuredBuffer<float4> columns)
{
    const int2 cell = int2(floor((modelXZ - grid.xy) * grid.z));
    const uint nx = asuint(span.z);
    const uint nz = asuint(span.w);
    if (any(cell < 0) || cell.x >= (int)nx || cell.y >= (int)nz) { return float4(3.0e38f, -3.0e38f, -3.0e38f, 3.0e38f); }
    return columns[asuint(grid.w) + (uint)cell.y * nx + (uint)cell.x];
}

// Over a hull's plan and not under its lowest surface there: in its solid, in a cockpit's air, or in the air over its
// deck -- where no spray is (a particle's centre: particle_common.hlsli InsideHull; a sprite's pixel: particles.hlsl).
bool OverHullPlan(float3 worldPos, float4 r0, float4 r1, float4 r2, float4 grid, float4 span, StructuredBuffer<float4> columns)
{
    const float3 p = float3(dot(r0.xyz, worldPos) + r0.w, dot(r1.xyz, worldPos) + r1.w, dot(r2.xyz, worldPos) + r2.w);
    if (p.y < span.x) { return false; }
    const float planBottom = HullColumnOver(p.xz, grid, span, columns).w;
    return planBottom < 1.0e37f && p.y >= planBottom;
}

#endif // WATER_MASK_COLUMNS_HLSLI
