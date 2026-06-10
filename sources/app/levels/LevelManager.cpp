#include "app/levels/LevelManager.h"

#include <cassert>

#include "app/scene/Scene.h"
#include "rendering/core/UploadBatch.h"

void LevelManager::RegisterLevel(std::unique_ptr<Level> level)
{
    if (!level)
    {
        return;
    }

    const std::string levelName(level->GetName());
    levels_[levelName] = std::move(level);
}

bool LevelManager::HasLevel(std::string_view name) const
{
    return levels_.find(std::string(name)) != levels_.end();
}

bool LevelManager::LoadLevel(std::string_view name, const LevelLoadContext& ctx)
{
    const std::string key(name);
    auto it = levels_.find(key);
    if (it == levels_.end())
    {
        return false;
    }

    if (!ctx.uploads.IsOpen())
    {
        assert(false && "Level loading requires an open upload batch");
        return false;
    }

    auto& renderer = ctx.renderer;
    auto& scene = ctx.scene;

    if (activeLevel_)
    {
        activeLevel_->Unload(ctx);
    }

    scene.InitializeCommonResources(&renderer, ctx.uploads.CommandList(), ctx.uploads.KeepAlive());

    activeLevel_ = it->second.get();
    activeLevelName_ = it->first;
    pendingLevelRequest_.reset();

    activeLevel_->Load(ctx);

    scene.FinalizeLevelLoad(&renderer, ctx.uploads.CommandList(), ctx.uploads.KeepAlive());

    return true;
}

void LevelManager::Tick(float deltaTime)
{
    if (activeLevel_)
    {
        activeLevel_->Tick(deltaTime);
    }
}

void LevelManager::RequestLevelChange(std::string levelName)
{
    pendingLevelRequest_ = std::move(levelName);
}

std::optional<std::string> LevelManager::ConsumePendingLevelRequest()
{
    if (!pendingLevelRequest_)
    {
        return std::nullopt;
    }

    std::optional<std::string> result = std::move(pendingLevelRequest_);
    pendingLevelRequest_.reset();
    return result;
}

