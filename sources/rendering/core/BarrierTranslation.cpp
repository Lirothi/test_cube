#include "rendering/core/BarrierTranslation.h"

namespace barriers {
namespace {

// One row per legacy BIT. A combined state is the OR of its rows' sync/access; `layout` is the
// texture layout that bit implies, or UNDEFINED for a buffer-only state.
struct Row
{
    D3D12_RESOURCE_STATES bit;
    D3D12_BARRIER_SYNC    sync;
    D3D12_BARRIER_ACCESS  access;
    D3D12_BARRIER_LAYOUT  layout;      // UNDEFINED => contributes no texture layout
    bool                  readOnly;    // several read layouts may collapse to GENERIC_READ
};

constexpr Row kRows[] = {
    // Render targets and depth.
    { D3D12_RESOURCE_STATE_RENDER_TARGET,
      D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET,
      D3D12_BARRIER_LAYOUT_RENDER_TARGET, false },
    { D3D12_RESOURCE_STATE_DEPTH_WRITE,
      D3D12_BARRIER_SYNC_DEPTH_STENCIL, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,
      D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE, false },
    { D3D12_RESOURCE_STATE_DEPTH_READ,
      D3D12_BARRIER_SYNC_DEPTH_STENCIL, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ,
      D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ, true },

    // Shader reads. The two shader-resource bits differ ONLY in sync — same access, same layout —
    // which is exactly why 0xC0 (both) has to OR rather than pick.
    { D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
      D3D12_BARRIER_SYNC_VERTEX_SHADING | D3D12_BARRIER_SYNC_COMPUTE_SHADING |
          D3D12_BARRIER_SYNC_RAYTRACING,
      D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, true },
    { D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
      D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
      D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, true },

    // UAV. Writable, so it never collapses into GENERIC_READ.
    { D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_BARRIER_SYNC_COMPUTE_SHADING | D3D12_BARRIER_SYNC_PIXEL_SHADING,
      D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, false },

    // Copies and resolves.
    { D3D12_RESOURCE_STATE_COPY_DEST,
      D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_DEST,
      D3D12_BARRIER_LAYOUT_COPY_DEST, false },
    { D3D12_RESOURCE_STATE_COPY_SOURCE,
      D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_COPY_SOURCE,
      D3D12_BARRIER_LAYOUT_COPY_SOURCE, true },
    { D3D12_RESOURCE_STATE_RESOLVE_DEST,
      D3D12_BARRIER_SYNC_RESOLVE, D3D12_BARRIER_ACCESS_RESOLVE_DEST,
      D3D12_BARRIER_LAYOUT_RESOLVE_DEST, false },
    { D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
      D3D12_BARRIER_SYNC_RESOLVE, D3D12_BARRIER_ACCESS_RESOLVE_SOURCE,
      D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE, true },

    // Buffer-only states: no texture layout, hence UNDEFINED.
    { D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
      D3D12_BARRIER_SYNC_VERTEX_SHADING | D3D12_BARRIER_SYNC_DRAW,
      D3D12_BARRIER_ACCESS_VERTEX_BUFFER | D3D12_BARRIER_ACCESS_CONSTANT_BUFFER,
      D3D12_BARRIER_LAYOUT_UNDEFINED, true },
    { D3D12_RESOURCE_STATE_INDEX_BUFFER,
      D3D12_BARRIER_SYNC_INDEX_INPUT, D3D12_BARRIER_ACCESS_INDEX_BUFFER,
      D3D12_BARRIER_LAYOUT_UNDEFINED, true },
    { D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, // == _PREDICATION, same bit
      D3D12_BARRIER_SYNC_EXECUTE_INDIRECT, D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,
      D3D12_BARRIER_LAYOUT_UNDEFINED, true },
    { D3D12_RESOURCE_STATE_STREAM_OUT,
      D3D12_BARRIER_SYNC_VERTEX_SHADING, D3D12_BARRIER_ACCESS_STREAM_OUTPUT,
      D3D12_BARRIER_LAYOUT_UNDEFINED, false },
    { D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
      D3D12_BARRIER_SYNC_RAYTRACING | D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,
      D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ,
      D3D12_BARRIER_LAYOUT_UNDEFINED, true },

    // Shading-rate image (texture, read by the rasteriser).
    { D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE,
      D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADING_RATE_SOURCE,
      D3D12_BARRIER_LAYOUT_SHADING_RATE_SOURCE, true },
};

} // namespace

Translated LegacyStateToBarrier(D3D12_RESOURCE_STATES state, bool isBuffer)
{
    Translated out;

    // COMMON is ZERO, not a bit — and PRESENT has the same value, so the two are indistinguishable
    // here by construction. LAYOUT_PRESENT and LAYOUT_COMMON are also the same value in D3D12, so
    // that ambiguity costs nothing: both want the common layout.
    if (state == D3D12_RESOURCE_STATE_COMMON) {
        out.sync = D3D12_BARRIER_SYNC_ALL;
        out.access = D3D12_BARRIER_ACCESS_COMMON;
        out.layout = isBuffer ? D3D12_BARRIER_LAYOUT_UNDEFINED : D3D12_BARRIER_LAYOUT_COMMON;
        return out;
    }

    D3D12_BARRIER_LAYOUT single = D3D12_BARRIER_LAYOUT_UNDEFINED;
    int layoutCount = 0;      // distinct layouts the combination asks for
    bool allLayoutsRead = true;
    D3D12_RESOURCE_STATES unmatched = state;

    for (const Row& row : kRows) {
        if ((state & row.bit) != row.bit) { continue; }
        unmatched = static_cast<D3D12_RESOURCE_STATES>(unmatched & ~row.bit);
        out.sync = static_cast<D3D12_BARRIER_SYNC>(out.sync | row.sync);
        out.access = static_cast<D3D12_BARRIER_ACCESS>(out.access | row.access);
        if (row.layout == D3D12_BARRIER_LAYOUT_UNDEFINED) { continue; }
        if (layoutCount == 0 || row.layout != single) { ++layoutCount; }
        single = row.layout;
        allLayoutsRead = allLayoutsRead && row.readOnly;
    }

    // A bit this table does not know. Fall back to the widest correct answer rather than silently
    // dropping it — an under-specified barrier is a race, and a too-wide one is only slow.
    if (unmatched != 0) {
        out.sync = D3D12_BARRIER_SYNC_ALL;
        out.access = D3D12_BARRIER_ACCESS_COMMON;
        out.layout = isBuffer ? D3D12_BARRIER_LAYOUT_UNDEFINED : D3D12_BARRIER_LAYOUT_COMMON;
        return out;
    }

    if (isBuffer) {
        out.layout = D3D12_BARRIER_LAYOUT_UNDEFINED; // buffers carry no layout
        return out;
    }

    // A texture has exactly ONE layout. Several read layouts at once (NON_PIXEL|COPY_SOURCE, the
    // combined read states this engine declares for VSM.PhysOwner and friends) is precisely what
    // LAYOUT_GENERIC_READ exists for. Several layouts where one is writable is not expressible and
    // would be a mis-declaration, so it takes the COMMON layout — legal everywhere, and slow
    // enough to be noticed rather than silently wrong.
    if (layoutCount <= 1) { out.layout = single; }
    else if (allLayoutsRead) { out.layout = D3D12_BARRIER_LAYOUT_GENERIC_READ; }
    else { out.layout = D3D12_BARRIER_LAYOUT_COMMON; }
    return out;
}

bool IsTextureCompatible(D3D12_RESOURCE_STATES state)
{
    if (state == D3D12_RESOURCE_STATE_COMMON) { return true; }
    D3D12_RESOURCE_STATES remaining = state;
    for (const Row& row : kRows) {
        if ((state & row.bit) != row.bit) { continue; }
        if (row.layout == D3D12_BARRIER_LAYOUT_UNDEFINED) { return false; } // buffer-only bit
        remaining = static_cast<D3D12_RESOURCE_STATES>(remaining & ~row.bit);
    }
    return remaining == 0;
}

} // namespace barriers
