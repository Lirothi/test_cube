#pragma once
#include <array>
#include <cstdint>

namespace render
{
// S0 of docs/occlusion_culling_plan.md: per-view visibility counters. RenderStats knows draw calls
// and primitives for the whole frame; nothing said how many OBJECTS a view was offered, how many
// its frustum kept, how many an occlusion pass will cut, or how many terrain CHUNKS it draws --
// and those are the numbers every later step of that plan is accepted on.
//
// Written by Scene::PrepareViewQueue, one slot per view, BEFORE instanced batches are built so an
// object counts once whether or not it later joins a batch. Each slot has exactly one writer
// (the view's queue task), so the fields are plain integers; NextFrame() snapshots on the main
// thread after the frame's tasks are joined, the same moment RenderStats snapshots.
struct VisibilityViewCounters
{
    std::uint32_t objectsIn = 0;         // what the view's source offered (after layer/caster filters)
    std::uint32_t objectsFrustum = 0;    // survived the frustum test, per object
    std::uint32_t objectsOccluded = 0;   // cut by an occlusion test (S3a/S3b/S5); 0 until then
    std::uint32_t chunksIn = 0;          // terrain chunks of the surviving chunked objects
    std::uint32_t chunksDrawn = 0;       // chunks the view will draw (S1 mask); == chunksIn until S1
    std::uint32_t instancesDrawn = 0;    // GI instances + batch members + plain objects submitted
    std::uint64_t trianglesSubmitted = 0; // index count / 3 at the selected LOD -- CPU-path estimate
};

// Slot layout: 0 = camera, 1..4 = directional cascades c0..c3. Local-light and clipmap views have
// no slot -- the plan's steps are accepted on the camera and, for the "shadows must not change"
// rule, on the cascades.
inline constexpr unsigned kVisibilityViews = 5;
inline constexpr unsigned kVisibilityViewCamera = 0;
inline constexpr unsigned kVisibilityViewCascade0 = 1;

struct VisibilityStats
{
    std::array<VisibilityViewCounters, kVisibilityViews> current{}; // this frame, being written
    std::array<VisibilityViewCounters, kVisibilityViews> last{};    // completed frame, read by HUD/readout

    void NextFrame()
    {
        last = current;
        for (VisibilityViewCounters& c : current) { c = VisibilityViewCounters{}; }
    }
};

inline VisibilityStats g_visibilityStats;

// Set by `--vis-readout`: dump the per-view table once to logs/visibility_readout.log, on frame
// 600 (not earlier -- the level is still streaming in, see the csm_readout note in Scene.cpp).
inline bool g_visDumpReadout = false;

// S1: the frustum test BELOW the object level -- terrain chunks (camera: RenderableObject::SelectLod
// writes the mask; cascades: RenderShadow tests on the spot against the cascade's cull volume) and
// GI instances (GpuInstancedModels::BuildLodPartition). `--set=vis.chunkMask:0` is the step's
// rollback: every chunk/instance whose OBJECT passed is drawn, as before S1. Shadows cast by the
// GPU-driven path (ExecuteIndirect, VSM) never read this -- their per-chunk cull is the GPU's.
inline bool g_visChunkMask = true;
} // namespace render
