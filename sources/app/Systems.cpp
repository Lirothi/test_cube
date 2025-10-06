#include "app/Systems.h"

#include <cassert>

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

} // namespace Systems
