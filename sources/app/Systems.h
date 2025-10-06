#pragma once

#include "rendering/core/Renderer.h"
#include "app/Scene.h"
#include "input/InputManager.h"

namespace Systems {

struct AppSystems {
    Renderer renderer;
    Scene scene;
    InputManager input;
};

void Set(AppSystems* systems);
AppSystems& Get();
Renderer& GetRenderer();
Scene& GetScene();
InputManager& GetInput();

} // namespace Systems
