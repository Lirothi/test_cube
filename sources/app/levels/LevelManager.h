#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "app/levels/Level.h"

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

    bool LoadLevel(std::string_view name, const LevelLoadContext& ctx);

    void Tick(float deltaTime);

    void RequestLevelChange(std::string levelName);
    std::optional<std::string> ConsumePendingLevelRequest();

    Level* GetActiveLevel() const { return activeLevel_; }
    std::string_view GetActiveLevelName() const { return activeLevelName_; }

    bool HasLevel(std::string_view name) const;

private:
    std::unordered_map<std::string, std::unique_ptr<Level>> levels_;
    Level* activeLevel_ = nullptr;
    std::string activeLevelName_;
    std::optional<std::string> pendingLevelRequest_;
};

