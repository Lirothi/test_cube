#include "app/ui/DeveloperWindow.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>
#include <string>
#include <string_view>
#include <system_error>

#include "app/Systems.h"
#include "app/GraphicsSettings.h"
#include "app/levels/JsonLevel.h"
#include "app/levels/LevelManager.h"
#include "app/scene/Scene.h"
#include "core/profiling/ProfilerScopes.h"
#if WITH_EDITOR
#include "editor/EditorController.h"
#endif
#include "imgui.h"
#include "input/InputManager.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/RenderStats.h"
#include "rendering/core/VisibilityStats.h" // S0 occlusion plan: per-view visibility table
#include "rendering/meshes/LodSelect.h"
#include "rendering/debug/LodDebugView.h"
#include "rendering/renderables/InstanceTypes.h"
#include "rendering/shadows/VirtualShadowMap.h"
#include "rendering/shadows/ShadowSettings.h"
#include "ocean/OceanSimulation.h"
#include "text/TextManager.h"

namespace
{
    std::string NormalizeLevelPath(std::string path)
    {
        std::replace(path.begin(), path.end(), '\\', '/');
        return path;
    }

    std::string LevelPathString(const std::filesystem::path& path)
    {
        return NormalizeLevelPath(path.string());
    }

    template <size_t N>
    void SetTextBuffer(char (&buffer)[N], const std::string& text)
    {
        const std::string normalized = NormalizeLevelPath(text);
        std::snprintf(buffer, N, "%s", normalized.c_str());
    }

    bool LevelFileExists(const std::string& path)
    {
        const std::string normalizedPath = NormalizeLevelPath(path);
        if (normalizedPath.empty())
        {
            return false;
        }

        std::error_code ec;
        const std::filesystem::path fsPath(normalizedPath);
        const bool exists = std::filesystem::exists(fsPath, ec);
        const bool regularFile = exists && std::filesystem::is_regular_file(fsPath, ec);
        return !ec && regularFile;
    }

    struct DlssModeOption
    {
        sl::DLSSMode mode;
        const char* label;
    };

    constexpr DlssModeOption kDlssModes[] = {
        { sl::DLSSMode::eOff, "Off" },
        { sl::DLSSMode::eMaxPerformance, "Max Performance" },
        { sl::DLSSMode::eBalanced, "Balanced" },
        { sl::DLSSMode::eMaxQuality, "Max Quality" },
        { sl::DLSSMode::eUltraPerformance, "Ultra Performance" },
        { sl::DLSSMode::eUltraQuality, "Ultra Quality" },
        { sl::DLSSMode::eDLAA, "DLAA" },
    };

    constexpr const char* kSsrTechniqueLabels[] = {
        "Log March",
        "UE SSR (HZB march)",
    };

    constexpr const char* kUeSsrQualityLabels[] = {
        "Custom",
        "UE Low (8x1)",
        "UE Medium (16x1)",
        "UE High (8x4)",
        "UE Epic (12x12)",
    };

    const char* DlssModeLabel(sl::DLSSMode mode)
    {
        for (const DlssModeOption& option : kDlssModes)
        {
            if (option.mode == mode)
            {
                return option.label;
            }
        }
        return "Unknown";
    }

    ID3D12Resource* GetOceanShoreDepthResource()
    {
        OceanSimulation* oceanSimulation = Systems::GetOceanSimulation();
        return oceanSimulation ? oceanSimulation->GetShoreDepthResource() : nullptr;
    }

}

// Hover text for a developer-window control. Kept deliberately blunt about GPU cost: several of
// these knobs are linear in frame time and nothing about the label says so.
static void DevHelp(const char* text)
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        return;
    }
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void DeveloperWindow::RefreshLevelList()
{
    availableLevelPaths_.clear();
    levelChangeStatus_.clear();
    levelListScanned_ = true;

    std::error_code ec;
    std::filesystem::directory_iterator it("data/levels", ec);
    if (ec)
    {
        levelChangeStatus_ = "Cannot scan data/levels";
        return;
    }

    const std::filesystem::directory_iterator end;
    while (it != end)
    {
        const std::filesystem::directory_entry& entry = *it;
        std::error_code entryEc;
        if (entry.is_regular_file(entryEc) && !entryEc && entry.path().extension() == ".json")
        {
            availableLevelPaths_.push_back(LevelPathString(entry.path()));
        }

        it.increment(ec);
        if (ec)
        {
            levelChangeStatus_ = "Cannot scan all level files";
            break;
        }
    }

    std::sort(availableLevelPaths_.begin(), availableLevelPaths_.end());
    if (availableLevelPaths_.empty())
    {
        levelChangeStatus_ = "No JSON levels found in data/levels";
    }
}

void DeveloperWindow::ToggleTextureInspector()
{
    textureDebugViewer_.SetOpen(!textureDebugViewer_.IsOpen());
}

void DeveloperWindow::DrawTraceControls()
{
    if (!traceWindowOpen_)
    {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(420.0f, 190.0f), ImGuiCond_FirstUseEver);
    bool open = traceWindowOpen_;
    if (ImGui::Begin("Trace capture###TraceCapture", &open))
    {
        const Profiler::TraceCaptureStatus status = Profiler::Get().GetTraceCaptureStatus();

        if (status.active)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "RECORDING");
            ImGui::SameLine();
            if (status.openEnded)
            {
                ImGui::Text("%u frames, %zu events", status.framesRecorded, status.events);
            }
            else
            {
                ImGui::Text("%u frames left", status.framesRemaining);
            }
        }
        else if (status.pending)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "starting next frame...");
        }
        else
        {
            ImGui::TextDisabled("idle");
        }

        ImGui::Separator();

        // Start/Stop, not a frame count: the things worth capturing (a drag, a hitch while walking)
        // have no length you can know in advance.
        ImGui::BeginDisabled(status.active || status.pending);
        if (ImGui::Button("Start", ImVec2(120.0f, 0.0f)))
        {
            Profiler::Get().BeginTraceCapture();
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(!status.active && !status.pending);
        if (ImGui::Button("Stop", ImVec2(120.0f, 0.0f)))
        {
            Profiler::Get().StopTraceCapture();
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(status.active || status.pending);
        if (ImGui::Button("Quick 120", ImVec2(120.0f, 0.0f)))
        {
            Profiler::Get().RequestTraceCapture(120);
        }
        ImGui::EndDisabled();

        ImGui::TextDisabled("F11 does the same as Quick 120 (unless ImGui has the keyboard).");

        ImGui::Separator();
        if (status.lastPath.empty())
        {
            ImGui::TextDisabled("no capture written yet");
        }
        else
        {
            ImGui::TextWrapped("last: %s", status.lastPath.c_str());
            if (ImGui::Button("Copy path"))
            {
                ImGui::SetClipboardText(status.lastPath.c_str());
            }
        }
    }
    ImGui::End();
    traceWindowOpen_ = open;
}

bool DeveloperWindow::Draw(Renderer& renderer, Scene& scene, const InputManager& input,
    LevelManager& levelManager, SceneRenderSettings& settings,
    GraphicsSettingsManager& graphicsSettings
#if WITH_EDITOR
    , EditorController& editorController
#endif
)
{
    CPU_SCOPE(ProfilerScopes::kBuildDeveloperWindow);
    bool graphicsSettingsDirty = false;
    const auto graphicsEdit = [&graphicsSettingsDirty](bool changed)
    {
        graphicsSettingsDirty |= changed;
        return changed;
    };

    if (!open_)
    {
        textureDebugViewer_.Draw(renderer, GetOceanShoreDepthResource());
    // The inspector cannot brighten its own preview (ImGui clamps the image tint), so it can hand
    // the request over to the fullscreen view, which draws through a shader we control.
    if (const int requested = textureDebugViewer_.TakeFullscreenRequest(); requested >= 0)
    {
        settings.debugTexTarget = requested;
        settings.debugTexMip = textureDebugViewer_.RequestedMip();
        settings.debugTexMode = true;
    }
        oceanControlsWindow_.Draw(renderer);
        logWindow_.Draw(); // returns immediately unless open (logging plan L8 cost rule)
        DrawTraceControls();
        return false;
    }

    ImGui::SetNextWindowSize(ImVec2(700.0f, 800.0f), ImGuiCond_FirstUseEver);

    bool open = open_;
    const ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoCollapse |
        (windowMaximize_.maximized ? ImGuiWindowFlags_NoMove : 0);
    if (ImGui::Begin("Developer Controls [F1]###DeveloperControls", &open, windowFlags))
    {
        ui::HandleWindowTitleDoubleClickMaximize(windowMaximize_);

        ImGui::SeparatorText("Project graphics settings");
        ImGui::TextUnformatted(GraphicsSettingsManager::kPath);
        ImGui::SameLine();
        if (ImGui::SmallButton("Save now"))
        {
            graphicsSettings.SaveCurrent(renderer, scene, settings);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reload file"))
        {
            graphicsSettings.Reload(renderer, scene, settings);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset all graphics"))
        {
            graphicsSettings.ResetAll(renderer, scene, settings);
        }
        if (!graphicsSettings.Status().empty())
        {
            ImGui::TextDisabled("%s", graphicsSettings.Status().c_str());
        }
        ImGui::TextDisabled("Global quality autosaves after an edit; level look stays in the level file.");

        if (ImGui::BeginTabBar("DeveloperControlsTabs"))
        {
            if (ImGui::BeginTabItem("Render"))
            {
                const float fps = renderer.GetFPS();
                const float frameMs = fps > 0.0f ? 1000.0f / fps : 0.0f;
                ImGui::Text("FPS: %.1f (%.2f ms)", fps, frameMs);
                ImGui::Text("Display: %ux%u", renderer.GetWidth(), renderer.GetHeight());
                ImGui::Text("Render: %ux%u (scale %.2f)", renderer.GetRenderWidth(), renderer.GetRenderHeight(), renderer.GetRenderResolutionScale());
                ImGui::Text("Draw calls: %u   Primitives: %.3fM",
                    render::g_renderStats.lastDrawCalls,
                    static_cast<double>(render::g_renderStats.lastPrimitives) / 1.0e6);

                // S0 (docs/occlusion_culling_plan.md): per-view visibility. Same numbers a headless
                // run gets from --vis-readout, so a HUD reading and a log reading never disagree.
                if (ImGui::BeginTable("VisibilityStats", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
                {
                    ImGui::TableSetupColumn("view");
                    ImGui::TableSetupColumn("objects in");
                    ImGui::TableSetupColumn("frustum");
                    ImGui::TableSetupColumn("occluded");
                    ImGui::TableSetupColumn("chunks in");
                    ImGui::TableSetupColumn("chunks drawn");
                    ImGui::TableSetupColumn("instances");
                    ImGui::TableSetupColumn("tris (M)");
                    ImGui::TableHeadersRow();
                    static const char* kViewNames[render::kVisibilityViews] = { "camera", "c0", "c1", "c2", "c3" };
                    for (unsigned v = 0; v < render::kVisibilityViews; ++v)
                    {
                        const render::VisibilityViewCounters& c = render::g_visibilityStats.last[v];
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::Text("%s", kViewNames[v]);
                        ImGui::TableSetColumnIndex(1); ImGui::Text("%u", c.objectsIn);
                        ImGui::TableSetColumnIndex(2); ImGui::Text("%u", c.objectsFrustum);
                        ImGui::TableSetColumnIndex(3); ImGui::Text("%u", c.objectsOccluded);
                        ImGui::TableSetColumnIndex(4); ImGui::Text("%u", c.chunksIn);
                        ImGui::TableSetColumnIndex(5); ImGui::Text("%u", c.chunksDrawn);
                        ImGui::TableSetColumnIndex(6); ImGui::Text("%u", c.instancesDrawn);
                        ImGui::TableSetColumnIndex(7); ImGui::Text("%.3f", static_cast<double>(c.trianglesSubmitted) / 1.0e6);
                    }
                    ImGui::EndTable();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Per view, last completed frame. objects in = what the view's source offered;\n"
                                      "frustum = kept by the frustum test (per object, before instancing);\n"
                                      "occluded = cut by an occlusion test (0 until the occlusion plan's S3/S5);\n"
                                      "chunks = terrain chunks of surviving chunked meshes, in vs kept by the\n"
                                      "view's frustum (S1 mask); instances = GI instances the camera draws;\n"
                                      "tris = CPU-path estimate at the selected LOD.\n"
                                      "Headless: --vis-readout -> logs/visibility_readout.log.");
                // S1 rollback, live: with it off every chunk/instance of a passed object draws again.
                ImGui::Checkbox("Chunk / instance frustum mask (S1)", &render::g_visChunkMask);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Frustum test below the object level: terrain chunks (camera + Legacy CSM\n"
                                      "CPU loop) and GPU-instanced cloud instances (camera). Off = pre-S1: an\n"
                                      "object that passes draws all of its chunks. --set=vis.chunkMask:0");

                ImGui::Checkbox("Trace capture window", &traceWindowOpen_);

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
                    bool asyncCompute = !render::g_noAsyncCompute;
                    if (graphicsEdit(ImGui::Checkbox("Async compute", &asyncCompute)))
                    {
                        render::g_noAsyncCompute = !asyncCompute;
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

                ImGui::Separator();

                const bool dlssAvailable = renderer.IsDlssAvailable();
                ImGui::Text("DLSS status: %s", renderer.IsDlssActive() ? "Active" : (dlssAvailable ? "Inactive" : "Unavailable"));

                ImGui::BeginDisabled(!dlssAvailable);
                bool dlssEnabled = renderer.IsDlssActive();
                if (graphicsEdit(ImGui::Checkbox("DLSS enabled", &dlssEnabled)))
                {
                    if (dlssEnabled)
                    {
                        if (renderer.GetDlssMode() == sl::DLSSMode::eOff)
                        {
                            renderer.SetDlssMode(sl::DLSSMode::eBalanced);
                        }
                        renderer.SetDlssActive(true);
                    }
                    else
                    {
                        renderer.SetDlssActive(false);
                    }
                }

                const sl::DLSSMode currentDlssMode = renderer.GetDlssMode();
                if (ImGui::BeginCombo("DLSS quality", DlssModeLabel(currentDlssMode)))
                {
                    for (const DlssModeOption& option : kDlssModes)
                    {
                        const bool selected = option.mode == currentDlssMode;
                        if (graphicsEdit(ImGui::Selectable(option.label, selected)))
                        {
                            renderer.SetDlssMode(option.mode);
                            renderer.SetDlssActive(option.mode != sl::DLSSMode::eOff);
                        }
                        if (selected)
                        {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::EndDisabled();

                if (!dlssAvailable)
                {
                    ImGui::TextDisabled("Streamline DLSS is not available for this run.");
                }

                ImGui::Separator();

                graphicsEdit(ImGui::Checkbox("FXAA", &settings.doFxaa));

                int ssrTechnique = static_cast<int>(settings.ssrTechnique);
                const int ssrTechniqueCount = static_cast<int>(SsrTechnique::Count);
                if (ssrTechnique < 0 || ssrTechnique >= ssrTechniqueCount)
                {
                    ssrTechnique = 0;
                }
                if (graphicsEdit(ImGui::Combo("SSR technique", &ssrTechnique,
                                               kSsrTechniqueLabels, ssrTechniqueCount)))
                {
                    settings.ssrTechnique = static_cast<SsrTechnique>(ssrTechnique);
                }
                DevHelp("How the reflection ray searches for its hit. Log March takes 128 growing "
                        "view-space steps against full depth. UE SSR follows SSRTReflections.usf: "
                        "8/12/16 fixed screen-space steps issued in Batch4 against the FURTHEST HZB "
                        "at a mostly fixed mip, with roughness-aware High/Epic ray counts. It is "
                        "usually cheaper but deliberately approximate. Only affects SSR, not RT.");

                if (settings.ssrTechnique == SsrTechnique::UeHzb)
                {
                    UeSsrSettings& ue = settings.ssrUe;
                    int preset = static_cast<int>(ue.preset);
                    if (preset < 0 || preset >= static_cast<int>(UeSsrQualityPreset::Count))
                    {
                        preset = 0;
                    }
                    if (graphicsEdit(ImGui::Combo("UE SSR quality", &preset, kUeSsrQualityLabels,
                                     static_cast<int>(UeSsrQualityPreset::Count))))
                    {
                        ApplyUeSsrQualityPreset(ue, static_cast<UeSsrQualityPreset>(preset));
                    }
                    DevHelp("The actual SSRTReflections.usf presets. High/Epic use 4/12 GGX rays "
                            "only at roughness >= 0.1; smoother surfaces collapse the whole budget "
                            "to one 24-step mirror ray, exactly as UE do.");

                    if (ImGui::TreeNode("UE SSR advanced"))
                    {
                        int steps = static_cast<int>(ue.numSteps);
                        if (graphicsEdit(ImGui::SliderInt("UE steps / ray", &steps, 4, 64)))
                        {
                            ue.numSteps = static_cast<uint32_t>(std::min(64, (steps + 3) & ~3));
                            ue.preset = UeSsrQualityPreset::Custom;
                        }
                        DevHelp("Rounded up to a multiple of four because UE issue depth requests "
                                "in Batch4. More steps fill thin/far silhouettes and narrow the "
                                "per-step depth interval; cost is linear per ray.");

                        int rays = static_cast<int>(ue.numRays);
                        if (graphicsEdit(ImGui::SliderInt("UE rays / pixel", &rays, 1, 12)))
                        {
                            ue.numRays = static_cast<uint32_t>(rays);
                            ue.preset = UeSsrQualityPreset::Custom;
                        }
                        DevHelp("Used only with glossy rays. Cost is rays x steps; 12x12 is UE Epic "
                                "and is intentionally expensive on rough reflectors.");

                        if (graphicsEdit(ImGui::Checkbox("UE roughness GGX rays", &ue.glossyRays)))
                        {
                            ue.preset = UeSsrQualityPreset::Custom;
                        }
                        DevHelp("OFF traces one geometric mirror ray. ON importance-samples the "
                                "reflector roughness when Rays > 1; roughness < 0.1 still collapses "
                                "to UE's one 24-step mirror ray.");

                        graphicsEdit(ImGui::Checkbox("UE use surface roughness", &ue.useSurfaceRoughness));
                        DevHelp("Normally reads packed roughness from GB0, matching UE. Disable to "
                                "force one value below -- useful for proving which quality branch "
                                "is active without editing a material.");
                        ImGui::BeginDisabled(ue.useSurfaceRoughness);
                        graphicsEdit(ImGui::SliderFloat("UE roughness override", &ue.roughnessOverride,
                                           0.0f, 1.0f, "%.2f"));
                        ImGui::EndDisabled();

                        graphicsEdit(ImGui::SliderFloat("UE intensity", &ue.intensity,
                                                        0.0f, 1.0f, "%.2f"));
                        DevHelp("SSRParams.r: ScreenSpaceReflectionIntensity / 100. UE stock 1.0. "
                                "Multiplies the whole SSR output after the roughness fade.");

                        graphicsEdit(ImGui::SliderFloat("UE max roughness", &ue.maxRoughness,
                                                        0.01f, 1.0f, "%.2f"));
                        DevHelp("ScreenSpaceReflectionMaxRoughness, UE stock 0.6: full SSR up to "
                                "half this roughness, faded to nothing at it. Below the High tier "
                                "UE double the fade slope, and so does this port.");
                        ImGui::TreePop();
                    }
                }

                graphicsEdit(ImGui::Checkbox("Reflection temporal resolve", &settings.ssrTemporal));
                DevHelp("Accumulates the reflection over time instead of showing each frame raw. "
                        "Applies to BOTH sources: an SSR march is violently sensitive to its "
                        "jittered start (measured 7.9x less frame-to-frame movement), and RT at "
                        "half reflection res boils the same way once reflected foliage is "
                        "subpixel -- 1 sharp ray/px under DLSS jitter. Unreal never display "
                        "either unfiltered. Perf: ~0.014 ms. Off = the tracer's raw output, "
                        "which is what you want when judging a tracer rather than the picture.");
                ImGui::BeginDisabled(!settings.ssrTemporal);
                {
                    ImGui::SetNextItemWidth(140.0f);
                    graphicsEdit(ImGui::SliderFloat("Temporal blend", &settings.ssrTemporalBlendWeight,
                                       0.02f, 1.0f, "%.3f"));
                    DevHelp("Weight of the CURRENT frame. UE use 1/8 = 0.125 for their SSR TAA "
                            "config. Lower = longer history, steadier but slower to react; 1 = no "
                            "accumulation at all.");
                    ImGui::SetNextItemWidth(140.0f);
                    graphicsEdit(ImGui::SliderFloat("Temporal still inertia",
                                       &settings.ssrTemporalClampExpand, 0.0f, 2.0f, "%.2f"));
                    DevHelp("Extra inertia for STILL pixels, on top of the blend above. At zero "
                            "motion this knob both RELAXES the neighbourhood clamp (the measured "
                            "limiter of still-camera boil) and divides the frame weight by up to "
                            "(1 + 4x this). Measured on the bronze bench: 0 = baseline 0.46, "
                            "0.5 = 0.37, 2.0 = 0.20 frame-to-frame boil. Any motion restores the "
                            "hard clamp and full blend, so response while moving is unchanged. "
                            "Cost at high values: the reflection of something moving in a STILL "
                            "mirror (a swaying palm) can trail slightly. 0 = off.");
                }
                ImGui::EndDisabled();

                float ssrScale = renderer.GetReflectionTextureScale().x;
                if (graphicsEdit(ImGui::SliderFloat("Reflection resolution", &ssrScale,
                                                     0.25f, 1.0f, "%.2f")))
                {
                    renderer.SetReflectionTextureScale(ssrScale);
                }
                ImGui::Text("Reflection target: %ux%u", renderer.GetReflectionTextureWidth(), renderer.GetReflectionTextureHeight());
                // S15b: glass off-screen reflections render into a glass G-buffer + glassReflection at
                // this same reflection resolution and follow the reflection source below.
                // Inspect them via Texture inspector [F4] -> "Glass Refl Normal/Depth" + "Glass Reflection".
                ImGui::TextDisabled("Glass: traced in SSR/RT, cubemap in Sky only, disabled in None.");

                // S16: glossy reflections — blur radius scales with surface roughness (0 = sharp mirror).
                graphicsEdit(ImGui::SliderFloat("Glossy blur", &settings.reflectionGlossyScale,
                                                 0.0f, 24.0f, "%.1f"));

                // Analytic sun specular boost on metals: spec lobe *= (1 + metal*coef). 0 = physical.
                graphicsEdit(ImGui::SliderFloat("Sun spec on metal", &settings.sunMetalSpecInfluence,
                                                 0.0f, 16.0f, "%.1f"));

                // Sun angular size: floors the analytic specular lobe width so smooth surfaces show
                // a bright, sample-able sun glint instead of a sub-pixel spike. 0 = punctual.
                graphicsEdit(ImGui::SliderFloat("Sun angular size", &settings.sunAngularSize,
                                                 0.0f, 0.25f, "%.3f"));

                float oceanReflectionScale = renderer.GetOceanReflectionTextureScale().x;
                if (graphicsEdit(ImGui::SliderFloat("Ocean reflection resolution",
                    &oceanReflectionScale, 0.25f, 1.0f, "%.2f")))
                {
                    renderer.SetOceanReflectionTextureScale(oceanReflectionScale);
                }
                ImGui::Text("Ocean reflection target: %ux%u", renderer.GetOceanReflectionTextureWidth(), renderer.GetOceanReflectionTextureHeight());

                ImGui::Separator();

                // Reflection source (S8): None / Sky only / SSR / RT. RT is greyed out on
                // non-RT hardware (and the renderer falls back to SSR anyway).
                const bool rtSupported = renderer.IsRaytracingSupported();
                const char* srcLabels[] = { "None", "Sky only", "SSR", "RT" };
                constexpr int srcCount = static_cast<int>(ReflectionSource::Count);
                int curSrc = static_cast<int>(settings.reflectionSource);
                if (curSrc < 0 || curSrc >= srcCount) { curSrc = static_cast<int>(ReflectionSource::SSR); }
                if (ImGui::BeginCombo("Reflections [F5]", srcLabels[curSrc]))
                {
                    for (int i = 0; i < srcCount; ++i)
                    {
                        const bool isRT = (i == static_cast<int>(ReflectionSource::RT));
                        ImGui::BeginDisabled(isRT && !rtSupported);
                        if (graphicsEdit(ImGui::Selectable(srcLabels[i], curSrc == i)))
                        {
                            settings.reflectionSource = static_cast<ReflectionSource>(i);
                        }
                        ImGui::EndDisabled();
                    }
                    ImGui::EndCombo();
                }
                if (!rtSupported)
                {
                    ImGui::TextDisabled("RT requires hardware ray tracing (unavailable).");
                }

                ImGui::BeginDisabled(!rtSupported);
                {
                    static const char* kAlphaModes[] = { "Off (solid cards)",
                                                         "First hit (holes)",
                                                         "Full (exact)" };
                    int alphaMode = static_cast<int>(std::min(settings.rtAlphaMode, 2u));
                    ImGui::SetNextItemWidth(180.0f);
                    if (graphicsEdit(ImGui::Combo("RT foliage alpha", &alphaMode, kAlphaModes, 3)))
                    {
                        settings.rtAlphaMode = static_cast<uint32_t>(alphaMode);
                    }
                    DevHelp("Off: FORCE_OPAQUE traversal, leaves reflect/occlude as solid cards "
                            "-- cheapest. First hit: same cheap traversal, but the committed hit "
                            "is alpha-tested with the albedo sample shading fetches anyway; a "
                            "transparent texel becomes a MISS, so crowns get their holes -- the "
                            "holes show the sky fallback, not what is really behind, and crown "
                            "shadows lean lighter. Costs the same as Off. Full: exact "
                            "per-candidate testing during traversal -- the expensive one. "
                            "Headless: --set=rt.alphaMode:0/1/2.");
                }
                ImGui::BeginDisabled(settings.rtAlphaMode == 0u);
                ImGui::SetNextItemWidth(140.0f);
                graphicsEdit(ImGui::SliderFloat("RT foliage fill", &settings.rtAlphaMissKeep,
                                                 0.0f, 1.0f, "%.2f"));
                DevHelp("Stochastic coverage inflation for the RT alpha test. One sharp ray per "
                        "pixel at reflection res undersamples thin fronds, so the reflected crown "
                        "reads smaller than the real one. On a failed alpha test the hit is still "
                        "kept with this probability, re-rolled per pixel per frame -- the temporal "
                        "resolve averages the dither back into density. 0 = honest cutouts, "
                        "1 = the old solid cards. In FIRST-HIT mode it matters more: the "
                        "single-layer test discards the crown's own depth, so raise it (0.3-0.5) "
                        "to buy the density back. Headless: --set=rt.alphaMissKeep:<v>.");
                ImGui::EndDisabled();
                graphicsEdit(ImGui::Checkbox("RT wind sway", &settings.rtWindBlas));
                DevHelp("Wind-deform the nearest casters' BLASes every frame so foliage sway "
                        "reaches RT reflections. Off = reflections show the rest pose. "
                        "Cost scales with the animated set (~0.3 ms for 24 palms). Headless: "
                        "--set=rt.windBlas.");
                ImGui::BeginDisabled(!settings.rtWindBlas);
                ImGui::SetNextItemWidth(140.0f);
                graphicsEdit(ImGui::SliderFloat("RT wind radius", &settings.rtWindBlasRadius,
                                                 0.0f, 100.0f, "%.0f m"));
                DevHelp("Casters inside this radius sway in RT (nearest-first, 24-slot cap); "
                        "beyond it the shared rest-pose BLAS stands. Headless: "
                        "--set=rt.windRadius.");
                ImGui::EndDisabled();
                ImGui::Checkbox("RT debug view -> Reflection target [F6]", &settings.rtDebugView);
                ImGui::EndDisabled();
                if (rtSupported && settings.rtDebugView)
                {
                    ImGui::TextDisabled("Open the texture inspector [F4] and select 'Reflection'.");
                }

                ImGui::Separator();

                const bool dlssControlsRenderScale = renderer.IsDlssActive();
                ImGui::BeginDisabled(dlssControlsRenderScale);
                float renderScale = renderer.GetRenderResolutionScale();
                if (graphicsEdit(ImGui::SliderFloat("Render scale", &renderScale,
                                                     0.1f, 1.0f, "%.2f")))
                {
                    renderer.SetRenderResolutionScale(renderScale);
                }
                if (ImGui::Button("Native render scale"))
                {
                    renderer.SetRenderResolutionScale(1.0f);
                    graphicsSettingsDirty = true;
                }
                ImGui::EndDisabled();
                if (dlssControlsRenderScale)
                {
                    ImGui::TextDisabled("DLSS quality controls the render scale while active.");
                }

                ImGui::Separator();
                if (ImGui::Button("Reset Render quality to defaults"))
                {
                    graphicsSettings.ResetRender(renderer, scene, settings);
                }

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Debug"))
            {
                const std::string_view activeLevelName = levelManager.GetActiveLevelName();
                ImGui::Text("Active level: %.*s", static_cast<int>(activeLevelName.size()), activeLevelName.data());
                const std::string_view activeLevelPath = levelManager.GetActiveLevelSourcePath();
                if (!activeLevelPath.empty())
                {
                    ImGui::Text("Active path: %.*s", static_cast<int>(activeLevelPath.size()), activeLevelPath.data());
                }

                if (!levelListScanned_)
                {
                    RefreshLevelList();
                }

                const auto requestLevelPathChange = [&](const std::string& path)
                {
                    const std::string normalizedPath = NormalizeLevelPath(path);
                    if (normalizedPath.empty())
                    {
                        levelChangeStatus_ = "Level path is empty";
                        return;
                    }
                    if (!LevelFileExists(normalizedPath))
                    {
                        levelChangeStatus_ = "Level file not found: " + normalizedPath;
                        return;
                    }

                    SetTextBuffer(levelPathBuffer_, normalizedPath);
#if WITH_EDITOR
                    if (editorController.RequestOpenLevelPath(levelManager, normalizedPath, preserveCameraOnLevelChange_))
                    {
                        levelChangeStatus_ = "Queued " + normalizedPath;
                    }
                    else
                    {
                        levelChangeStatus_ = "Could not queue " + normalizedPath;
                    }
#else
                    LevelLoadOptions loadOptions;
                    loadOptions.preserveCameraTransform = preserveCameraOnLevelChange_;
                    levelManager.RequestLevelPathChange(normalizedPath, loadOptions);
                    levelChangeStatus_ = "Queued " + normalizedPath;
#endif
                };

                const char* preview = levelPathBuffer_[0] ? levelPathBuffer_ : "Select level";
                if (ImGui::BeginCombo("JSON level", preview))
                {
                    for (const std::string& levelPath : availableLevelPaths_)
                    {
                        const bool selected = NormalizeLevelPath(levelPathBuffer_) == levelPath;
                        if (ImGui::Selectable(levelPath.c_str(), selected))
                        {
                            SetTextBuffer(levelPathBuffer_, levelPath);
                        }
                        if (selected)
                        {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::Button("Refresh levels"))
                {
                    RefreshLevelList();
                }
                if (ImGui::Button("Load JSON Level"))
                {
                    requestLevelPathChange(levelPathBuffer_);
                }
                ImGui::SameLine();
                if (ImGui::Button("Reload JSON Level"))
                {
                    requestLevelPathChange(levelPathBuffer_);
                }
                ImGui::Checkbox("Preserve camera transform", &preserveCameraOnLevelChange_);
                if (!levelChangeStatus_.empty())
                {
                    ImGui::TextDisabled("%s", levelChangeStatus_.c_str());
                }

                ImGui::Separator();

                bool wireframe = renderer.GetWireframeMode();
                if (ImGui::Checkbox("Wireframe", &wireframe))
                {
                    renderer.SetWireframeMode(wireframe);
                }
                bool textureViewerOpen = textureDebugViewer_.IsOpen();
                // Render-resolution targets are re-sampled at a different sub-pixel offset every
                // frame, so a half-res intermediate (GTAO, SSR) shimmers while you stare at it.
                // This holds the grid still. DLSS needs that jitter, so its own output loses its
                // anti-aliasing while this is on — hence the warning rather than a silent toggle.
                {
                    bool jitterPaused = renderer.IsJitterPaused();
                    ImGui::BeginDisabled(!renderer.IsDlssActive());
                    if (ImGui::Checkbox("Pause DLSS jitter (steady preview)", &jitterPaused))
                    {
                        renderer.SetJitterPaused(jitterPaused);
                    }
                    ImGui::EndDisabled();
                    if (jitterPaused && renderer.IsDlssActive())
                    {
                        ImGui::TextDisabled("  DLSS is reconstructing without jitter: expect aliasing.");
                    }
                }
                if (ImGui::Checkbox("Texture inspector [F4]", &textureViewerOpen))
                {
                    textureDebugViewer_.SetOpen(textureViewerOpen);
                }
                bool oceanControlsOpen = oceanControlsWindow_.IsOpen();
                if (ImGui::Checkbox("Ocean controls [F7]", &oceanControlsOpen))
                {
                    oceanControlsWindow_.SetOpen(oceanControlsOpen);
                }
                bool logWindowOpen = logWindow_.IsOpen();
                if (ImGui::Checkbox("Session log", &logWindowOpen))
                {
                    logWindow_.SetOpen(logWindowOpen);
                }
#if WITH_EDITOR
                bool levelEditorOpen = editorController.IsOpen();
                if (ImGui::Checkbox("Level Editor [F2]", &levelEditorOpen))
                {
                    editorController.SetOpen(levelEditorOpen);
                }
#endif
                // Mirrors SceneRenderer::PickDebugTexTarget — keep the order identical.
                static const char* kDebugTexLabels[] = {
                    "Cascade shadow atlas", "GTAO raw", "GTAO denoised", "GTAO temporal",
                    "GTAO upsampled", "HZB furthest (AO + UE SSR)", "Scene depth", "HZB closest (debug/P9)",
                    "SSR hit mask (alpha)" };

                ImGui::SeparatorText("Fullscreen debug view");
                // Picking a target TURNS THE VIEW ON. Two separate controls where one is useless
                // without the other is not two controls, it is one control and a trap: the combo
                // silently did nothing until the checkbox above it was found.
                if (ImGui::Combo("Show", &settings.debugTexTarget, kDebugTexLabels,
                                 IM_ARRAYSIZE(kDebugTexLabels)))
                {
                    settings.debugTexMode = true;
                }
                DevHelp("Replaces the whole frame with the chosen render target. Picking one here "
                        "switches the view on; use Off below to get the scene back.");

                ImGui::BeginDisabled(settings.debugTexTarget != 5 && settings.debugTexTarget != 7);
                ImGui::SliderInt("HZB mip", &settings.debugTexMip, 0, 12);
                ImGui::EndDisabled();
                DevHelp("Which level of the depth pyramid to show. Mip 0 is half the render "
                        "resolution, and each level halves again. Depth-like targets go through a "
                        "monotone pow() stretch, or reversed-Z would blit as a black rectangle. "
                        "Furthest is what the AO horizon search reads; Closest is what the HiZ "
                        "reflection march reads, and it is only built while that technique is the "
                        "active reflection source.");

                if (settings.debugTexMode)
                {
                    if (ImGui::Button("Off (show the scene)"))
                    {
                        settings.debugTexMode = false;
                    }
                    ImGui::SameLine();
                    // Say what is actually on screen, including the grid size -- "GTAO raw" alone
                    // does not tell you that you are looking at a half-resolution target.
                    const unsigned halfW = (renderer.GetRenderWidth() + 1u) / 2u;
                    const unsigned halfH = (renderer.GetRenderHeight() + 1u) / 2u;
                    switch (settings.debugTexTarget)
                    {
                    case 1: case 2: case 3:
                        ImGui::Text("showing %s  (%ux%u, half res)",
                                    kDebugTexLabels[settings.debugTexTarget], halfW, halfH);
                        break;
                    case 4: case 6:
                        ImGui::Text("showing %s  (%ux%u, render res)",
                                    kDebugTexLabels[settings.debugTexTarget],
                                    renderer.GetRenderWidth(), renderer.GetRenderHeight());
                        break;
                    case 8:
                        ImGui::Text("showing %s  (%ux%u)  white = the ray hit something",
                                    kDebugTexLabels[settings.debugTexTarget],
                                    renderer.GetReflectionTextureWidth(),
                                    renderer.GetReflectionTextureHeight());
                        break;
                    case 5: case 7:
                        ImGui::Text("showing %s mip %d  (%ux%u)",
                                    settings.debugTexTarget == 5 ? "furthest" : "closest",
                                    settings.debugTexMip,
                                    std::max(1u, halfW >> settings.debugTexMip),
                                    std::max(1u, halfH >> settings.debugTexMip));
                        break;
                    default:
                        ImGui::Text("showing %s", kDebugTexLabels[settings.debugTexTarget]);
                        break;
                    }

                    // The AO targets are only written while the pass runs. Without this the view is
                    // whatever was last left in the texture, which reads as "the feature is broken".
                    if (settings.debugTexTarget >= 1 && settings.debugTexTarget <= 4 &&
                        !scene.GetGtao().enabled)
                    {
                        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                            "GTAO is OFF in the level postProcess settings - this target is stale, not empty.");
                    }
                }
                ImGui::Checkbox("Profiler overlay", &settings.showProfiler);
#if WITH_EDITOR
                ImGui::Checkbox("GPU instancing [F12]", &render::g_instancingEnabled);
#else
                ImGui::Checkbox("GPU instancing", &render::g_instancingEnabled);
#endif

                // (Mesh LOD enable/force + the selection boundaries moved to the "LOD" tab.)
                const Camera& camera = scene.CameraRef();
                const auto& camPos = camera.GetPosition();
                const auto& camDir = camera.GetDirection();
                ImGui::Text("Camera position: %.2f, %.2f, %.2f", camPos.x, camPos.y, camPos.z);
                ImGui::Text("Camera direction: %.2f, %.2f, %.2f", camDir.x, camDir.y, camDir.z);
                ImGui::Text("Move speed multiplier: %.2f", camera.GetMoveSpeedMult());
                ImGui::Text("Frame: %llu", static_cast<unsigned long long>(renderer.GetTotalFrameNumber()));

                ImGui::Separator();
                if (ImGui::TreeNodeEx("TextManager stats"))
                {
                    TextManager* textManager = renderer.GetTextManager();
                    bool textStatsEnabled = textManager->GetPerfStatsEnabled();
                    if (ImGui::Checkbox("Enable TextManager instrumentation", &textStatsEnabled))
                    {
                        textManager->SetPerfStatsEnabled(textStatsEnabled);
                    }
                    const TextManager::PerfStats& textStats = renderer.GetTextManager()->GetPerfStats();
                    ImGui::Text("Instrumentation: %s", textStatsEnabled ? "enabled" : "disabled");
                    ImGui::Text("Regions:%u backgrounds:%u AddText:%u AddTextf:%u cached:%u positional:%u",
                        textStats.regions, textStats.backgrounds, textStats.addTextCalls,
                        textStats.addTextfCalls, textStats.addCachedTextCalls, textStats.positionalTextCalls);
                    ImGui::Text("Lines direct:%u deferred:%u chars:%u direct glyphs:%u run glyphs:%u retarget verts:%u",
                        textStats.directLines, textStats.deferredLines, textStats.inputChars,
                        textStats.directGlyphs, textStats.runGlyphs, textStats.retargetedVertices);
                    ImGui::Text("AddText %.2fus  cached %.2fus  format %.2fus",
                        textStats.addTextUs, textStats.addCachedTextUs, textStats.formatUs);
                    ImGui::Text("GlyphRun %.2fus (%u)  direct emit %.2fus (%u)  run emit %.2fus (%u)",
                        textStats.buildGlyphRunUs, textStats.glyphRunBuilds,
                        textStats.directEmitUs, textStats.directEmitCalls,
                        textStats.runEmitUs, textStats.runEmitCalls);
                    ImGui::Text("Direct split reserve %.2fus  setup %.2fus  loop %.2fus",
                        textStats.directEmitReserveUs, textStats.directEmitSetupUs, textStats.directEmitLoopUs);
                    ImGui::Text("Retarget %.2fus  Build %.2fus  reserve %.2fus  regions %.2fus",
                        textStats.lineRetargetUs, textStats.buildUs,
                        textStats.buildReserveUs, textStats.buildRegionsUs);
                    ImGui::Text("Upload rect %.2fus  upload text %.2fus  Draw %.2fus  Begin %.2fus",
                        textStats.uploadRectUs, textStats.uploadTextUs, textStats.drawUs, textStats.beginUs);
                    ImGui::TreePop();
                }

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("LOD"))
            {
                // Every mesh-LOD SELECTION control in one place (shadow-specific LOD lives with the
                // shadows: the per-view curve bias stays on the VSM tab). All of these apply live.
                ImGui::TextWrapped("Mesh LOD selection. Regular meshes pick a tier from "
                    "distance / instance radius (object-size-relative: a palm switches later than a "
                    "pebble); chunked terrain picks a tier PER CHUNK in metres, and its shadow "
                    "casters always match the drawn LOD by construction.");
                ImGui::Separator();

                if (ImGui::Button("Reset LOD quality to defaults"))
                {
                    graphicsSettings.ResetLod(renderer, scene, settings);
                }

#if WITH_EDITOR
                graphicsEdit(ImGui::Checkbox("Mesh LOD [F10]", &render::g_lodEnabled));
#else
                graphicsEdit(ImGui::Checkbox("Mesh LOD", &render::g_lodEnabled));
#endif
                ImGui::BeginDisabled(!render::g_lodEnabled);
                // -1 = automatic (screen-size / cascade); 0..3 force that level on every mesh.
                static const char* kForcedLodLabels[] = { "Auto", "0 (full)", "1", "2", "3" };
                int forcedLodCombo = render::g_forcedLod + 1; // map -1..3 -> 0..4
                if (ImGui::Combo("Force LOD level", &forcedLodCombo, kForcedLodLabels, IM_ARRAYSIZE(kForcedLodLabels)))
                {
                    render::g_forcedLod = forcedLodCombo - 1;
                }

                ImGui::SeparatorText("Regular meshes (distance / radius)");
                graphicsEdit(ImGui::SliderFloat("LOD1 at ratio", &render::g_lodBound0,
                                                 2.0f, 60.0f, "%.0f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("distance / instance radius where LOD0 steps to LOD1. A 2 m-radius palm\n"
                                      "at ratio 15 switches at 30 m. +/-15%% hysteresis on every boundary; each\n"
                                      "boundary is forced at least 5%% past the previous one.");
                graphicsEdit(ImGui::SliderFloat("LOD2 at ratio", &render::g_lodBound1,
                                                 4.0f, 120.0f, "%.0f"));
                graphicsEdit(ImGui::SliderFloat("LOD3 at ratio", &render::g_lodBound2,
                                                 8.0f, 240.0f, "%.0f"));
                graphicsEdit(ImGui::SliderFloat("Crossfade band", &render::g_lodFadeBand,
                                                 0.0f, 0.35f, "%.2f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Dithered LOD crossfade: half-width of the transition band around each\n"
                                      "boundary (fraction of the boundary ratio). Inside it BOTH tiers draw with\n"
                                      "complementary screen-door masks that DLSS/TAA resolves into a smooth\n"
                                      "blend - no pop. Costs a second draw of the object across the band.\n"
                                      "0 = off (hard switches with the classic +/-15%% hysteresis).");

                ImGui::SeparatorText("Chunked terrain (metres, per chunk)");
                graphicsEdit(ImGui::SliderFloat("Chunk LOD distance (m)",
                                                 &render::g_chunkLodDist0,
                                                 24.0f, 400.0f, "%.0f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("A terrain chunk closer than this (closest point of its box) draws AND\n"
                                      "casts LOD0; each 'factor' further steps one LOD coarser. The caster always\n"
                                      "matches the drawn LOD per chunk, so no setting here can cause the terrain\n"
                                      "self-shadow banding/phantom family -- this knob trades triangles for pop-in\n"
                                      "distance only.");
                graphicsEdit(ImGui::SliderFloat("Chunk LOD factor", &render::g_chunkLodDistFactor,
                                                 1.2f, 4.0f, "%.2f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Distance multiplier between LOD steps: LOD2 starts at distance*factor,\n"
                                      "LOD3 at distance*factor^2. +/-15%% hysteresis keeps a boundary chunk from\n"
                                      "flipping while the camera breathes.");
                ImGui::EndDisabled();

                ImGui::SeparatorText("Debug view");
                ImGui::TextWrapped("Draws the LOD DECISION next to the INPUTS it was made from, so "
                    "a wrong-looking LOD can be pinned on the curve or on the geometry without a "
                    "rebuild. Every number shown is read back from what selection already stored "
                    "this frame, never re-derived.");
                static const char* kLodDebugLabels[] = {
                    "Off", "Tier (selected LOD)", "Density (apparent triangle size)" };
                int lodDebugCombo = static_cast<int>(render::g_lodDebugMode);
                if (ImGui::Combo("LOD debug view", &lodDebugCombo, kLodDebugLabels, IM_ARRAYSIZE(kLodDebugLabels)))
                {
                    render::g_lodDebugMode = static_cast<render::LodDebugMode>(lodDebugCombo);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Tier: colour = the LOD each chunk selected (green 0 -> red 3).\n"
                                      "Density: colour = the triangle size that LOD actually DELIVERS on\n"
                                      "screen. The two disagree whenever chunks differ in source density --\n"
                                      "simplification is a fixed ratio of each chunk's own LOD0 triangle\n"
                                      "count, so an equal tier is not equal detail. That gap is exactly why\n"
                                      "a nearer chunk can look coarser than a farther one.");

                ImGui::BeginDisabled(render::g_lodDebugMode == render::LodDebugMode::Off);
#if WITH_EDITOR
                // Only offered where a selection can exist; in a non-editor build there is nothing
                // to select, so the control is absent rather than present and inert.
                static const char* kLodDebugFilterLabels[] = { "Whole level", "Selection only" };
                int lodDebugFilterCombo = static_cast<int>(render::g_lodDebugFilter);
                if (ImGui::Combo("Report on", &lodDebugFilterCombo, kLodDebugFilterLabels, IM_ARRAYSIZE(kLodDebugFilterLabels)))
                {
                    render::g_lodDebugFilter = static_cast<render::LodDebugFilter>(lodDebugFilterCombo);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Selection only narrows the view to the objects picked in the viewport\n"
                                      "(all chunks of a selected chunked mesh included, and never range-culled).\n"
                                      "It does not quietly widen back to the whole level when nothing is\n"
                                      "selected -- the readout says the selection is empty instead.");
#endif
                ImGui::Checkbox("Boxes", &render::g_lodDebugBoxes);
                ImGui::SameLine();
                ImGui::Checkbox("Labels", &render::g_lodDebugLabels);
                ImGui::SameLine();
                ImGui::Checkbox("Criteria", &render::g_lodDebugCriteria);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Draws the boundaries as what they geometrically ARE: spheres around\n"
                                      "the camera, sliced at sea level. It is why height alone can put every\n"
                                      "chunk past a boundary. Also probes the chunk under the crosshair --\n"
                                      "white box, plus the exact closest-point segment its distance was\n"
                                      "measured along.");
                ImGui::Checkbox("Include regular meshes", &render::g_lodDebugRegularMeshes);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Palms and props select on distance/RADIUS (the ratio sliders above),\n"
                                      "not on metres. Off by default so the two curves stay separable.");
                ImGui::SliderFloat("Debug range (m)", &render::g_lodDebugRange, 40.0f, 800.0f, "%.0f");
                ImGui::SliderInt("Max boxes", &render::g_lodDebugMaxBoxes, 0, 800);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("A populated level has hundreds of meshes and every box is 12 lines,\n"
                                      "which buries the terrain the view is usually there to judge. Chunks\n"
                                      "are drawn first, then the nearest regular meshes fill what is left.\n"
                                      "The readout always counts everything in range, so this hides\n"
                                      "geometry, never numbers. 0 = no limit.");
                ImGui::EndDisabled();

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("CSM"))
            {
                // S0: the enabler for the whole CSM improvement plan (docs/csm_improvement_plan.md).
                // Without a live readout there is nothing to judge the fit/density steps on, and
                // without live config there is nothing to A/B them against.
                ImGui::TextWrapped("Legacy cascaded shadow maps \xE2\x80\x94 the fast alternative to VSM. "
                    "Every number in the readout is what the cascade was actually built with this "
                    "frame (Scene::UpdateCascades), not a value the UI re-derived.");
                ImGui::Separator();

                bool legacyMode = !render::VsmActive();
                if (graphicsEdit(ImGui::Checkbox("Legacy CSM active [Ctrl+V]", &legacyMode)))
                    render::g_shadowMode = legacyMode ? render::ShadowMode::Legacy : render::ShadowMode::VSM;
                if (!legacyMode)
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
                        "VSM drives directional shadows right now. The cascades below are still\n"
                        "computed every frame, but nothing samples them \xE2\x80\x94 the readout is live,\n"
                        "the picture is not. Uncheck the box above before judging any change.");

                CascadeShadowConfig& csmCfg = scene.CascadeConfig();

                ImGui::SeparatorText("Coverage");
                graphicsEdit(ImGui::SliderFloat("Max distance (m)", &csmCfg.maxDistance,
                                                 20.0f, 1000.0f, "%.0f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Far edge of cascade 3 \xE2\x80\x94 the hard shadow terminator.\n"
                                      "Shrinking it is the cheapest way to buy texel density everywhere\n"
                                      "(no extra memory, no extra rasterization).");
                graphicsEdit(ImGui::Checkbox("Auto splits (UE distribution)",
                                              &csmCfg.useUeSplitDistribution));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("ON: author ONLY the max distance above; the three intermediate splits come from\n"
                                      "UE's exponential distribution (ComputeAccumulatedScale, exponent below).\n"
                                      "OFF: the hand-authored row is used.\n"
                                      "The two schemes are stored SEPARATELY -- turning this on never overwrites the\n"
                                      "custom distances, so the toggle is reversible and both are shown here.\n");

                ImGui::BeginDisabled(!csmCfg.useUeSplitDistribution);
                graphicsEdit(ImGui::SliderFloat("Distribution exponent",
                    &csmCfg.cascadeDistributionExponent, 0.1f, 10.0f, "%.2f"));
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("UE's CascadeDistributionExponent (default 3, clamp 0.1..10). Cascade weights are\n"
                                      "exponent^i and a split sits at the running sum over the total: for exponent 3 and\n"
                                      "4 cascades that is 1,3,9,27 of 40 = 2.5% / 10% / 32.5% / 100% of the range.\n"
                                      "Higher = more resolution pulled toward the camera. UE substitutes 4 when a scene\n"
                                      "has no valid precomputed lighting, i.e. the fully dynamic case.\n");

                // Both schemes side by side. The row that is NOT driving is greyed rather than hidden,
                // so the toggle never conceals the numbers it is not using -- nor destroys them.
                ImGui::BeginDisabled(csmCfg.useUeSplitDistribution);
                graphicsEdit(ImGui::DragFloat4("Split distances (m)",
                    csmCfg.sliceDistances.data(), 0.25f, 0.5f, 1000.0f, "%.1f"));
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Hand-authored far plane of cascades 0..3 in view space. BuildSplitScheme clamps them\n"
                                      "monotonic and caps them at max distance, so out-of-order values are safe.\n"
                                      "Greyed while auto splits drive the cascades -- the values are kept, not cleared.\n");
                {
                    const std::array<float, 4> ueSplits =
                        csmCfg.ComputeUeSplitDistances(scene.CameraRef().GetZNear());
                    ImGui::Text("UE auto splits (m):  %7.2f %7.2f %7.2f %7.2f",
                        ueSplits[0], ueSplits[1], ueSplits[2], ueSplits[3]);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("What UE's distribution produces for the current max distance and exponent. Shown\n"
                                          "whether or not the toggle is on, so the two schemes can be compared BEFORE\n"
                                          "switching. A readout, not an editable control.\n");
                }

                ImGui::SeparatorText("Fit");
                graphicsEdit(ImGui::SliderFloat("Overlap (texels)", &csmCfg.overlapInTexels,
                                                 0.0f, 8.0f, "%.2f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Padding added to the fitted sphere radius to absorb the texel-snap shift,\n"
                                      "in CASCADE TEXELS. The snap moves the centre by at most one texel per axis,\n"
                                      "so 2 already leaves a full texel of slack, and the same number is correct\n"
                                      "for every cascade. At 2 the measured worst-case slack is 1.2 texels; 1.0 is\n"
                                      "still positive but thin, and by 0.5 the ortho no longer covers the snapped\n"
                                      "slice \xE2\x80\x94 the UpdateCascades assert fires in Debug, shadows clip at the\n"
                                      "cascade edge in Release. Watch 'R fit/pad': the two should nearly coincide.");
                graphicsEdit(ImGui::SliderFloat("Z padding (m)", &csmCfg.zPadding,
                                                 0.0f, 100.0f, "%.1f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Slack added past the far side of the light-space depth range.");
                graphicsEdit(ImGui::SliderFloat("Caster reach (m)", &csmCfg.casterReachWS,
                                                 0.0f, 400.0f, "%.0f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How far TOWARD the light the ortho near plane is pulled back so casters\n"
                                      "between the sun and the slice still render. This is what inflates zRange\n"
                                      "(and the D16 step) in the readout. Drop it to 0 and watch tall casters'\n"
                                      "shadows get clipped \xE2\x80\x94 that is the problem pancaking exists to solve.");

                // S11 [r.Shadow.CSMScissorOptim]. Off by default like UE's, and the readout column
                // shows the rect it WOULD apply, so the saving is visible before the risk is taken.
                graphicsEdit(ImGui::Checkbox("Scissor to view cone (UE CSMScissorOptim)", &csmCfg.scissorOptim));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("The cascade tile is a square around the slice's bounding sphere; the camera\n"
                                      "sees only a cone inside it. UE scissor the depth pass to that cone's projection\n"
                                      "(camera + 4 far corners, rays extended to the tile border) -- pure rasterisation\n"
                                      "saving, identical result for every receiver the CAMERA can see. Watch the\n"
                                      "'scissor %%' column: 100 = nothing to cut (camera outside the tile, or looking\n"
                                      "along the sun).\n\n"
                                      "OFF by default, as in UE, and for a reason: GLASS shades reflected/refracted\n"
                                      "receivers that can lie outside the camera cone, and those read the undrawn\n"
                                      "part of the tile as LIT. Check water and glass before shipping it on.");
                graphicsEdit(ImGui::SliderFloat("Scissor pad (texels)", &csmCfg.scissorPadTexels,
                                                 0.0f, 16.0f, "%.0f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("OURS -- UE pad nothing. The filter's taps (6x6 tent = 3 texels) plus the\n"
                                      "receiver normal offset can reach past the cone's edge at the screen border;\n"
                                      "a tap on a scissored-out texel reads LIT and lightens the shadow there.\n"
                                      "Set 0 to see the flaw UE ship, 4 covers the widest kernel.");

                // S14 [UE ShadowBoundsAccurate]. ON by default: it is what UE always do for a
                // directional cascade, and it is the cut the scissor cannot make -- before the VS.
                graphicsEdit(ImGui::Checkbox("Accurate caster cull (UE ShadowBoundsAccurate)", &csmCfg.accurateCasterCull));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Cull a cascade's casters against the camera SLICE EXTRUDED TOWARD THE SUN\n"
                                      "(UE ComputeShadowCullingVolume) instead of the cascade's full ortho box.\n"
                                      "In the shadow map that is the convex hull of the slice's projection -- tighter\n"
                                      "than the scissor's rectangle -- and it acts on both the CPU and the GPU cull\n"
                                      "before any vertex is shaded. Tall casters outside the view are kept: what\n"
                                      "shadows the slice projects INTO it along the light, by definition.\n\n"
                                      "Readout: 'cull pl' = planes of the volume (6 = box, 7..11 = accurate),\n"
                                      "'casters' = objects the CPU cull passed last frame. Same glass caveat as the\n"
                                      "scissor -- UE ship with it anyway. OFF = the old box, for the A/B.");

                // UE's three legacy-CSM bias cvars first, then the one knob that is OURS. Split in
                // two on purpose: the user could not tell which of the four numbers were a
                // transcription and which were invented here.
                // Exactly UE's three legacy-CSM bias cvars, nothing else. The receiver normal
                // offset that used to sit here is deleted: UE has no such term, and it only
                // existed to cover a depth-bias budget that was a quarter of theirs.
                // The three that decide ACNE vs PETER-PANNING. Everything here is measured on
                // wind_test at the user camera; see docs/csm_improvement_plan.md S6.
                graphicsEdit(ImGui::Checkbox("Pancake casters", &csmCfg.pancakeCasters));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("WHAT IT DOES: a caster in front of the cascade's near plane is pressed ONTO it\n"
                                      "instead of being clipped away. That is what lets the near plane be fitted TIGHT to\n"
                                      "the slice instead of pulled back by Caster reach, and that is where the D16 precision\n"
                                      "comes from - watch zRange and D16 step in the readout table drop on cascade 0.\n"
                                      "\n"
                                      "Casters are kept by a SEPARATE, wider culling near plane, so Caster reach still\n"
                                      "matters; its meaning just narrows from \"how far to push the projection back\" to\n"
                                      "\"how far toward the light to look for casters\".\n"
                                      "\n"
                                      "COST: a triangle with some vertices clamped and some not is deformed. Only on casters\n"
                                      "straddling the near plane - UE document the same side effect.\n"
                                      "\n"
                                      "Turn OFF and tall casters should visibly lose their tops. If nothing changes, they are\n"
                                      "being CULLED instead and pancaking is not doing anything.");

                graphicsEdit(ImGui::SliderFloat("Pancake slack (m)", &csmCfg.pancakeSlackWS,
                                                 0.0f, 160.0f, "%.1f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Metres of room between the slice and the near plane casters are clamped to.\n"
                                      "0 = fitted tight: maximum D16 precision, and everything in front of the\n"
                                      "slice gets pancaked.\n"
                                      "Raise it if a caster STRADDLING the near plane shadows itself wrongly - a\n"
                                      "triangle with some vertices clamped and some not is deformed, and slack\n"
                                      "pushes the plane out of that geometry.\n"
                                      "At slack == Caster reach the projection near equals the CULLING near and\n"
                                      "the whole step degenerates to its pre-pancaking baseline - which also makes\n"
                                      "this the A/B lever without rebuilding.\n"
                                      "Watch zRange / D16 step in the readout table pay for it.");

                ImGui::SeparatorText("Bias \xE2\x80\x94 depth pass");
                graphicsEdit(ImGui::SliderFloat("Depth bias (texels)", &csmCfg.depthBiasInTexels,
                                                 0.0f, 12.0f, "%.2f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("WHAT IT DOES: pushes every caster AWAY from the sun when its depth is written,\n"
                                      "by this many cascade texels. Uniform - it does not care how the surface is angled.\n"
                                      "\n"
                                      "RAISE to kill acne. COST: peter-panning, i.e. shadows detach from what casts them.\n"
                                      "\n"
                                      "Measured here: acne collapses between 0.5 (79% of lit pixels) and 1.0 (0.03%), then\n"
                                      "stays FLAT to 7.0 - so above ~1 it buys NOTHING but peter-panning, which keeps\n"
                                      "growing (shadow lift -0.81 at 1.0, +0.60 at 1.5, +3.13 at 5.0).\n"
                                      "\n"
                                      "[r.Shadow.CSMDepthBias = 10] Their 10 is 5.00 here: their bias scales by\n"
                                      "radius/resolution, our texel is 2*radius/resolution, exactly twice theirs. We ship\n"
                                      "1.50 because we measured this scene; they ship 10 to cover every scene.\n"
                                      "SIDE EFFECT: this number also sets the penumbra WIDTH (UE derive TransitionSize from\n"
                                      "the same expression), so raising it softens shadow edges as well.");
                graphicsEdit(ImGui::SliderFloat("Slope scale", &csmCfg.slopeScale,
                                                 0.0f, 8.0f, "%.2f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("WHAT IT DOES: adds EXTRA depth push, proportional to tan(angle between the surface\n"
                                      "and the sun) - so a wall raking the light gets more, a surface facing it gets none.\n"
                                      "Total push = depthBias * (1 + this * slope).\n"
                                      "\n"
                                      "THIS IS THE KNOB THAT SEPARATES THE TWO PROBLEMS. Acne only happens where the light\n"
                                      "rakes the surface; peter-panning is only visible where it does not. Spending here\n"
                                      "instead of on Depth bias buys acne protection that costs nothing where you can see it.\n"
                                      "It needs the CASTER's normal, which only the depth pass has - the sampler cannot\n"
                                      "do this at all.\n"
                                      "\n"
                                      "0 = flat bias everywhere. [r.Shadow.CSMSlopeScaleDepthBias = 3]");
                graphicsEdit(ImGui::SliderFloat("Max slope", &csmCfg.maxSlope,
                                                 0.0f, 4.0f, "%.2f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("WHAT IT DOES: caps the tan() above. Mandatory: at exactly 90 degrees to the light the\n"
                                      "required bias is infinite.\n"
                                      "\n"
                                      "WATCH OUT: at 1.0 the cap engages at 45 degrees from the sun, so with a LOW sun almost\n"
                                      "all flat ground is already AT the cap. Raising this then moves the entire ground plane,\n"
                                      "not just steep geometry - it stops behaving like a slope knob and behaves like a second\n"
                                      "Depth bias.\n"
                                      "\n"
                                      "[r.Shadow.ShadowMaxSlopeScaleDepthBias = 1]");
                ImGui::Text("effective: %.2f texels facing the sun, %.2f grazing",
                            csmCfg.depthBiasInTexels,
                            csmCfg.depthBiasInTexels * (1.0f + csmCfg.slopeScale * csmCfg.maxSlope));

                ImGui::SeparatorText("Bias \xE2\x80\x94 sample time (not in UE)");
                graphicsEdit(ImGui::SliderFloat("Normal bias (texels)",
                    &csmCfg.normalBiasInTexels, 0.0f, 4.0f, "%.2f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("WHAT IT DOES: before looking the shadow up, slides the RECEIVER's sample point out\n"
                                      "along its own normal by this many cascade texels. It moves WHERE we look, not what\n"
                                      "was stored - so unlike the three knobs above it costs NO depth push.\n"
                                      "\n"
                                      "WHY IT IS THE CHEAPEST ACNE FIX: acne is a surface sampling its own stored depth.\n"
                                      "Stepping off the surface stops that without moving the shadow toward the light, so\n"
                                      "peter-panning barely responds to it.\n"
                                      "\n"
                                      "COST: it detaches shadows from CONVEX EDGES and thin geometry, because the offset\n"
                                      "point can leave the object. Too high and contact shadows creep away from corners.\n"
                                      "\n"
                                      "UE has no equivalent - their legacy CSM relies on the depth pass alone, and their\n"
                                      "stock defaults acne and peter-pan visibly. This is deliberately not a transcription.");
                ImGui::SeparatorText("Cascade transition");
                graphicsEdit(ImGui::SliderFloat("Blend fraction", &csmCfg.blendFraction,
                                                 0.0f, 0.3f, "%.3f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("WHAT IT DOES: over the last this-much of a cascade's OWN SLICE LENGTH, cross-fade\n"
                                      "into the next cascade instead of switching at the split. Hides the jump in texel\n"
                                      "density, bias and kernel footprint that a hard switch makes visible as a seam.\n"
                                      "\n"
                                      "COST: pixels inside the band take a SECOND shadow sample. 0 = hard switch.\n"
                                      "\n"
                                      "Fraction of the SLICE, not of the absolute distance - that was the bug this fixes:\n"
                                      "measured off the distance, c2 (35..100 m) faded over 10 m where UE fade over 6.5.\n"
                                      "[CascadeTransitionFraction = 0.1, UE clamp it to 0.3 and so does this slider]");
                graphicsEdit(ImGui::SliderFloat("Distance fade", &csmCfg.distanceFadeFraction,
                                                 0.0f, 0.3f, "%.3f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("WHAT IT DOES: the LAST cascade has no coarser neighbour to hand over to, so instead\n"
                                      "of blending it fades the shadow out to fully lit over the last this-much of its slice.\n"
                                      "\n"
                                      "WITHOUT IT shadows end in a hard terminator LINE at the shadow distance - everything\n"
                                      "is shadowed at 299 m and nothing at 301 m. That line is the artifact this removes.\n"
                                      "\n"
                                      "COST: shadows go missing slightly earlier than the shadow distance says. Raise the\n"
                                      "Split distances / max distance if that bites before this does.\n"
                                      "0 = the hard line back (the rollback check).\n"
                                      "\n"
                                      "UE do the same by moving the last cascade's fade plane inward by the same extension.");

                if (ImGui::Button("Reset CSM config to defaults"))
                    graphicsSettings.ResetCsm(renderer, scene, settings);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Back to the compile-time defaults in SceneRenderConfig.h \xE2\x80\x94 the\n"
                                      "baseline every plan step is measured against. The reset is saved to\n"
                                      "graphics_settings.json and applied to subsequently loaded levels.");

                ImGui::SeparatorText("Filtering");
                int csmFilter = static_cast<int>(csmCfg.filterMode);
                if (graphicsEdit(ImGui::SliderInt("Filter kernel", &csmFilter, 0, 2,
                    csmFilter == 0 ? "3x3 box" : (csmFilter == 1 ? "4x4 tent (UE q3)" :
                                                                  "6x6 tent (UE q5, default)"))))
                    csmCfg.filterMode = static_cast<uint32_t>(csmFilter);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("0 = the original 3x3 SampleCmp box with its per-cascade radius shrink (the A/B arm).\n"
                                      "1 = soft-occlusion RAMP + 4x4 tent from 4 Gather quads -- UE's Manual3x3PCF.\n"
                                      "2 = the same ramp + 6x6 tent from 9 gathers -- UE's Manual5x5PCF, and the DEFAULT,\n"
                                      "    because r.ShadowQuality defaults to 5 in Unreal and ManualPCF selects 5x5 there.\n"
                                      "The ramp's width is proportional to the cascade's world texel, so softness and\n"
                                      "resolution drop by the SAME factor at a boundary; the kernel WIDTH is what sets the\n"
                                      "absolute softness. A stock UE reads softer than a 4x4 tent for exactly that reason.\n");

                graphicsEdit(ImGui::SliderFloat("Shadow filter sharpen",
                    &csmCfg.shadowFilterSharpen, 0.0f, 1.0f, "%.2f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("UE's per-light Shadow Filter Sharpen, same 0..1 artist range and same\n"
                                      "default of 0 (= off). The shader receives x*7+1 and multiplies the raw\n"
                                      "PCF ratio by it before saturating, so partial occlusion is pushed toward\n"
                                      "0 or 1: the penumbra keeps its WIDTH but loses its gradient. 1.0 leaves a\n"
                                      "kernel roughly as hard as no filtering at all.\n"
                                      "Only affects the two tent kernels -- the 3x3 box arm never had it.");

                graphicsEdit(ImGui::SliderFloat("Receiver bias", &csmCfg.csmReceiverBias,
                                                 0.0f, 1.0f, "%.2f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("UE r.Shadow.CSMReceiverBias, default 0.9. Scales the soft-occlusion ramp\n"
                                      "by how edge-on the receiver is to the light: at grazing angles a texel\n"
                                      "spans much more depth, so the ramp has to widen or the surface\n"
                                      "shadows itself. 0 removes the term entirely (acne on shallow ground),\n"
                                      "1 is the widest ramp (peter-panning on those same slopes).\n"
                                      "Only affects the two tent kernels.");

                graphicsEdit(ImGui::Checkbox("PCF over-blur correction",
                                              &csmCfg.pcfOverBlurCorrection));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("UE's ApplyPCFOverBlurCorrection: square the resulting visibility.\n"
                                      "A wide tent leaks light INTO the umbra, because every tap inside the\n"
                                      "kernel votes equally regardless of how far the blocker is; squaring\n"
                                      "darkens the mid-tones and pulls the shadow core back to solid while\n"
                                      "leaving 0 and 1 untouched. UE apply it unconditionally to every\n"
                                      "filtered shadow, so ON is the matching default.\n"
                                      "Applied AFTER sharpen, in UE's order. Only affects the tent kernels.");

                ImGui::SeparatorText("Debug");
                bool csmTint = render::g_csmDebugMode == render::CsmDebugMode::CascadeTint;
                if (ImGui::Checkbox("Cascade tint", &csmTint))
                    render::g_csmDebugMode = csmTint ? render::CsmDebugMode::CascadeTint
                                                     : render::CsmDebugMode::Off;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Tint each pixel by the cascade its shadow sample RESOLVED to:\n"
                                      "c0 red, c1 green, c2 blue, c3 yellow, grey = past the last cascade.\n"
                                      "Resolved, not selected: where a pixel falls into the tile-border margin\n"
                                      "it silently drops to a coarser cascade, and the tint is what makes that\n"
                                      "ring visible. Ignored in VSM mode.\n"
                                      "It rides the SUN's contribution only — spot/point lights are added by\n"
                                      "later passes and stay untinted, so read the zones outside their pools.");

                ImGui::SeparatorText("Readout");
                const SceneFrameData::CascadeData& csm = scene.GetCascadeData();
                const ImGuiTableFlags csmTableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                      ImGuiTableFlags_SizingFixedFit;
                if (ImGui::BeginTable("CsmReadout", 11, csmTableFlags))
                {
                    ImGui::TableSetupColumn("c");
                    ImGui::TableSetupColumn("slice (m)");
                    ImGui::TableSetupColumn("tile");
                    ImGui::TableSetupColumn("texel (mm)");
                    ImGui::TableSetupColumn("R fit/pad (m)");
                    ImGui::TableSetupColumn("zRange (m)");
                    ImGui::TableSetupColumn("D16 (mm)");
                    ImGui::TableSetupColumn("bias (mm)");
                    ImGui::TableSetupColumn("scissor %");
                    ImGui::TableSetupColumn("cull pl");
                    ImGui::TableSetupColumn("casters");
                    ImGui::TableHeadersRow();

                    for (int c = 0; c < SceneFrameData::kCascades; ++c)
                    {
                        // zRange is what D16 has to resolve; the depth bias is stored in NDC, so
                        // multiplying it back by the range recovers the world-space peter-panning.
                        const float range = csm.farLsDbg[c] - csm.nearLsDbg[c];
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::Text("%d", c);
                        ImGui::TableSetColumnIndex(1); ImGui::Text("%.1f-%.1f", csm.splitsVS[c], csm.splitsVS[c + 1]);
                        ImGui::TableSetColumnIndex(2); ImGui::Text("%u", csm.tileSizeDbg[c]);
                        ImGui::TableSetColumnIndex(3); ImGui::Text("%.2f", csm.unitsPerTexelDbg[c] * 1000.0f);
                        ImGui::TableSetColumnIndex(4); ImGui::Text("%.2f / %.2f", csm.sphereRadiusDbg[c], csm.radiusDbg[c]);
                        ImGui::TableSetColumnIndex(5); ImGui::Text("%.1f", range);
                        ImGui::TableSetColumnIndex(6); ImGui::Text("%.2f", (range / 65535.0f) * 1000.0f);
                        ImGui::TableSetColumnIndex(7); ImGui::Text("%.1f", csm.depthBiasNDC[c] * range * 1000.0f);
                        ImGui::TableSetColumnIndex(8); ImGui::Text("%.0f", csm.scissorAreaDbg[c] * 100.0f);
                        ImGui::TableSetColumnIndex(9); ImGui::Text("%u", csm.cullPlanesDbg[c]);
                        ImGui::TableSetColumnIndex(10); ImGui::Text("%u", csm.cullCastersDbg[c]);
                    }
                    ImGui::EndTable();
                }
                ImGui::TextDisabled("texel = world mm per shadow texel (lower is sharper).  R fit/pad = sphere\n"
                                    "radius before/after overlap.  D16 = quantization step of the 16-bit depth\n"
                                    "atlas over zRange.  bias = depth bias in world mm (= peter-panning).\n"
                                    "scissor %% = share of the tile the view-cone scissor would rasterise (S11).");

                ImGui::EndTabItem();
            }

            // Its own tab because it belongs to NEITHER shadow mode: the trace reads the camera
            // depth buffer, so Legacy CSM and VSM get the identical term. Putting it under either
            // one would say it is a property of that mode, which it is not.
            if (ImGui::BeginTabItem("Contact"))
            {
                if (ImGui::Button("Reset contact shadows to defaults"))
                {
                    graphicsSettings.ResetContactShadows(renderer, scene, settings);
                }
                graphicsEdit(ImGui::Checkbox("Contact shadows ENABLED",
                                              &render::contact::g_enabled));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Master switch, and OFF is the default -- which matches UE, whose per-light\n"
                                      "ContactShadowLength defaults to 0. Off takes not a single depth sample.\n\n"
                                      "A short march through the CAMERA depth buffer toward the sun, recovering the\n"
                                      "scale a shadow-map texel cannot. Works in BOTH shadow modes and for"
                                      " EVERY light -- sun, spot and point -- as UE run it from one"
                                      " GetShadowTermsBase.");
                ImGui::BeginDisabled(!render::contact::g_enabled);

                {
                    // Either/or for spot + point. The sun is not on this list: there the contact
                    // term sits on top of CSM/VSM by design.
                    static const char* kLocalModes[] = { "Shadow map (contacts off for locals)",
                                                         "Contacts INSTEAD of the shadow map",
                                                         "Auto: contacts only where no shadow slot" };
                    int mode = (int)std::min<std::uint32_t>(render::contact::g_localMode, 2u);
                    if (graphicsEdit(ImGui::Combo("Local lights (spot/point)",
                                                   &mode, kLocalModes, 3)))
                        render::contact::g_localMode = (std::uint32_t)mode;
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("A local light uses ONE shadow source, never both. Stacking a contact trace\n"
                                          "on a small-range light's shadow map darkens the same contact twice and\n"
                                          "buys nothing.\n\n"
                                          "Shadow map: what you had; this tab does nothing for locals.\n"
                                          "Contacts instead: the map is not even sampled (9 atlas taps saved per\n"
                                          "pixel), the 8-step depth trace is the light's only shadow. Cheap, but a\n"
                                          "contact only reaches as far as the trace length -- nothing casts across\n"
                                          "the room.\n"
                                          "Auto: slotted lights keep their map, unslotted ones get contacts, so a\n"
                                          "light that lost the atlas budget is not left with no shadow at all.");
                }

                ImGui::Separator();
                ImGui::TextDisabled("As in UE (CastScreenSpaceShadowRay)");

                graphicsEdit(ImGui::Checkbox("Temporal dither (TAA/DLSS averages it)",
                                              &render::contact::g_temporalDither));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("UE: InterleavedGradientNoise(PixelPos, StateFrameIndexMod8). The dither\n"
                                      "pattern shifts every frame over an 8-frame cycle, so the temporal pass\n"
                                      "averages the binary per-pixel hit/miss into a smooth value. This is the\n"
                                      "cheapest denoiser there is -- no extra pass, no extra buffer.\n\n"
                                      "It makes a SINGLE frame noisier and only pays off through DLSS/TAA; turn\n"
                                      "it off to judge a still or when running --dlss=off.");

                graphicsEdit(ImGui::Checkbox("Length in METRES",
                                              &render::contact::g_lengthInWorldSpace));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("UE support both readings and pick between them by the SIGN of the value.\n\n"
                                      "OFF (their default): length is a MULTIPLE OF VIEW DEPTH, so the trace covers\n"
                                      "the same number of SCREEN pixels near and far and keeps working at distance.\n"
                                      "ON: plain metres. Predictable, but shrinks below a pixel far away and stops\n"
                                      "doing anything there.");

                const float lenMax = render::contact::g_lengthInWorldSpace ? 2.0f : 0.3f;
                graphicsEdit(ImGui::SliderFloat(
                    render::contact::g_lengthInWorldSpace ? "Length (m)" : "Length (x view depth)",
                    &render::contact::g_length, 0.0f, lenMax, "%.4f"));

                graphicsEdit(ImGui::SliderFloat("Intensity", &render::contact::g_intensity,
                                                 0.0f, 1.0f, "%.2f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How dark a hit makes the pixel (UE ContactShadowCastingIntensity).");

                int csSteps = static_cast<int>(render::contact::g_steps);
                if (graphicsEdit(ImGui::SliderInt("Steps", &csSteps, 1, 16)))
                    render::contact::g_steps = static_cast<std::uint32_t>(csSteps);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Depth samples along the ray. UE hardcode 8, and that is not laziness: their\n"
                                      "compare tolerance is |rayDepthSpan| * (1/steps) * 2, so THE STEP COUNT IS\n"
                                      "BAKED INTO THE ACCEPTANCE WINDOW.\n\n"
                                      "Raising it narrows the window per sample while adding samples along a ray\n"
                                      "that hugs the surface, so each pixel gets more sensitive to the dither\n"
                                      "phase -- MORE speckle, not less. Measured added speckle: 4 -> +3.49pp,\n"
                                      "8 -> +4.00, 16 -> +4.05, 32 -> +5.77. Capped at 16 for that reason.");

                ImGui::Separator();
                ImGui::TextDisabled("OURS -- Epic ship no denoiser and no distance fade");

                graphicsEdit(ImGui::SliderFloat("Max thickness (x ray length)",
                    &render::contact::g_maxThicknessFrac, 0.0f, 3.0f, "%.2f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("THE knob for far-field speckle. A hit whose occluder sits further BEHIND\n"
                                      "the ray point than this fraction of the ray length is rejected -- it is not\n"
                                      "a contact, it is the far side of something.\n\n"
                                      "A FRACTION, not metres, on purpose: the ray length is itself a multiple of\n"
                                      "view depth, so it grows with distance. A fixed metre threshold cannot\n"
                                      "track it -- near it is a no-op, far it kills the real contacts too. Tied\n"
                                      "to the ray it stays meaningful at 10 m and at 3 km alike.\n\n"
                                      "0 = no test, which is UE behaviour and is only safe at close range.");

                graphicsEdit(ImGui::SliderFloat("Normal offset (x ray length)",
                    &render::contact::g_normalOffsetFrac, 0.0f, 0.5f, "%.3f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Starts the ray this far off the surface along the normal. A ray that begins\n"
                                      "ON the surface is ambiguous at its very first step. UE do not do this --\n"
                                      "they start at the shaded point.\n\n"
                                      "A FRACTION of the ray length, not metres: what it fights is the world\n"
                                      "footprint of a SCREEN PIXEL plus depth precision, and both grow with\n"
                                      "distance. 0.02 m is meaningful at 10 m and far below a pixel at 350 m,\n"
                                      "where it would quietly stop doing anything.");

                graphicsEdit(ImGui::SliderFloat("Grazing fade (NdotL)",
                    &render::contact::g_grazingFadeNdotL, 0.0f, 0.5f, "%.3f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Fades the term out below this NdotL. As the sun grazes a surface the ray\n"
                                      "runs nearly PARALLEL to it and the march measures depth-buffer quantisation\n"
                                      "rather than geometry -- that is the speckle field on flat distant ground.\n"
                                      "0 = no guard (UE behaviour).");

                graphicsEdit(ImGui::SliderFloat("Min distance (m)",
                    &render::contact::g_minDistanceM, 0.0f, 200.0f, "%.1f"));
                graphicsEdit(ImGui::SliderFloat("Max distance (m)",
                    &render::contact::g_maxDistanceM, 0.0f, 2000.0f, "%.0f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Metres from the camera. 0 max = no far limit. Outside the window the term\n"
                                      "is off entirely -- which is the blunt way to kill artifacts in the far field\n"
                                      "where contacts buy the least anyway.");
                graphicsEdit(ImGui::SliderFloat("Far fade band (m)",
                    &render::contact::g_fadeBandM, 0.1f, 200.0f, "%.1f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("The last N metres before Max fade out instead of cutting, or the boundary\n"
                                      "itself reads as a line across the ground.");

                ImGui::EndDisabled();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("VSM"))
            {
                if (ImGui::Button("Reset VSM quality to defaults"))
                {
                    graphicsSettings.ResetVsm(renderer, scene, settings);
                }
                ImGui::TextWrapped("Virtual Shadow Maps (Rung 2, experimental) for local lights "
                    "(spot + point + glass). Legacy = CSM directional + spot/point atlas. Tunes "
                    "quality vs. cost live (no reallocation). Directional still uses CSM until Step 24.");
                ImGui::Separator();
                bool vsmMode = render::VsmActive();
                if (graphicsEdit(ImGui::Checkbox("VSM shadows enabled [Ctrl+V]", &vsmMode)))
                    render::g_shadowMode = vsmMode ? render::ShadowMode::VSM : render::ShadowMode::Legacy;

                // Applies to BOTH modes (folds GPU-instanced casters into the indirect cull): ON =
                // GI casts in VSM + via indirect in Legacy; OFF = GI reverts to the Legacy CPU tail.
                graphicsEdit(ImGui::Checkbox("GPU-instanced casters -> indirect/VSM [Ctrl+G]",
                                              &render::g_giIndirectShadowsEnabled));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("ON: GPU-instanced objects cast shadows in VSM (and via the indirect path in\n"
                                      "Legacy), dropping their CPU RenderShadow tail. OFF: Legacy CPU tail only (no VSM).");

                // (Chunked-terrain LOD selection moved to the "LOD" tab — it is a camera-LOD
                // control, not a shadow one; the caster follows the drawn LOD by construction.)

                ImGui::BeginDisabled(!render::VsmActive());

                // S3.6: these two used to live OUTSIDE this block because they also applied to the
                // Legacy cascades. They no longer do: the Legacy/Rung-0 caster LOD now comes from the
                // RECEIVER (UE's rule), so viewLod_ -- the only thing these knobs feed -- is zeroed for
                // cascades and is not read by the Legacy draw at all. A control that claims a scope it
                // does not have is worse than no control, hence the move and the renames.
                graphicsEdit(ImGui::SliderInt("Shadow LOD bias (VSM only)",
                                               &render::g_shadowLodBias, -2, 3));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("ADDITIVE offset on the per-view shadow LOD, VSM ONLY (clipmap levels plus\n"
                                      "the local lights, which are pinned at tier 0). Each view picks a base LOD by\n"
                                      "its tier (near = fine, far = coarse); this shifts the whole curve. 0 = the\n"
                                      "tier curve alone; + = coarser everywhere (cheaper), - = sharper.\n"
                                      "Legacy CSM ignores it: its casters draw at their RECEIVER's LOD.\n"
                                      "Changing it rebuilds the caster tables at GPU idle (a hitch).\n");

                graphicsEdit(ImGui::Checkbox("Bias the NEAREST tier too (VSM only)",
                                              &render::g_shadowLodBiasNearTier));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Default OFF. The bias shifts the TIER curve, but clipmap level 0 and the local\n"
                                      "lights (pinned at tier 0) have no distance to hide a coarser caster behind, so\n"
                                      "biasing them lands as self-shadow blobs on thin shells. Measured on demo.json's\n"
                                      "tent, 5 interleaved samples: dark canvas pixels 2698 OFF vs 3996 ON, ~+0.2 ms.\n"
                                      "A change rebuilds the caster tables at GPU idle.\n");

                graphicsEdit(ImGui::SliderInt("Shadow tiers per LOD (VSM only)",
                                               &render::g_shadowLodTierStride, 1, 8));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How many consecutive shadow-view tiers share one caster mesh LOD.\n"
                                      "1 = the aggressive 0,1,2,3 curve; 2 = 0,0,1,1,2,2, which avoids changing caster\n"
                                      "geometry at every VSM clip boundary. The bias above is added afterwards.\n"
                                      "VSM only -- Legacy CSM has no per-view LOD curve at all.\n"
                                      "Changing it rebuilds the caster tables (a hitch).\n");

                graphicsEdit(ImGui::SliderFloat("LOD ref distance", &vsm::g_refDist,
                                                 1.0f, 40.0f, "%.1f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Smaller = coarser pages: fewer/faster + more stable, softer near.\n"
                                      "Larger = sharper near but more resident pages + higher render cost.");

                int ds = static_cast<int>(vsm::g_requestDownscale);
                if (graphicsEdit(ImGui::SliderInt("Request downscale", &ds, 1, 8)))
                    vsm::g_requestDownscale = static_cast<std::uint32_t>(ds < 1 ? 1 : ds);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Screen sub-sampling for page discovery.\n"
                                      "1 = full res (best coverage, costliest); higher = cheaper, may miss pages.");

                int lru = static_cast<int>(vsm::g_lruThreshold);
                if (graphicsEdit(ImGui::SliderInt("LRU eviction frames", &lru, 1, 120)))
                    vsm::g_lruThreshold = static_cast<std::uint32_t>(lru < 1 ? 1 : lru);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Frames a resident page survives unrequested before it is freed.");

                graphicsEdit(ImGui::SliderFloat("Clipmap base extent", &vsm::g_clipmapBaseExtent,
                                                 4.0f, 200.0f, "%.1f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Step 24f directional clipmap: finest level's world extent (level i = base*2^i).\n"
                                      "Smaller = sharper near shadows but less far coverage; larger = the reverse.");
                graphicsEdit(ImGui::Checkbox("Clipmap level blend", &vsm::g_clipmapBlendEnabled));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Hard performance A/B switch. OFF sends width 0 to both the page requester\n"
                                      "and samplers: no new parent requests and no second PCF sample. Parent pages\n"
                                      "requested before the switch age out after the configured LRU frame count.");
                ImGui::BeginDisabled(!vsm::g_clipmapBlendEnabled);
                graphicsEdit(ImGui::SliderFloat("Clipmap blend width", &vsm::g_clipmapBlendWidth,
                                                 0.0f, 0.30f, "%.3f"));
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Outer fraction of each fine clip level blended into the next coarser level.\n"
                                      "Hides caster-LOD silhouette changes at clip boundaries. 0 is a strict off\n"
                                      "switch: no parent page requests and no second 3x3 PCF sample. Non-zero\n"
                                      "costs the extra sample only inside this transition band.");
                // ---- SMRT (docs/vsm_smrt_plan.md) --------------------------------------------
                // Marches a ray from the receiver toward the light through the clipmap instead of
                // taking one biased comparison. Directional clipmap only, so it sits with the
                // clipmap knobs and inside the same VsmActive() gate.
                ImGui::Separator();
                bool smrtOn = vsm::g_smrtRayCount > 0u;
                // Remembers the ray count across an off/on cycle, so unticking is not destructive.
                // 7 = UE's RayCountDirectional default, and the right thing to hand someone who
                // just ticked the box. ONE ray is the noisiest configuration this feature has: the
                // per-pixel direction jitter is not averaged and the march point-samples one texel
                // per step with no filter, so a single ray measures twice the reference's
                // salt-and-pepper (10.09 vs 5.37; 7 rays 6.06, 16 rays 5.29).
                static int smrtLastRays = 7;
                if (graphicsEdit(ImGui::Checkbox("SMRT ray-marched clipmap", &smrtOn)))
                {
                    vsm::g_smrtRayCount = smrtOn
                        ? static_cast<std::uint32_t>(smrtLastRays)
                        : 0u;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("OFF = one SampleCmp per level with a constant depth bias (today's path).\n"
                                      "ON  = UE's SMRT: a ray from the receiver toward the light, whose own\n"
                                      "depth step per sample IS the tolerance -- so the constant bias below is\n"
                                      "not applied at all and its sliders grey out.\n\n"
                                      "1 ray is HARD-EDGED by design (no dither, no filter). Softness needs\n"
                                      "more rays; measured on wind_test it moves shadow coverage by 0.56pp,\n"
                                      "i.e. the shadow stays where it was.");

                ImGui::BeginDisabled(!smrtOn);
                int smrtRays = static_cast<int>(vsm::g_smrtRayCount);
                if (graphicsEdit(ImGui::SliderInt("SMRT rays", &smrtRays, 1,
                                                  static_cast<int>(vsm::kSmrtMaxRays))))
                {
                    vsm::g_smrtRayCount = static_cast<std::uint32_t>(smrtRays);
                    smrtLastRays = smrtRays;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Rays per pixel; UE's r.Shadow.Virtual.SMRT.RayCountDirectional default is 7.\n"
                                      "Cost lands in Pass_Lighting, not in the shadow passes.\n"
                                      "Until Step 3 adds the per-ray dither every ray is identical, so counts\n"
                                      "above 1 buy nothing yet and only cost.");

                int smrtSteps = static_cast<int>(vsm::g_smrtSamplesPerRay);
                if (graphicsEdit(ImGui::SliderInt("SMRT samples/ray", &smrtSteps, 1,
                                     static_cast<int>(vsm::kSmrtMaxSamplesPerRay))))
                    vsm::g_smrtSamplesPerRay = static_cast<std::uint32_t>(smrtSteps);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Steps along each ray (UE default 8). Too few and a thin occluder is\n"
                                      "stepped over, so its shadow disappears rather than softening.");

                graphicsEdit(ImGui::Checkbox("SMRT temporal dither", &vsm::g_smrtTemporalDither));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Rotates the sample set once per frame, so the temporal pass sees a\n"
                                      "different set each frame and averages them (UE feed StateFrameIndex\n"
                                      "into their blue noise the same way).\n\n"
                                      "It makes a SINGLE frame noisier and only pays off through DLSS, so\n"
                                      "turn it off when judging a still. Measured with DLSS on: -13.7%% noise\n"
                                      "at 1 ray, -2.6%% at 7 -- it matters most where quality is worst.");

                int adaptive = static_cast<int>(vsm::g_smrtAdaptiveRayCount);
                if (graphicsEdit(ImGui::SliderInt("SMRT adaptive after N rays", &adaptive, 0, 8)))
                    vsm::g_smrtAdaptiveRayCount = static_cast<std::uint32_t>(adaptive);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("After N rays, a wave whose lanes ALL agree stops early (UE default 1).\n"
                                      "Fully lit and fully umbral pixels are most of the screen and need one\n"
                                      "or two rays; only penumbrae need the full count. 0 disables it.\n\n"
                                      "Measured at 7 rays: Pass_Lighting 0.861 -> 0.425 ms, a 2.0x saving for\n"
                                      "0.23%% of pixels changed. Compute only -- glass always shoots them all.");

                graphicsEdit(ImGui::SliderFloat("SMRT level margin", &vsm::g_smrtLevelMargin,
                                                 0.25f, 1.0f, "%.2f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Fraction of a clipmap level square within which a receiver is accepted.\n"
                                      "1.0 = the finest level that contains it at all -- the sharpest data there is.\n"
                                      "Lower values mirror UE, whose level covers twice the radius its selection\n"
                                      "tests, giving a marched ray room before it leaves the level.\n\n"
                                      "A margin is bought with A LEVEL OF SHADOW RESOLUTION: at 0.5 the coarse\n"
                                      "level pages show through as rectangular slabs and palm fronds lose their\n"
                                      "leaflets. Measured as unnecessary here, hence the 1.0 default.");

                graphicsEdit(ImGui::SliderFloat("SMRT sun angle (deg)",
                    &vsm::g_smrtSourceAngleDeg, 0.0f, 8.0f, "%.3f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("The light angular SIZE -- what a penumbra is actually made of.\n"
                                      "UE default is 0.5357 deg, the real sun disc. At 0 every ray collapses\n"
                                      "onto the light axis and the ray count buys nothing at all.\n\n"
                                      "Raise it for visibly soft shadows: the penumbra then WIDENS with\n"
                                      "distance from the contact point, which no single-tap filter can do.");

                graphicsEdit(ImGui::SliderFloat("SMRT texel dither",
                    &vsm::g_smrtTexelDitherScale, 0.0f, 4.0f, "%.2f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Jitters each ray start by up to this many shadow texels (UE default 2.0),\n"
                                      "trading the resolution staircase for noise the temporal pass absorbs.\n"
                                      "0 is a clean off switch. Each offset carries its own receiver-plane\n"
                                      "bias; without that the jitter is simply acne.");

                graphicsEdit(ImGui::SliderFloat("SMRT ray length scale",
                    &vsm::g_smrtRayLengthScale, 0.0f, 4.0f, "%.2f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Ray length as a multiple of distance-to-camera (UE default 1.5). This is\n"
                                      "the knob that REPLACES the depth bias as the thing to tune, and UE's own\n"
                                      "warning applies: too high detaches shadows from their contact points\n"
                                      "unless samples/ray goes up; too low caps how large a penumbra can get.");
                ImGui::EndDisabled();
                ImGui::Separator();

                // The constant depth bias is DEAD on the SMRT path -- VsmClipmapShadow returns from
                // the march before levelDepthBias is even computed. Greyed rather than left live,
                // because a slider that visibly does nothing is worse than an absent one.
                ImGui::BeginDisabled(smrtOn);
                graphicsEdit(ImGui::SliderFloat("Clipmap depth bias", &vsm::g_clipmapDepthBias,
                                                 0.0f, 0.01f, "%.4f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Directional clipmap NDC depth bias at LEVEL 0 (0.0001 = 1.23 shadow texels).\n"
                                      "Constant in texels across levels when decay = 1, so its world size doubles per\n"
                                      "level -- raise it with decay < 1 or far thin shadows detach.");
                graphicsEdit(ImGui::SliderFloat("Depth bias decay /level",
                    &vsm::g_clipmapDepthBiasDecay, 0.25f, 1.0f, "%.2f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("bias(L) = max(depthBias * decay^L, floor). 1.0 = legacy constant-in-texels;\n"
                                      "0.5 = constant WORLD-size bias (the near value everywhere). Lets the near bias\n"
                                      "rise against acne without peter-panning the far levels.");
                graphicsEdit(ImGui::SliderFloat("Depth bias floor (texels)",
                    &vsm::g_clipmapDepthBiasFloorTexels, 0.0f, 1.5f, "%.2f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Lower bound of the decayed bias, in texels of the level actually sampled.\n"
                                      "The D32 pool has effectively no quantization floor, so 0 is legal -- the\n"
                                      "receiver-plane bias carries the slope; raise this only if residual acne shows.");
                ImGui::EndDisabled(); // constant depth bias group (inert under SMRT)

                // NOT in the disabled group: the normal offset is applied on BOTH paths (SMRT keeps
                // it -- it is already UE's formula and it is what stops the ray starting inside the
                // receiver).
                graphicsEdit(ImGui::SliderFloat("Clipmap normal bias (UE units)",
                    &vsm::g_clipmapNormalBias, 0.0f, 4.0f, "%.3f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Receiver offset along the normal, UE's r.Shadow.Virtual.NormalBias units\n"
                                      "(their default 0.5): scaled by distance-to-camera and the FOV, /1000 on the CPU.\n"
                                      "Values well above 0.5 are masking a caster/receiver geometry mismatch\n"
                                      "(terrain shadow LOD) -- see docs/terrain_shadow_chunking_plan.md.");

                graphicsEdit(ImGui::SliderFloat("Local lateral bias (texels)",
                    &vsm::g_localLateralTexels, 0.0f, 4.0f, "%.2f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Spot + point VSM: surface-normal offset in shadow texels. ~1 texel.\n"
                                      "Higher = less acne but the shadow Peter-pans (lifts off the base).");
                graphicsEdit(ImGui::SliderFloat("Local depth push (texels)",
                    &vsm::g_localDepthPushTexels, 0.0f, 4.0f, "%.2f"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Spot + point VSM: along-the-light-ray depth push in shadow texels,\n"
                                      "slope-scaled by 1/N.L. The main acne knob; barely Peter-pans (depth-only).");

                graphicsEdit(ImGui::Checkbox("Resident-only render (faster, may flicker)",
                                              &vsm::g_residentIterOnly));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("ON: render only pages a 3-frame-old snapshot says are resident (fewer CPU\n"
                                      "draws), but shadows blink for ~3 frames when the set changes (motion/churn).\n"
                                      "OFF: render the whole pool every frame (correct, ~4x the render CPU).\n"
                                      "Ignored while 'Single-draw page render' is on (that path skips nothing).");

                graphicsEdit(ImGui::Checkbox("Single-draw page render (dormant)",
                                              &vsm::g_pageDrawSingle));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("ON: one ExecuteIndirect over every (page, group) arg instead of the 1024-page\n"
                                      "CPU loop; the per-page viewport becomes a VS clip remap + SV_ClipDistance page\n"
                                      "borders. Removes the resident-snapshot blink at no CPU cost. Needs the mega\n"
                                      "buffer. OFF: the per-page loop (A/B, and per-page inspection in PIX).\n"
                                      "Measured: CPU 0.182 -> 0.058 ms, GPU neutral, frame time unchanged\n"
                                      "(the pass records on a worker, so the CPU saving is off the hot path).");

                // Barrier plan step 7 prereq: this one is BOOT-ONLY (`--vsm-page-compact`).
                // It is the only config flag whose two paths rest a buffer in states that cannot
                // be combined — PageArgCount is UAV when off and INDIRECT_ARGUMENT when on, and
                // UAV is exclusive. Toggling it live would invalidate the canonical table the
                // barrier compile seeds from. The other page-draw flag needs no such treatment:
                // its two states are both reads and are declared as a union.
                ImGui::BeginDisabled();
                ImGui::Checkbox("Compact draw args (boot-only: --vsm-page-compact)", &vsm::g_pageDrawCompact);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("ON: the setup CS appends only NON-EMPTY (page, group) records and the draw\n"
                                      "uses that counter as its count buffer, instead of walking all 1024 x groups\n"
                                      "fixed-layout records. Measured ~+0.017 ms GPU here: the walk it removes was\n"
                                      "already free, the atomic it adds lands in the setup CS. Kept for group-heavy\n"
                                      "scenes (records = pages x groups). No effect while single-draw is off.");

                graphicsEdit(ImGui::Checkbox("Page cache (experimental)", &vsm::g_pageCaching));
                {
                    int windLvl = static_cast<int>(vsm::g_windAnimateMaxLevel);
                    ImGui::SetNextItemWidth(150.0f);
                    if (graphicsEdit(ImGui::SliderInt("Wind animate below level", &windLvl, 0,
                                         static_cast<int>(vsm::kNumClipmapLevels))))
                    {
                        vsm::g_windAnimateMaxLevel = static_cast<std::uint32_t>(windLvl);
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Clipmap levels BELOW this sway in the shadow map and re-render every\n"
                                          "frame while wind blows; levels at/above draw their casters RIGID, so\n"
                                          "the page cache can keep them (a cached page and a fresh render agree\n"
                                          "exactly). Level extent doubles per step from the clipmap base extent,\n"
                                          "so 2 = sway within ~2x base extent of the camera. Locals always sway.\n"
                                          "Max = everything sways (the old behavior; cache gains nothing in wind).");
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Skips re-rendering pages whose content did not change. Two rules make the\n"
                                      "cached content valid: (1) wind past 'wind animate below level' renders\n"
                                      "RIGID, so a swaying grove caches its far levels (-0.33 ms Pass_VsmPageRender\n"
                                      "in the wind_test grove, neutral on demo.json); (2) any view whose MATRIX\n"
                                      "changed re-renders wholesale - a page id has no scroll offset, so a moved\n"
                                      "clipmap/sun/spot would otherwise serve depth for the wrong world rect.\n"
                                      "Net effect: parked camera + settled lights cache fully; motion redraws\n"
                                      "exactly what it invalidates (the cost of the uncached path, no worse).");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("ON: re-render only new / dynamic-overlapping pages; cached pages keep their\n"
                                      "depth. Measured no avg gain + worse spikes on this scene (cull-bound + dynamic\n"
                                      "teapots + gated clear slower than the hardware clear). Kept for a future coarse-skip.");

                ImGui::Checkbox("Log page stats to debug output", &vsm::g_logPageStats);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Periodically print \"[VSM] request ... | resident=...\" to DBWIN. Off by default\n"
                                      "(spammy); the live numbers above/below always update regardless.");

                ImGui::EndDisabled();
                ImGui::Separator();
                ImGui::TextDisabled("Pool: %u pages (%ux%u). refDist=%.1f downscale=%u lru=%u",
                    vsm::kPoolPageCount, vsm::kPoolTexels, vsm::kPoolTexels,
                    vsm::g_refDist, vsm::g_requestDownscale, vsm::g_lruThreshold);

                // --- Live page stats + physical-pool residency grid (kFrameCount-old snapshot). ---
                ImGui::Separator();
                ImGui::TextUnformatted("Pages");
                const VirtualShadowMap::DebugStats& vstats = scene.Vsm().Stats();
                if (!render::VsmActive())
                {
                    ImGui::TextDisabled("(enable VSM to sample live page stats)");
                }
                else if (!vstats.valid)
                {
                    ImGui::TextDisabled("(sampling\xE2\x80\xA6 move the camera to populate)");
                }
                else
                {
                    const float frac = static_cast<float>(vstats.resident) /
                                       static_cast<float>(vsm::kPoolPageCount);
                    char resLabel[64];
                    std::snprintf(resLabel, sizeof(resLabel), "%u / %u resident (%.0f%%)",
                        vstats.resident, vsm::kPoolPageCount, frac * 100.0f);
                    ImGui::ProgressBar(frac, ImVec2(240.0f, 0.0f), resLabel);
                    ImGui::Text("Requested %u  |  new this frame %u", vstats.requested, vstats.newAlloc);
                    if (vstats.fail > 0)
                        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.30f, 1.0f),
                            "Alloc fails: %u (pool oversubscribed \xE2\x80\x94 raise refDist/downscale or lower LOD)",
                            vstats.fail);
                    else
                        ImGui::TextDisabled("Alloc fails: 0");
                    ImGui::Text("Request LOD  L0=%u  L1=%u  L2=%u  L3=%u  L4=%u",
                        vstats.perLevel[0], vstats.perLevel[1], vstats.perLevel[2],
                        vstats.perLevel[3], vstats.perLevel[4]);

                    const std::vector<std::uint32_t>& owners = scene.Vsm().PhysOwnerSnapshot();
                    if (owners.size() == vsm::kPoolPageCount)
                    {
                        ImGui::Spacing();
                        ImGui::TextDisabled("Physical pool %ux%u pages \xE2\x80\x94 colour = owning shadow view:",
                            vsm::kPoolPagesPerAxis, vsm::kPoolPagesPerAxis);
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        const ImVec2 origin = ImGui::GetCursorScreenPos();
                        const int   axis = static_cast<int>(vsm::kPoolPagesPerAxis);
                        const float cell = 9.0f, gap = 1.0f, pitch = cell + gap;
                        for (int py = 0; py < axis; ++py)
                        {
                            for (int px = 0; px < axis; ++px)
                            {
                                const std::uint32_t owner = owners[static_cast<size_t>(py) * axis + px];
                                ImU32 col;
                                if (owner == 0xFFFFFFFFu)
                                {
                                    col = IM_COL32(38, 38, 44, 255); // free
                                }
                                else
                                {
                                    const std::uint32_t view = owner / vsm::kPagesPerView;
                                    const bool clip = view >= vsm::kNumLocalVirtualViews; // directional clipmap
                                    const float hue = static_cast<float>(view % vsm::kMaxVirtualViews) /
                                                      static_cast<float>(vsm::kMaxVirtualViews);
                                    col = ImColor::HSV(hue, clip ? 0.85f : 0.55f, clip ? 1.0f : 0.85f);
                                }
                                const ImVec2 a(origin.x + px * pitch, origin.y + py * pitch);
                                dl->AddRectFilled(a, ImVec2(a.x + cell, a.y + cell), col, 1.5f);
                            }
                        }
                        const float gridPx = axis * pitch;
                        ImGui::Dummy(ImVec2(gridPx, gridPx));
                        if (ImGui::IsItemHovered())
                        {
                            const ImVec2 m = ImGui::GetIO().MousePos;
                            const int cx = static_cast<int>((m.x - origin.x) / pitch);
                            const int cy = static_cast<int>((m.y - origin.y) / pitch);
                            if (cx >= 0 && cx < axis && cy >= 0 && cy < axis)
                            {
                                const std::uint32_t owner = owners[static_cast<size_t>(cy) * axis + cx];
                                if (owner == 0xFFFFFFFFu)
                                    ImGui::SetTooltip("phys #%d (%d,%d): free", cy * axis + cx, cx, cy);
                                else
                                {
                                    const std::uint32_t view = owner / vsm::kPagesPerView;
                                    const char* kind = view < 8 ? "spot"
                                        : (view < vsm::kNumLocalVirtualViews ? "point face" : "directional clipmap");
                                    ImGui::SetTooltip("phys #%d (%d,%d)\nview %u (%s)",
                                        cy * axis + cx, cx, cy, view, kind);
                                }
                            }
                        }
                    }
                }
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Bindings"))
            {
                const auto& descs = input.GetBindingDescs();
                if (descs.empty())
                {
                    ImGui::TextDisabled("No bindings loaded.");
                }
                else
                {
                    const ImGuiTableFlags tableFlags =
                        ImGuiTableFlags_Borders |
                        ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_Resizable |
                        ImGuiTableFlags_ScrollY;
                    const float tableHeight = ImGui::GetContentRegionAvail().y;
                    if (ImGui::BeginTable("BindingsTable", 2, tableFlags, ImVec2(0.0f, tableHeight)))
                    {
                        ImGui::TableSetupColumn("Binding", ImGuiTableColumnFlags_WidthFixed, 180.0f);
                        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableHeadersRow();

                        for (const auto& desc : descs)
                        {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextUnformatted(desc.keys.c_str());
                            ImGui::TableSetColumnIndex(1);
                            ImGui::TextUnformatted(desc.action.c_str());
                        }

                        ImGui::EndTable();
                    }
                }

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    open_ = open;

    textureDebugViewer_.Draw(renderer, GetOceanShoreDepthResource());
    oceanControlsWindow_.Draw(renderer);
    logWindow_.Draw();
    DrawTraceControls();
    return graphicsSettingsDirty;
}
