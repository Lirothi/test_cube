#pragma once

#include <cstddef>

struct ID3D12GraphicsCommandList;

// R4.5a: the per-command-list half of `--gbv-passes` -- a list's GBV shader patch mode -- apart from the pass names
// (GbvPassFilter.h, the render level's): the frame's list pool resets a list here and the device takes its default
// mode from here, and neither knows a render pass. Why the filter exists and how it is used: GbvPassFilter.h.
namespace render::gbv
{
// `--gbv-rest=none|state`: the mode of every UNLISTED list, and the device default while the filter
// is armed. 1 = STATE_TRACKING_ONLY (default: exact state tracking, +1..3.5 s on wind_test), 0 = NONE
// (fastest; the runtime then calls state tracking "possibly imprecise", id=1044).
void RequestRestPatchMode(int mode);
int RestPatchMode();
// The mode a listed pass's lists get (`--gbv-mode`); ArmPassFilter stores it.
void SetListedPatchMode(int mode);
const char* PatchModeName(int mode); // D3D12_GPU_BASED_VALIDATION_SHADER_PATCH_MODE's, for the log

// One load and a branch on every list when the flag is absent.
inline bool g_passFilterArmed = false;

// Gives a list the listed mode (`patched`) or the rest mode.
void SetListMode(ID3D12GraphicsCommandList* cl, bool patched);

// Back to the rest mode when the pool hands a list out. Pooled lists move between passes from frame to
// frame, and a list a listed pass patched must not stay patched for whatever records into it next
// -- a renderer-internal list that never names a pass included.
void ResetListPatchModeSlow(ID3D12GraphicsCommandList* cl);
inline void ResetListPatchMode(ID3D12GraphicsCommandList* cl)
{
    if (g_passFilterArmed && cl) { ResetListPatchModeSlow(cl); }
}
} // namespace render::gbv
