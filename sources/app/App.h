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
#include "app/Camera.h"
#include "app/Systems.h"
#include "core/task/TaskSystem.h"

class App {
public:
    void Run(HINSTANCE hInstance, int nCmdShow);

private:
    std::unique_ptr<Systems::AppSystems> systems_;
    bool isRunning_ = true;

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
    void InitWindow(HINSTANCE hInstance, int nCmdShow);
    void InitScene();

    void SetRunnig(bool running)
    {
        isRunning_ = running;
    }
};
