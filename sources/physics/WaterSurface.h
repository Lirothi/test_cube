#pragma once

namespace physics
{
    // The water the world floats its bodies on and the player swims in, as the world reads it (architecture plan R14.1):
    // the physics names no ocean. The game's side hands one to World::SetWater (app/scene/OceanWaterSurface: the ocean's
    // readback); a gate hands its own. Read on the game thread only, in World::Tick (the step's thread is given what was
    // read, never the surface).
    class WaterSurface
    {
    public:
        virtual ~WaterSurface() = default;
        // There is water at all this frame (a level's ocean, shown). False: nothing floats, nobody swims.
        virtual bool Present() const = 0;
        // The surface has waves to read (the ocean's readback has landed); false = flat at Level().
        virtual bool HasWaves() const = 0;
        virtual float Level() const = 0;
        // The surface's height at (x, z) when HasWaves(); `wakeShare` = how much of the boats' wake it takes (1 = all).
        virtual float SampleHeight(float x, float z, float wakeShare) const = 0;
    };
} // namespace physics
