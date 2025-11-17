#include "app/Systems.h"

#include <cassert>
#include <memory>

namespace Systems {

namespace {
AppSystems* gSystems = nullptr;
}

void Set(AppSystems* systems) {
    gSystems = systems;
}

AppSystems& Get() {
    assert(gSystems != nullptr);
    return *gSystems;
}

Renderer& GetRenderer() {
    return Get().renderer;
}

Scene& GetScene() {
    return Get().scene;
}

InputManager& GetInput() {
    return Get().input;
}

LevelManager& GetLevelManager() {
    return Get().levelManager;
}

OceanSimulation* GetOceanSimulation()
{
    auto& systems = Get();
    return systems.oceanSimulation.get();
}

OceanSimulation* EnsureOceanSimulation()
{
    auto& systems = Get();
    if (!systems.oceanSimulation)
    {
        systems.oceanSimulation = std::make_unique<OceanSimulation>();
    }
    return systems.oceanSimulation.get();
}

} // namespace Systems
