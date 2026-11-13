#include "app/AppController.h"
#include "core/memory/MemScope.h"

#include "app/App.h" // g_hudHidden (--no-hud)
#include "app/levels/LevelManager.h"
#include "app/scene/Scene.h"
#include "core/logging/Log.h"
#include "core/math/Math.h"
#include "core/profiling/ProfilerScopes.h"
#include "imgui.h"
#include "input/InputManager.h"
#include "rendering/core/Renderer.h"
#include "rendering/meshes/LodSelect.h"
#include "rendering/renderables/InstanceTypes.h"
#include "rendering/shadows/ShadowSettings.h"
#include "text/TextManager.h"
#include "core/threading/ThreadRole.h" // G6: PublishHud is the hand-over's
#include "core/vfs/Vfs.h" // Shipping's menu lists the packaged levels

#include <algorithm>
#include <cstdarg>
#include <cwchar>

void AppController::InitializeGraphicsSettings(Renderer& renderer, Scene& scene)
{
    graphicsSettings_.Initialize(renderer, scene, settings_);
}

void AppController::ApplySceneGraphicsSettings(Scene& scene) const
{
    graphicsSettings_.ApplySceneSettings(scene);
}

void AppController::FlushGraphicsSettings(Renderer& renderer, const Scene& scene)
{
    graphicsSettings_.Flush(renderer, scene, settings_);
}

void AppController::ReleaseGpuResources(Renderer& renderer)
{
    developerWindow_.ReleaseGpuResources();
#if WITH_EDITOR
    editorController_.ReleaseGpuResources(renderer);
#else
    (void)renderer;
#endif
}

void AppController::Tick(InputManager& input, Renderer& renderer, Scene& scene, LevelManager& levelManager, float deltaTime)
{
    CPU_SCOPE(ProfilerScopes::kAppControllerTick);

    const bool uiCapturingMouse = renderer.ImGuiWantsMouse();
    const bool uiCapturingKeyboard = renderer.ImGuiWantsKeyboard();
    const bool uiCapturingInput = uiCapturingMouse || uiCapturingKeyboard;
    // A shipped build has no developer keys (docs/distribution_build_plan.md §7): no Developer Controls (F1), texture
    // inspector (F4), ocean window (F7), profiler overlay (F9), A/B toggles (F2-F10, Ctrl+I/G/V), DLSS modes (Ctrl+1..7).
#if TC_SHIPPING
    constexpr bool kDeveloperKeys = false;
#else
    constexpr bool kDeveloperKeys = true;
#endif
    const bool toggleDeveloperWindow = kDeveloperKeys &&
        (input.WasActionPressed("ToggleDeveloperPanel") ||
        input.WasActionPressed("ToggleBindings") ||
        ImGui::IsKeyPressed(ImGuiKey_F1, false)) &&
        !(playSession_.Playing() && input.IsKeyDown(VK_SHIFT)); // Shift+F1 is the mouse's in Play
#if WITH_EDITOR
    const bool toggleLevelEditor = ImGui::IsKeyPressed(ImGuiKey_F2, false);
#endif
    const bool toggleTextureInspector = kDeveloperKeys &&
        (input.WasActionPressed("ToggleTextureInspector") ||
        ImGui::IsKeyPressed(ImGuiKey_F4, false));
    const bool toggleOceanControls = kDeveloperKeys &&
        (input.WasActionPressed("ToggleOceanControls") ||
        ImGui::IsKeyPressed(ImGuiKey_F7, false));

    if (toggleDeveloperWindow)
    {
        developerWindow_.ToggleOpen();
    }
#if WITH_EDITOR
    if (toggleLevelEditor && !playSession_.Playing()) // in Play it stops the session (TickPlay), and that opens the editor
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
    if (g_bootLogWindow)
    {
        g_bootLogWindow = false; // once: "--log-window" opens the viewer for a headless capture
        developerWindow_.ToggleLogWindow();
    }
    if (g_bootMemoryWindow)
    {
        g_bootMemoryWindow = false; // once: "--mem-window" opens the Memory tab for a headless capture
        developerWindow_.OpenMemoryTab();
    }
    if (!g_bootDevTab.empty())
    {
        if (!developerWindow_.OpenTab(g_bootDevTab)) // once, like --mem-window
        {
            LOG_WARNING(logging::LogCategory::App, "--dev-tab={}: no Developer Controls tab has that label "
                        "(Frame, AA / Scale, Visibility, Reflections, GI, Fog, Sky, Debug, LOD, CSM, Contact, VSM, "
                        "Streaming, Memory, Bindings)", g_bootDevTab);
        }
        g_bootDevTab.clear();
    }
#if WITH_EDITOR
    if (g_bootEditor)
    {
        g_bootEditor = false; // once: "--editor" opens the editor for a headless capture
        editorController_.SetOpen(true);
    }
#endif

    // The function-key toggles never type text, so only a text field being typed in holds them back. Gated on
    // WantCaptureKeyboard -- true whenever an ImGui window has the focus, i.e. nearly always with the editor's panels
    // open -- F9 (the profiler overlay), F3, F5, F6, F10 and F12 did nothing in the editor (2026-11-03, owner: "F9 does
    // not work for the overlay"). The Ctrl combinations below still yield to any keyboard focus (Ctrl+V pastes).
    if (kDeveloperKeys && !renderer.ImGuiWantsTextInput())
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
            render::g_instancingEnabled_game = !render::g_instancingEnabled_game;
        }
#else
        if (input.WasActionPressed("ToggleInstancing"))
        {
            render::g_instancingEnabled_game = !render::g_instancingEnabled_game; // F2: A/B Step 4 auto-instancing
        }
#endif
        if (input.WasActionPressed("ToggleLOD"))
        {
            render::g_lodEnabled_game = !render::g_lodEnabled_game; // F10: A/B Step 6 mesh LOD
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
    }

    if (kDeveloperKeys && !uiCapturingKeyboard)
    {
        if (input.WasActionPressed("ToggleIndirectShadows"))
        {
            // Ctrl+I: A/B Rung-0 GPU-driven indirect shadow submission vs the CPU path.
            render::g_indirectShadowsEnabled_game = !render::g_indirectShadowsEnabled_game;
        }
        if (input.WasActionPressed("ToggleGiIndirectShadows"))
        {
            // Ctrl+G: A/B folding GPU-instanced casters into the indirect/VSM shadow path (ON) vs
            // the Legacy CPU RenderShadow tail only (OFF — nothing in VSM, today's behavior).
            render::g_giIndirectShadowsEnabled_game = !render::g_giIndirectShadowsEnabled_game;
        }
        if (input.WasActionPressed("ToggleVsmPageRequest"))
        {
            // Ctrl+V: CYCLE the active shadow method Legacy(CSM/atlas) -> VSM -> SDSM -> Legacy
            // (Step 24a; S15 added the third). Drives which sampler the light/glass shaders use,
            // whether the VSM pipeline runs, and whether the cascades are fitted on the CPU
            // (Legacy) or reduced from this frame's depth on the GPU (SDSM).
            switch (render::g_shadowMode_game)
            {
            case render::ShadowMode::Legacy: render::g_shadowMode_game = render::ShadowMode::VSM;    break;
            case render::ShadowMode::VSM:    render::g_shadowMode_game = render::ShadowMode::Sdsm;   break;
            default:                         render::g_shadowMode_game = render::ShadowMode::Legacy; break;
            }
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

    const bool graphicsSettingsDirty = developerWindow_.Draw(
        renderer, scene, input, levelManager, settings_, graphicsSettings_
#if WITH_EDITOR
        , editorController_
#endif
    );
    graphicsSettings_.ObserveUiEdit(renderer, scene, settings_, graphicsSettingsDirty, deltaTime);
    scene.SetRenderSettings(settings_);

#if WITH_EDITOR
    {
        MEM_SCOPE(MemTag::Editor); // memory profiling M3: the editor's panels, document and tasks
        editorController_.Draw(
            renderer, scene, levelManager, developerWindow_.IsLogWindowOpen());
    }
    if (editorController_.ConsumeOpenOceanPresetEditorRequest())
    {
        developerWindow_.OpenOceanControls();
    }
    if (editorController_.ConsumeToggleLogWindowRequest())
    {
        developerWindow_.ToggleLogWindow();
    }
#endif

#if WITH_EDITOR
    TickPlay(input, renderer, scene, levelManager, toggleLevelEditor);
#else
    TickPlay(input, renderer, scene, levelManager, false);
    DrawGameMenu(levelManager);
#endif
    // A race's results (game::Race): a window of this frame's, the mouse and Enter its own while it is up.
    if (playSession_.Playing())
    {
        scene.RaceRef().DrawResults();
        scene.RaceRef().DrawLabels(scene.CameraRef()); // on the grid: who is who, a bot's level over its boat
        scene.RaceRef().DrawBotCards(scene.CameraRef()); // bot.debug: what each bot decides and why
        playSession_.DrawBoatCard(scene); // on foot beside a boat: what it does
        playSession_.DrawSpectatorPanel(scene); // the camera on a bot's boat: which boat, its view, the bots' vision
    }
    const bool raceResults = playSession_.Playing() && scene.RaceRef().ResultsOpen();

    // No window of ours on screen, and ImGui still holds the keyboard: a panel that closed leaves its focus behind (F2, Play), and App::WndProc gives every letter to whoever holds it.
    // A held right button clears it for the free camera (ImGuiLayer), which is why it went unseen; Play has no right button to hold, and WASD never reached the player.
    {
        ImGuiIO& io = ImGui::GetIO();
#if WITH_EDITOR
        const bool panelsOpen = editorController_.IsOpen() || developerWindow_.AnyWindowOpen() || raceResults;
#else
        const bool panelsOpen = developerWindow_.AnyWindowOpen() || raceResults || gameMenuOpen_;
#endif
        if (!panelsOpen && io.WantCaptureKeyboard)
        {
            ImGui::SetWindowFocus(nullptr);
            io.WantCaptureKeyboard = false;
        }
    }

    // Camera input runs here (before Scene::Tick) so Scene itself never touches input.
    if (playSession_.Playing())
    {
        playSession_.Tick(input, scene, deltaTime, developerWindow_.AnyWindowOpen() || raceResults || gameMenuOpen_, uiCapturingMouse);
        if (DebugDrawSystem* draw = renderer.GetDebugDrawSystem()) { scene.RaceRef().DebugDraw(*draw); } // bot.debug
    }
    else
    {
        bool allowCameraInput = !uiCapturingInput && !gameMenuOpen_;
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
    }

    ScheduleHudBuild(renderer, scene);
}

void AppController::TickPlay(InputManager& input, Renderer& renderer, Scene& scene, LevelManager& levelManager, bool editorKeyPressed)
{
    const bool typing = renderer.ImGuiWantsTextInput();
    // Through ImGui as well: a panel with the keyboard's focus keeps a letter from InputManager (App::WndProc).
    const bool escape = !typing && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || input.WasActionPressed("StopPlay"));
    const bool eject = !typing && input.WasActionPressed("PossessEject");
    const bool freeMouse = !typing && input.WasActionPressed("FreeMouse");
#if WITH_EDITOR
    // The editor's world stands: only a session runs it. The panels go with the start and come back with the stop.
    using PlayRequest = EditorController::PlayRequest;
    PlayRequest request = editorController_.ConsumePlayRequest();
    if (!typing && ((ImGui::IsKeyPressed(ImGuiKey_P, false) && ImGui::GetIO().KeyAlt) || input.WasActionPressed("Play")))
    {
        request = PlayRequest{ PlayRequest::Kind::Play, false, true };
    }
    const bool fromBoot = g_bootPlay;
    if (fromBoot)
    {
        g_bootPlay = false;
        request = PlayRequest{ PlayRequest::Kind::Play, g_camOverride, false }; // --cam-pos names where to start; a script does not take the owner's mouse
    }
    if (!playSession_.Playing())
    {
        if (request.kind == PlayRequest::Kind::Play)
        {
            game::PlayerStart start;
            const bool hasStart = !request.fromCamera && editorController_.FindPlayerStart(start);
            game::RaceCourse course;
            editorController_.FindRaceCourse(course);
            course.level = std::string(scene.LevelRaceCourse().level); // the document has no file of its own: the loaded level's
            editorOpenBeforePlay_ = editorController_.IsOpen();
            editorController_.SetOpen(false);
            playSession_.Begin(renderer, scene, input, hasStart ? &start : nullptr, &course, request.takeMouse, !fromBoot);
        }
    }
    else if (request.kind == PlayRequest::Kind::Stop || escape || editorKeyPressed || editorController_.IsOpen()) // open by F2 or Developer Controls' checkbox: opening it stops the session
    {
        const bool editorAsked = editorKeyPressed || editorController_.IsOpen();
        playSession_.End(renderer, scene, input);
        editorController_.SetOpen(editorOpenBeforePlay_ || editorAsked);
    }
    else
    {
        if (eject) { playSession_.TogglePossess(scene); }
        if (freeMouse) { playSession_.ToggleFreeMouse(); }
        if (!typing && input.WasActionPressed("KeepSimulation"))
        {
            playSession_.SetStatus(editorController_.KeepSimulation(renderer, scene, levelManager), ImGui::GetTime() + 4.0);
        }
    }
    editorController_.SetPlaying(playSession_.Playing());
    physics::GameSettings().simulate = playSession_.Playing();
#else
    // The game: the world always runs, and there is no Stop. The camera is free until --play or F8: every shot and measurement of a Release build stands on that.
    (void)levelManager;
    (void)editorKeyPressed;
    physics::GameSettings().simulate = true;
    if (!playSession_.Playing())
    {
        if (g_bootPlay || eject)
        {
            const std::optional<game::PlayerStart>& start = scene.LevelPlayerStart();
            const bool fromCamera = g_bootPlay && g_camOverride;
            // Shipping's level rule: a level without a Player Start keeps the free camera (F8 still walks from it).
            const bool freeByRule = g_bootPlay && !eject && g_playByLevelRule && !start;
            if (!freeByRule)
            {
                playSession_.Begin(renderer, scene, input, (start && !fromCamera) ? &*start : nullptr, &scene.LevelRaceCourse(), eject, false);
            }
            g_bootPlay = false;
        }
    }
    else
    {
        if (eject) { playSession_.TogglePossess(scene); }
#if TC_SHIPPING
        if (freeMouse) { playSession_.ToggleFreeMouse(); }
#else
        if (escape || freeMouse) { playSession_.ToggleFreeMouse(); }
#endif
    }
#if TC_SHIPPING
    // Esc is the game's menu here, on foot, at a helm or with the camera free; the mouse is the menu's while it is up and
    // the game's again when Esc or Continue closes it.
    if (escape)
    {
        gameMenuOpen_ = !gameMenuOpen_;
        if (playSession_.Playing()) { playSession_.SetMouseFree(gameMenuOpen_); }
    }
    else if (gameMenuOpen_ && playSession_.Playing())
    {
        playSession_.SetMouseFree(true);
    }
#endif
#endif
    hudPlayLine_ = playSession_.HudLine(scene, ImGui::GetTime());
#if TC_SHIPPING
    if (!playSession_.Playing()) { hudPlayLine_ = L"FREE CAMERA: right mouse look, WASD, wheel speed   F8 walk from here   Esc menu"; }
#endif
    hudRaceLine_ = playSession_.RaceHudLine(scene);
    hudBigText_ = playSession_.Playing() ? scene.RaceRef().BigText() : std::wstring{};
}

void AppController::DrawGameMenu(LevelManager& levelManager)
{
#if TC_SHIPPING
    // The levels are the packages' (the cook puts in only the profile's): data/levels/*.json, read once.
    if (!gameMenuOpen_) { return; }
    if (gameMenuLevels_.empty())
    {
        vfs::List("data/levels", "*.json", [this](const vfs::ListEntry& entry)
        {
            if (!entry.directory) { gameMenuLevels_.push_back("data/levels/" + entry.name); }
        });
        std::sort(gameMenuLevels_.begin(), gameMenuLevels_.end());
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::Begin("Menu", nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
    const ImVec2 button(320.0f, 0.0f);
    if (ImGui::Button("Continue", button))
    {
        gameMenuOpen_ = false;
        if (playSession_.Playing()) { playSession_.SetMouseFree(false); }
    }
    ImGui::SeparatorText("Levels");
    std::string current(levelManager.GetActiveLevelSourcePath());
    std::replace(current.begin(), current.end(), '\\', '/');
    for (const std::string& level : gameMenuLevels_)
    {
        const std::size_t slash = level.find_last_of('/');
        std::string name = level.substr(slash + 1, level.size() - slash - 1 - 5); // without the folder and ".json"
        if (current == level || current.ends_with("/" + level)) { name += "  (here: start again)"; }
        ImGui::PushID(level.c_str());
        if (ImGui::Button(name.c_str(), button))
        {
            LOG_INFO(logging::LogCategory::App, "menu: level {}", level);
            levelManager.RequestLevelPathChange(level);
            gameMenuOpen_ = false;
        }
        ImGui::PopID();
    }
    if (gameMenuLevels_.empty()) { ImGui::TextDisabled("no levels in the packages"); }
    ImGui::Separator();
    if (ImGui::Button("Quit", button))
    {
        PostMessageW(static_cast<HWND>(viewport->PlatformHandleRaw), WM_CLOSE, 0, 0); // the window's own close: settings and log flushed as by its [x]
    }
    ImGui::End();
#else
    (void)levelManager;
#endif
}

void AppController::OnLevelLoaded(Renderer& renderer, Scene& scene, InputManager& input)
{
#if WITH_EDITOR
    game::PlayerStart start;
    const bool hasStart = editorController_.FindPlayerStart(start);
    game::RaceCourse course;
    editorController_.FindRaceCourse(course);
    course.level = std::string(scene.LevelRaceCourse().level);
    if (!playSession_.OnLevelLoaded(renderer, scene, input, hasStart ? &start : nullptr, &course))
    {
        editorController_.SetOpen(editorOpenBeforePlay_);
        editorController_.SetPlaying(false);
    }
#else
    const std::optional<game::PlayerStart>& start = scene.LevelPlayerStart();
    if (g_playByLevelRule)
    {
        // Shipping (owner, 2026-11-13): each level by its own rule -- its Player Start: first person from it; none: the
        // free camera where the level put it (its freeCameraStart, else the origin -- JsonLevel::Load).
        if (!start)
        {
            playSession_.StopForLevel(scene, input);
            LOG_INFO(logging::LogCategory::App, "level rule: no Player Start -- the free camera");
            return;
        }
        if (!playSession_.Playing())
        {
            playSession_.Begin(renderer, scene, input, &*start, &scene.LevelRaceCourse(), true, false);
            return;
        }
    }
    playSession_.OnLevelLoaded(renderer, scene, input, start ? &*start : nullptr, &scene.LevelRaceCourse());
#endif
}

void AppController::BuildHud(Renderer& renderer, const Scene& scene)
{
    // Game thread plan G6: the task only FORMATS the lines into the game half's list; PublishHud writes them into the
    // render half's text buffer at the hand-over (the profiler overlay and the LOD view add to it on the render side).
    hudLines_.clear();
    const auto line = [this](int x, int y, float px, const float4& color, const wchar_t* fmt, ...)
    {
        wchar_t buffer[512];
        va_list args;
        va_start(args, fmt);
        std::vswprintf(buffer, sizeof(buffer) / sizeof(buffer[0]), fmt, args);
        va_end(args);
        hudLines_.push_back(HudLine{ x, y, px, color, buffer });
    };
    // "--no-hud" (see App.h): PublishHud still begins the buffer, so it is emptied rather than left holding the
    // previous frame's text; only the lines are skipped.
    if (g_hudHidden)
    {
        return;
    }
    const float fps = renderer.GetFPS();
    const float frameMs = fps > 0.0f ? 1000.0f / fps : 0.0f;
    line(8, 8, 32.0f, float4(1, 1, 1, 0.6f), L"FPS:%.0f MS:%0.2f Scale:%0.2f", fps, frameMs, renderer.GetRenderResolutionScale());
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
    line(8, 8 + 32, 16.0f, float4(1, 1, 1, 0.9f),
        L"Cam: %0.2f %0.2f %0.2f, rot: %0.4f %0.4f %0.4f %0.4f, speed: %0.2f, DLSS: %i, SSR: %i, Shadows: %hs",
        camPos.x, camPos.y, camPos.z, camRotF.x, camRotF.y, camRotF.z, camRotF.w, camera.GetMoveSpeedMult(),
        renderer.IsDlssActive() ? (int)renderer.GetDlssMode() : -1, (int)settings_.ssrTechnique,
        render::ShadowModeLabel(render::g_shadowMode_game));
    // The level's clock, so a screenshot also carries its hour (`--time=HH:MM` reproduces it).
    if (hudTimeOfDayHours_ >= 0.0f)
    {
        const int minutes = static_cast<int>(hudTimeOfDayHours_ * 60.0f) % (24 * 60);
        if (g_timeScale != 1.0f)
        {
            line(8, 8 + 32 + 18, 16.0f, float4(1, 1, 1, 0.9f), L"Time: %02d:%02d   WORLD x%.3g (frame.timeScale)", minutes / 60, minutes % 60, g_timeScale);
        }
        else { line(8, 8 + 32 + 18, 16.0f, float4(1, 1, 1, 0.9f), L"Time: %02d:%02d", minutes / 60, minutes % 60); }
    }
    if (!hudPlayLine_.empty())
    {
        line(8, 8 + 32 + 18 + 18, 16.0f, float4(0.6f, 1, 0.6f, 1), L"%ls", hudPlayLine_.c_str());
    }
    if (!hudRaceLine_.empty())
    {
        line(8, 8 + 32 + 18 + 18 + 18, 16.0f, float4(1.0f, 0.9f, 0.5f, 1), L"%ls", hudRaceLine_.c_str());
    }
    if (!hudBigText_.empty())
    {
        // The race's countdown: in the middle, a third of the way down (a glyph is some 0.55 of its height wide).
        const float px = 160.0f;
        const int x = static_cast<int>(renderer.GetWidth() * 0.5f - 0.28f * px * static_cast<float>(hudBigText_.size()));
        line(x, static_cast<int>(renderer.GetHeight() * 0.25f), px, float4(1.0f, 0.85f, 0.2f, 1), L"%ls", hudBigText_.c_str());
    }

    //tb->AddTextfShadow(8, 8 + 32 + 32, 32.0f, float4(1, 1, 1, 0.99f), true, L"A quick brown fox 1234567890 ABCDEEFG");
    //tb->AddTextfShadow(8, 8 + 32 + 32 + 32, 64.0f, float4(1, 1, 1, 0.75f), true, L"A quick brown fox 1234567890 ABCDEEFG");
    //tb->AddTextfShadow(8, 8 + 32 + 32 + 32 + 64, 64.0f + 32.0f, float4(1, 1, 1, 0.5f), true, L"A quick brown fox 1234567890 ABCDEEFG");
}

void AppController::PublishHud(Renderer& renderer)
{
    TC_CHECK_SYNC("AppController::PublishHud");
    auto* tb = renderer.GetTextManager();
    tb->Begin(renderer.GetWidth(), renderer.GetHeight(), 1.0f);
    for (const HudLine& hud : hudLines_)
    {
        tb->AddText(hud.x, hud.y, hud.px, hud.color, std::wstring_view(hud.text), true);
    }
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
    hudTimeOfDayHours_ = scene.GetTimeOfDay().Present() ? scene.GetTimeOfDay().Settings().timeHours : -1.0f;
    hudBuildTask_ = TaskSystem::Get().Submit([this, &renderer, &scene]()
    {
        BuildHud(renderer, scene);
    });
}
