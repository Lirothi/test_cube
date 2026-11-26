#include "app/scene/Scene.h"
#include "app/settings/SettingContext.h"
#include "core/settings/SettingsRegistry.h"
#include "rendering/debug/LodDebugView.h"
#include "rendering/meshes/LodSelect.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// --set / --sweep / MCP set_settings names lod.* (mesh LOD) (architecture plan R2.2). Each row is the branch App.cpp's
// ApplySweepValue had, its clamp included, and the reader ReadSweepValue had (R2.2 gave every simple name one first, so
// --settings-dump compared the move); a row without a reader had none.
namespace settings
{
namespace
{
const SettingDesc kLodRows[] = {
    // Chunked-terrain LOD selection curve (per-chunk camera tiers; the caster matches the drawn
    // LOD by construction, so these trade triangles for pop-in distance only — no rebuild).
    TC_SETTING("lod.chunkDist0", Float, SettingPersistent,
               [](SettingContext&, float value) { render::g_chunkLodDist0_game = std::max(1.0f, value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_chunkLodDist0_game); return true; }),
    TC_SETTING("lod.chunkFactor", Float, SettingPersistent | SettingScalable | SettingCostNextFrame,
               [](SettingContext&, float value) { render::g_chunkLodDistFactor_game = std::max(1.01f, value); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(render::g_chunkLodDistFactor_game); return true; }),
    // Regular-mesh LOD selection (distance/radius tier boundaries; the dev "LOD" tab's sliders)
    // + the master enable and the per-mesh force, so a LOD capture needs no GUI.
    TC_SETTING("lod.bound0", Float, SettingPersistent,
               [](SettingContext&, float value) { render::g_lodBound0_game = std::max(0.5f, value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_lodBound0_game); return true; }),
    TC_SETTING("lod.bound1", Float, SettingPersistent,
               [](SettingContext&, float value) { render::g_lodBound1_game = std::max(0.5f, value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_lodBound1_game); return true; }),
    TC_SETTING("lod.bound2", Float, SettingPersistent,
               [](SettingContext&, float value) { render::g_lodBound2_game = std::max(0.5f, value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_lodBound2_game); return true; }),
    // UE's lod.TemporalLag: the seconds a LOD change dissolves over (temporal dither); 0 = hard switches.
    TC_SETTING("lod.temporalLag", Float, SettingPersistent,
               [](SettingContext&, float value) { render::g_lodTemporalLag_game = std::clamp(value, 0.0f, 2.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_lodTemporalLag_game); return true; }),
    // UE's static-mesh LOD (LodSelect.h): 1 = per-mesh screen sizes (default), 0 = the bounds above;
    // its PixelError (the automatic screen sizes) and r.StaticMeshLODDistanceScale.
    TC_SETTING("lod.ue", Bool, SettingPersistent,
               [](SettingContext&, float value) { render::g_lodUeScreenSize_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_lodUeScreenSize_game); return true; }),
    TC_SETTING("lod.pixelError", Float, SettingPersistent | SettingScalable | SettingCostNextFrame,
               [](SettingContext&, float value) { render::g_lodPixelError_game = std::clamp(value, 0.25f, 64.0f); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(render::g_lodPixelError_game); return true; }),
    TC_SETTING("lod.distanceScale", Float, SettingPersistent,
               [](SettingContext&, float value) { render::g_lodDistanceScale_game = std::clamp(value, 0.1f, 8.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_lodDistanceScale_game); return true; }),
    TC_SETTING("lod.enabled", Bool, SettingPersistent,
               [](SettingContext&, float value) { render::g_lodEnabled_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_lodEnabled_game); return true; }),
    TC_SETTING("lod.forced", Float, SettingTransient,
               [](SettingContext&, float value) { render::g_forcedLod_game = std::clamp((int)value, -1, 3); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_forcedLod_game); return true; }),
    // LOD selection debug view (dev "LOD" tab). 0 off, 1 tier colours, 2 apparent-triangle-size
    // colours. Exposed here so a debug capture is a command line, not a click path.
    TC_SETTING("lod.debug", Float, SettingTransient,
               [](SettingContext&, float value) { render::g_lodDebugMode_game = static_cast<render::LodDebugMode>(std::clamp((int)value, 0, 2)); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_lodDebugMode_game); return true; }),
    TC_SETTING("lod.debugBoxes", Bool, SettingTransient,
               [](SettingContext&, float value) { render::g_lodDebugBoxes_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_lodDebugBoxes_game); return true; }),
    TC_SETTING("lod.debugLabels", Bool, SettingTransient,
               [](SettingContext&, float value) { render::g_lodDebugLabels_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_lodDebugLabels_game); return true; }),
    TC_SETTING("lod.debugCriteria", Bool, SettingTransient,
               [](SettingContext&, float value) { render::g_lodDebugCriteria_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_lodDebugCriteria_game); return true; }),
    TC_SETTING("lod.debugRegular", Bool, SettingTransient,
               [](SettingContext&, float value) { render::g_lodDebugRegularMeshes_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_lodDebugRegularMeshes_game); return true; }),
    TC_SETTING("lod.debugRange", Float, SettingTransient,
               [](SettingContext&, float value) { render::g_lodDebugRange_game = std::max(10.0f, value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_lodDebugRange_game); return true; }),
    TC_SETTING("lod.debugMaxBoxes", Float, SettingTransient,
               [](SettingContext&, float value) { render::g_lodDebugMaxBoxes_game = std::max(0, (int)value); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_lodDebugMaxBoxes_game); return true; }),
    // 0 = whole level, 1 = editor selection only. A headless run has no selection, so 1 there
    // reports an empty view on purpose rather than silently meaning 0.
    TC_SETTING("lod.debugFilter", Float, SettingTransient,
               [](SettingContext&, float value) { render::g_lodDebugFilter_game = static_cast<render::LodDebugFilter>(std::clamp((int)value, 0, 1)); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_lodDebugFilter_game); return true; }),
};
TC_SETTINGS(kLodRows);
}
}
