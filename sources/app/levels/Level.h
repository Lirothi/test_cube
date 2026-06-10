#pragma once

#include <string_view>

class UploadBatch;

struct LevelLoadContext
{
    // Open upload batch for the duration of the load; the caller submits and
    // waits after the level finishes loading.
    UploadBatch& uploads;
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
