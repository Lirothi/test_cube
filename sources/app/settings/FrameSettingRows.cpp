#include "app/BootOptions.h"
#include "app/FrameRequests.h"
#include "app/game/VesselCamera.h"
#include "app/scene/Scene.h"
#include "app/settings/Scalability.h"
#include "app/settings/SettingContext.h"
#include "app/Systems.h"
#include "core/logging/Log.h"
#include "core/settings/SettingsRegistry.h"
#include "core/threading/RenderThread.h"
#include "ocean/OceanReadback.h"
#include "rendering/core/DlssHandler.h"
#include "rendering/core/GpuAlloc.h"
#include "rendering/core/GraphicsDevice.h"
#include "rendering/core/Renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// --set / --sweep / MCP set_settings names frame.*, camera.*, scalability.*, dlss.preset, aa.fxaa (the frame, the
// camera, the graphics levels) (architecture plan R2.2). Each row is the branch App.cpp's ApplySweepValue had, its
// clamp included, and the reader ReadSweepValue had (R2.2 gave every simple name one first, so --settings-dump compared
// the move); a row without a reader had none.
namespace settings
{
namespace
{
const SettingDesc kFrameRows[] = {
    // Frame pacing (UE r.VSync / t.MaxFPS), so a run can pin them over graphics_settings.json.
    TC_SETTING("frame.vsync", Bool, SettingPersistent,
               [](SettingContext&, float value) { render::g_vsync_game = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_vsync_game); return true; }),
    TC_SETTING("frame.maxFps", Int, SettingPersistent,
               [](SettingContext&, float value) { render::g_maxFps_game = render::ClampMaxFps(static_cast<int>(value)); return true; },
               [](SettingContext&, float& out) { out = AsFloat(render::g_maxFps_game); return true; }),
    // Game thread plan G8: Scene::Render on a thread of its own; the loop takes it at the next frame's start.
    TC_SETTING("frame.renderThread", Bool, SettingPersistent,
               [](SettingContext&, float value) { threading::g_renderThreadEnabled = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(threading::g_renderThreadEnabled); return true; }),
    // UE's r.OneFrameThreadLag: 0 = the game half waits for each frame right after handing it over.
    TC_SETTING("frame.oneFrameThreadLag", Bool, SettingPersistent,
               [](SettingContext&, float value) { threading::g_oneFrameThreadLag = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(threading::g_oneFrameThreadLag); return true; }),
    // G10b, UE's r.RHIThread.Enable: the frame's submission and Present on a thread of their own (next BeginFrame).
    TC_SETTING("frame.rhiThread", Bool, SettingPersistent,
               [](SettingContext&, float value) { threading::g_rhiThreadEnabled = value != 0.0f; return true; },
               [](SettingContext&, float& out) { out = AsFloat(threading::g_rhiThreadEnabled); return true; }),
    TC_SETTING("frame.gbvCreationExcludesSubmission", Float, SettingTransient,
               [](SettingContext&, float value) { gpu::SetCreationExcludesSubmission(value != 0.0f && GraphicsDevice::GbvEnabled()); return true; },
               nullptr),
    // ...and turn at this many degrees a second (positive = to the right, 0 stops): --cam-fly-yaw for a running
    // editor, the other motion a temporal history has to survive.
    TC_SETTING("camera.mouseSmoothing", Float, SettingPersistent,
               [](SettingContext&, float value) { g_cameraMouseSmoothingMs = std::clamp(value, 0.0f, kCameraMouseSmoothingMaxMs); return true; },
               [](SettingContext&, float& out) { out = AsFloat(g_cameraMouseSmoothingMs); return true; }),
    TC_SETTING("camera.spin", Float, SettingTransient,
               [](SettingContext&, float value) { Boot().frame.camFlyYaw = std::clamp(value, -720.0f, 720.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(Boot().frame.camFlyYaw); return true; }),
    // A boat's camera, at the helm and on a bot's boat (race.watch): 0 behind, 1 the cockpit, 2 cinema (V in Play).
    TC_SETTING("camera.boatView", Int, SettingTransient,
               [](SettingContext& c, float value)
               {
                   const auto view = static_cast<game::VesselCamera::View>(std::clamp(static_cast<int>(std::lround(value)), 0, 2));
                   c.scene.VesselCameraRef().SetView(view);
                   c.scene.SpectatorCameraRef().SetView(view);
                   return true;
               },
               [](SettingContext& c, float& out) { out = AsFloat(c.scene.VesselCameraRef().GetView()); return true; }),
    TC_SETTING("aa.fxaa", Bool, SettingPersistent | SettingScalable | SettingCostNextFrame,
               [](SettingContext& c, float value) { c.renderSettings.doFxaa = value != 0.0f; return true; },
               [](SettingContext& c, float& out) { out = (c.renderSettings.doFxaa) ? 1.0f : 0.0f; return true; }),
    // Auto-detect's control (docs/scalability_plan.md §8.4) and a run of it now.
    TC_SETTING("scalability.gpuTimeScale", Float, SettingTransient,
               [](SettingContext&, float value) { settings::TheScalability().autodetectGpuScale = std::clamp(value, 0.1f, 64.0f); return true; },
               [](SettingContext&, float& out) { out = AsFloat(settings::TheScalability().autodetectGpuScale); return true; }),
    // a script's: never saved
    TC_SETTING("scalability.autodetect", Float, SettingTransient,
               [](SettingContext&, float value) { if (value != 0.0f) { settings::TheScalability().RequestAutodetect(false); } return true; },
               nullptr),
    TC_SETTING("dlss.preset", Float, SettingTransient,
               [](SettingContext&, float value) { g_dlssPresetOverride.store(std::clamp(static_cast<int>(value), -1, 15)); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(g_dlssPresetOverride.load()); return true; }),
    // Occlusion plan S0: recorded, applied after the --set loop (needs the renderer + level path).
    TC_SETTING("scene.replicate", Int, SettingTransient,
               [](SettingContext&, float value) { g_sceneReplicateRequest = static_cast<std::uint32_t>(std::clamp(value, 1.0f, 32.0f)); return true; },
               [](SettingContext&, float& out) { out = static_cast<float>(g_sceneReplicateRequest); return true; }),
    // A film, frame by frame (g_recordDt, app/FrameRequests.h): seconds of the world a captured frame advances; 0 = real
    // time.
    TC_SETTING("frame.recordDt", Float, SettingTransient,
               [](SettingContext&, float value) { g_recordDt = std::clamp(value, 0.0f, 0.1f); return true; },
               [](SettingContext&, float& out) { out = g_recordDt; return true; }),
    // The world's clock, times this (g_timeScale, app/FrameRequests.h). The copies of the water's heights follow it.
    TC_SETTING("frame.timeScale", Float, SettingTransient,
               [](SettingContext&, float value)
               {
                   g_timeScale = std::clamp(value, 0.1f, 16.0f);
                   OceanReadback::SetTimeScale(g_timeScale);
                   LOG_INFO(logging::LogCategory::App, "frame.timeScale: the world's clock runs x{:.2f}", g_timeScale);
                   return true;
               },
               [](SettingContext&, float& out) { out = g_timeScale; return true; }),
    // Fly along the view at this many m/s (negative = backwards, 0 stops) -- motion through a running editor.
    TC_SETTING("camera.fly", Float, SettingTransient,
               [](SettingContext&, float value) { g_camFlyForward = std::clamp(value, -100.0f, 100.0f); return true; },
               [](SettingContext&, float& out) { out = g_camFlyForward; return true; }),
    // The upscaler at the frame's safe point (g_requestDlssMode); it reads as asked until the frame takes it.
    TC_SETTING("dlss.mode", Int, SettingTransient | SettingScalable | SettingCostTargets,
               [](SettingContext&, float value) { g_requestDlssMode = std::clamp(static_cast<int>(value), 0, 6); return true; },
               [](SettingContext&, float& out)
               {
                   out = static_cast<float>(static_cast<std::uint64_t>(static_cast<std::uint32_t>(
                       g_requestDlssMode >= 0 ? g_requestDlssMode : static_cast<int>(Systems::GetRenderer().GetDlssMode()))));
                   return true;
               }),
    // Without DLSS (the resolution group's native road): the render scale at the frame's safe point, FXAA at once.
    TC_SETTING("aa.renderScale", Float, SettingTransient | SettingScalable | SettingCostTargets,
               [](SettingContext&, float value) { g_requestRenderScale = std::clamp(value, 0.25f, 1.0f); return true; },
               [](SettingContext&, float& out)
               {
                   out = g_requestRenderScale >= 0.0f ? g_requestRenderScale : Systems::GetRenderer().GetRenderResolutionScale();
                   return true;
               }),
};
TC_SETTINGS(kFrameRows);
}
}
