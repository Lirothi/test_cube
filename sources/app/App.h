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
#include "rendering/view/Camera.h"
#include "app/Systems.h"
#include "app/AppController.h"
#include "core/task/TaskSystem.h"
#include "app/BootOptions.h"

// `frame.timeScale`: the world's clock runs this many times faster (App.cpp). The HUD says so while it is not 1.
extern float g_timeScale;

// True while the local model has a request in flight. The frame loop reads it and caps the
// frame rate, because the renderer at 640 fps and the model's experts want the same cores
// and the renderer wins -- which is why alt-tabbing away from the editor visibly speeds the
// model up. Set by the editor every frame; always false in a build without one.
extern std::atomic<bool> g_modelBusy;

// The model is doing work nobody is waiting for: the startup warmup, or a note written
// after a refusal. Worth yielding some cores to, but not at the price of a sluggish editor
// in the first minute after it opens -- so this caps frames far more gently than the above.
extern std::atomic<bool> g_modelBusyBackground;

class App {
public:
    ~App();

    void Run(HINSTANCE hInstance, int nCmdShow);


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
