#include "app/scene/PlayerSystem.h"

#include <algorithm>
#include <utility>

#include "core/logging/Log.h"
#include "physics/WaterSurface.h"

PlayerSystem::PlayerSystem(physics::World& world) : world_(world)
{
    physics::World::StepHooks hooks;
    hooks.frame = [this](float deltaTime, const physics::WaterSurface* water) { Frame(deltaTime, water); };
    hooks_ = world_.AddStepHooks(std::move(hooks));
}

PlayerSystem::~PlayerSystem()
{
    world_.RemoveCharacter(body_);
    world_.RemoveStepHooks(hooks_);
}

void PlayerSystem::Spawn(const Math::float3& feet)
{
    world_.RemoveCharacter(body_);
    body_ = physics::kNoCharacter;
    wanted_ = true;
    possessed_ = true;
    spawn_ = feet;
    eyeValid_ = false;
    input_ = physics::PlayerInput{};
}

void PlayerSystem::Remove()
{
    world_.RemoveCharacter(body_);
    body_ = physics::kNoCharacter;
    wanted_ = false;
    eyeValid_ = false;
    input_ = physics::PlayerInput{};
}

void PlayerSystem::SetPossessed(bool possessed)
{
    possessed_ = possessed;
    eyeValid_ = eyeValid_ && possessed;
}

void PlayerSystem::SetInput(const physics::PlayerInput& input)
{
    const bool jump = input_.jump || input.jump; // a press stays until a Tick has taken it
    input_ = input;
    input_.jump = jump;
}

bool PlayerSystem::Eye(Math::float3& out) const
{
    // A world gone (physics.enabled:0) took the capsule: no eye till it comes back.
    if (!eyeValid_ || !world_.HasCharacter(body_)) { return false; }
    out = eye_;
    return true;
}

physics::PlayerState PlayerSystem::State() const
{
    physics::PlayerState state;
    physics::CharacterState body;
    if (!world_.GetCharacter(body_, physics::GameSettings().player, body)) { return state; }
    state.exists = true;
    state.possessed = possessed_;
    state.onGround = body.onGround;
    state.crouched = body.crouched;
    state.mode = body.mode;
    state.speed = body.speed;
    state.feet = body.feet;
    return state;
}

void PlayerSystem::LevelLeft()
{
    world_.RemoveCharacter(body_);
    body_ = physics::kNoCharacter;
    eyeValid_ = false;
}

// The player, once a frame in the world's Tick (World::StepHooks::frame): the capsule where it comes in, this frame's keys,
// the water under its feet, the move on the frame's clock, physics.killY, the eye.
void PlayerSystem::Frame(float deltaTime, const physics::WaterSurface* surface)
{
    if (!wanted_) { return; }
    physics::Settings& settings = physics::GameSettings();
    physics::PlayerSettings& tuning = settings.player;
    if (!world_.HasCharacter(body_)) // none yet, or the world it stood in went (physics.enabled:0) and came back without it
    {
        body_ = world_.AddCharacter(spawn_, tuning);
        if (body_ == physics::kNoCharacter) { return; }
        LOG_INFO(logging::LogCategory::Physics, "player: a body at {:.2f} {:.2f} {:.2f}", spawn_.x, spawn_.y, spawn_.z);
    }
    physics::PlayerInput input = possessed_ ? input_ : physics::PlayerInput{};
    input_.jump = false;

    physics::CharacterState body;
    float water = physics::kNoWater;
    if (surface && surface->Present() && world_.GetCharacter(body_, tuning, body))
    {
        water = surface->HasWaves() ? surface->SampleHeight(body.feet.x, body.feet.z, 1.0f) : surface->Level();
    }
    world_.MoveCharacter(body_, std::clamp(deltaTime, 1.0e-4f, 1.0f / 30.0f), input, tuning, water);
    world_.GetCharacter(body_, tuning, body);
    // UE's KillZ, for the one thing nothing else stops: no ground and no water under it.
    if (body.feet.y < settings.killY)
    {
        LOG_WARNING(logging::LogCategory::Physics, "player: fell out of the world at {:.1f} {:.1f}, back at the start", body.feet.x, body.feet.z);
        world_.PlaceCharacter(body_, spawn_);
        world_.GetCharacter(body_, tuning, body);
    }
    eye_ = body.eye;
    eyeValid_ = possessed_;
}
