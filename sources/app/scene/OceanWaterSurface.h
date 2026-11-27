#pragma once

#include "physics/WaterSurface.h"

class OceanRenderable;

// The ocean as the physics' water (architecture plan R14.1): the world binds the level's ocean every frame before the physics'
// Tick, and the physics reads it through physics::WaterSurface without naming the ocean. Null = no water.
class OceanWaterSurface final : public physics::WaterSurface
{
public:
    void Bind(const OceanRenderable* ocean) { ocean_ = ocean; }

    bool Present() const override;
    bool HasWaves() const override;
    float Level() const override;
    float SampleHeight(float x, float z, float wakeShare) const override;

private:
    const OceanRenderable* ocean_ = nullptr;
};
