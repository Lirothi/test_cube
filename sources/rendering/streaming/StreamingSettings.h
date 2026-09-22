#pragma once

// Texture streaming knobs (docs/texture_streaming_vt_plan.md §6). `--set=streaming.<name>:<v>`.
namespace streaming {

inline bool g_enabled = true;       // 0 = pass not built, no requests, textures stay as loaded
inline int  g_tempMemoryMB = 50;    // upload ring size (UE r.Streaming.MaxTempMemoryAllowed); read at first use
inline int  g_maxPerFrame = 8;      // resource swaps recorded per frame (amortised copies, plan §5)
inline int  g_forceMips = 0;        // A2 test knob: every streamable texture wants clamp(N) mips; 0 = off
inline int  g_maxIoInFlight = 16;   // reads outstanding on the worker at once

} // namespace streaming
