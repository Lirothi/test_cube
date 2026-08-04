#include "rendering/core/BarrierTranslation.h"

#include <atomic>

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

namespace {
std::atomic<std::uint32_t> gEnhancedEmits{ 0 };
std::atomic<std::uint32_t> gLegacyEmits{ 0 };
std::atomic<std::uint32_t> gAsEnhancedEmits{ 0 };
std::atomic<std::uint32_t> gAsLegacyEmits{ 0 };
} // namespace

std::atomic<bool> gEnabled{ false };

void SetEnabled(bool enable) { gEnabled.store(enable, std::memory_order_relaxed); }
bool Enabled() { return gEnabled.load(std::memory_order_relaxed); }

void EmitOne(ID3D12GraphicsCommandList* cl, const D3D12_RESOURCE_BARRIER& barrier)
{
    if (cl == nullptr) { return; }

    if (Enabled()) {
        ID3D12Resource* res = (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
            ? barrier.Transition.pResource
            : (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV ? barrier.UAV.pResource : nullptr);
        ID3D12GraphicsCommandList7* cl7 = nullptr;
        if (SUCCEEDED(cl->QueryInterface(IID_PPV_ARGS(&cl7))) && cl7) {
            cl7->Release(); // borrowed view; `cl` keeps it alive
            const bool isBuffer =
                res != nullptr && res->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
            if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                const auto isBufferFn = [](void* ctx, ID3D12Resource*) {
                    return *static_cast<const bool*>(ctx);
                };
                bool flag = isBuffer;
                if (EmitEnhanced(cl7, &barrier, 1, isBufferFn, &flag)) { return; }
            }
            else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV && res == nullptr) {
                // A blanket UAV barrier (null resource) is a GLOBAL barrier in the enhanced model.
                D3D12_GLOBAL_BARRIER g{};
                g.SyncBefore = D3D12_BARRIER_SYNC_ALL;
                g.SyncAfter = D3D12_BARRIER_SYNC_ALL;
                g.AccessBefore = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
                g.AccessAfter = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
                D3D12_BARRIER_GROUP grp{};
                grp.Type = D3D12_BARRIER_TYPE_GLOBAL;
                grp.NumBarriers = 1;
                grp.pGlobalBarriers = &g;
                cl7->Barrier(1, &grp);
                gEnhancedEmits.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
                // A UAV barrier on ONE resource: same layout and access on both sides, which is
                // how the enhanced model spells "finish the writes before the next reads".
                D3D12_BARRIER_GROUP grp{};
                if (isBuffer) {
                    D3D12_BUFFER_BARRIER bb{};
                    bb.SyncBefore = D3D12_BARRIER_SYNC_COMPUTE_SHADING | D3D12_BARRIER_SYNC_PIXEL_SHADING;
                    bb.SyncAfter = bb.SyncBefore;
                    bb.AccessBefore = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
                    bb.AccessAfter = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
                    bb.pResource = res;
                    bb.Offset = 0;
                    bb.Size = UINT64_MAX;
                    grp.Type = D3D12_BARRIER_TYPE_BUFFER;
                    grp.NumBarriers = 1;
                    grp.pBufferBarriers = &bb;
                    cl7->Barrier(1, &grp);
                }
                else {
                    D3D12_TEXTURE_BARRIER tb{};
                    tb.SyncBefore = D3D12_BARRIER_SYNC_COMPUTE_SHADING | D3D12_BARRIER_SYNC_PIXEL_SHADING;
                    tb.SyncAfter = tb.SyncBefore;
                    tb.AccessBefore = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
                    tb.AccessAfter = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
                    tb.LayoutBefore = D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
                    tb.LayoutAfter = D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
                    tb.pResource = res;
                    tb.Subresources.IndexOrFirstMipLevel = 0xffffffff;
                    tb.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;
                    grp.Type = D3D12_BARRIER_TYPE_TEXTURE;
                    grp.NumBarriers = 1;
                    grp.pTextureBarriers = &tb;
                    cl7->Barrier(1, &grp);
                }
                gEnhancedEmits.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }

    cl->ResourceBarrier(1, &barrier);
    gLegacyEmits.fetch_add(1, std::memory_order_relaxed);
}

void EmitAccelerationStructureBuildBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* as)
{
    if (cl == nullptr || as == nullptr) { return; }

    if (Enabled()) {
        ID3D12GraphicsCommandList7* cl7 = nullptr;
        if (SUCCEEDED(cl->QueryInterface(IID_PPV_ARGS(&cl7))) && cl7) {
            cl7->Release(); // borrowed view; `cl` keeps it alive
            D3D12_BUFFER_BARRIER bb{};
            bb.SyncBefore = D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE;
            bb.AccessBefore = D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE;
            // Readers are: another BUILD (the TLAS consuming this BLAS), DispatchRays, and — the
            // case this engine actually uses — inline RayQuery inside a compute shader, which syncs
            // as COMPUTE_SHADING rather than RAYTRACING. All three, deliberately: a too-wide sync
            // is only slow, a missing one is a race. Step 16 narrows it.
            bb.SyncAfter = D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE |
                           D3D12_BARRIER_SYNC_RAYTRACING | D3D12_BARRIER_SYNC_COMPUTE_SHADING;
            bb.AccessAfter = D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ;
            bb.pResource = as;
            bb.Offset = 0;
            bb.Size = UINT64_MAX;
            D3D12_BARRIER_GROUP grp{};
            grp.Type = D3D12_BARRIER_TYPE_BUFFER;
            grp.NumBarriers = 1;
            grp.pBufferBarriers = &bb;
            cl7->Barrier(1, &grp);
            gEnhancedEmits.fetch_add(1, std::memory_order_relaxed);
            gAsEnhancedEmits.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    // Unchanged legacy form: a UAV barrier is how the pre-enhanced model spells "the build wrote
    // this, order the readers after it".
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = as;
    cl->ResourceBarrier(1, &uav);
    gLegacyEmits.fetch_add(1, std::memory_order_relaxed);
    gAsLegacyEmits.fetch_add(1, std::memory_order_relaxed);
}

void AsEmitStats(std::uint32_t& enhanced, std::uint32_t& legacy)
{
    enhanced = gAsEnhancedEmits.load(std::memory_order_relaxed);
    legacy = gAsLegacyEmits.load(std::memory_order_relaxed);
}

void EmitStats(std::uint32_t& enhanced, std::uint32_t& legacy)
{
    enhanced = gEnhancedEmits.load(std::memory_order_relaxed);
    legacy = gLegacyEmits.load(std::memory_order_relaxed);
}

void NoteLegacyEmit() { gLegacyEmits.fetch_add(1, std::memory_order_relaxed); }

bool EmitEnhanced(ID3D12GraphicsCommandList7* cl7,
                  const D3D12_RESOURCE_BARRIER* entries,
                  std::uint32_t count,
                  bool (*isBuffer)(void* ctx, ID3D12Resource* res),
                  void* ctx)
{
    if (cl7 == nullptr || entries == nullptr || count == 0 || isBuffer == nullptr) { return false; }
    // Bounded by the compile: a point holds at most one barrier per registered use, and the use
    // budget per pass is kResourceUsesPerPassBudget (48). Sized to that with a hard refusal rather
    // than a silent truncation — a dropped barrier is a race, and falling back to the legacy call
    // is always correct.
    constexpr std::uint32_t kMax = 64;
    if (count > kMax) { return false; }

    D3D12_TEXTURE_BARRIER tex[kMax]{};
    D3D12_BUFFER_BARRIER buf[kMax]{};
    std::uint32_t texCount = 0;
    std::uint32_t bufCount = 0;

    for (std::uint32_t i = 0; i < count; ++i) {
        const D3D12_RESOURCE_BARRIER& b = entries[i];
        // Only TRANSITIONs are compiled today. Anything else (UAV, aliasing) belongs to step 13.
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) { return false; }
        ID3D12Resource* res = b.Transition.pResource;
        if (res == nullptr) { return false; }

        const bool asBuffer = isBuffer(ctx, res);
        const Translated before = LegacyStateToBarrier(b.Transition.StateBefore, asBuffer);
        const Translated after = LegacyStateToBarrier(b.Transition.StateAfter, asBuffer);

        if (asBuffer) {
            D3D12_BUFFER_BARRIER& out = buf[bufCount++];
            out.SyncBefore = before.sync;
            out.SyncAfter = after.sync;
            out.AccessBefore = before.access;
            out.AccessAfter = after.access;
            out.pResource = res;
            out.Offset = 0;
            out.Size = UINT64_MAX; // whole resource, mirroring the legacy all-subresources barrier
        }
        else {
            // A texture transition MUST name both layouts. If either side has none the state is
            // buffer-only and was mis-declared for this resource — refuse rather than invent one.
            if (before.layout == D3D12_BARRIER_LAYOUT_UNDEFINED ||
                after.layout == D3D12_BARRIER_LAYOUT_UNDEFINED) {
                return false;
            }
            D3D12_TEXTURE_BARRIER& out = tex[texCount++];
            out.SyncBefore = before.sync;
            out.SyncAfter = after.sync;
            out.AccessBefore = before.access;
            out.AccessAfter = after.access;
            out.LayoutBefore = before.layout;
            out.LayoutAfter = after.layout;
            out.pResource = res;
            // All subresources, matching D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES.
            out.Subresources.IndexOrFirstMipLevel = 0xffffffff;
            out.Subresources.NumMipLevels = 0;
            out.Subresources.FirstArraySlice = 0;
            out.Subresources.NumArraySlices = 0;
            out.Subresources.FirstPlane = 0;
            out.Subresources.NumPlanes = 0;
            out.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;
        }
    }

    // One group per TYPE — D3D12 requires a group to be homogeneous.
    D3D12_BARRIER_GROUP groups[2]{};
    UINT32 groupCount = 0;
    if (texCount > 0) {
        D3D12_BARRIER_GROUP& g = groups[groupCount++];
        g.Type = D3D12_BARRIER_TYPE_TEXTURE;
        g.NumBarriers = texCount;
        g.pTextureBarriers = tex;
    }
    if (bufCount > 0) {
        D3D12_BARRIER_GROUP& g = groups[groupCount++];
        g.Type = D3D12_BARRIER_TYPE_BUFFER;
        g.NumBarriers = bufCount;
        g.pBufferBarriers = buf;
    }
    if (groupCount == 0) { return false; }

    cl7->Barrier(groupCount, groups);
    // Counted per POINT, symmetrically with the caller's NoteLegacyEmit() on the fallback — so
    // "enhanced vs legacy" compares like with like. Without this the compiled path, which is the
    // bulk of the engine's barriers, was invisible in the totals and only its FAILURES showed up.
    gEnhancedEmits.fetch_add(1, std::memory_order_relaxed);
    return true;
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
