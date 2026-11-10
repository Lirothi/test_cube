// A hull's columns (ocean/WaterMask.h): its LOD 0 rasterized from above on a grid in the model's xz, per cell (bottom, the
// top of the water it keeps out, the top of its solid, the plan: the lowest surface over the cell wherever the mesh
// touches it, +big where it does not); behind the nx * nz columns, a float4 a cell each, the SKIN under the cell's four
// samples (component sk * 2 + si, the sample at ((si + 0.5) / 2, (sk + 0.5) / 2) of the cell; +big = none) and the TOPS of
// its four quarters (-big = none). The ocean's surface discards its water between the skin and the top
// (HullKeepsWaterOut), a particle with EmitterDesc::collideHulls dies anywhere over the plan, from its lowest surface up
// (particle_common.hlsli InsideHull).
// A hull is five float4 rows (ocean::WaterMaskFrame): the world-to-model rows (the model's x y z = dot(row.xyz, world) +
// row.w), the grid (origin x, origin z, 1 / cell, asuint its first cell in `columns`), the span (the mesh's lowest and
// highest y, asuint nx, asuint nz).
#ifndef WATER_MASK_COLUMNS_HLSLI
#define WATER_MASK_COLUMNS_HLSLI

// The skin is taken this far lower (m): the interpolation's error over a curved bottom must leave no film of water inside.
#define WATER_MASK_SKIN_BIAS 0.01f

// Does this hull keep the water out of the point? Under the top of the point's quarter of its cell, and over the hull's
// skin there -- the lowest surface, interpolated between the four samples round the point (two a cell each way), so the
// water ends on the skin itself and not a cell inside it. A sample with no skin (a cell that keeps nothing) takes no part.
bool HullKeepsWaterOut(float3 worldPos, float4 r0, float4 r1, float4 r2, float4 grid, float4 span, StructuredBuffer<float4> columns)
{
    const float3 p = float3(dot(r0.xyz, worldPos) + r0.w, dot(r1.xyz, worldPos) + r1.w, dot(r2.xyz, worldPos) + r2.w);
    if (p.y < span.x || p.y > span.y) { return false; }
    const float2 inCells = (p.xz - grid.xy) * grid.z;
    const int2 cell = int2(floor(inCells));
    const uint nx = asuint(span.z);
    const uint nz = asuint(span.w);
    if (any(cell < 0) || cell.x >= (int)nx || cell.y >= (int)nz) { return false; }
    const uint first = asuint(grid.w);
    const uint cells = nx * nz;
    const int2 quarter = int2(floor(inCells * 2.0f)) & 1;
    const float top = columns[first + 2u * cells + (uint)cell.y * nx + (uint)cell.x][(uint)quarter.y * 2u + (uint)quarter.x];
    if (!(p.y <= top)) { return false; }
    const float2 inSamples = inCells * 2.0f - 0.5f;
    const int2 corner = int2(floor(inSamples));
    const float2 t = inSamples - (float2)corner;
    float sum = 0.0f;
    float weight = 0.0f;
    for (uint c = 0u; c < 4u; ++c)
    {
        const int2 q = corner + int2((int)(c & 1u), (int)(c >> 1u));
        if (any(q < 0) || q.x >= (int)(2u * nx) || q.y >= (int)(2u * nz)) { continue; }
        const float skin = columns[first + cells + (uint)(q.y >> 1) * nx + (uint)(q.x >> 1)][(uint)(q.y & 1) * 2u + (uint)(q.x & 1)];
        const float w = ((c & 1u) != 0u ? t.x : 1.0f - t.x) * ((c >> 1u) != 0u ? t.y : 1.0f - t.y);
        if (skin < 1.0e37f)
        {
            sum += w * skin;
            weight += w;
        }
    }
    return weight > 1.0e-4f && p.y >= sum / weight - WATER_MASK_SKIN_BIAS;
}

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
