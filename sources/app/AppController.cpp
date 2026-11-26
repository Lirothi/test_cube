#include "app/AppController.h"
#include "core/memory/MemScope.h"

#include "app/App.h" // Boot().frame.hudHidden (--no-hud)
#include "app/BuildVersion.h" // the Development build's mark
#include "app/levels/LevelManager.h"
#include "app/scene/Scene.h"
#include "app/settings/Scalability.h" // the forced auto-detect holds the game's input back
#include "core/logging/Log.h"
#include "core/math/Math.h"
#include "core/profiling/ProfilerScopes.h"
#include "imgui.h"
#include "input/InputManager.h"
#include "input/InputActions.h"
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
#include "rendering/debug/DebugDraw.h"
#include "ui/ImGuiRenderer.h" // R4.3: renderer.ImGui()

void AppController::InitializeGraphicsSettings(Renderer& renderer, Scene& scene)
{
    graphicsSettings_.Initialize(renderer, scene, settings_);
    // The settings menu's Apply keeps the frame pacing and the mouse smoothing where they live: the graphics section.
    settingsMenu_.SetSaveGraphics([this, &renderer, &scene]() { return graphicsSettings_.SaveCurrent(renderer, scene, settings_); });
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

    const bool uiCapturingMouse = renderer.ImGui().WantsMouse();
    const bool uiCapturingKeyboard = renderer.ImGui().WantsKeyboard();
    const bool uiCapturingInput = uiCapturingMouse || uiCapturingKeyboard;
    // A shipped build has no developer keys (docs/distribution_build_plan.md §7): no Developer Controls (F1), texture
    // inspector (F4), ocean window (F7), profiler overlay (F9), A/B toggles (F2-F10, Ctrl+I/G/V), DLSS modes (Ctrl+1..7).
#if TC_SHIPPING
    constexpr bool kDeveloperKeys = false;
#else
    constexpr bool kDeveloperKeys = true;
#endif
    const bool toggleDeveloperWindow = kDeveloperKeys &&
        (input.WasActionPressed(actions::kToggleDeveloperPanel) ||
        ImGui::IsKeyPressed(ImGuiKey_F1, false)) &&
        !(playSession_.Playing() && input.IsKeyDown(VK_SHIFT)); // Shift+F1 is the mouse's in Play
#if WITH_EDITOR
    const bool toggleLevelEditor = ImGui::IsKeyPressed(ImGuiKey_F2, false);
#endif
    const bool toggleTextureInspector = kDeveloperKeys &&
        (input.WasActionPressed(actions::kToggleTextureInspector) ||
        ImGui::IsKeyPressed(ImGuiKey_F4, false));
    const bool toggleOceanControls = kDeveloperKeys &&
        (input.WasActionPressed(actions::kToggleOceanControls) ||
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
    if (Boot().editor.logWindow)
    {
        Boot().editor.logWindow = false; // once: "--log-window" opens the viewer for a headless capture
        developerWindow_.ToggleLogWindow();
    }
    if (Boot().editor.memoryWindow)
    {
        Boot().editor.memoryWindow = false; // once: "--mem-window" opens the Memory tab for a headless capture
        developerWindow_.OpenMemoryTab();
    }
    if (!Boot().editor.devTab.empty())
    {
        if (!developerWindow_.OpenTab(Boot().editor.devTab)) // once, like --mem-window
        {
            LOG_WARNING(logging::LogCategory::App, "--dev-tab={}: no Developer Controls tab has that label "
                        "(Scalability, Frame, AA / Scale, Visibility, Reflections, GI, Fog, Sky, Debug, LOD, CSM, Contact, VSM, "
                        "Tessy, Physics, Streaming, Memory, Bindings)", Boot().editor.devTab);
        }
        Boot().editor.devTab.clear();
    }
    if (!Boot().editor.settingsMenu.empty())
    {
        // once: "--settings-menu[=<tab>|menu]" for a headless capture
        if (Boot().editor.settingsMenu == "menu") { gameMenuOpen_ = true; }
        else { settingsMenu_.OpenTab(Boot().editor.settingsMenu); }
        Boot().editor.settingsMenu.clear();
    }
#if WITH_EDITOR
    if (Boot().editor.openEditor)
    {
        Boot().editor.openEditor = false; // once: "--editor" opens the editor for a headless capture
        editorController_.SetOpen(true);
    }
#endif

    // The function-key toggles never type text, so only a text field being typed in holds them back. Gated on
    // WantCaptureKeyboard -- true whenever an ImGui window has the focus, i.e. nearly always with the editor's panels
    // open -- F9 (the profiler overlay), F3, F5, F6, F10 and F12 did nothing in the editor (2026-11-03, owner: "F9 does
    // not work for the overlay"). The Ctrl combinations below still yield to any keyboard focus (Ctrl+V pastes).
    if (kDeveloperKeys && !renderer.ImGui().WantsTextInput())
    {
        if (input.WasActionPressed(actions::kToggleProfiler))
        {
            settings_.showProfiler = !settings_.showProfiler;
        }
        if (input.WasActionPressed(actions::kWireframe))
        {
            renderer.SetWireframeMode(!renderer.GetWireframeMode());
        }
#if WITH_EDITOR
        if (ImGui::IsKeyPressed(ImGuiKey_F12, false)) // editor build relocates instancing to F12 (F2 = Level Editor)
        {
            render::g_instancingEnabled_game = !render::g_instancingEnabled_game;
        }
#else
        if (input.WasActionPressed(actions::kToggleInstancing))
        {
            render::g_instancingEnabled_game = !render::g_instancingEnabled_game; // F2: A/B Step 4 auto-instancing
        }
#endif
        if (input.WasActionPressed(actions::kToggleLOD))
        {
            render::g_lodEnabled_game = !render::g_lodEnabled_game; // F10: A/B Step 6 mesh LOD
        }
        if (input.WasActionPressed(actions::kCycleReflectionSource))
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
        if (input.WasActionPressed(actions::kToggleRTDebugView))
        {
            settings_.rtDebugView = !settings_.rtDebugView; // F6: S6 RT hit/visibility debug viz -> Reflection target
        }
    }

    if (kDeveloperKeys && !uiCapturingKeyboard)
    {
        if (input.WasActionPressed(actions::kToggleIndirectShadows))
        {
            // Ctrl+I: A/B Rung-0 GPU-driven indirect shadow submission vs the CPU path.
            render::g_indirectShadowsEnabled_game = !render::g_indirectShadowsEnabled_game;
        }
        if (input.WasActionPressed(actions::kToggleGiIndirectShadows))
        {
            // Ctrl+G: A/B folding GPU-instanced casters into the indirect/VSM shadow path (ON) vs
            // the Legacy CPU RenderShadow tail only (OFF — nothing in VSM, today's behavior).
            render::g_giIndirectShadowsEnabled_game = !render::g_giIndirectShadowsEnabled_game;
        }
        if (input.WasActionPressed(actions::kToggleVsmPageRequest))
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

        if (input.WasActionPressed(actions::kSetDlssQualityOff))
        {
            setDlssMode(sl::DLSSMode::eOff);
        }
        if (input.WasActionPressed(actions::kSetDlssQualityMaxPerformance))
        {
            setDlssMode(sl::DLSSMode::eMaxPerformance);
        }
        if (input.WasActionPressed(actions::kSetDlssQualityBalanced))
        {
            setDlssMode(sl::DLSSMode::eBalanced);
        }
        if (input.WasActionPressed(actions::kSetDlssQualityMaxQuality))
        {
            setDlssMode(sl::DLSSMode::eMaxQuality);
        }
        if (input.WasActionPressed(actions::kSetDlssQualityUltraPerformance))
        {
            setDlssMode(sl::DLSSMode::eUltraPerformance);
        }
        if (input.WasActionPressed(actions::kSetDlssQualityUltraQuality))
        {
            setDlssMode(sl::DLSSMode::eUltraQuality);
        }
        if (input.WasActionPressed(actions::kSetDlssQualityDLAA))
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
    DrawGameMenu(renderer, scene, levelManager, input);
    DrawAutodetectScreen();
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
    // The forced auto-detect measures the boot's view: nobody walks or looks away until it is done.
    const bool detecting = settings::TheScalability().AutodetectForced();

    // No window of ours on screen, and ImGui still holds the keyboard: a panel that closed leaves its focus behind (F2, Play), and App::WndProc gives every letter to whoever holds it.
    // A held right button clears it for the free camera (ImGuiLayer), which is why it went unseen; Play has no right button to hold, and WASD never reached the player.
    {
        ImGuiIO& io = ImGui::GetIO();
#if WITH_EDITOR
        const bool panelsOpen = editorController_.IsOpen() || developerWindow_.AnyWindowOpen() || raceResults;
#else
        const bool panelsOpen = developerWindow_.AnyWindowOpen() || raceResults || gameMenuOpen_ || settingsMenu_.IsOpen();
#endif
        if (!panelsOpen && io.WantCaptureKeyboard)
        {
            ImGui::SetWindowFocus(nullptr);
            io.WantCaptureKeyboard = false;
        }
    }

    // The player's field of view (the menu's Image; 0 = the level's), on the game camera: the render camera takes it with
    // the pose at the hand-over.
    scene.CameraRef().SetPlayerFovDeg(g_cameraPlayerFovDeg);

    // Camera input runs here (before Scene::Tick) so Scene itself never touches input.
    if (playSession_.Playing())
    {
        playSession_.Tick(input, scene, deltaTime, developerWindow_.AnyWindowOpen() || raceResults || gameMenuOpen_ || detecting, uiCapturingMouse);
        if (DebugDrawSystem* draw = renderer.GetDebugDrawSystem()) { scene.RaceRef().DebugDraw(*draw); } // bot.debug
    }
    else
    {
        bool allowCameraInput = !uiCapturingInput && !gameMenuOpen_ && !detecting;
#if WITH_EDITOR
        if (allowCameraInput && editorController_.IsOpen())
        {
            allowCameraInput = input.IsActionDown(actions::kLookToggle) && !input.IsKeyDown(VK_MENU);
        }
#endif
        if (allowCameraInput)
        {
            scene.CameraControllerRef().UpdateFromInput(input, deltaTime);
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
    const bool typing = renderer.ImGui().WantsTextInput();
    // Through ImGui as well: a panel with the keyboard's focus keeps a letter from InputManager (App::WndProc).
    const bool escape = !typing && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || input.TakeActionPress(actions::kStopPlay));
    const bool eject = !typing && input.TakeActionPress(actions::kPossessEject);
    const bool freeMouse = !typing && input.TakeActionPress(actions::kFreeMouse);
#if WITH_EDITOR
    // The editor's world stands: only a session runs it. The panels go with the start and come back with the stop.
    using PlayRequest = EditorController::PlayRequest;
    PlayRequest request = editorController_.ConsumePlayRequest();
    if (!typing && ((ImGui::IsKeyPressed(ImGuiKey_P, false) && ImGui::GetIO().KeyAlt) || input.TakeActionPress(actions::kPlay)))
    {
        request = PlayRequest{ PlayRequest::Kind::Play, false, true };
    }
    const bool fromBoot = Boot().editor.play;
    if (fromBoot)
    {
        Boot().editor.play = false;
        request = PlayRequest{ PlayRequest::Kind::Play, Boot().frame.camOverride, false }; // --cam-pos names where to start; a script does not take the owner's mouse
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
        if (!typing && input.TakeActionPress(actions::kKeepSimulation))
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
    // The forced auto-detect (a packaged build's first start on a system): no F8, no Esc menu until it is done. The boot's
    // own Play (Shipping's startMode) still starts: it is the view the game opens on, so the one worth measuring.
    const bool detecting = settings::TheScalability().AutodetectForced();
    if (!playSession_.Playing())
    {
        if (Boot().editor.play || (eject && !detecting))
        {
            const std::optional<game::PlayerStart>& start = scene.LevelPlayerStart();
            const bool fromCamera = Boot().editor.play && Boot().frame.camOverride;
            // Shipping's level rule: a level without a Player Start keeps the free camera (F8 still walks from it).
            const bool freeByRule = Boot().editor.play && !eject && Boot().dist.playByLevelRule && !start;
            if (!freeByRule)
            {
                playSession_.Begin(renderer, scene, input, (start && !fromCamera) ? &*start : nullptr, &scene.LevelRaceCourse(), eject, false);
            }
            Boot().editor.play = false;
        }
    }
    else if (!detecting)
    {
        if (eject) { playSession_.TogglePossess(scene); }
        if (freeMouse) { playSession_.ToggleFreeMouse(); }
    }
    // Esc is the game's menu in every build without the editor (docs/scalability_plan.md §6.1; Release too since
    // 2026-11-16 -- F1 stays Developer Controls), on foot, at a helm or with the camera free; the mouse is the menu's while
    // it is up and the game's again when Esc or Continue closes it.
    if (detecting)
    {
        gameMenuOpen_ = false; // Esc waits for the measurement: nothing cancels it
    }
    else if (escape && settingsMenu_.IsOpen())
    {
        settingsMenu_.Cancel(); // Esc on the settings: back to the menu, nothing kept
    }
    else if (escape)
    {
        gameMenuOpen_ = !gameMenuOpen_;
        if (playSession_.Playing()) { playSession_.SetMouseFree(gameMenuOpen_); }
    }
    else if ((gameMenuOpen_ || settingsMenu_.IsOpen()) && playSession_.Playing())
    {
        playSession_.SetMouseFree(true);
    }
#endif
    hudPlayLine_ = playSession_.HudLine(scene, ImGui::GetTime());
#if !WITH_EDITOR
    if (!playSession_.Playing()) { hudPlayLine_ = L"FREE CAMERA: right mouse look, WASD, wheel speed   F8 walk from here   Esc menu"; }
    if (detecting) { hudPlayLine_ = L"SETTING UP THE GRAPHICS: the keys wait until the measurement is done"; }
#endif
    hudRaceLine_ = playSession_.RaceHudLine(scene);
    hudBigText_ = playSession_.Playing() ? scene.RaceRef().BigText() : std::wstring{};
}

void AppController::DrawGameMenu(Renderer& renderer, Scene& scene, LevelManager& levelManager, InputManager& input)
{
    // The settings screen covers the menu while it is open.
    if (settingsMenu_.IsOpen())
    {
        settingsMenu_.Draw(input);
        return;
    }
    if (!gameMenuOpen_) { return; }
    // The levels: a packaged build's are its packages' (the cook puts in only the profile's), a dev build's the tree's --
    // data/levels/*.json, read once.
    if (gameMenuLevels_.empty())
    {
        vfs::List("data/levels", "*.json", [this](const vfs::ListEntry& entry)
        {
            if (!entry.directory) { gameMenuLevels_.push_back("data/levels/" + entry.name); }
        });
        std::sort(gameMenuLevels_.begin(), gameMenuLevels_.end());
    }
    GameMenuStyle style;
    style.Backdrop();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::Begin(menu_text::kMenuTitle, nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);
    const ImVec2 button(style.Em(18.0f), 0.0f);
    style.Title(menu_text::kMenuTitle);
    if (ImGui::Button(menu_text::kContinue, button))
    {
        gameMenuOpen_ = false;
        if (playSession_.Playing()) { playSession_.SetMouseFree(false); }
    }
    if (ImGui::Button(menu_text::kSettings, button)) { settingsMenu_.Open(); }
    // A developer's build (Release, Development -- not Shipping; owner, 2026-11-17: "a Play button in the Esc menu, from
    // the Player Start if there is one or from the camera, and one that stops the game and goes back to the free camera;
    // developer buttons"). The editor's Play and Stop without the editor: F8 starts the same session, and nothing else
    // ended one. Stop puts the camera back where it stood at Play; the world runs on (no editor stands it still).
    if (!build::Shipping())
    {
        ImGui::SeparatorText(menu_text::kDeveloper);
        if (!playSession_.Playing())
        {
            const std::optional<game::PlayerStart>& start = scene.LevelPlayerStart();
            if (ImGui::Button(start ? menu_text::kPlayFromStart : menu_text::kPlayFromCamera, button))
            {
                gameMenuOpen_ = false;
                playSession_.Begin(renderer, scene, input, start ? &*start : nullptr, &scene.LevelRaceCourse(), true, false);
            }
        }
        else if (ImGui::Button(menu_text::kStopPlay, button))
        {
            gameMenuOpen_ = false;
            playSession_.End(renderer, scene, input);
        }
    }
    // The levels as ONE list (owner, 2026-11-16: "a drop-down, not a million buttons"): pick one, then Load; the open
    // one is marked and its button says Restart.
    ImGui::SeparatorText(menu_text::kLevels);
    std::string current(levelManager.GetActiveLevelSourcePath());
    std::replace(current.begin(), current.end(), '\\', '/');
    const auto isOpen = [&current](const std::string& level) { return current == level || current.ends_with("/" + level); };
    const auto nameOf = [&isOpen](const std::string& level) {
        const std::size_t slash = level.find_last_of('/');
        std::string name = level.substr(slash + 1, level.size() - slash - 1 - 5); // without the folder and ".json"
        if (isOpen(level)) { name += menu_text::kLevelHere; }
        return name;
    };
    if (gameMenuLevels_.empty())
    {
        ImGui::TextDisabled("%s", menu_text::kNoLevels);
    }
    else
    {
        if (gameMenuLevelChoice_ < 0 || gameMenuLevelChoice_ >= static_cast<int>(gameMenuLevels_.size()))
        {
            gameMenuLevelChoice_ = 0;
            for (std::size_t i = 0; i < gameMenuLevels_.size(); ++i)
            {
                if (isOpen(gameMenuLevels_[i])) { gameMenuLevelChoice_ = static_cast<int>(i); }
            }
        }
        const std::string& chosen = gameMenuLevels_[static_cast<std::size_t>(gameMenuLevelChoice_)];
        ImGui::SetNextItemWidth(button.x);
        if (ImGui::BeginCombo("##level", nameOf(chosen).c_str(), ImGuiComboFlags_HeightLarge))
        {
            for (std::size_t i = 0; i < gameMenuLevels_.size(); ++i)
            {
                const bool selected = static_cast<int>(i) == gameMenuLevelChoice_;
                if (ImGui::Selectable(nameOf(gameMenuLevels_[i]).c_str(), selected)) { gameMenuLevelChoice_ = static_cast<int>(i); }
                if (selected) { ImGui::SetItemDefaultFocus(); }
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button(isOpen(chosen) ? menu_text::kRestartLevel : menu_text::kLoadLevel, button))
        {
            LOG_INFO(logging::LogCategory::App, "menu: level {}", chosen);
            levelManager.RequestLevelPathChange(chosen);
            gameMenuOpen_ = false;
            gameMenuLevelChoice_ = -1; // the next opening starts at the level then open
        }
    }
    ImGui::Separator();
    if (ImGui::Button(menu_text::kQuit, button))
    {
        PostMessageW(static_cast<HWND>(viewport->PlatformHandleRaw), WM_CLOSE, 0, 0); // the window's own close: settings and log flushed as by its [x]
    }
    ImGui::End();
}

void AppController::DrawAutodetectScreen()
{
    // The first start on a system (App.cpp, docs/scalability_plan.md §5): the frame goes on behind it -- it is what is
    // measured -- under a card that says what happens and which level is being measured.
    const settings::Scalability& scalability = settings::TheScalability();
    if (!scalability.AutodetectForced()) { return; }
    GameMenuStyle style;
    style.Backdrop();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(style.Em(30.0f), 0.0f), ImGuiCond_Always); // 0: the height fits the text
    ImGui::Begin("##autodetect", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    style.Title(menu_text::kDetectTitle);
    ImGui::TextWrapped("%s", menu_text::kDetectBody);
    ImGui::Spacing();
    const std::string& status = scalability.AutodetectStatus();
    ImGui::TextDisabled("%s", scalability.AutodetectRunning() && !status.empty() ? status.c_str() : menu_text::kDetectStarting);
    ImGui::End();
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
    if (Boot().dist.playByLevelRule)
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
    if (Boot().frame.hudHidden)
    {
        return;
    }
    const float fps = renderer.GetFPS();
    const float frameMs = fps > 0.0f ? 1000.0f / fps : 0.0f;
#if TC_PACKAGED && !TC_SHIPPING
    // A Development build says so on every frame: its screenshots are not the shipped game's.
    line(8, 8, 32.0f, float4(1, 1, 1, 0.6f), L"FPS:%.0f MS:%0.2f Scale:%0.2f   DEVELOPMENT BUILD %hs", fps, frameMs,
         renderer.GetRenderResolutionScale(), build::VersionString());
#else
    line(8, 8, 32.0f, float4(1, 1, 1, 0.6f), L"FPS:%.0f MS:%0.2f Scale:%0.2f", fps, frameMs, renderer.GetRenderResolutionScale());
#endif
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
        camPos.x, camPos.y, camPos.z, camRotF.x, camRotF.y, camRotF.z, camRotF.w, scene.CameraControllerRef().GetMoveSpeedMult(),
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
