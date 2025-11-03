#pragma once

#include "rendering/core/Renderer.h"
#include "app/scene/Scene.h"
#include "input/InputManager.h"
#include "app/levels/LevelManager.h"

namespace Systems {

struct AppSystems {
    Renderer renderer;
    Scene scene;
    InputManager input;
    LevelManager levelManager;
};

void Set(AppSystems* systems);
AppSystems& Get();
Renderer& GetRenderer();
Scene& GetScene();
InputManager& GetInput();
LevelManager& GetLevelManager();

} // namespace Systems
