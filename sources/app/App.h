#pragma once

#include <windows.h>
#include <wrl.h>
#include <d3d12.h>
#include <atomic>
#include <vector>
#include <string>
#include <DirectXMath.h>
#include <stdexcept>
#include <memory>
#include <unordered_map>

#include "core/Helpers.h"
#include "app/camera/Camera.h"
#include "app/Systems.h"
#include "app/AppController.h"
#include "core/task/TaskSystem.h"

// Optional boot-level override (set from the "--level=<path>" command line). Empty = the default
// demo level. Used for headless verification of specific levels (see docs atoll plan A2+).
extern std::string g_bootLevelPath;
// "--cam-pos=x,y,z" / "--cam-rot=x,y,z,w": override the boot camera AFTER the level's
// freeCameraStart. Both values are exactly what the on-screen HUD prints, so a screenshot round-trips
// into a headless repro of the same view (see AGENTS.md).
extern bool  g_camOverride;
extern float g_camPos[3];
// "--cam-fly=x,z": constant camera drift in m/s (world XZ), the headless stand-in for flying —
// exercises motion-gated paths (window relocation, clipmap snap, DLSS history).
extern float g_camFly[2];
// "--cam-fly-delay=<sec>": hold still this long before the drift starts (motion-onset capture).
extern float g_camFlyDelay;
// "--cam-fly-yaw=<deg/s>": constant yaw rate on top of the drift (headless mouse look).
extern float g_camFlyYaw;
// "--cam-orbit=<radius m>,<deg/s>": circle the START position at that radius, looking along the
// tangent -- the headless stand-in for flying rings over a grove (the RT retire-bin leak's repro).
extern float g_camOrbit[2];
extern float g_camRot[4]; // orientation quaternion (x,y,z,w)

// "--shot=<path>" one-shot capture: after g_shotDelaySec of runtime, read back the presented
// backbuffer to a PNG (reliable on the flip-model swapchain, unlike GDI/PrintWindow) and exit.
// The delay lets the ocean FFT + particle sim warm up before the grab. "--shot-delay=<sec>".
extern std::string g_shotPath;
extern double g_shotDelaySec;
// "--shot-count=<n> --shot-step=<sec> [--shot-interval=<sec>]": phase series from one process.
// With --wind-freeze, saves n frames (path suffixed _NN), advancing the frozen wind/ocean clock by
// exactly --shot-step between them; --shot-interval is the settle time for the temporal stack.
extern int    g_shotCount;
extern double g_shotStepSec;
extern double g_shotIntervalSec;

// "--profdump=<path>" (temporary perf harness): after g_shotDelaySec of runtime, write the current
// CPU+GPU profiler overlay rows (name / avg / max / usages) to a text file and exit. Used to sweep
// VSM tunables headlessly without reading the on-screen HUD from a screenshot. Empty = disabled.
extern std::string g_profDumpPath;

// "--trace=<frames>" (headless trace capture): after the same warmup delay, request the profiler
// trace the CaptureTrace key would, then exit once it has been written to traces/. 0 = disabled.
extern uint32_t g_traceFrames;

// "--dlss=<off|perf|balanced|quality|ultraperf|ultraquality|dlaa>": pick the upscaler mode at boot.
// The build defaults to Balanced and the mode is otherwise only reachable through the F-keys or the
// dev window, i.e. every native-resolution capture would need a keypress by hand — which makes the
// native/DLSS pair the photographic-lighting plan is judged on impossible to capture headlessly.
// Holds an sl::DLSSMode as int so this header stays free of the Streamline includes; -1 = leave the
// compiled default alone.
extern int g_bootDlssMode;

// "--window=<w>x<h>": a BORDERLESS window whose client area is exactly w x h, at the primary
// monitor's origin. The default window is 2560x1440 clamped to the work area, so on the 3840x2160
// panel there was no way to measure (or play at) true 4K: a maximised window loses the title bar and
// the taskbar. 0 = the default window.
extern int g_windowSize[2];

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
// writes out_00..out_03.png. Empty = disabled.
extern std::string g_sweepSetting;
extern std::vector<float> g_sweepValues;

// "--set=<name>:<value>[;<name>:<value>...]": pin settings for the whole run, using the SAME name
// table --sweep uses. --sweep varies exactly one setting; anything else a measurement needs held at
// a non-default value had no way in at all, so an A/B that needed two switches (a feature on AND
// the debug view that shows it) could only be done by editing defaults and rebuilding. Applied once,
// after the first level is up, and before the first shot's settle delay.
extern std::vector<std::pair<std::string, float>> g_fixedSettings;

// "--log-window": open the session-log viewer at boot, so a headless --shot can capture the
// viewer itself — the only way its rendering is verifiable without driving the GUI by hand.
extern bool g_bootLogWindow;
// "--editor": open the Level Editor at boot, for the same reason g_bootLogWindow exists --
// so a headless --shot can capture a panel that otherwise needs a keypress to appear.
// WITH_EDITOR only; ignored elsewhere.
extern bool g_bootEditor;
// "--edit-mesh=<models/x.mesh.json>": open the Mesh Editor on that asset at boot (implies
// --editor). Same reason again: its preview is otherwise reachable only by a double-click in
// the content browser, and a lighting change there is judged by LOOKING at it.
extern std::string g_bootEditMesh;
// "--show-import[=<name>]": open the Import Assets window at boot with <name> selected (implies
// --editor). Its details pane is otherwise reachable only by clicking a row in a window that is
// itself behind a menu, and the pane is judged by LOOKING at it.
extern bool g_bootShowImport;
extern std::string g_bootImportItem;

// "--intent=<phrase>": run ONE phrase through the editor's real command pipeline at boot,
// log the verdict, and quit. This is the only harness that exercises the whole chain as the
// app actually wires it -- level file -> document -> asset vocabulary -> grammar or model ->
// resolver -> action registry -> command stack. The regression tool runs on a synthetic
// document and the live probes run on a dumped prompt; neither sees that wiring.
//
// Preview only unless "--intent-run" is also given: showing what a phrase WOULD do is the
// safe default for something that can touch six hundred objects.
extern std::string g_intentPhrase;
extern bool   g_intentRun;
extern bool   g_intentFinished;   // set by the editor once the verdict is logged
extern double g_intentTimeoutSec;
// "--intent-repeat=<n>": ask the same phrase n times in one process. The first request pays
// for the system prompt's prefill and every later one hits the server's cache, so a single
// run only ever reports the worst case.
extern int g_intentRepeat;

// "--chat=<phrase>": send one message through the Model Chat panel once the editor is warm.
// The chat's reply is STREAMED, so the only thing worth looking at exists only while it is
// arriving -- which makes a headless --shot the only way to see it at all.
extern std::string g_chatPhrase;
extern std::string g_chatThenPhrase;

// True while the local model has a request in flight. The frame loop reads it and caps the
// frame rate, because the renderer at 640 fps and the model's experts want the same cores
// and the renderer wins -- which is why alt-tabbing away from the editor visibly speeds the
// model up. Set by the editor every frame; always false in a build without one.
extern std::atomic<bool> g_modelBusy;

// The model is doing work nobody is waiting for: the startup warmup, or a note written
// after a refusal. Worth yielding some cores to, but not at the price of a sluggish editor
// in the first minute after it opens -- so this caps frames far more gently than the above.
extern std::atomic<bool> g_modelBusyBackground;

// "--no-hud": build an EMPTY HUD text buffer. The FPS/MS readout is composited into the backbuffer
// that "--shot" reads back, so it differs between two runs of the same frozen frame — which would
// make every "no intentional image delta" check downstream diff the frame counter instead of the
// image. Off by default: the HUD is what makes an exploratory shot carry its own camera.
extern bool g_hudHidden;

class App {
public:
    ~App();

    void Run(HINSTANCE hInstance, int nCmdShow);

    // Boots the real renderer/scene exactly like Run(), then hands control to
    // the autonomous scene-lifecycle stress driver instead of the interactive
    // loop. Returns the process exit code (0 = clean, nonzero = fault caught).
    int RunSceneStress(HINSTANCE hInstance, int nCmdShow, int iterations, bool gbvContinue,
                       bool roughnessEdits, bool skyEdits);

private:
    std::unique_ptr<Systems::AppSystems> systems_;
    AppController appController_;
    HWND hWnd_ = nullptr;
    HBITMAP loadingBitmap_ = nullptr;
    BITMAP loadingBitmapInfo_{};
    bool isRunning_ = true;
    bool loadingScreenVisible_ = true;

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
    void InitWindow(HINSTANCE hInstance, int nCmdShow);
    void InitScene();
    // "--set=<name>:<value>;..." applied to a live scene. Run() calls it on the first frame; the
    // stress harness after its bootstrap -- a gate that cannot pin the setting under test
    // validates the default, not the change.
    void ApplyFixedSettings(Scene& scene);
    void LoadLoadingScreen();
    void ReleaseLoadingScreen();
    void HideLoadingScreen();
    void PaintLoadingScreen(HDC dc) const;

    void SetRunnig(bool running)
    {
        isRunning_ = running;
    }
};
