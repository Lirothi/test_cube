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

// "--profdump=<path>" (temporary perf harness): after g_shotDelaySec of runtime, write the current
// CPU+GPU profiler overlay rows (name / avg / max / usages) to a text file and exit. Used to sweep
// VSM tunables headlessly without reading the on-screen HUD from a screenshot. Empty = disabled.
extern std::string g_profDumpPath;

// "--trace=<frames>" (headless trace capture): after the same warmup delay, request the profiler
// trace the CaptureTrace key would, then exit once it has been written to traces/. 0 = disabled.
extern uint32_t g_traceFrames;

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
