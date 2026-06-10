#pragma once

#include <string_view>

class Renderer;
class Scene;
class UploadBatch;

// Everything a level needs to load/unload, passed explicitly instead of being
// fetched through Systems::Get*().
struct LevelLoadContext
{
    // Open upload batch for the duration of the load; the caller submits and
    // waits after the level finishes loading.
    UploadBatch& uploads;
    Renderer& renderer;
    Scene& scene;
};

class Level
{
public:
    virtual ~Level() = default;

    virtual std::string_view GetName() const = 0;
    virtual void Load(const LevelLoadContext& ctx) = 0;
    virtual void Unload(const LevelLoadContext& ctx);
    virtual void Tick(float deltaTime) { (void)deltaTime; }
};
