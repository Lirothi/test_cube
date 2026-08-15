#include "app/AppController.h"

#include "app/App.h" // g_hudHidden (--no-hud)
#include "app/levels/LevelManager.h"
#include "app/scene/Scene.h"
#include "core/math/Math.h"
#include "core/profiling/ProfilerScopes.h"
#include "imgui.h"
#include "input/InputManager.h"
#include "rendering/core/Renderer.h"
#include "rendering/meshes/LodSelect.h"
#include "rendering/renderables/InstanceTypes.h"
#include "text/TextManager.h"

void AppController::Tick(InputManager& input, Renderer& renderer, Scene& scene, LevelManager& levelManager, float deltaTime)
{
    CPU_SCOPE(ProfilerScopes::kAppControllerTick);

    const bool uiCapturingMouse = renderer.ImGuiWantsMouse();
    const bool uiCapturingKeyboard = renderer.ImGuiWantsKeyboard();
    const bool uiCapturingInput = uiCapturingMouse || uiCapturingKeyboard;
    const bool toggleDeveloperWindow =
        input.WasActionPressed("ToggleDeveloperPanel") ||
        input.WasActionPressed("ToggleBindings") ||
        ImGui::IsKeyPressed(ImGuiKey_F1, false);
#if WITH_EDITOR
    const bool toggleLevelEditor = ImGui::IsKeyPressed(ImGuiKey_F2, false);
#endif
    const bool toggleTextureInspector =
        input.WasActionPressed("ToggleTextureInspector") ||
        ImGui::IsKeyPressed(ImGuiKey_F4, false);
    const bool toggleOceanControls =
        input.WasActionPressed("ToggleOceanControls") ||
        ImGui::IsKeyPressed(ImGuiKey_F7, false);

    if (toggleDeveloperWindow)
    {
        developerWindow_.ToggleOpen();
    }
#if WITH_EDITOR
    if (toggleLevelEditor)
    {
        editorController_.ToggleOpen();
    }
#endif
    if (toggleTextureInspector)
    {
        developerWindow_.ToggleTextureInspector();
    }
    if (toggleOceanControls)
    {
        developerWindow_.ToggleOceanControls();
    }

    if (!uiCapturingKeyboard)
    {
        if (input.WasActionPressed("ToggleProfiler"))
        {
            settings_.showProfiler = !settings_.showProfiler;
        }
        if (input.WasActionPressed("Wireframe"))
        {
            renderer.SetWireframeMode(!renderer.GetWireframeMode());
        }
#if WITH_EDITOR
        if (ImGui::IsKeyPressed(ImGuiKey_F12, false)) // editor build relocates instancing to F12 (F2 = Level Editor)
        {
            render::g_instancingEnabled = !render::g_instancingEnabled;
        }
#else
        if (input.WasActionPressed("ToggleInstancing"))
        {
            render::g_instancingEnabled = !render::g_instancingEnabled; // F2: A/B Step 4 auto-instancing
        }
#endif
        if (input.WasActionPressed("ToggleLOD"))
        {
            render::g_lodEnabled = !render::g_lodEnabled; // F10: A/B Step 6 mesh LOD
        }
        if (input.WasActionPressed("ToggleIndirectShadows"))
        {
            // Ctrl+I: A/B Rung-0 GPU-driven indirect shadow submission vs the CPU path.
            render::g_indirectShadowsEnabled = !render::g_indirectShadowsEnabled;
        }
        if (input.WasActionPressed("ToggleGiIndirectShadows"))
        {
            // Ctrl+G: A/B folding GPU-instanced casters into the indirect/VSM shadow path (ON) vs
            // the Legacy CPU RenderShadow tail only (OFF — nothing in VSM, today's behavior).
            render::g_giIndirectShadowsEnabled = !render::g_giIndirectShadowsEnabled;
        }
        if (input.WasActionPressed("ToggleVsmPageRequest"))
        {
            // Ctrl+V: switch the active shadow method Legacy(CSM/atlas) <-> VSM (Step 24a). Drives
            // both which sampler the light/glass shaders use and whether the VSM pipeline runs.
            render::g_shadowMode = render::VsmActive() ? render::ShadowMode::Legacy
                                                       : render::ShadowMode::VSM;
        }
        if (input.WasActionPressed("CycleReflectionSource"))
        {
            // F5: cycle None -> SkyOnly -> SSR -> RT -> None (skip RT on
            // non-RT hardware).
            const bool rt = renderer.IsRaytracingSupported();
            switch (settings_.reflectionSource)
            {
            case ReflectionSource::None:    settings_.reflectionSource = ReflectionSource::SkyOnly; break;
            case ReflectionSource::SkyOnly: settings_.reflectionSource = ReflectionSource::SSR; break;
            case ReflectionSource::SSR:     settings_.reflectionSource = rt ? ReflectionSource::RT : ReflectionSource::None; break;
            default:                        settings_.reflectionSource = ReflectionSource::None; break;
            }
        }
        if (input.WasActionPressed("ToggleRTDebugView"))
        {
            settings_.rtDebugView = !settings_.rtDebugView; // F6: S6 RT hit/visibility debug viz -> Reflection target
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

    developerWindow_.Draw(renderer, scene, input, levelManager, settings_
#if WITH_EDITOR
        , editorController_
#endif
    );
    scene.SetRenderSettings(settings_);

#if WITH_EDITOR
    editorController_.Draw(renderer, scene, levelManager);
    if (editorController_.ConsumeOpenOceanPresetEditorRequest())
    {
        developerWindow_.OpenOceanControls();
    }
#endif

    // Camera input runs here (before Scene::Tick) so Scene itself never touches input.
    bool allowCameraInput = !uiCapturingInput;
#if WITH_EDITOR
    if (allowCameraInput && editorController_.IsOpen())
    {
        allowCameraInput = input.IsActionDown("LookToggle") && !input.IsKeyDown(VK_MENU);
    }
#endif
    if (allowCameraInput)
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
    // "--no-hud" (see App.h): Begin() still runs, so the buffer is emptied rather than left holding
    // the previous frame's text; only the emission below is skipped.
    if (g_hudHidden)
    {
        return;
    }
    const float fps = renderer.GetFPS();
    const float frameMs = fps > 0.0f ? 1000.0f / fps : 0.0f;
    tb->AddTextfShadow(8, 8, 32.0f, float4(1, 1, 1, 0.6f), true, L"FPS:%.0f MS:%0.2f Scale:%0.2f", fps, frameMs, renderer.GetRenderResolutionScale());
    const Camera& camera = scene.CameraRef();
    const auto& camPos = camera.GetPosition();
    // Orientation as a QUATERNION next to the position: the pair is everything needed to reproduce
    // the exact shot from the command line (a level's freeCameraStart, or --cam-pos/--cam-rot), so
    // a screenshot carries its own camera. Degrees would need the reader to know this camera's
    // rotation order and sign conventions; a quaternion does not.
    const mat4 camRot = camera.GetRotationMatrix(); // same builder the view matrix uses (roll included)
    const DirectX::XMFLOAT4 camRotF = [&]
    {
        DirectX::XMFLOAT4 q{};
        DirectX::XMStoreFloat4(&q, DirectX::XMQuaternionRotationMatrix(DirectX::XMLoadFloat4x4(&camRot.m)));
        return q;
    }();
    tb->AddTextfShadow(8, 8 + 32, 16.0f, float4(1, 1, 1, 0.9f), true,
        L"Cam: %0.2f %0.2f %0.2f, rot: %0.4f %0.4f %0.4f %0.4f, speed: %0.2f, DLSS: %i, SSR: %i, FXAA: %i",
        camPos.x, camPos.y, camPos.z, camRotF.x, camRotF.y, camRotF.z, camRotF.w, camera.GetMoveSpeedMult(),
        renderer.IsDlssActive() ? (int)renderer.GetDlssMode() : -1, (int)settings_.ssrTechnique, (int)settings_.doFxaa);

    //tb->AddTextfShadow(8, 8 + 32 + 32, 32.0f, float4(1, 1, 1, 0.99f), true, L"A quick brown fox 1234567890 ABCDEEFG");
    //tb->AddTextfShadow(8, 8 + 32 + 32 + 32, 64.0f, float4(1, 1, 1, 0.75f), true, L"A quick brown fox 1234567890 ABCDEEFG");
    //tb->AddTextfShadow(8, 8 + 32 + 32 + 32 + 64, 64.0f + 32.0f, float4(1, 1, 1, 0.5f), true, L"A quick brown fox 1234567890 ABCDEEFG");
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

#if WITH_EDITOR
void AppController::OnLevelChangeRequestCompleted(const LevelChangeRequest& request,
    bool loaded,
    Renderer& renderer,
    Scene& scene,
    LevelManager& levelManager)
{
    editorController_.OnLevelChangeRequestCompleted(request, loaded, renderer, scene, levelManager);
}
#endif

void AppController::ScheduleHudBuild(Renderer& renderer, const Scene& scene)
{
    WaitForHudBuild();
    hudBuildTask_ = TaskSystem::Get().Submit([this, &renderer, &scene]()
    {
        BuildHud(renderer, scene);
    });
}
