#include "app/levels/LevelManager.h"

#include <cassert>

#include "app/levels/JsonLevel.h"
#include "app/scene/Scene.h"
#include "rendering/core/UploadBatch.h"

namespace
{
struct CameraTransformSnapshot
{
    Math::float3 position;
    float yaw = 0.0f;
    float pitch = 0.0f;
};

CameraTransformSnapshot CaptureCameraTransform(const Camera& camera)
{
    return { camera.GetPosition(), camera.GetYaw(), camera.GetPitch() };
}

CameraTransformSnapshot ToSnapshot(const LevelCameraOverride& cameraOverride)
{
    return { cameraOverride.position, cameraOverride.yaw, cameraOverride.pitch };
}

void RestoreCameraTransform(Camera& camera, const CameraTransformSnapshot& snapshot)
{
    camera.SetPosition(snapshot.position);
    camera.SetYawPitch(snapshot.yaw, snapshot.pitch);
}
} // namespace

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

bool LevelManager::LoadLevel(std::string_view name, const LevelLoadContext& ctx, const LevelLoadOptions& options)
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
    std::optional<CameraTransformSnapshot> preservedCameraTransform;
    if (options.preserveCameraTransform)
    {
        preservedCameraTransform = CaptureCameraTransform(scene.CameraRef());
    }

    if (activeLevel_)
    {
        activeLevel_->Unload(ctx);
    }

    scene.InitializeCommonResources(&renderer, ctx.uploads.CommandList(), ctx.uploads.KeepAlive());

    activeLevel_ = it->second.get();
    activeLevelName_ = it->first;
    pendingLevelRequest_.reset();

    activeLevel_->Load(ctx);
    if (options.cameraOverride)
    {
        RestoreCameraTransform(scene.CameraRef(), ToSnapshot(*options.cameraOverride));
    }
    else if (preservedCameraTransform)
    {
        RestoreCameraTransform(scene.CameraRef(), *preservedCameraTransform);
    }

    scene.FinalizeLevelLoad(&renderer, ctx.uploads.CommandList(), ctx.uploads.KeepAlive());

    return true;
}

bool LevelManager::LoadLevelFromPath(std::string path, const LevelLoadContext& ctx, const LevelLoadOptions& options)
{
    const std::string key(JsonLevel::kName);
    auto it = levels_.find(key);
    if (it == levels_.end())
    {
        RegisterLevel<JsonLevel>(path);
    }
    else
    {
        it->second->SetSourcePath(std::move(path));
    }

    return LoadLevel(JsonLevel::kName, ctx, options);
}

void LevelManager::Tick(float deltaTime)
{
    if (activeLevel_)
    {
        activeLevel_->Tick(deltaTime);
    }
}

void LevelManager::RequestLevelChange(std::string levelName, const LevelLoadOptions& options)
{
    pendingLevelRequest_ = LevelChangeRequest{ std::move(levelName), std::string(), false, options };
}

void LevelManager::RequestLevelPathChange(std::string path, const LevelLoadOptions& options)
{
    pendingLevelRequest_ = LevelChangeRequest{ std::string(), std::move(path), true, options };
}

std::optional<LevelChangeRequest> LevelManager::ConsumePendingLevelRequest()
{
    if (!pendingLevelRequest_)
    {
        return std::nullopt;
    }

    std::optional<LevelChangeRequest> result = std::move(pendingLevelRequest_);
    pendingLevelRequest_.reset();
    return result;
}

