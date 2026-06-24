#include "app/Systems.h"

#include <cassert>
#include <memory>
#include <mutex>

namespace Systems {

namespace {
    // Set once in App::Run before any worker threads start and cleared after they
    // stop, so plain reads are safe — Get() is called from task threads every frame
    // and must not take a lock.
    AppSystems* gSystems = nullptr;
    std::mutex gOceanMutex;
}

void Init(AppSystems* systems) {
    assert(gSystems == nullptr && "Systems already initialized");
    gSystems = systems;
}

void Shutdown() {
    gSystems = nullptr;
}

AppSystems& Get() {
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

OceanSimulation* CreateOceanSimulation(const std::wstring& configPath)
{
    auto& systems = Get();
    std::lock_guard<std::mutex> lock(gOceanMutex);
    systems.oceanSimulation = std::make_unique<OceanSimulation>(configPath);
    return systems.oceanSimulation.get();
}

void DestroyOceanSimulation()
{
    auto& systems = Get();
    std::lock_guard<std::mutex> lock(gOceanMutex);
    systems.oceanSimulation.reset();
}

} // namespace Systems
