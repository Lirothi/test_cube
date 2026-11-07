#pragma once

#include "physics/PhysicsWorld.h"

class Camera;

// The camera of a boat at the helm: a chase camera behind it, its horizon held still while the hull rides the swell, or the helmsman's own eyes on the hull. Set at the hand-over (Scene::Sync), from
// the very pose the frame draws the hull at: read a step earlier a boat at speed shakes against the camera by a fifth of a metre. docs/play_mode_plan.md.
namespace game
{
    class VesselCamera
    {
    public:
        enum class View { Chase, Helm };

        bool Active() const { return active_; }
        View GetView() const { return view_; }
        void Begin();
        void End() { active_ = false; }
        void ToggleView() { view_ = view_ == View::Chase ? View::Helm : View::Chase; orbitYaw_ = 0.0f; orbitPitch_ = 0.0f; }
        // Once a frame, before the hand-over: the mouse this frame (radians) and the frame's clock.
        void Tick(float lookYaw, float lookPitch, float speed, float scriptOrbit, float scriptOrbitPitch, float deltaTime);
        // At the hand-over.
        void Apply(Camera& camera, const physics::HelmState& helm, float waterLevel);

    private:
        View view_ = View::Chase;
        bool active_ = false;
        bool first_ = true;
        float dt_ = 0.0f;
        float heading_ = 0.0f;   // the hull's, followed slowly: a turn swings the view out, it does not whip it
        float targetY_ = 0.0f;   // the same for its height: the swell moves the hull in the frame, not the horizon
        float orbitYaw_ = 0.0f;  // the mouse's, about the hull
        float orbitPitch_ = 0.0f;
        float sinceLook_ = 0.0f;
        float pull_ = 0.0f;      // m further back with the speed
    };
}
