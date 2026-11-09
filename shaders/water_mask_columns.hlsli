// A hull's columns (ocean/WaterMask.h): its LOD 0 rasterized from above on a grid in the model's xz, per cell (bottom, the
// top of the water it keeps out, the top of its solid, 0). The ocean's surface discards its water inside the first span
// (ocean_surface_surf_sim.hlsli), a particle with EmitterDesc::collideHulls dies inside the second (particle_update_cs.hlsl).
// A hull is five float4 rows (ocean::WaterMaskFrame): the world-to-model rows (the model's x y z = dot(row.xyz, world) +
// row.w), the grid (origin x, origin z, 1 / cell, asuint its first cell in `columns`), the span (the mesh's lowest and
// highest y, asuint nx, asuint nz).
#ifndef WATER_MASK_COLUMNS_HLSLI
#define WATER_MASK_COLUMNS_HLSLI

// The point's column in this hull, or (+big, -big, -big, 0) where it has none. modelY: the point's height in the model.
float4 HullColumnAt(float3 worldPos, float4 r0, float4 r1, float4 r2, float4 grid, float4 span,
                    StructuredBuffer<float4> columns, out float modelY)
{
    const float3 p = float3(dot(r0.xyz, worldPos) + r0.w, dot(r1.xyz, worldPos) + r1.w, dot(r2.xyz, worldPos) + r2.w);
    modelY = p.y;
    const float4 none = float4(3.0e38f, -3.0e38f, -3.0e38f, 0.0f);
    if (p.y < span.x || p.y > span.y) { return none; }
    const int2 cell = int2(floor((p.xz - grid.xy) * grid.z));
    const uint nx = asuint(span.z);
    const uint nz = asuint(span.w);
    if (any(cell < 0) || cell.x >= (int)nx || cell.y >= (int)nz) { return none; }
    return columns[asuint(grid.w) + (uint)cell.y * nx + (uint)cell.x];
}

#endif // WATER_MASK_COLUMNS_HLSLI
