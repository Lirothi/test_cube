#pragma once
// Architecture plan R13.5 (docs/architecture_review_plan.md): what the player takes over and steers, and what within their
// reach the interact key does something with. A boat's helm is the first of each (game::BoatHelm); the play session
// knows these two faces, not the boat -- the next thing a player sits in or picks up is one class more, not a branch
// more in PlaySession::Tick.
#include <optional>
#include <string>

#include "core/math/Math.h"

class InputManager;
class RenderableObject;
class Scene;

namespace game
{
// Where the player's body goes when they let go of what they controlled: their feet, and the camera's yaw.
struct BodyPlacement
{
    Math::float3 feet;
    float cameraYawRad = 0.0f;
};

// What the player takes over and steers. While they hold it their body is out of the world, and the session hands it
// the keys every frame.
class Controllable
{
public:
    enum class Held : unsigned char
    {
        Yes,   // still held
        LetGo, // the player let go (its key): the session lets go of it over the side
        Gone,  // it is gone (with its level, physics.enabled:0): nothing to let go of
    };

    virtual const char* Name() const = 0;
    // The player takes `target` (what an Interactable found within reach). False: it could not be taken.
    virtual bool Take(Scene& scene, RenderableObject* target) = 0;
    // The player lets go. `overTheSide`: they step off it, and where their body goes comes back; otherwise (Stop) none.
    virtual std::optional<BodyPlacement> Leave(Scene& scene, bool overTheSide) = 0;
    // Its level went: let go of it without a word (the session goes on in the next level, or ends there).
    virtual void Forget(Scene& scene) = 0;
    // Every frame while it is held; the keys count only with `controls`.
    virtual Held Tick(InputManager& input, Scene& scene, bool controls, float deltaTime) = 0;
    // The HUD's line while it is held (the session adds its own keys after it).
    virtual std::wstring HudLine(const Scene& scene) const = 0;

protected:
    ~Controllable() = default;
};

// What within the player's reach the interact key does something with: it hands the player something to control.
class Interactable
{
public:
    virtual const char* Name() const = 0;
    // What is within reach of the player's feet now, or null.
    virtual RenderableObject* InReach(Scene& scene, const Math::float3& feet) const = 0;
    // The HUD's prompt while something is within reach.
    virtual const wchar_t* Prompt() const = 0;
    // What the key hands the player (Controllable::Take with what InReach found).
    virtual Controllable& Controls() = 0;

protected:
    ~Interactable() = default;
};
} // namespace game
