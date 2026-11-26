#include "app/scene/Scene.h"
#include "app/settings/SettingContext.h"
#include "core/settings/SettingsRegistry.h"
#include "rendering/shadows/TessySettings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// --set / --sweep / MCP set_settings names tessy.* (cluster geometry) (architecture plan R2.2). Each row is the branch
// App.cpp's ApplySweepValue had, its clamp included, and the reader ReadSweepValue had (R2.2 gave every simple name one
// first, so --settings-dump compared the move); a row without a reader had none.
namespace settings
{
namespace
{
const SettingDesc kTessyRows[] = {
    // Tessy N3 (docs/tessy_plan.md): the VSM page render draws the casters with clusters cluster by cluster; the
    // cull's sway pad (0 only to see what it prevents). Live: the next frame's builder reads them.
    // the master switch
    TC_SETTING("tessy.enabled", Bool, SettingPersistent,
               [](SettingContext&, float value) { tessy::g_enabled_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(tessy::g_enabled_game); return true; }),
    // the cluster view
    TC_SETTING("tessy.debug", Float, SettingTransient,
               [](SettingContext&, float value) { tessy::g_debugView_game = static_cast<unsigned>(std::clamp(value, 0.0f, 3.0f)); return true; },
               [](SettingContext&, float& out) { out = AsFloat(tessy::g_debugView_game); return true; }),
    TC_SETTING("tessy.vsm", Bool, SettingPersistent,
               [](SettingContext&, float value) { tessy::g_vsm_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(tessy::g_vsm_game); return true; }),
    TC_SETTING("tessy.windPad", Float, SettingTransient,
               [](SettingContext&, float value) { tessy::g_vsmWindPad_game = static_cast<unsigned>(std::clamp(value, 0.0f, 2.0f)); return true; },
               [](SettingContext&, float& out) { out = AsFloat(tessy::g_vsmWindPad_game); return true; }),
    TC_SETTING("tessy.validate", Bool, SettingTransient,
               [](SettingContext&, float value) { tessy::g_vsmValidate_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(tessy::g_vsmValidate_game); return true; }),
    // N6: 1 mesh shader, 0 VS
    TC_SETTING("tessy.raster", Bool, SettingPersistent,
               [](SettingContext&, float value) { tessy::g_vsmMeshShader_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(tessy::g_vsmMeshShader_game); return true; }),
    // N4: the lamps' atlas views
    TC_SETTING("tessy.atlas", Bool, SettingPersistent,
               [](SettingContext&, float value) { tessy::g_atlas_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(tessy::g_atlas_game); return true; }),
    // N5: the camera's G-buffer
    TC_SETTING("tessy.gbuffer", Bool, SettingPersistent,
               [](SettingContext&, float value) { tessy::g_gbuffer_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(tessy::g_gbuffer_game); return true; }),
    // the SDSM sun partitions
    TC_SETTING("tessy.sdsm", Bool, SettingPersistent,
               [](SettingContext&, float value) { tessy::g_sdsm_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(tessy::g_sdsm_game); return true; }),
    // N5: its clusters' HZB test
    TC_SETTING("tessy.gbufferHzb", Bool, SettingTransient,
               [](SettingContext&, float value) { tessy::g_gbufferHzb_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(tessy::g_gbufferHzb_game); return true; }),
    // the camera's records in draw order
    TC_SETTING("tessy.ordered", Bool, SettingTransient,
               [](SettingContext&, float value) { tessy::g_ordered_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(tessy::g_ordered_game); return true; }),
    TC_SETTING("tessy.camSplit", Float, SettingTransient,
               [](SettingContext&, float value) { tessy::g_camSplit_game = static_cast<unsigned>(std::clamp(value, 1.0f, 32.0f)); return true; },
               [](SettingContext&, float& out) { out = AsFloat(tessy::g_camSplit_game); return true; }),
    // N7: the cluster DAG's cut
    TC_SETTING("tessy.lod", Bool, SettingPersistent,
               [](SettingContext&, float value) { tessy::g_lod_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(tessy::g_lod_game); return true; }),
    TC_SETTING("tessy.lodShadowTexels", Float, SettingTransient,
               [](SettingContext&, float value) { tessy::g_lodShadowTexels_game = std::max(value, 0.01f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(tessy::g_lodShadowTexels_game); return true; }),
    // the DAG's
    TC_SETTING("tessy.lodPixelError", Float, SettingPersistent | SettingScalable | SettingCostNextFrame,
               [](SettingContext&, float value) { tessy::g_lodPixelError_game = std::clamp(value, 0.25f, 64.0f); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(tessy::g_lodPixelError_game); return true; }),
    // N7e: the camera DAG's level band
    TC_SETTING("tessy.lodBand", Bool, SettingTransient,
               [](SettingContext&, float value) { tessy::g_lodBand_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(tessy::g_lodBand_game); return true; }),
    // SDSM: the camera's DAG cut
    TC_SETTING("tessy.sunCut", Bool, SettingTransient,
               [](SettingContext&, float value) { tessy::g_sunCameraCut_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(tessy::g_sunCameraCut_game); return true; }),
    // VSM clipmap pages: the same
    TC_SETTING("tessy.vsmCameraCut", Bool, SettingTransient,
               [](SettingContext&, float value) { tessy::g_vsmCameraCut_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(tessy::g_vsmCameraCut_game); return true; }),
    // clusters holding no sample
    TC_SETTING("tessy.clusterCull", Bool, SettingTransient,
               [](SettingContext&, float value) { tessy::g_clusterCull_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(tessy::g_clusterCull_game); return true; }),
    TC_SETTING("tessy.msCull", Float, SettingTransient,
               [](SettingContext&, float value) { tessy::g_vsmMsCull_game = static_cast<unsigned>(std::clamp(value, 0.0f, 1.0f)); return true; },
               [](SettingContext&, float& out) { out = AsFloat(tessy::g_vsmMsCull_game); return true; }),
};
TC_SETTINGS(kTessyRows);
}
}
