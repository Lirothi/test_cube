#pragma once
// Architecture plan R13.1 (docs/architecture_review_plan.md): objects made and taken out while the world runs -- a race's
// own boats, a boat's spray emitters -- through one road, which the scene gives at its hand-over (Scene::Sync, the render
// half parked) to whoever makes or drops them there: the game's part of the hand-over (game::PlayWorld::HandOver) and
// the spray (vfx::VesselSpray::Sync). Before it each had its own upload batch and GPU wait (the spray one a boat) and
// reached Scene from below app/.
#include <cstddef>
#include <memory>
#include <vector>

class RenderableObjectBase;

class RuntimeObjects
{
public:
    // A runtime object (no editor identity, never saved) into the world, initialized into the hand-over's one upload
    // batch -- it goes to the GPU, waited for, once the hand-over's objects are made (before anything draws them). The
    // object as it is in the world, or null: not made (no renderer, the batch could not start, its Init refused).
    virtual RenderableObjectBase* Spawn(std::unique_ptr<RenderableObjectBase> object) = 0;
    // Runtime objects out of the world now, their physics entries with them, their GPU resources once the frames in flight
    // are done (World::RemoveRuntimeObjects). How many went.
    virtual std::size_t Despawn(const std::vector<const RenderableObjectBase*>& gone) = 0;

protected:
    ~RuntimeObjects() = default; // the scene's, for the hand-over only: never deleted through this type
};
