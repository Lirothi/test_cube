#pragma once

#include "core/math/Math.h"
#include "physics/PhysicsWorld.h" // World, CharacterKey, PlayerInput, PlayerState

// The player (UE's character on its capsule; docs/play_mode_plan.md, docs/agents/play_and_physics.md): where it comes in,
// the keys as they reach it, the water under its feet, physics.killY, where its eye stands -- the camera in Play. Moved out
// of physics::World by architecture plan R14.6: the physics keeps the capsule (World::AddCharacter: UE's movement over
// Jolt's CharacterVirtual) and runs this system's frame hook (World::StepHooks::frame) once a frame in its Tick -- on the
// frame's clock, not the fixed step: it is the camera, and a camera moves every frame. Owned by the scene's world
// (app/scene/World, which puts the camera at the eye after the Tick), beside the boats; a gate makes its own.
//
// Threads: the game thread's, all of it.
class PlayerSystem
{
public:
    explicit PlayerSystem(physics::World& world);
    ~PlayerSystem();
    PlayerSystem(const PlayerSystem&) = delete;
    PlayerSystem& operator=(const PlayerSystem&) = delete;

    // A body from `feet` on, falling if there is nothing under them; below physics.killY it comes back there. A world gone
    // (physics.enabled:0) brings it back with the world. Possessed, no key held.
    void Spawn(const Math::float3& feet);
    void Remove();
    // False = F8: the body stands where it is, takes no keys, and Eye says nothing.
    void SetPossessed(bool possessed);
    // What the keys say this frame; a jump's press stays until a Tick has taken it.
    void SetInput(const physics::PlayerInput& input);
    // Where the eye stands after this frame's Tick. False = no player, or not possessed: the camera is free.
    bool Eye(Math::float3& out) const;
    physics::PlayerState State() const;
    // The level that left (JsonLevel, the end of a load): the capsule stood on it and goes. The play session says where the
    // next one starts (AppController::OnLevelLoaded); till then it comes back where it last came in.
    void LevelLeft();

private:
    void Frame(float deltaTime, const physics::WaterSurface* surface);

    physics::World& world_;
    int hooks_ = 0;
    physics::CharacterKey body_ = physics::kNoCharacter;
    bool wanted_ = false;  // spawned and not removed: a capsule gone with its world comes back
    bool possessed_ = true;
    Math::float3 spawn_;
    physics::PlayerInput input_;
    Math::float3 eye_;
    bool eyeValid_ = false;
};
