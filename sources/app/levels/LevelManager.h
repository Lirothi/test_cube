#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "app/levels/Level.h"
#include "core/math/Math.h"

struct LevelCameraOverride
{
    Math::float3 position{ 0.0f, 0.0f, 0.0f };
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
};

struct LevelLoadOptions
{
    bool preserveCameraTransform = false;
    std::optional<LevelCameraOverride> cameraOverride;
#if WITH_EDITOR
    EditorSceneDocument* editorDocument = nullptr;
#endif
};

struct LevelChangeRequest
{
    std::string levelName;
    std::string sourcePath;
    bool loadFromPath = false;
    LevelLoadOptions options;
};

class LevelManager
{
public:
    LevelManager() = default;

    void RegisterLevel(std::unique_ptr<Level> level);

    template <typename TLevel, typename... TArgs>
    void RegisterLevel(TArgs&&... args)
    {
        static_assert(std::is_base_of_v<Level, TLevel>, "TLevel must derive from Level");
        RegisterLevel(std::make_unique<TLevel>(std::forward<TArgs>(args)...));
    }

    bool LoadLevel(std::string_view name, const LevelLoadContext& ctx, const LevelLoadOptions& options = {});
    bool LoadLevelFromPath(std::string path, const LevelLoadContext& ctx, const LevelLoadOptions& options = {});

    void Tick(float deltaTime);

    void RequestLevelChange(std::string levelName, const LevelLoadOptions& options = {});
    void RequestLevelPathChange(std::string path, const LevelLoadOptions& options = {});
    std::optional<LevelChangeRequest> ConsumePendingLevelRequest();

    Level* GetActiveLevel() const { return activeLevel_; }
    std::string_view GetActiveLevelName() const { return activeLevelName_; }
    std::string_view GetActiveLevelSourcePath() const { return activeLevel_ ? activeLevel_->GetSourcePath() : std::string_view{}; }

    bool HasLevel(std::string_view name) const;

private:
    std::unordered_map<std::string, std::unique_ptr<Level>> levels_;
    Level* activeLevel_ = nullptr;
    std::string activeLevelName_;
    std::optional<LevelChangeRequest> pendingLevelRequest_;
};

