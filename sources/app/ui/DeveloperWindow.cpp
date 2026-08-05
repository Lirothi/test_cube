#include "app/ui/DeveloperWindow.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>
#include <string>
#include <string_view>
#include <system_error>

#include "app/Systems.h"
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
#include "rendering/meshes/LodSelect.h"
#include "rendering/renderables/InstanceTypes.h"
#include "rendering/shadows/VirtualShadowMap.h"
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
        "Lettier",
        "Log March",
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

void DeveloperWindow::Draw(Renderer& renderer, Scene& scene, const InputManager& input, LevelManager& levelManager, SceneRenderSettings& settings
#if WITH_EDITOR
    , EditorController& editorController
#endif
)
{
    CPU_SCOPE(ProfilerScopes::kBuildDeveloperWindow);

    if (!open_)
    {
        textureDebugViewer_.Draw(renderer, GetOceanShoreDepthResource());
        oceanControlsWindow_.Draw(renderer);
        DrawTraceControls();
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(700.0f, 800.0f), ImGuiCond_FirstUseEver);

    bool open = open_;
    const ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoCollapse |
        (windowMaximize_.maximized ? ImGuiWindowFlags_NoMove : 0);
    if (ImGui::Begin("Developer Controls [F1]###DeveloperControls", &open, windowFlags))
    {
        ui::HandleWindowTitleDoubleClickMaximize(windowMaximize_);

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

                ImGui::Checkbox("Trace capture window", &traceWindowOpen_);

                ImGui::Separator();

                const bool dlssAvailable = renderer.IsDlssAvailable();
                ImGui::Text("DLSS status: %s", renderer.IsDlssActive() ? "Active" : (dlssAvailable ? "Inactive" : "Unavailable"));

                ImGui::BeginDisabled(!dlssAvailable);
                bool dlssEnabled = renderer.IsDlssActive();
                if (ImGui::Checkbox("DLSS enabled", &dlssEnabled))
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
                        if (ImGui::Selectable(option.label, selected))
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

                ImGui::Checkbox("FXAA", &settings.doFxaa);

                int ssrTechnique = static_cast<int>(settings.ssrTechnique);
                const int ssrTechniqueCount = static_cast<int>(SsrTechnique::Count);
                if (ssrTechnique < 0 || ssrTechnique >= ssrTechniqueCount)
                {
                    ssrTechnique = 0;
                }
                if (ImGui::Combo("SSR technique", &ssrTechnique, kSsrTechniqueLabels, ssrTechniqueCount))
                {
                    settings.ssrTechnique = static_cast<SsrTechnique>(ssrTechnique);
                }

                float ssrScale = renderer.GetReflectionTextureScale().x;
                if (ImGui::SliderFloat("Reflection resolution", &ssrScale, 0.25f, 1.0f, "%.2f"))
                {
                    renderer.SetReflectionTextureScale(ssrScale);
                }
                ImGui::Text("Reflection target: %ux%u", renderer.GetReflectionTextureWidth(), renderer.GetReflectionTextureHeight());
                // S15b: glass off-screen reflections render into a glass G-buffer + glassReflection at
                // this same reflection resolution and follow the reflection source below.
                // Inspect them via Texture inspector [F4] -> "Glass Refl Normal/Depth" + "Glass Reflection".
                ImGui::TextDisabled("Glass: traced in SSR/RT, cubemap in Sky only, disabled in None.");

                // S16: glossy reflections — blur radius scales with surface roughness (0 = sharp mirror).
                ImGui::SliderFloat("Glossy blur", &settings.reflectionGlossyScale, 0.0f, 24.0f, "%.1f");

                // Analytic sun specular boost on metals: spec lobe *= (1 + metal*coef). 0 = physical.
                ImGui::SliderFloat("Sun spec on metal", &settings.sunMetalSpecInfluence, 0.0f, 16.0f, "%.1f");

                // Sun angular size: floors the analytic specular lobe width so smooth surfaces show
                // a bright, sample-able sun glint instead of a sub-pixel spike. 0 = punctual.
                ImGui::SliderFloat("Sun angular size", &settings.sunAngularSize, 0.0f, 0.25f, "%.3f");

                float oceanReflectionScale = renderer.GetOceanReflectionTextureScale().x;
                if (ImGui::SliderFloat("Ocean reflection resolution", &oceanReflectionScale, 0.25f, 1.0f, "%.2f"))
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
                        if (ImGui::Selectable(srcLabels[i], curSrc == i))
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
                if (ImGui::SliderFloat("Render scale", &renderScale, 0.1f, 1.0f, "%.2f"))
                {
                    renderer.SetRenderResolutionScale(renderScale);
                }
                if (ImGui::Button("Native render scale"))
                {
                    renderer.SetRenderResolutionScale(1.0f);
                }
                ImGui::EndDisabled();
                if (dlssControlsRenderScale)
                {
                    ImGui::TextDisabled("DLSS quality controls the render scale while active.");
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
                if (ImGui::Checkbox("Texture inspector [F4]", &textureViewerOpen))
                {
                    textureDebugViewer_.SetOpen(textureViewerOpen);
                }
                bool oceanControlsOpen = oceanControlsWindow_.IsOpen();
                if (ImGui::Checkbox("Ocean controls [F7]", &oceanControlsOpen))
                {
                    oceanControlsWindow_.SetOpen(oceanControlsOpen);
                }
#if WITH_EDITOR
                bool levelEditorOpen = editorController.IsOpen();
                if (ImGui::Checkbox("Level Editor [F2]", &levelEditorOpen))
                {
                    editorController.SetOpen(levelEditorOpen);
                }
#endif
                ImGui::Checkbox("Fullscreen debug texture", &settings.debugTexMode);
                ImGui::Checkbox("Profiler overlay", &settings.showProfiler);
#if WITH_EDITOR
                ImGui::Checkbox("GPU instancing [F12]", &render::g_instancingEnabled);
#else
                ImGui::Checkbox("GPU instancing", &render::g_instancingEnabled);
#endif

                ImGui::Checkbox("Mesh LOD [F10]", &render::g_lodEnabled);
                ImGui::BeginDisabled(!render::g_lodEnabled);
                // -1 = automatic (screen-size / cascade); 0..3 force that level on every mesh.
                static const char* kForcedLodLabels[] = { "Auto", "0 (full)", "1", "2", "3" };
                int forcedLodCombo = render::g_forcedLod + 1; // map -1..3 -> 0..4
                if (ImGui::Combo("Force LOD level", &forcedLodCombo, kForcedLodLabels, IM_ARRAYSIZE(kForcedLodLabels)))
                {
                    render::g_forcedLod = forcedLodCombo - 1;
                }
                ImGui::EndDisabled();

                ImGui::Separator();

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
                if (ImGui::Checkbox("Legacy CSM active [Ctrl+V]", &legacyMode))
                    render::g_shadowMode = legacyMode ? render::ShadowMode::Legacy : render::ShadowMode::VSM;
                if (!legacyMode)
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
                        "VSM drives directional shadows right now. The cascades below are still\n"
                        "computed every frame, but nothing samples them \xE2\x80\x94 the readout is live,\n"
                        "the picture is not. Uncheck the box above before judging any change.");

                CascadeShadowConfig& csmCfg = scene.CascadeConfig();

                ImGui::SeparatorText("Coverage");
                ImGui::SliderFloat("Max distance (m)", &csmCfg.maxDistance, 20.0f, 1000.0f, "%.0f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Far edge of cascade 3 \xE2\x80\x94 the hard shadow terminator.\n"
                                      "Shrinking it is the cheapest way to buy texel density everywhere\n"
                                      "(no extra memory, no extra rasterization).");
                ImGui::DragFloat4("Split distances (m)", csmCfg.sliceDistances.data(), 0.25f, 0.5f, 1000.0f, "%.1f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Far plane of cascades 0..3 in view space. BuildSplitScheme clamps them\n"
                                      "monotonic and caps them at max distance, so out-of-order values are safe.");

                ImGui::SeparatorText("Fit");
                ImGui::SliderFloat("Overlap (texels)", &csmCfg.overlapInTexels, 0.0f, 8.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Padding added to the fitted sphere radius to absorb the texel-snap shift,\n"
                                      "in CASCADE TEXELS. The snap moves the centre by at most one texel per axis,\n"
                                      "so 2 already leaves a full texel of slack, and the same number is correct\n"
                                      "for every cascade. At 2 the measured worst-case slack is 1.2 texels; 1.0 is\n"
                                      "still positive but thin, and by 0.5 the ortho no longer covers the snapped\n"
                                      "slice \xE2\x80\x94 the UpdateCascades assert fires in Debug, shadows clip at the\n"
                                      "cascade edge in Release. Watch 'R fit/pad': the two should nearly coincide.");
                ImGui::SliderFloat("Z padding (m)", &csmCfg.zPadding, 0.0f, 100.0f, "%.1f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Slack added past the far side of the light-space depth range.");
                ImGui::SliderFloat("Caster reach (m)", &csmCfg.casterReachWS, 0.0f, 400.0f, "%.0f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How far TOWARD the light the ortho near plane is pulled back so casters\n"
                                      "between the sun and the slice still render. This is what inflates zRange\n"
                                      "(and the D16 step) in the readout. Drop it to 0 and watch tall casters'\n"
                                      "shadows get clipped \xE2\x80\x94 that is the problem pancaking exists to solve.");

                ImGui::SeparatorText("Bias");
                ImGui::SliderFloat("Normal bias (texels)", &csmCfg.normalBiasInTexels, 0.0f, 4.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Receiver offset along its normal, in cascade texels (so it scales with\n"
                                      "each cascade's world texel size). Raise to kill acne, at the cost of\n"
                                      "detaching contact shadows.");
                ImGui::SliderFloat("Depth bias (texels)", &csmCfg.depthBiasInTexels, 0.0f, 8.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Sample-time depth offset, in cascade texels. The readout converts it to\n"
                                      "millimetres of peter-panning \xE2\x80\x94 that number is the one to minimise.");

                if (ImGui::Button("Reset CSM config to defaults"))
                    csmCfg = CascadeShadowConfig{};
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Back to the compile-time defaults in SceneRenderConfig.h \xE2\x80\x94 the\n"
                                      "baseline every plan step is measured against.");

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
                if (ImGui::BeginTable("CsmReadout", 8, csmTableFlags))
                {
                    ImGui::TableSetupColumn("c");
                    ImGui::TableSetupColumn("slice (m)");
                    ImGui::TableSetupColumn("tile");
                    ImGui::TableSetupColumn("texel (mm)");
                    ImGui::TableSetupColumn("R fit/pad (m)");
                    ImGui::TableSetupColumn("zRange (m)");
                    ImGui::TableSetupColumn("D16 (mm)");
                    ImGui::TableSetupColumn("bias (mm)");
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
                    }
                    ImGui::EndTable();
                }
                ImGui::TextDisabled("texel = world mm per shadow texel (lower is sharper).  R fit/pad = sphere\n"
                                    "radius before/after overlap.  D16 = quantization step of the 16-bit depth\n"
                                    "atlas over zRange.  bias = depth bias in world mm (= peter-panning).");

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("VSM"))
            {
                ImGui::TextWrapped("Virtual Shadow Maps (Rung 2, experimental) for local lights "
                    "(spot + point + glass). Legacy = CSM directional + spot/point atlas. Tunes "
                    "quality vs. cost live (no reallocation). Directional still uses CSM until Step 24.");
                ImGui::Separator();
                bool vsmMode = render::VsmActive();
                if (ImGui::Checkbox("VSM shadows enabled [Ctrl+V]", &vsmMode))
                    render::g_shadowMode = vsmMode ? render::ShadowMode::VSM : render::ShadowMode::Legacy;

                // Applies to BOTH modes (folds GPU-instanced casters into the indirect cull): ON =
                // GI casts in VSM + via indirect in Legacy; OFF = GI reverts to the Legacy CPU tail.
                ImGui::Checkbox("GPU-instanced casters -> indirect/VSM [Ctrl+G]", &render::g_giIndirectShadowsEnabled);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("ON: GPU-instanced objects cast shadows in VSM (and via the indirect path in\n"
                                      "Legacy), dropping their CPU RenderShadow tail. OFF: Legacy CPU tail only (no VSM).");

                // Shadow LOD bias applies to BOTH Legacy cascades and VSM (it shifts the per-view caster
                // LOD the shadow passes rasterize), so it lives OUTSIDE the VSM-only disabled block below.
                // A change triggers a GPU-idle caster rebuild (Scene::ReconcileShadowLodBias) next frame.
                ImGui::SliderInt("Shadow LOD bias", &render::g_shadowLodBias, -2, 3);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("ADDITIVE offset on the per-view shadow LOD. Each shadow view (CSM cascade /\n"
                                      "VSM clipmap level / local light) already picks a base LOD by its tier (near =\n"
                                      "fine, far = coarse); this shifts the whole curve. 0 = the tier curve alone;\n"
                                      "+ = coarser everywhere (cheaper), - = sharper. Shadows don't resolve fine\n"
                                      "geometry, so coarser is usually invisible. Changing it rebuilds casters (a hitch).");

                ImGui::BeginDisabled(!render::VsmActive());

                ImGui::SliderFloat("LOD ref distance", &vsm::g_refDist, 1.0f, 40.0f, "%.1f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Smaller = coarser pages: fewer/faster + more stable, softer near.\n"
                                      "Larger = sharper near but more resident pages + higher render cost.");

                int ds = static_cast<int>(vsm::g_requestDownscale);
                if (ImGui::SliderInt("Request downscale", &ds, 1, 8))
                    vsm::g_requestDownscale = static_cast<std::uint32_t>(ds < 1 ? 1 : ds);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Screen sub-sampling for page discovery.\n"
                                      "1 = full res (best coverage, costliest); higher = cheaper, may miss pages.");

                int lru = static_cast<int>(vsm::g_lruThreshold);
                if (ImGui::SliderInt("LRU eviction frames", &lru, 1, 120))
                    vsm::g_lruThreshold = static_cast<std::uint32_t>(lru < 1 ? 1 : lru);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Frames a resident page survives unrequested before it is freed.");

                ImGui::SliderFloat("Clipmap base extent", &vsm::g_clipmapBaseExtent, 4.0f, 200.0f, "%.1f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Step 24f directional clipmap: finest level's world extent (level i = base*2^i).\n"
                                      "Smaller = sharper near shadows but less far coverage; larger = the reverse.");
                ImGui::SliderFloat("Clipmap depth bias", &vsm::g_clipmapDepthBias, 0.0f, 0.01f, "%.4f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Directional clipmap NDC depth bias. Raise to kill shadow acne; too high = peter-panning.\n"
                                      "Now uniform across levels (per-level depth range), so one value works everywhere.");
                ImGui::SliderFloat("Clipmap normal bias (texels)", &vsm::g_clipmapNormalBias, 0.0f, 8.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Receiver normal offset in texels, scaled per level by world-units-per-texel.\n"
                                      "Raise if far terrain self-shadows (the 'darkened area' when flying away).");

                ImGui::SliderFloat("Local lateral bias (texels)", &vsm::g_localLateralTexels, 0.0f, 4.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Spot + point VSM: surface-normal offset in shadow texels. ~1 texel.\n"
                                      "Higher = less acne but the shadow Peter-pans (lifts off the base).");
                ImGui::SliderFloat("Local depth push (texels)", &vsm::g_localDepthPushTexels, 0.0f, 4.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Spot + point VSM: along-the-light-ray depth push in shadow texels,\n"
                                      "slope-scaled by 1/N.L. The main acne knob; barely Peter-pans (depth-only).");

                ImGui::Checkbox("Resident-only render (faster, may flicker)", &vsm::g_residentIterOnly);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("ON: render only pages a 3-frame-old snapshot says are resident (fewer CPU\n"
                                      "draws), but shadows blink for ~3 frames when the set changes (motion/churn).\n"
                                      "OFF: render the whole pool every frame (correct, ~4x the render CPU).\n"
                                      "Ignored while 'Single-draw page render' is on (that path skips nothing).");

                ImGui::Checkbox("Single-draw page render (dormant)", &vsm::g_pageDrawSingle);
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

                ImGui::Checkbox("Page cache (experimental, off = net loss here)", &vsm::g_pageCaching);
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
    DrawTraceControls();
}
