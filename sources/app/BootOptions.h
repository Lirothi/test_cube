#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// The command line (architecture plan R3.1): what main.cpp parsed, one struct instead of the extern globals App.h held.
// Grouped by what the switches are for. A request the run takes once (a window open at boot, a shot to take) is cleared
// by whoever took it, as the globals were.
struct BootOptions
{
    // The view and the capture: the level, the camera's pose and motion, screenshots, sweeps, the settings pinned for the
    // run, the HUD.
    struct Frame
    {
        // "--level=<path>": the level at boot instead of the default demo level. Used for headless verification of
        // specific levels (see docs atoll plan A2+).
        std::string levelPath;

        // "--cam-pos=x,y,z" / "--cam-rot=x,y,z,w": override the boot camera AFTER the level's
        // freeCameraStart. Both values are exactly what the on-screen HUD prints, so a screenshot round-trips
        // into a headless repro of the same view (see AGENTS.md).
        bool camOverride = false;
        float camPos[3] = { 0.0f, 0.0f, 0.0f };
        float camRot[4] = { 0.0f, 0.0f, 0.0f, 1.0f }; // orientation quaternion (x,y,z,w)
        // "--cam-fly=x,z": constant camera drift in m/s (world XZ), the headless stand-in for flying —
        // exercises motion-gated paths (window relocation, clipmap snap, DLSS history).
        float camFly[2] = { 0.0f, 0.0f };
        // "--cam-fly-delay=<sec>": hold the camera STILL for this long, then start the drift. Captures
        // the motion-ONSET transient (converged temporal histories -> first frames of motion), which a
        // from-boot drift can never show.
        float camFlyDelay = 0.0f;
        // "--cam-fly-yaw=<deg/s>": constant yaw rate on top of the drift -- the headless stand-in for
        // mouse look. A turning camera is what moves the cascade boxes between frames (S14 cross-check).
        // Also `--set camera.spin` / MCP set_settings in a running process.
        float camFlyYaw = 0.0f;
        // "--cam-orbit=<radius m>,<deg/s>[,<sec per frame>]": circle the START position at that radius, looking
        // along the tangent -- the headless stand-in for flying rings over a grove (the RT retire-bin leak's
        // repro: a straight drift leaves a grove in seconds; a ring keeps crossing it, which is what churns the
        // RT wind-BLAS slots, and what leaked their retired buffers until 2026-09-03). The optional third value
        // advances the orbit by a FIXED time per frame instead of the wall clock: a consecutive --shot-count series
        // saves each PNG synchronously (~150 ms), so on the wall clock its frames are ten 60 fps frames apart; with
        // 0.016667 they move exactly as at 60 fps.
        float camOrbit[3] = { 0.0f, 0.0f, 0.0f };

        // "--shot=<path>" one-shot capture: after shotDelaySec of runtime, read back the presented
        // backbuffer to a PNG (reliable on the flip-model swapchain, unlike GDI/PrintWindow) and exit.
        // The delay lets the ocean FFT + particle sim warm up before the grab. "--shot-delay=<sec>".
        std::string shotPath;
        double shotDelaySec = 7.0;
        // "--shot-count=<n> --shot-step=<sec> [--shot-interval=<sec>]": phase series from one process.
        // With --wind-freeze, saves n frames (path suffixed _NN), advancing the frozen wind/ocean clock by
        // exactly --shot-step between them; --shot-interval is the settle time for the temporal stack.
        int shotCount = 1;
        double shotStepSec = 0.0;
        double shotIntervalSec = 1.0;

        // "--profdump=<path>" (temporary perf harness): after shotDelaySec of runtime, write the current
        // CPU+GPU profiler overlay rows (name / avg / max / usages) to a text file and exit. Used to sweep
        // VSM tunables headlessly without reading the on-screen HUD from a screenshot. Empty = disabled.
        std::string profDumpPath;

        // "--trace=<frames>" (headless trace capture): after the same warmup delay, request the profiler
        // trace the CaptureTrace key would, then exit once it has been written to traces/. 0 = disabled.
        uint32_t traceFrames = 0;

        // "--dlss=<off|perf|balanced|quality|ultraperf|ultraquality|dlaa>": pick the upscaler mode at boot.
        // The build defaults to Balanced and the mode is otherwise only reachable through the F-keys or the
        // dev window, i.e. every native-resolution capture would need a keypress by hand — which makes the
        // native/DLSS pair the photographic-lighting plan is judged on impossible to capture headlessly.
        // Holds an sl::DLSSMode as int so this header stays free of the Streamline includes; -1 = leave the
        // compiled default alone.
        int dlssMode = -1;

        // "--window=<w>x<h>": a BORDERLESS window whose client area is exactly w x h, at the primary
        // monitor's origin. The default window is 2560x1440 clamped to the work area, so on the 3840x2160
        // panel there was no way to measure (or play at) true 4K: a maximised window loses the title bar and
        // the taskbar. 0 = the default window.
        int windowSize[2] = { 0, 0 };

        // "--sweep=<setting>:<v0>,<v1>,...": capture a settings sweep from ONE process run, instead of one
        // process launch per value. Sets the shot count from the value list, applies value[i] before shot i
        // and resets the exposure adaptation so each shot settles on its own value.
        //
        // Recognised settings (see App.cpp for the dispatch):
        //   exposure.lowPercentile  exposure.highPercentile  exposure.compensationEv
        //   exposure.manualEv100    exposure.minEv100        exposure.maxEv100
        //   exposure.speedUp        exposure.speedDown       exposure.enabled  exposure.autoExposure
        //   exposure.meterMaskStrength   exposure.meterMaskInnerRadius
        //   exposure.meterMaskOuterRadius exposure.meterMaskSkyBias
        //   color.toneCurve         color.agxSlope           color.agxPower    color.agxSaturation
        //
        // Example: --shot=out.png --sweep=exposure.lowPercentile:0.02,0.35,0.5,0.65 --shot-interval=2
        // writes out_00..out_03.png. With --profdump=p.txt it also writes p_00..p_03.txt, the profiler rows at
        // each shot, so every value's cost comes from the same process as its picture. Empty = disabled.
        //
        // "--sweep=<a>:<a0>,<a1>;<b>:<b0>,<b1>": further settings after a ';' advance IN STEP with the first
        // (shot i gets a_i AND b_i), so one run can photograph a grid of combined looks instead of one knob.
        // The FIRST list sets the shot count; a shorter list keeps its last value for the remaining shots.
        std::string sweepSetting;
        std::vector<float> sweepValues;
        std::vector<std::pair<std::string, std::vector<float>>> sweepExtra;

        // "--set=<name>:<value>[;<name>:<value>...]": pin settings for the whole run, using the SAME name
        // table --sweep uses. --sweep varies exactly one setting; anything else a measurement needs held at
        // a non-default value had no way in at all, so an A/B that needed two switches (a feature on AND
        // the debug view that shows it) could only be done by editing defaults and rebuilding. Applied once,
        // after the first level is up, and before the first shot's settle delay.
        std::vector<std::pair<std::string, float>> fixedSettings;

        // "--no-hud": build an EMPTY HUD text buffer. The FPS/MS readout is composited into the backbuffer
        // that "--shot" reads back, so it differs between two runs of the same frozen frame — which would
        // make every "no intentional image delta" check downstream diff the frame counter instead of the
        // image. Off by default: the HUD is what makes an exploratory shot carry its own camera.
        // Also `--set debug.hud`.
        bool hudHidden = false;

        // "--scalability=low|medium|high|ultra|off|auto" (main.cpp): the graphics preset at boot over what the player
        // saved, 0..3 = Low..Ultra, -1 = off (no group managed), -2 = not given, -3 = auto.
        int scalability = -2;
    } frame;

    // The harnesses a run can be (gates, the model's probes): what each was asked, and what it hands back to main.
    struct Harness
    {
        // "--stress-level=<path>": one more level in --scene-stress's switch rotation (after its three demo levels) -- a
        // level with what the gate must churn through, e.g. more shadowed lights than the initial view capacity (plan
        // S9e). Empty = none.
        std::string sceneStressExtraLevel;

        // "--intent=<phrase>": run ONE phrase through the editor's real command pipeline at boot,
        // log the verdict, and quit. This is the only harness that exercises the whole chain as the
        // app actually wires it -- level file -> document -> asset vocabulary -> grammar or model ->
        // resolver -> action registry -> command stack. The regression tool runs on a synthetic
        // document and the live probes run on a dumped prompt; neither sees that wiring.
        //
        // Preview only unless "--intent-run" is also given: showing what a phrase WOULD do is the
        // safe default for something that can touch six hundred objects.
        std::string intentPhrase;
        bool intentRun = false;
        bool intentFinished = false;   // set by the editor once the verdict is logged
        double intentTimeoutSec = 180.0;
        // "--intent-repeat=<n>": ask the same phrase n times in one process. The first request pays
        // for the system prompt's prefill and every later one hits the server's cache, so a single
        // run only ever reports the worst case.
        int intentRepeat = 1;

        // "--chat=<phrase>": send one message through the Model Chat panel once the editor is warm.
        // The chat's reply is STREAMED, so the only thing worth looking at exists only while it is
        // arriving -- which makes a headless --shot the only way to see it at all.
        std::string chatPhrase;
        std::string chatThenPhrase;

        // "--scalability-check": the gate of docs/scalability_plan.md §8.1 in a running process -- every level of every
        // group applied and every knob read back, the ceilings over the level's own values, Ultra against the boot's
        // values (App.cpp).
        bool scalabilityCheck = false;
        int scalabilityCheckExit = 0; // its failed reads: main's exit code

        // "--settings-dump=<file>" (architecture plan R2.0): after the warmup, every readable setting as `name value`
        // (app/harness/SettingsDumpHarness.h), then quit.
        std::string settingsDumpPath;
        // "--boot-dump=<file>" (architecture plan R3.2b): every field here as the command line set it, then out before the
        // App starts (app/BootOptionsDump.cpp) -- the gate of moving a switch's parsing.
        std::string bootDumpPath;
        // "--scene-stress[=<n>]" and its variants (main.cpp, before device creation): the level-churn stress as a
        // harness of App::Run (app/diagnostics/SceneStress.h, architecture plan R3.3e). 0 iterations = its default.
        bool sceneStress = false;
        int sceneStressIterations = 0;
        bool sceneStressGbv = false;       // --scene-stress-gbv: InfoQueue errors logged, the run goes on
        bool sceneStressRoughness = false; // --scene-stress-roughness: RT roughness edits on wind_test #722
        bool sceneStressSky = false;       // --scene-stress-sky: sun / exposure / IBL edits
        int sceneStressExit = 0;           // 1 from its first frame until the CLEAN verdict: main's exit code
    } harness;

    // What opens at boot -- the editor and its windows, the play session, the player's settings screen -- so a headless
    // --shot can photograph what otherwise needs a keypress or a click; and the editor's MCP port. Each is taken once.
    struct Editor
    {
        // "--log-window": open the session-log viewer at boot, so a headless --shot can capture the
        // viewer itself — the only way its rendering is verifiable without driving the GUI by hand.
        bool logWindow = false;
        // "--mem-window": open Developer Controls on the Memory tab at boot (memory profiling plan M2), for
        // the same reason -- a --shot can then photograph the tab.
        bool memoryWindow = false;
        // "--dev-tab=<label>": open Developer Controls on that tab at boot ("gi", "lod", ... case-insensitive), for the
        // same reason; an unknown label logs a warning naming the tabs.
        std::string devTab;
        // "--mcp-port=<n>": this process serves the editor's MCP on that port instead of editor_state.json's mcpPort
        // (8128) -- a script's editor beside the owner's, which holds 8128. 0 = editor_state's.
        int mcpPort = 0;
        // "--editor": open the Level Editor at boot, for the same reason logWindow exists --
        // so a headless --shot can capture a panel that otherwise needs a keypress to appear.
        // WITH_EDITOR only; ignored elsewhere.
        bool openEditor = false;
        bool play = false; // "--play": boot into the play session (docs/play_mode_plan.md)
        // "--edit-mesh=<models/x/x.mesh.json>": open the Mesh Editor on that asset at boot (implies
        // --editor). Same reason again: its preview is otherwise reachable only by a double-click in
        // the content browser, and a lighting change there is judged by LOOKING at it.
        std::string editMesh;
        // "--show-import[=<name>]": open the Import Assets window at boot with <name> (importItem) selected (implies
        // --editor). Its details pane is otherwise reachable only by clicking a row in a window that is
        // itself behind a menu, and the pane is judged by LOOKING at it.
        bool showImport = false;
        std::string importItem;
        // "--inspect=<environment type>[#<group>]": select the level's environment object of that type at boot and
        // scroll the Inspector to the group (its JSON key: timeOfDay#fog, timeOfDay#lamps, postProcess#heightFog;
        // implies --editor). The same reason as --edit-mesh: a group far down an object is otherwise reachable only by
        // the mouse wheel.
        std::string inspect;
        // "--settings-menu[=display|graphics|image|controls|menu]": the player's settings screen open at boot on that
        // tab, or the Esc menu ("menu"), for a --shot (§8.5). A build without the editor.
        std::string settingsMenu;
    } editor;

    // Distribution: the editor's Build Distribution window at boot, and how a shipped build starts its levels.
    struct Dist
    {
        bool buildDist = false; // "--build-dist": the Level Editor's Build Distribution window open at boot (implies --editor)
        // "--build-dist=<profile>[,override-licences][,delete=<build>]": that profile loaded in it, its licence override
        // ticked, a build's delete confirmation open (a --shot of them).
        std::string buildDistProfile;
        // Shipping, boot.json startMode "play": every level by its own rule -- a Player Start: first person from it; none:
        // the free camera at its freeCameraStart, else at the origin (owner, 2026-11-13). AppController::TickPlay /
        // OnLevelLoaded.
        bool playByLevelRule = false;
    } dist;
};

// The run's options: main.cpp fills them before App::Run, everything else reads them (and clears what it consumes).
BootOptions& Boot();
// "--boot-dump=<file>": writes every field of Boot() but bootDumpPath, one `group.field=value` a line (BootOptionsDump.cpp).
bool WriteBootDump(const std::string& path);
