#pragma once

#include <string_view>

#include "app/levels/Level.h"

class DemoLevel final : public Level
{
public:
    static constexpr std::string_view kName = "DemoLevel";

    std::string_view GetName() const override { return kName; }

    void Load(const LevelLoadContext& ctx) override;
    void Unload(const LevelLoadContext& ctx) override;
};

