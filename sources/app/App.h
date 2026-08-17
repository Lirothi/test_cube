#pragma once

#include <windows.h>
#include <wrl.h>
#include <d3d12.h>
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
    int RunSceneStress(HINSTANCE hInstance, int nCmdShow, int iterations, bool gbvContinue);

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
    void LoadLoadingScreen();
    void ReleaseLoadingScreen();
    void HideLoadingScreen();
    void PaintLoadingScreen(HDC dc) const;

    void SetRunnig(bool running)
    {
        isRunning_ = running;
    }
};
