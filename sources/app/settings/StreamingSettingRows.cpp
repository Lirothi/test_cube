#include "app/scene/Scene.h"
#include "app/settings/SettingContext.h"
#include "core/settings/SettingsRegistry.h"
#include "rendering/streaming/StreamingSettings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// --set / --sweep / MCP set_settings names streaming.* (texture streaming) (architecture plan R2.2). Each row is the
// branch App.cpp's ApplySweepValue had, its clamp included, and the reader ReadSweepValue had (R2.2 gave every simple
// name one first, so --settings-dump compared the move); a row without a reader had none.
namespace settings
{
namespace
{
const SettingDesc kStreamingRows[] = {
    // The directional-clipmap knobs the Developer window already carries, exposed to the
    // harness. Shadow bias is judged by A/B and the GUI cannot be driven headlessly, so
    // without these a clipmap artifact can only be argued about, not measured. They are
    // process globals, not scene state -- deliberately, so they survive a level switch the
    // way the dev-window sliders do.
    // Texture streaming (plan §6). forceMips is the A2 test knob: every streamable texture wants
    // clamp(N) mips, so `--sweep=streaming.forceMips:12,4,12,4` drives swaps both ways.
    TC_SETTING("streaming.enabled", Bool, SettingPersistent,
               [](SettingContext&, float value) { streaming::g_enabled_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(streaming::g_enabled_game); return true; }),
    TC_SETTING("streaming.tempMemoryMB", Int, SettingTransient,
               [](SettingContext&, float value) { streaming::g_tempMemoryMB_game = std::max(1, static_cast<int>(value)); return true; },
               [](SettingContext&, float& out) { out = AsFloat(streaming::g_tempMemoryMB_game); return true; }),
    TC_SETTING("streaming.maxPerFrame", Int, SettingTransient,
               [](SettingContext&, float value) { streaming::g_maxPerFrame_game = std::max(0, static_cast<int>(value)); return true; },
               [](SettingContext&, float& out) { out = AsFloat(streaming::g_maxPerFrame_game); return true; }),
    TC_SETTING("streaming.forceMips", Int, SettingTransient,
               [](SettingContext&, float value) { streaming::g_forceMips_game = static_cast<int>(value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(streaming::g_forceMips_game); return true; }),
    TC_SETTING("streaming.maxIoInFlight", Int, SettingTransient,
               [](SettingContext&, float value) { streaming::g_maxIoInFlight_game = std::max(1, static_cast<int>(value)); return true; },
               [](SettingContext&, float& out) { out = AsFloat(streaming::g_maxIoInFlight_game); return true; }),
    TC_SETTING("streaming.poolSizeMB", Int, SettingPersistent | SettingScalable | SettingCostNextFrame,
               [](SettingContext&, float value) { streaming::g_poolSizeMB_game = std::max(-1, static_cast<int>(value)); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(static_cast<float>(streaming::g_poolSizeMB_game)); return true; }),
    TC_SETTING("streaming.mipBias", Int, SettingPersistent | SettingScalable | SettingCostNextFrame,
               [](SettingContext&, float value) { streaming::g_mipBias_game = std::clamp(static_cast<int>(value), 0, 8); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(static_cast<float>(streaming::g_mipBias_game)); return true; }),
    TC_SETTING("streaming.boost", Float, SettingPersistent,
               [](SettingContext&, float value) { streaming::g_boost_game = std::clamp(value, 0.1f, 8.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(streaming::g_boost_game); return true; }),
    TC_SETTING("streaming.hiddenScale", Float, SettingPersistent,
               [](SettingContext&, float value) { streaming::g_hiddenScale_game = std::clamp(value, 0.0f, 1.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(streaming::g_hiddenScale_game); return true; }),
    TC_SETTING("streaming.framesForFullUpdate", Int, SettingTransient,
               [](SettingContext&, float value) { streaming::g_framesForFullUpdate_game = std::max(1, static_cast<int>(value)); return true; },
               [](SettingContext&, float& out) { out = AsFloat(streaming::g_framesForFullUpdate_game); return true; }),
    TC_SETTING("streaming.minMipForSplit", Int, SettingTransient,
               [](SettingContext&, float value) { streaming::g_minMipForSplit_game = static_cast<int>(value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(streaming::g_minMipForSplit_game); return true; }),
    TC_SETTING("streaming.perTextureBias", Bool, SettingPersistent,
               [](SettingContext&, float value) { streaming::g_perTextureBias_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(streaming::g_perTextureBias_game); return true; }),
    TC_SETTING("streaming.fullyLoadUsed", Bool, SettingTransient,
               [](SettingContext&, float value) { streaming::g_fullyLoadUsed_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(streaming::g_fullyLoadUsed_game); return true; }),
    TC_SETTING("streaming.dropMips", Int, SettingTransient,
               [](SettingContext&, float value) { streaming::g_dropMips_game = std::clamp(static_cast<int>(value), 0, 2); return true; },
               [](SettingContext&, float& out) { out = AsFloat(streaming::g_dropMips_game); return true; }),
    TC_SETTING("streaming.selftest", Bool, SettingTransient,
               [](SettingContext&, float value) { streaming::g_selftest_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(streaming::g_selftest_game); return true; }),
    TC_SETTING("streaming.dumpRows", Bool, SettingTransient,
               [](SettingContext&, float value) { streaming::g_dumpRows_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(streaming::g_dumpRows_game); return true; }),
    TC_SETTING("streaming.mipFade", Bool, SettingPersistent,
               [](SettingContext&, float value) { streaming::g_mipFade_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(streaming::g_mipFade_game); return true; }),
    TC_SETTING("streaming.mipFadeIn", Float, SettingTransient,
               [](SettingContext&, float value) { streaming::g_mipFadeIn_game = std::clamp(value, 0.01f, 10.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(streaming::g_mipFadeIn_game); return true; }),
    TC_SETTING("streaming.mipFadeOut", Float, SettingTransient,
               [](SettingContext&, float value) { streaming::g_mipFadeOut_game = std::clamp(value, 0.01f, 10.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(streaming::g_mipFadeOut_game); return true; }),
};
TC_SETTINGS(kStreamingRows);
}
}
