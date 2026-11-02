#pragma once

#include "rendering/streaming/DdsMipTable.h"
#include "core/threading/Knob.h" // game thread plan G5: knobs with a game twin

// Texture streaming knobs (docs/texture_streaming_vt_plan.md §6, UE r.Streaming.*). `--set=streaming.<name>:<v>`.
namespace streaming {

inline bool  g_enabled = true;          // r.TextureStreaming: 0 = no manager, no pass, textures load whole
inline bool  g_enabled_game = true;
inline const bool g_enabled_knob = ::knob::Register(g_enabled_game, g_enabled, "streaming::g_enabled");
inline int   g_tempMemoryMB = 50;       // r.Streaming.MaxTempMemoryAllowed: upload ring size, read at first use
inline int   g_tempMemoryMB_game = 50;
inline const bool g_tempMemoryMB_knob = ::knob::Register(g_tempMemoryMB_game, g_tempMemoryMB, "streaming::g_tempMemoryMB");
inline int   g_maxPerFrame = 8;         // resource swaps recorded per frame (plan §5)
inline int   g_maxPerFrame_game = 8;
inline const bool g_maxPerFrame_knob = ::knob::Register(g_maxPerFrame_game, g_maxPerFrame, "streaming::g_maxPerFrame");
inline int   g_forceMips = 0;           // A2 test knob: every texture wants clamp(N) mips, manager idle; 0 = off
inline int   g_forceMips_game = 0;
inline const bool g_forceMips_knob = ::knob::Register(g_forceMips_game, g_forceMips, "streaming::g_forceMips");
inline int   g_maxIoInFlight = 16;      // reads outstanding on the worker
inline int   g_maxIoInFlight_game = 16;
inline const bool g_maxIoInFlight_knob = ::knob::Register(g_maxIoInFlight_game, g_maxIoInFlight, "streaming::g_maxIoInFlight");
// A3 manager
inline int   g_poolSizeMB = -1;         // r.Streaming.PoolSize: -1 = 70 % of dedicated VRAM, 0 = unlimited
inline int   g_poolSizeMB_game = -1;
inline const bool g_poolSizeMB_knob = ::knob::Register(g_poolSizeMB_game, g_poolSizeMB, "streaming::g_poolSizeMB");
inline int   g_mipBias = 0;             // r.Streaming.MipBias (GlobalMipBias)
inline int   g_mipBias_game = 0;
inline const bool g_mipBias_knob = ::knob::Register(g_mipBias_game, g_mipBias, "streaming::g_mipBias");
inline float g_boost = 1.0f;            // r.Streaming.Boost
inline float g_boost_game = 1.0f;
inline const bool g_boost_knob = ::knob::Register(g_boost_game, g_boost, "streaming::g_boost");
inline float g_hiddenScale = 0.5f;      // r.Streaming.HiddenPrimitiveScale
inline float g_hiddenScale_game = 0.5f;
inline const bool g_hiddenScale_knob = ::knob::Register(g_hiddenScale_game, g_hiddenScale, "streaming::g_hiddenScale");
inline int   g_framesForFullUpdate = 5; // r.Streaming.FramesForFullUpdate
inline int   g_framesForFullUpdate_game = 5;
inline const bool g_framesForFullUpdate_knob = ::knob::Register(g_framesForFullUpdate_game, g_framesForFullUpdate, "streaming::g_framesForFullUpdate");
inline int   g_minMipForSplit = 10;     // r.Streaming.MinMipForSplitRequest
inline int   g_minMipForSplit_game = 10;
inline const bool g_minMipForSplit_knob = ::knob::Register(g_minMipForSplit_game, g_minMipForSplit, "streaming::g_minMipForSplit");
inline bool  g_perTextureBias = true;   // r.Streaming.UsePerTextureBias
inline bool  g_perTextureBias_game = true;
inline const bool g_perTextureBias_knob = ::knob::Register(g_perTextureBias_game, g_perTextureBias, "streaming::g_perTextureBias");
inline bool  g_fullyLoadUsed = false;   // r.Streaming.FullyLoadUsedTextures
inline bool  g_fullyLoadUsed_game = false;
inline const bool g_fullyLoadUsed_knob = ::knob::Register(g_fullyLoadUsed_game, g_fullyLoadUsed, "streaming::g_fullyLoadUsed");
inline int   g_dropMips = 0;            // r.Streaming.DropMips (1 = perfect, 2 = visible only)
inline int   g_dropMips_game = 0;
inline const bool g_dropMips_knob = ::knob::Register(g_dropMips_game, g_dropMips, "streaming::g_dropMips");
inline bool  g_selftest = false;        // LOG_INFO the wanted-mips invariant at manager start
inline bool  g_selftest_game = false;
inline const bool g_selftest_knob = ::knob::Register(g_selftest_game, g_selftest, "streaming::g_selftest");
inline bool  g_dumpRows = false;        // LOG_INFO the per-texture table once, then clears itself
inline bool  g_dumpRows_game = false;
inline const bool g_dumpRows_knob = ::knob::Register(g_dumpRows_game, g_dumpRows, "streaming::g_dumpRows");
// A4 (UE GEnableMipLevelFading, GMipFadeSettings[MipFade_Normal] = {0.3, 0.1} seconds per mip)
inline bool  g_mipFade = true;
inline bool  g_mipFade_game = true;
inline const bool g_mipFade_knob = ::knob::Register(g_mipFade_game, g_mipFade, "streaming::g_mipFade");
inline float g_mipFadeIn = 0.3f;
inline float g_mipFadeIn_game = 0.3f;
inline const bool g_mipFadeIn_knob = ::knob::Register(g_mipFadeIn_game, g_mipFadeIn, "streaming::g_mipFadeIn");
inline float g_mipFadeOut = 0.1f;
inline float g_mipFadeOut_game = 0.1f;
inline const bool g_mipFadeOut_knob = ::knob::Register(g_mipFadeOut_game, g_mipFadeOut, "streaming::g_mipFadeOut");

inline constexpr float kExtraBoost = 0.71f;      // StreamingTexture.h GetDefaultExtraBoost(new metrics)
inline constexpr float kVisibleWindowSec = 0.5f; // a bound seen this recently counts as visible
inline constexpr float kPoolVramFraction = 0.7f; // GPoolSizeVRAMPercentage
inline constexpr std::uint64_t kMemoryMarginMB = 5; // [TextureStreaming] MemoryMargin, BaseEngine.ini

} // namespace streaming
