#pragma once

#include <memory>

#include "rendering/core/Renderer.h"
#include "app/scene/Scene.h"
#include "input/InputManager.h"
#include "app/levels/LevelManager.h"
#include "ocean/OceanSimulation.h"

namespace Systems {

struct AppSystems {
    Renderer renderer;
    Scene scene;
    InputManager input;
    LevelManager levelManager;
    std::unique_ptr<OceanSimulation> oceanSimulation;
};

// Explicit lifecycle control to avoid implicit global mutation.
void Init(AppSystems* systems);
void Shutdown();

AppSystems& Get();
Renderer& GetRenderer();
Scene& GetScene();
InputManager& GetInput();
LevelManager& GetLevelManager();
OceanSimulation* GetOceanSimulation();
OceanSimulation* EnsureOceanSimulation();

} // namespace Systems
