#pragma once

#include "rendering/core/RenderContext.h"

#include <cstddef>

// gbuffer.hlsl's root signature declares root parameter 3 as the material's SRV table (t0..t2) and
// root parameter 4 as its sampler table. A mesh slot with NO MaterialData -- or a material with no
// textures at all -- leaves both handles null. Material::Bind then skips those binds, because it has
// nothing to bind, and the draw goes out anyway with two root parameters UNBOUND while the pixel
// shader samples them (gbuffer_common.hlsli, `txAlbedo.Sample`).
//
// The consequence is not a cosmetic glitch. GPU-based validation names it
//   "Draw, Uninitialized root argument accessed. Root Parameter Index: [3] / [4]"
// and the symptom in the wild is the GRAPHICS QUEUE going quiet in the middle of its batch: the
// device stays healthy, GetDeviceRemovedReason keeps returning S_OK, TDR never fires because nothing
// faulted, and every fence wait in the engine simply never returns. Dragging an un-materialed mesh
// into the viewport hung the editor exactly that way, and none of the fence, barrier or async-queue
// instrumentation could see it, because nothing was wrong with any of them.
//
// So the rule is: SKIP THE DRAW, not just the bind. An invisible submesh is a bug you can see; an
// unbound root argument is undefined behaviour that surfaces as a hang somewhere else entirely.
namespace render
{

inline bool HasGBufferMaterialBindings(const RenderContext& ctx)
{
    return ctx.srvTable[0].ptr != 0 && ctx.samplerTable[0].ptr != 0;
}

// Writes logs/missing_material.log once per (owner, slot) pair. Silence is what let this stay
// invisible until it turned into a queue stall, so it says so exactly once and then shuts up.
void ReportMissingGBufferBindings(const void* owner, std::size_t slot, const char* where);

// SELF-TEST (`--gbv-selftest=N`). Deliberately issues N draws that commit the exact violation
// above: root signature re-set (which invalidates every root argument), descriptor tables then NOT
// bound, draw issued anyway.
//
// It exists to answer one question that cannot be answered by reading a mode's name: does
// `--gbv-mode=state` still CATCH this? A validation mode chosen for being 11x faster is worth
// nothing if it is silent on the bug that motivated the gate, and "it probably still catches it"
// is not something to hand someone who is deciding what to trust.
//
// Never armed by default; the flag is Debug-only and the counter is one-shot.
inline int g_gbvSelfTestDraws = 0;

} // namespace render
