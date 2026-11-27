#pragma once
// Architecture plan R13.5: a boat's helm -- the first Interactable (a hull within a few metres of the player's feet: the
// interact key takes the helm) and the first Controllable (W S throttle, A D rudder, the mouse orbits the chase camera,
// V its view, X the anchor, B the race's autopilot, E over the side). PlaySession::TakeHelm / LeaveHelm / TickHelm and
// the helm's part of its HUD line, moved verbatim (docs/play_mode_plan.md).
#include <functional>
#include <string>

#include "app/game/Control.h"

namespace game
{
struct PlayWorld;

class BoatHelm final : public Controllable, public Interactable
{
public:
    // `world`: the race (its autopilot holds or drives the helm) and the helm's chase camera; `status`: the session's
    // status line (text, seconds).
    BoatHelm(PlayWorld& world, std::function<void(std::string, double)> status) : world_(world), status_(std::move(status)) {}

    const char* Name() const override { return "boat helm"; }

    // Controllable
    bool Take(Scene& scene, RenderableObject* vessel) override;
    std::optional<BodyPlacement> Leave(Scene& scene, bool overTheSide) override;
    void Forget(Scene& scene) override;
    Held Tick(InputManager& input, Scene& scene, bool controls, float deltaTime) override;
    std::wstring HudLine(const Scene& scene) const override;

    // Interactable: a hull within kHelmReach of the feet.
    RenderableObject* InReach(Scene& scene, const Math::float3& feet) const override;
    const wchar_t* Prompt() const override { return L"E TAKE THE HELM"; }
    Controllable& Controls() override { return *this; }

    static constexpr float kHelmReach = 3.0f; // m: the way aboard

private:
    PlayWorld& world_;
    std::function<void(std::string, double)> status_;
};
} // namespace game
