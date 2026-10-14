#pragma once

#include "rendering/core/RenderPass.h"

#include <cstddef>
#include <string>

struct ID3D12GraphicsCommandList;

// `--gbv-passes=<name,name,...>`: GPU-based validation's SHADER PATCHING on the listed passes only.
//
// Why. GBV rewrites a shader on the CPU the first time its pipeline is bound into a command list,
// and the cost follows the DXIL size: lighting_cs.hlsl alone takes over a minute, and a Debug
// wind_test run waits ~2.5 minutes for its first frame (docs/gbv_startup_profile.md). Checking the
// one pass you are working on should not cost the rewrite of every shader in the frame.
//
// How. The device's DEFAULT patch mode becomes the rest mode (`--gbv-rest`: STATE_TRACKING_ONLY, or
// NONE), and every command list recorded for a listed pass gets the requested `--gbv-mode` (guarded
// unless asked) through ID3D12DebugCommandList1::SetDebugParameter. An unlisted list is at most
// patched to track resource state, so nothing a shader does inside it is CHECKED -- uninitialized
// root arguments, out-of-bounds descriptors, the state of what it reads. That is the price, and
// `--gbv-selftest` measures it (the doc again). Validation that does not live in the shader -- the
// debug layer's API checks, barriers -- is unchanged.
//
// Names are RenderPassToWString's, case-insensitive. A name also covers its dotted sub-passes:
// `GBuffer` = GBuffer + GBuffer.Driver, GBuffer.OpaqueSimple, ..., because the outer pass records
// through its inner graph's lists. A CL GROUP records into ONE list, so one listed member makes the
// whole group listed (logged once). Debug only, like GBV itself. Not a gate: `--scene-stress-gbv`
// ignores the flag and patches everything.
namespace render::gbv
{

// main.cpp, before device creation: the raw comma list. Empty = off.
void RequestPassFilter(std::string list);

// `--gbv-rest=none|state`: the mode of every UNLISTED list, and the device default while the filter
// is armed. 1 = STATE_TRACKING_ONLY (default: exact state tracking, +1..3.5 s on wind_test), 0 = NONE
// (fastest; the runtime then calls state tracking "possibly imprecise", id=1044).
void RequestRestPatchMode(int mode);
int RestPatchMode();

// GraphicsDevice::InitDevice, GBV on: resolves the names -- logging the passes that will be patched
// and every name that matched none -- and arms the filter. Returns false, leaving the device to
// patch everything, when nothing was requested or no name matched a pass: a validation run must
// never quietly validate less than it was asked to.
bool ArmPassFilter(int listedPatchMode);

// One load and a branch on every list when the flag is absent.
inline bool g_passFilterArmed = false;

// Gives a list its pass's mode: the requested one if the pass is listed, the rest mode otherwise. Called by
// RenderGraphPassContext::BeginCL and by SetCommandListName -- the one call every list a pass
// records into goes through, including a body's own lists and its fan-out workers'.
void ApplyPassPatchModeSlow(ID3D12GraphicsCommandList* cl, RenderPass pass);
inline void ApplyPassPatchMode(ID3D12GraphicsCommandList* cl, RenderPass pass)
{
    if (g_passFilterArmed && cl) { ApplyPassPatchModeSlow(cl, pass); }
}

// Back to the rest mode when the pool hands a list out. Pooled lists move between passes from frame to
// frame, and a list a listed pass patched must not stay patched for whatever records into it next
// -- a renderer-internal list that never names a pass included.
void ResetListPatchModeSlow(ID3D12GraphicsCommandList* cl);
inline void ResetListPatchMode(ID3D12GraphicsCommandList* cl)
{
    if (g_passFilterArmed && cl) { ResetListPatchModeSlow(cl); }
}

// RenderGraph::EndCLGroup: the members share one list, so a listed member lists them all. Logged
// once, naming the members it added.
void JoinCLGroup(const RenderPass* members, std::size_t count);

} // namespace render::gbv
