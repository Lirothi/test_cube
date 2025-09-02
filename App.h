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
    void Run(HINSTANCE hInstance, int nCmdShow);

private:
    Renderer renderer_;
    ActionMap actions_;
    Scene scene_;
    InputManager input_;
    bool isRunning_ = true;

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
    void InitWindow(HINSTANCE hInstance, int nCmdShow);
    void InitScene();

    void SetRunnig(bool running)
    {
		isRunning_ = running;
    }
};