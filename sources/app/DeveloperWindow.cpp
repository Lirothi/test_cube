#include "app/DeveloperWindow.h"

#include <string>
#include <string_view>

#include "app/Systems.h"
#include "app/levels/DemoLevel.h"
#include "app/levels/LevelManager.h"
#include "app/scene/Scene.h"
#include "core/profiling/ProfilerScopes.h"
#include "editor/EditorController.h"
#include "imgui.h"
#include "input/InputManager.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/RenderStats.h"
#include "rendering/meshes/LodSelect.h"
#include "rendering/renderables/InstanceTypes.h"
#include "ocean/OceanSimulation.h"
#include "text/TextManager.h"

namespace
{
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

void DeveloperWindow::ToggleTextureInspector()
{
    textureDebugViewer_.SetOpen(!textureDebugViewer_.IsOpen());
}

void DeveloperWindow::Draw(Renderer& renderer, const Scene& scene, const InputManager& input, LevelManager& levelManager, SceneRenderSettings& settings, EditorController& editorController)
{
    CPU_SCOPE(ProfilerScopes::kBuildDeveloperWindow);

    if (!open_)
    {
        textureDebugViewer_.Draw(renderer, GetOceanShoreDepthResource());
        oceanControlsWindow_.Draw(renderer);
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
                ImGui::Text("Draw calls: %u   Primitives: %.2fM",
                    render::g_renderStats.lastDrawCalls,
                    static_cast<double>(render::g_renderStats.lastPrimitives) / 1.0e6);

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
                // this same reflection resolution and follow the reflection source below (RT/SSR/Off).
                // Inspect them via Texture inspector [F4] -> "Glass Refl Normal/Depth" + "Glass Reflection".
                ImGui::TextDisabled("Glass reflections share this target (active in SSR/RT; off in Off).");

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

                // Reflection source (S8): Off / SSR / RT. RT is greyed out on
                // non-RT hardware (and the renderer falls back to SSR anyway).
                const bool rtSupported = renderer.IsRaytracingSupported();
                const char* srcLabels[] = { "Off", "SSR", "RT" };
                int curSrc = static_cast<int>(settings.reflectionSource);
                if (curSrc < 0 || curSrc >= 3) { curSrc = 1; }
                if (ImGui::BeginCombo("Reflections [F5]", srcLabels[curSrc]))
                {
                    for (int i = 0; i < 3; ++i)
                    {
                        const bool isRT = (i == 2);
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
                ImGui::BeginDisabled(!levelManager.HasLevel(DemoLevel::kName));
                if (ImGui::Button("Reload Demo Level"))
                {
                    LevelLoadOptions reloadOptions;
                    reloadOptions.preserveCameraTransform = true;
                    levelManager.RequestLevelChange(std::string(DemoLevel::kName), reloadOptions);
                }
                ImGui::EndDisabled();

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
                bool levelEditorOpen = editorController.IsOpen();
                if (ImGui::Checkbox("Level Editor", &levelEditorOpen))
                {
                    editorController.SetOpen(levelEditorOpen);
                }
                ImGui::Checkbox("Fullscreen debug texture", &settings.debugTexMode);
                ImGui::Checkbox("Profiler overlay", &settings.showProfiler);
                ImGui::Checkbox("GPU instancing", &render::g_instancingEnabled);

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
}
