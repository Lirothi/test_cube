// R10.4: Developer Controls > Frame (DeveloperWindow.h). A shipped build compiles none of this.
#if !TC_SHIPPING
#include "app/ui/DeveloperWindow.h"
#include "app/ui/DeveloperTabs.h" // GRAPHICS_CONTROL
#include "app/settings/UserSettings.h"

#include <algorithm>
#include <string>

#include "app/GraphicsSettings.h"
#include "app/scene/Scene.h"
#include "core/logging/Log.h"
#include "imgui.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/RenderStats.h"
#include "rendering/shadows/ShadowSettings.h"
#include "rendering/core/RasterSettings.h" // A6: the bindless checkbox
#include "core/threading/RenderThread.h"  // game thread plan G8: the Frame tab's switch

void DeveloperWindow::DrawFrameTab(Renderer& renderer, Scene& scene, const InputManager& input,
    SceneRenderSettings& settings)
{
    const float fps = renderer.GetFPS();
    const float frameMs = fps > 0.0f ? 1000.0f / fps : 0.0f;
    ImGui::Text("FPS: %.1f (%.2f ms)", fps, frameMs);
    ImGui::Text("Display: %ux%u", renderer.GetWidth(), renderer.GetHeight());
    ImGui::Text("Render: %ux%u (scale %.2f)", renderer.GetRenderWidth(), renderer.GetRenderHeight(), renderer.GetRenderResolutionScale());
    // Two sources, because the CPU no longer sees most of the scene: the palms, the
    // CSM and the VSM pages are drawn by ExecuteIndirect, and the old line read
    // "Draw calls: 2" on wind_test. Draw calls are the CPU's own record -- each
    // ExecuteIndirect one call that MAY issue many draws. Triangles are the GPU's.
    const render::RenderStats& rs = render::g_renderStats;
    ImGui::Text("Draw calls: %u direct + %u ExecuteIndirect (up to %llu GPU draws)",
        rs.lastDrawCalls, rs.lastIndirectCalls,
        static_cast<unsigned long long>(rs.lastIndirectMaxDraws));
    render::RequestPipelineStats(); // this panel is what keeps the GPU counters on
    if (rs.gpuValid)
    {
        ImGui::Text("Triangles (GPU): %.3fM in, %.3fM past clipping   Pixels shaded: %.1fM",
            static_cast<double>(rs.gpuPrimitives) / 1.0e6,
            static_cast<double>(rs.gpuRasterized) / 1.0e6,
            static_cast<double>(rs.gpuPixelShaded) / 1.0e6);
        if (rs.gpuListsUnmeasured != 0)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                "%u of %u command lists past the query budget are NOT counted",
                rs.gpuListsUnmeasured, rs.gpuLists + rs.gpuListsUnmeasured);
        }
        if (ImGui::TreeNode("Triangles by pass"))
        {
            for (int i = 0; i < rs.gpuTopCount; ++i)
            {
                // In, its share of the frame's, and what survived the clipper (a VSM page draw feeds
                // every caster whole to each page it touches; the page borders throw the rest away).
                ImGui::Text("%-32s %8.3fM  %4.1f%%  past clipping %8.3fM", rs.gpuTop[i].name,
                    static_cast<double>(rs.gpuTop[i].primitives) / 1.0e6,
                    rs.gpuPrimitives > 0
                        ? 100.0 * static_cast<double>(rs.gpuTop[i].primitives) /
                              static_cast<double>(rs.gpuPrimitives)
                        : 0.0,
                    static_cast<double>(rs.gpuTop[i].rasterized) / 1.0e6);
            }
            ImGui::TreePop();
        }
    }
    else
    {
        ImGui::TextDisabled(render::g_pipelineStatsMode_game == 0
            ? "Triangles (GPU): off (--set=stats.pipeline:0)"
            : "Triangles (GPU): measuring...");
    }

    ImGui::Separator();
    // Frame pacing: UE's r.VSync and t.MaxFPS, both off by default as UE ships them.
    bool vsync = render::g_vsync_game;
    if (GRAPHICS_CONTROL(VSync, "vsync", ImGui::Checkbox("VSync", &vsync)))
    {
        render::g_vsync_game = vsync;
    }
    DevHelp("UE r.VSync. On: Present waits for the monitor's vblank (sync interval 1), "
            "so the frame rate tops out at the refresh rate and nothing tears. Off: "
            "present immediately, tearing allowed. Applies from the next Present.");
    // Enter-to-apply: typing "144" must not pass through a 1 fps and a 14 fps cap on
    // the way. The local copy is reseeded every frame; the field keeps its own text
    // while it is being edited.
    int maxFps = render::g_maxFps_game;
    if (GRAPHICS_CONTROL(MaxFps, "maxFps",
            ImGui::InputInt("Max FPS", &maxFps, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)))
    {
        render::g_maxFps_game = render::ClampMaxFps(maxFps);
    }
    DevHelp("UE t.MaxFPS. 0 = uncapped, otherwise 10..1000; press Enter to apply. The "
            "main loop sleeps out the rest of each 1/N s frame, so the GPU sits idle "
            "for that remainder instead of rendering frames nobody sees -- uncapped, it "
            "never idles, and video playing on another monitor stutters. Independent "
            "of VSync, as in UE: with both on, the lower rate wins.");
    float mouseSmoothing = g_cameraMouseSmoothingMs;
    if (GRAPHICS_CONTROL(MouseSmoothing, "mouseSmoothing",
            ImGui::SliderFloat("Mouse smoothing (ms)", &mouseSmoothing, 0.0f, 200.0f, "%.0f")))
    {
        g_cameraMouseSmoothingMs = std::clamp(mouseSmoothing, 0.0f, kCameraMouseSmoothingMaxMs);
    }
    DevHelp("The free camera's look follows the mouse this many milliseconds behind (an exponential "
            "ease, the same at any frame rate) instead of taking each frame's mouse counts at once, "
            "which at a few hundred fps arrive unevenly and make a turn stutter. 0 = off. For "
            "recording a video 40-80 ms reads smooth; the turn finishes after the button is let go. "
            "Ctrl+click to type a value.");
    // The player's look (user_settings.json controls, the game menu's Controls tab in a build without the
    // editor; owner, 2026-11-17: "move it to the settings"). A setting of the person's, never a binding:
    // input/bindings.json carried LookY inverted until then, so every distribution shipped the
    // developer's flight-style look and the menu's checkbox turned it NORMAL.
    settings::ControlsSettings controls = settings::CurrentControlsSettings();
    bool saveControls = false;
    if (ImGui::Checkbox("Invert vertical look", &controls.invertY))
    {
        settings::ApplyControlsSettings(controls);
        saveControls = true;
    }
    DevHelp("The mouse pushed away looks DOWN (flight style) -- the free camera, the player and the boat "
            "cameras alike. The person's setting, saved in user_settings.json (controls.invertY) and the "
            "same as the game menu's Controls > Invert vertical look; --set controls.invertY:1 for a run.");
    if (ImGui::SliderFloat("Mouse sensitivity", &controls.sensitivity, settings::kSensitivityMin,
                           settings::kSensitivityMax, "x%.2f", ImGuiSliderFlags_Logarithmic))
    {
        settings::ApplyControlsSettings(controls);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) { saveControls = true; }
    DevHelp("The look's speed times the bindings' own (input/bindings.json), 0.25-4. The person's setting, "
            "saved in user_settings.json (controls.sensitivity) when the slider is let go; the game menu's "
            "Controls > Mouse sensitivity.");
    if (saveControls)
    {
        std::string why;
        if (!settings::SaveControlsSettings(settings::CurrentControlsSettings(), why))
        {
            LOG_WARNING(logging::LogCategory::App, "controls: not saved: {}", why);
        }
    }
    bool renderThread = threading::g_renderThreadEnabled;
    if (GRAPHICS_CONTROL(RenderThread, "renderThread", ImGui::Checkbox("Render thread", &renderThread)))
    {
        threading::g_renderThreadEnabled = renderThread; // taken at the next frame's start
    }
    DevHelp("Game thread plan G8 (docs/game_thread_plan.md). On (the default): Scene::Render -- views, "
            "recording, submit, Present -- runs on a thread of its own, while this thread runs the next "
            "frame's game half (messages, the UI and the editor, MCP, Scene::Tick) and then waits for it "
            "before the hand-over. Off: one thread, the frame as before. The picture is the same either "
            "way.");
    ImGui::BeginDisabled(!threading::g_renderThreadEnabled);
    bool oneFrameLag = threading::g_oneFrameThreadLag;
    if (GRAPHICS_CONTROL(OneFrameThreadLag, "oneFrameThreadLag",
            ImGui::Checkbox("One frame thread lag", &oneFrameLag)))
    {
        threading::g_oneFrameThreadLag = oneFrameLag;
    }
    ImGui::EndDisabled();
    DevHelp("UE r.OneFrameThreadLag (default on, as in UE). On: the game half runs the next frame while "
            "the render thread draws this one -- the overlap, and one frame between input and picture. "
            "Off: the game half waits for each frame right after handing it over -- no overlap, no frame "
            "of lag. Only with the render thread on.");
    bool rhiThread = threading::g_rhiThreadEnabled;
    if (GRAPHICS_CONTROL(RhiThread, "rhiThread", ImGui::Checkbox("RHI thread", &rhiThread)))
    {
        threading::g_rhiThreadEnabled = rhiThread; // taken at the next frame's start (Renderer::BeginFrame)
    }
    DevHelp("Game thread plan G10b, UE r.RHIThread.Enable. On: the frame's submission -- its command "
            "lists, the waits between the queues, Present and the frame fences -- runs on a thread of its "
            "own, so the frame ends before Present and the next one records meanwhile (at most one frame "
            "ahead of it); the default. Off: the frame submits and presents itself. The picture is the "
            "same either way.");

    ImGui::Separator();
    // Async compute (plan step 8's `--no-async-compute`, made live). Sits next to the
    // trace controls because that is where its effect is READ: the passes it moves
    // change GPU track, and the counters below say whether they actually did.
    //
    // Stored inverted, because the command-line switch is a DISABLE and the checkbox
    // has to read the way the feature is thought about. On a device where the second
    // queue failed to create there is no toggle at all, only the state: a control that
    // cannot change anything is worse than no control.
    if (renderer.GetComputeQueue() == nullptr)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
            "Async compute: no second queue on this device - graphics queue only.");
    }
    else
    {
        bool asyncCompute = !render::g_noAsyncCompute_game;
        if (GRAPHICS_CONTROL(AsyncCompute, "asyncCompute",
                             ImGui::Checkbox("Async compute", &asyncCompute)))
        {
            render::g_noAsyncCompute_game = !asyncCompute;
        }
        DevHelp("Runs Pass_BuildAS and Pass_RTTrace on the second (compute) queue, "
                "overlapped with shadow rasterisation. Worth about -3% frame time on "
                "this scene; the passes themselves get SLOWER (BuildAS +17%, RTTrace "
                "+88%) and win by hiding behind raster. Takes effect next frame - the "
                "graph is rebuilt every frame. Turn it off to bisect a suspected async "
                "regression without a rebuild.");
        ImGui::SameLine();
        ImGui::TextDisabled("(%u list%s, %u cross-queue wait%s last frame)",
            render::g_asyncComputeLists, render::g_asyncComputeLists == 1u ? "" : "s",
            render::g_crossQueueWaits, render::g_crossQueueWaits == 1u ? "" : "s");
    }
    ImGui::Checkbox("Trace capture window", &traceWindowOpen_);

}

#endif // !TC_SHIPPING
