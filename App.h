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

#include "Helpers.h"
#include "Camera.h"
#include "Renderer.h"
#include "Scene.h"
#include "TaskSystem.h"
#include "InputManager.h"
#include "ActionMap.h"

class App {
public:
    struct Systems {
        Renderer renderer;
        ActionMap actions;
        Scene scene;
        InputManager input;
    };

    void Run(HINSTANCE hInstance, int nCmdShow);

private:
    std::unique_ptr<Systems> systems_;
    bool isRunning_ = true;

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
    void InitWindow(HINSTANCE hInstance, int nCmdShow);
    void InitScene();

    void SetRunnig(bool running)
    {
        isRunning_ = running;
    }
};
