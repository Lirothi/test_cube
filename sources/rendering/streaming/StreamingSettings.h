#pragma once

#include "rendering/streaming/DdsMipTable.h"

// Texture streaming knobs (docs/texture_streaming_vt_plan.md §6, UE r.Streaming.*). `--set=streaming.<name>:<v>`.
namespace streaming {

inline bool  g_enabled = true;          // r.TextureStreaming: 0 = no manager, no pass, textures load whole
inline int   g_tempMemoryMB = 50;       // r.Streaming.MaxTempMemoryAllowed: upload ring size, read at first use
inline int   g_maxPerFrame = 8;         // resource swaps recorded per frame (plan §5)
inline int   g_forceMips = 0;           // A2 test knob: every texture wants clamp(N) mips, manager idle; 0 = off
inline int   g_maxIoInFlight = 16;      // reads outstanding on the worker
// A3 manager
inline int   g_poolSizeMB = -1;         // r.Streaming.PoolSize: -1 = 70 % of dedicated VRAM, 0 = unlimited
inline int   g_mipBias = 0;             // r.Streaming.MipBias (GlobalMipBias)
inline float g_boost = 1.0f;            // r.Streaming.Boost
inline float g_hiddenScale = 0.5f;      // r.Streaming.HiddenPrimitiveScale
inline int   g_framesForFullUpdate = 5; // r.Streaming.FramesForFullUpdate
inline int   g_minMipForSplit = 10;     // r.Streaming.MinMipForSplitRequest
inline bool  g_perTextureBias = true;   // r.Streaming.UsePerTextureBias
inline bool  g_fullyLoadUsed = false;   // r.Streaming.FullyLoadUsedTextures
inline int   g_dropMips = 0;            // r.Streaming.DropMips (1 = perfect, 2 = visible only)
inline bool  g_selftest = false;        // LOG_INFO the wanted-mips invariant at manager start

inline constexpr float kExtraBoost = 0.71f;      // StreamingTexture.h GetDefaultExtraBoost(new metrics)
inline constexpr float kVisibleWindowSec = 0.5f; // a bound seen this recently counts as visible
inline constexpr float kPoolVramFraction = 0.7f; // GPoolSizeVRAMPercentage
inline constexpr std::uint64_t kMemoryMarginMB = 5; // [TextureStreaming] MemoryMargin, BaseEngine.ini

} // namespace streaming
