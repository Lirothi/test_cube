#pragma once

#include <cstdint>
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

// Step 12 — emit an already-COMPILED legacy transition array as enhanced barriers.
//
// The compile still produces `{resource, before, after}` triples; only the EMISSION changes. That
// is deliberate: steps 1-7 collapsed barrier emission to this single place, so the enhanced path
// is a translation at the very end rather than a second compiler.
//
// `isBuffer` answers per resource (Renderer holds that bit from step 10). Textures and buffers go
// into SEPARATE groups because D3D12 requires it — one group has one type.
//
// Returns false and emits NOTHING when `cl7` is null or a barrier cannot be expressed; the caller
// then falls back to ResourceBarrier, which keeps a gap in this table from becoming a lost barrier.
// Step 13 — the single global switch for enhanced EMISSION, published once by
// GraphicsDevice::InitDevice. Same process-global style the device file already uses for
// DRED/GBV/--legacy-barriers, and the same one texture creation reads.
void SetEnabled(bool enable);
bool Enabled();

// Step 13 — emit ONE already-built legacy barrier, enhanced when enabled and legacy otherwise.
//
// Takes the constructed `D3D12_RESOURCE_BARRIER` rather than its parts, because every direct site
// in the engine already builds one and then calls `ResourceBarrier(1, &b)` — so each converts by
// changing that one line. Handles TRANSITION and UAV.
//
// Buffer-vs-texture is derived from the resource itself: these sites are uploads, present and
// screenshots, i.e. rare, so one GetDesc costs nothing and spares every caller from knowing.
// Falls back to `ResourceBarrier` whenever the enhanced form cannot be built — never silently
// drops a barrier.
void EmitOne(ID3D12GraphicsCommandList* cl, const D3D12_RESOURCE_BARRIER& barrier);

// Step 14 — the acceleration-structure build barrier, which needs its own entry point.
//
// An AS is a BUFFER (so: no layout) that NO legacy state fully describes.
// `D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE` names only the READ side, and the
// build's WRITE has no legacy spelling at all — which is why the AS manager emits a bare UAV
// barrier today, and why routing it through `EmitOne` would be WRONG: a UAV barrier translates to
// `ACCESS_UNORDERED_ACCESS`, while an AS resource may only ever be accessed as
// `ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ/WRITE`. The enhanced model can say the real thing,
// so this says it rather than reusing the generic path.
//
// Meaning: "the build's writes to `as` are complete; readers may consume it" — the TLAS build for a
// BLAS, and the inline-RayQuery compute shader for the TLAS.
void EmitAccelerationStructureBuildBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* as);

// How many compiled points went out as enhanced barriers vs fell back to ResourceBarrier. The
// enhanced emission is written at step 12 but only becomes REACHABLE at step 13 (the direct
// upload/present sites still record legacy barriers, and mixing kills the run before a frame
// renders), so these exist to prove it actually executed rather than merely compiled.
void EmitStats(std::uint32_t& enhanced, std::uint32_t& legacy);
void NoteLegacyEmit();

// Step 14 — the same proof, for the AS path specifically. Broken out of the aggregate above
// because the AS barrier is the one emission this engine makes that NOTHING else can stand in
// for: it only happens when raytracing is actually enabled and a BLAS/TLAS actually builds, so a
// zero here means the step was never exercised rather than that it works.
void AsEmitStats(std::uint32_t& enhanced, std::uint32_t& legacy);

bool EmitEnhanced(ID3D12GraphicsCommandList7* cl7,
                  const D3D12_RESOURCE_BARRIER* entries,
                  std::uint32_t count,
                  bool (*isBuffer)(void* ctx, ID3D12Resource* res),
                  void* ctx);

} // namespace barriers
