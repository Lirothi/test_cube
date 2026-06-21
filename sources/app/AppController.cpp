#include "app/AppController.h"

#include "app/scene/Scene.h"
#include "imgui.h"
#include "input/InputManager.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/RenderStats.h"
#include "rendering/renderables/InstanceTypes.h"
#include "rendering/meshes/LodSelect.h"
#include "text/TextManager.h"
#include "core/math/Math.h"
#include "core/profiling/ProfilerScopes.h"

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
}

void AppController::Tick(InputManager& input, Renderer& renderer, Scene& scene, float deltaTime)
{
    CPU_SCOPE(ProfilerScopes::kAppControllerTick);

    const bool uiCapturingMouse = renderer.ImGuiWantsMouse();
    const bool uiCapturingKeyboard = renderer.ImGuiWantsKeyboard();
    const bool uiCapturingInput = uiCapturingMouse || uiCapturingKeyboard;
    const bool toggleDeveloperWindow =
        input.WasActionPressed("ToggleDeveloperPanel") ||
        input.WasActionPressed("ToggleBindings") ||
        ImGui::IsKeyPressed(ImGuiKey_F1, false);

    if (toggleDeveloperWindow)
    {
        showDeveloperWindow_ = !showDeveloperWindow_;
    }

    if (!uiCapturingKeyboard)
    {
        if (input.WasActionPressed("DebugTex"))
        {
            settings_.debugTexMode = !settings_.debugTexMode;
        }
        if (input.WasActionPressed("ToggleProfiler"))
        {
            settings_.showProfiler = !settings_.showProfiler;
        }
        if (input.WasActionPressed("Wireframe"))
        {
            renderer.SetWireframeMode(!renderer.GetWireframeMode());
        }
        if (input.WasActionPressed("ToggleInstancing"))
        {
            render::g_instancingEnabled = !render::g_instancingEnabled; // F2: A/B Step 4 auto-instancing
        }
        if (input.WasActionPressed("ToggleLOD"))
        {
            render::g_lodEnabled = !render::g_lodEnabled; // F10: A/B Step 6 mesh LOD
        }

        const auto setDlssMode = [&renderer](sl::DLSSMode mode)
        {
            renderer.SetDlssMode(mode);
        };

        if (input.WasActionPressed("SetDlssQualityOff"))
        {
            setDlssMode(sl::DLSSMode::eOff);
        }
        if (input.WasActionPressed("SetDlssQualityMaxPerformance"))
        {
            setDlssMode(sl::DLSSMode::eMaxPerformance);
        }
        if (input.WasActionPressed("SetDlssQualityBalanced"))
        {
            setDlssMode(sl::DLSSMode::eBalanced);
        }
        if (input.WasActionPressed("SetDlssQualityMaxQuality"))
        {
            setDlssMode(sl::DLSSMode::eMaxQuality);
        }
        if (input.WasActionPressed("SetDlssQualityUltraPerformance"))
        {
            setDlssMode(sl::DLSSMode::eUltraPerformance);
        }
        if (input.WasActionPressed("SetDlssQualityUltraQuality"))
        {
            setDlssMode(sl::DLSSMode::eUltraQuality);
        }
        if (input.WasActionPressed("SetDlssQualityDLAA"))
        {
            setDlssMode(sl::DLSSMode::eDLAA);
        }
    }

    BuildDeveloperWindow(renderer, scene, input);
    scene.SetRenderSettings(settings_);

    // Camera input runs here (before Scene::Tick) so Scene itself never touches input.
    if (!uiCapturingInput)
    {
        scene.CameraRef().UpdateFromInput(input, deltaTime);
    }
    else if (input.IsMouseCaptured())
    {
        input.SetMouseCapture(false);
    }

    ScheduleHudBuild(renderer, scene);
}

void AppController::BuildHud(Renderer& renderer, const Scene& scene) const
{
    auto* tb = renderer.GetTextManager();
    tb->Begin(renderer.GetWidth(), renderer.GetHeight(), 1.0f);
    const float fps = renderer.GetFPS();
    const float frameMs = fps > 0.0f ? 1000.0f / fps : 0.0f;
    tb->AddTextfShadow(8, 8, 32.0f, float4(1, 1, 1, 0.6f), true, L"FPS:%.0f MS:%0.2f Scale:%0.2f", fps, frameMs, renderer.GetRenderResolutionScale());
    const Camera& camera = scene.CameraRef();
    const auto& camPos = camera.GetPosition();
    tb->AddTextfShadow(8, 8 + 32, 16.0f, float4(1, 1, 1, 0.9f), true, L"Cam: %0.2f %0.2f %0.2f, speed: %0.2f, DLSS: %i, SSR: %i, FXAA: %i", camPos.x, camPos.y, camPos.z, camera.GetMoveSpeedMult(),
        renderer.IsDlssActive() ? (int)renderer.GetDlssMode() : -1, (int)settings_.ssrTechnique, (int)settings_.doFxaa);

    tb->AddTextfShadow(8, 8 + 32 + 32, 32.0f, float4(1, 1, 1, 0.9f), true, L"A quick brown fox 1234567890 ABCDEEFG");
    tb->AddTextfShadow(8, 8 + 32 + 32 + 32, 64.0f, float4(1, 1, 1, 0.9f), true, L"A quick brown fox 1234567890 ABCDEEFG");
    tb->AddTextfShadow(8, 8 + 32 + 32 + 32 + 64, 64.0f + 32.0f, float4(1, 1, 1, 0.9f), true, L"A quick brown fox 1234567890 ABCDEEFG");
}

void AppController::BuildDeveloperWindow(Renderer& renderer, const Scene& scene, const InputManager& input)
{
    CPU_SCOPE(ProfilerScopes::kBuildDeveloperWindow);

    if (!showDeveloperWindow_)
    {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(700.0f, 800.0f), ImGuiCond_FirstUseEver);

    bool open = showDeveloperWindow_;
    if (ImGui::Begin("Developer Controls [F1]###DeveloperControls", &open, ImGuiWindowFlags_NoCollapse))
    {
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

                ImGui::Checkbox("FXAA", &settings_.doFxaa);

                int ssrTechnique = static_cast<int>(settings_.ssrTechnique);
                const int ssrTechniqueCount = static_cast<int>(SsrTechnique::Count);
                if (ssrTechnique < 0 || ssrTechnique >= ssrTechniqueCount)
                {
                    ssrTechnique = 0;
                }
                if (ImGui::Combo("SSR technique", &ssrTechnique, kSsrTechniqueLabels, ssrTechniqueCount))
                {
                    settings_.ssrTechnique = static_cast<SsrTechnique>(ssrTechnique);
                }

                float ssrScale = renderer.GetSsrTextureScale().x;
                if (ImGui::SliderFloat("SSR resolution", &ssrScale, 0.25f, 1.0f, "%.2f"))
                {
                    renderer.SetSsrTextureScale(ssrScale);
                }
                ImGui::Text("SSR target: %ux%u", renderer.GetSsrTextureWidth(), renderer.GetSsrTextureHeight());

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
                bool wireframe = renderer.GetWireframeMode();
                if (ImGui::Checkbox("Wireframe", &wireframe))
                {
                    renderer.SetWireframeMode(wireframe);
                }
                ImGui::Checkbox("Debug texture", &settings_.debugTexMode);
                ImGui::Checkbox("Profiler overlay", &settings_.showProfiler);
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
                if (ImGui::TreeNodeEx("TextManager stats", ImGuiTreeNodeFlags_DefaultOpen))
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
    showDeveloperWindow_ = open;
}

void AppController::WaitForHudBuild()
{
    if (!hudBuildTask_)
    {
        return;
    }

    TaskSystem::Get().Wait(hudBuildTask_);
    TaskSystem::Get().Release(hudBuildTask_);
}

void AppController::ScheduleHudBuild(Renderer& renderer, const Scene& scene)
{
    WaitForHudBuild();
    hudBuildTask_ = TaskSystem::Get().Submit([this, &renderer, &scene]()
    {
        BuildHud(renderer, scene);
    });
}
