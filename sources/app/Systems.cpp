#include "app/Systems.h"

#include <cassert>
#include <memory>

namespace Systems {

namespace {
    AppSystems* gSystems = nullptr;
    std::mutex gSystemsMutex;
}

void Init(AppSystems* systems) {
    std::lock_guard<std::mutex> lock(gSystemsMutex);
    assert(gSystems == nullptr && "Systems already initialized");
    gSystems = systems;
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(gSystemsMutex);
    gSystems = nullptr;
}

AppSystems& Get() {
    std::lock_guard<std::mutex> lock(gSystemsMutex);
    assert(gSystems != nullptr && "Systems not initialized");
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
    static std::mutex oceanMutex;
    std::lock_guard<std::mutex> lock(oceanMutex);
    if (!systems.oceanSimulation)
    {
        systems.oceanSimulation = std::make_unique<OceanSimulation>();
    }
    return systems.oceanSimulation.get();
}

} // namespace Systems
