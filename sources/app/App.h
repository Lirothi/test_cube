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
    void LoadLoadingScreen();
    void ReleaseLoadingScreen();
    void HideLoadingScreen();
    void PaintLoadingScreen(HDC dc) const;

    void SetRunnig(bool running)
    {
        isRunning_ = running;
    }
};
