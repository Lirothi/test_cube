#pragma once

#include <string>
#include <string_view>
#include <utility>

#include "app/levels/Level.h"

class JsonLevel final : public Level
{
public:
    static constexpr std::string_view kName = "JsonLevel";

    explicit JsonLevel(std::string sourcePath = "data/levels/demo.json")
        : sourcePath_(std::move(sourcePath))
    {
    }

    std::string_view GetName() const override { return kName; }
    std::string_view GetSourcePath() const override { return sourcePath_; }

    void Load(const LevelLoadContext& ctx) override;
    void Unload(const LevelLoadContext& ctx) override;
    void SetSourcePath(std::string path) override { sourcePath_ = std::move(path); }

private:
    // The load itself. Load wraps it: a key of the wrong type throws out of nlohmann, and the level is a file people
    // and tools edit -- that is an Error naming the level and the key's complaint, not the end of the process.
    void LoadFromSource_(const LevelLoadContext& ctx);

    std::string sourcePath_;
};
