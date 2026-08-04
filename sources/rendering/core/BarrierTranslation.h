#pragma once

#include <d3d12.h>

// Barrier plan step 10 (design D4) — legacy state -> enhanced barrier triple.
//
// DORMANT: nothing calls this yet. Steps 11-16 build the emission path on top of it; this step is
// the table plus the buffer/texture classification it needs, and it is judged on both builds
// staying 0/0 and behaviour being unchanged.
//
// A legacy `D3D12_RESOURCE_STATES` says only "what the resource is used AS". An enhanced barrier
// wants three separate facts: WHEN the access happens (sync), WHAT KIND it is (access), and — for
// textures — HOW THE MEMORY IS ARRANGED (layout). The mapping is therefore one-to-many, and the
// first cut is deliberately CONSERVATIVE: where a legacy state could be several pipeline stages,
// this takes the widest correct `SYNC_*`. Step 16 narrows it for perf, once correctness is proven.
namespace barriers {

struct Translated
{
    D3D12_BARRIER_SYNC   sync = D3D12_BARRIER_SYNC_NONE;
    D3D12_BARRIER_ACCESS access = D3D12_BARRIER_ACCESS_COMMON;
    // Buffers have no layout: D3D12_BUFFER_BARRIER carries none, and passing anything but
    // UNDEFINED for one is meaningless. Textures always use it.
    D3D12_BARRIER_LAYOUT layout = D3D12_BARRIER_LAYOUT_UNDEFINED;
};

// `state` may be a COMBINATION — the engine leans on those heavily (NON_PIXEL|PIXEL = 0xC0 for a
// texture read from both stages, NON_PIXEL|COPY_SOURCE = 0x840 for VSM.PhysOwner, NON_PIXEL|
// VERTEX_AND_CONSTANT_BUFFER = 0x41 for VSM.PageProj). Sync and access simply OR together; the
// LAYOUT cannot, since a texture has exactly one, so several read layouts collapse to
// LAYOUT_GENERIC_READ. That collapse is the whole reason this returns a struct rather than three
// independent lookups.
Translated LegacyStateToBarrier(D3D12_RESOURCE_STATES state, bool isBuffer);

// True when the state is a legal enhanced-barrier destination for a texture at all. Buffer-only
// states (index/vertex buffer, indirect argument, acceleration structure) have no texture layout,
// so asking for one is a bug in the caller rather than something to guess at.
bool IsTextureCompatible(D3D12_RESOURCE_STATES state);

} // namespace barriers
