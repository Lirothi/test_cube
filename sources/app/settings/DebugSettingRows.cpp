#include "app/BootOptions.h"
#include "app/scene/Scene.h"
#include "app/settings/SettingContext.h"
#include "core/settings/SettingsRegistry.h"
#include "rendering/scene/SceneFrameData.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// --set / --sweep / MCP set_settings names debug.* (debug views, the pixel probe, the HUD) (architecture plan R2.2).
// Each row is the branch App.cpp's ApplySweepValue had, its clamp included, and the reader ReadSweepValue had (R2.2
// gave every simple name one first, so --settings-dump compared the move); a row without a reader had none.
namespace settings
{
namespace
{
const SettingDesc kDebugRows[] = {
    // --no-hud for a running process: 0 hides the HUD's text (a film's frames), 1 shows it.
    TC_SETTING("debug.hud", Float, SettingTransient,
               [](SettingContext&, float value) { Boot().frame.hudHidden = value == 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(Boot().frame.hudHidden); return true; }),
    // The fullscreen debug blit, so a half-res intermediate can be captured headlessly instead
    // of only judged by eye in the GUI. `debug.tex` = 0 shadow atlas, 1 GTAO raw, 2 denoised,
    // 3 temporal, 4 upsampled, 5 HZB furthest, 6 scene depth, 7 HZB closest, 8 reflection hit
    // mask, 9 GB1 normal (colour), 10 shading model (GBAux.b x5: water white), 11 GBAux alpha
    // (the water's reflection weight), 12 the water trace's hit mask, 13 GB0 alpha (~roughness);
    // `debug.texMode` turns the blit on.
    // The profiler overlay (F9) without a key: a headless --shot of it, or MCP set_settings.
    TC_SETTING("debug.profilerOverlay", Bool, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.showProfiler = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.showProfiler); return true; }),
    TC_SETTING("debug.texMode", Bool, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.debugTexMode = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.debugTexMode); return true; }),
    TC_SETTING("debug.tex", Float, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.debugTexTarget = (int)value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.debugTexTarget); return true; }),
    TC_SETTING("debug.texMip", Float, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.debugTexMip = (int)value; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.debugTexMip); return true; }),
    // The pixel probe (SceneRenderSettings::debugProbe*), read through get_readouts.
    TC_SETTING("debug.probeU", Float, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.debugProbeU = std::clamp(value, 0.0f, 1.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.debugProbeU); return true; }),
    TC_SETTING("debug.probeV", Float, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.debugProbeV = std::clamp(value, 0.0f, 1.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.debugProbeV); return true; }),
    TC_SETTING("debug.probeRadius", Float, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.debugProbeRadiusPx = std::clamp(value, 0.0f, 128.0f); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.debugProbeRadiusPx); return true; }),
    TC_SETTING("debug.probeSource", Float, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.debugProbeSource = value >= 1.5f ? 2u : (value >= 0.5f ? 1u : 0u); return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.debugProbeSource); return true; }),
    TC_SETTING("debug.sunProbeAfterDlss", Bool, SettingTransient,
               [](SettingContext& c, float value) { c.renderSettings.sunProbeAfterDlss = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = AsFloat(c.renderSettings.sunProbeAfterDlss); return true; }),
};
TC_SETTINGS(kDebugRows);
}
}
