#include "app/levels/LevelManager.h"

#include <cassert>

#include "app/Scene.h"
#include "app/Systems.h"

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

    if (ctx.uploadCmdList == nullptr)
    {
        assert(false && "Level loading requires a valid upload command list");
        return false;
    }

    auto& renderer = Systems::GetRenderer();
    auto& scene = Systems::GetScene();

    if (activeLevel_)
    {
        activeLevel_->Unload(ctx);
    }

    scene.InitializeCommonResources(&renderer, ctx.uploadCmdList, ctx.uploadKeepAlive);

    activeLevel_ = it->second.get();
    activeLevelName_ = it->first;
    pendingLevelRequest_.reset();

    activeLevel_->Load(ctx);

    scene.FinalizeLevelLoad(&renderer, ctx.uploadCmdList, ctx.uploadKeepAlive);

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

