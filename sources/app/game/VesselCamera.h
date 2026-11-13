#pragma once

#include "physics/PhysicsWorld.h"

class Camera;

// The camera of a boat: at the helm, and the spectator's on a bot's boat (race.watch). Set at the hand-over (Scene::Sync),
// from the very pose the frame draws the hull at: read a step earlier a boat at speed shakes against the camera by a fifth of
// a metre. docs/play_mode_plan.md.
//   Chase   behind it, its horizon held still while the hull rides the swell; the mouse orbits it (at the helm).
//   Helm    the helmsman's own eyes on the hull, its pitch and roll with it.
//   Cinema  a director (owner, 2026-11-13: "camera modes -- third person, from the cockpit, a cinema camera"): shots that
//           cut every few seconds -- a standing camera the boat storms past, tracking abeam, a drone over the wake, low
//           over the water behind, ahead of the bow looking back.
namespace game
{
    // A boat as the camera needs it: its settings (the eye at the helm, the chase distance), where it is, which way its bow
    // and its deck point, its scale and speed.
    struct VesselPose
    {
        const physics::VesselSettings* settings = nullptr;
        Math::float3 position;
        Math::float3 forward; // world, unit: the bow's way
        Math::float3 up;      // world, unit: the deck's up
        Math::float3 scale = Math::float3(1.0f, 1.0f, 1.0f);
        float speed = 0.0f;   // m/s along the hull
    };
    VesselPose PoseOf(const physics::HelmState& helm);
    VesselPose PoseOf(const physics::VesselHull& hull);

    class VesselCamera
    {
    public:
        enum class View { Chase, Helm, Cinema };
        static const char* ViewName(View view); // "behind", "cockpit", "cinema"

        bool Active() const { return active_; }
        View GetView() const { return view_; }
        void Begin();
        void End() { active_ = false; }
        void SetView(View view);
        void NextView() { SetView(view_ == View::Chase ? View::Helm : view_ == View::Helm ? View::Cinema : View::Chase); }
        // Once a frame, before the hand-over: the mouse this frame (radians) and the frame's clock.
        void Tick(float lookYaw, float lookPitch, float speed, float scriptOrbit, float scriptOrbitPitch, float deltaTime);
        // At the hand-over. `waterLevel`: the sea's still level (a camera stays over it).
        void Apply(Camera& camera, const VesselPose& pose, float waterLevel);

    private:
        // Cinema: a shot's kind and its numbers, chosen at its cut.
        enum class Shot : unsigned char { Pass, Abeam, Drone, Low, Bow, Count };
        void NewShot(Camera& camera, const VesselPose& pose, float waterLevel);
        void ApplyCinema(Camera& camera, const VesselPose& pose, float waterLevel);

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

        // Cinema
        bool cinemaFresh_ = true; // the next Apply cuts to a new shot
        Shot shot_ = Shot::Pass;
        float shotTime_ = 0.0f;   // s into the shot
        float shotLength_ = 6.0f;
        float side_ = 1.0f;       // +1 starboard, -1 port: which side the shot stands on
        float orbit_ = 0.0f;      // the drone's slow turn about the boat
        unsigned seed_ = 12345u;
        Math::float3 eye_;        // where the camera stands (a pass) or is going (the followers, smoothed)
        Math::float3 look_;       // where it looks, smoothed: a wave's heave does not shake the frame
    };
}
