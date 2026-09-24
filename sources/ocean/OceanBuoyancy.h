#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/math/Math.h"
#include "ocean/BuoyancyLayout.h"

class OceanRenderable;
class RenderableObject;
class RenderableObjectBase;
class Renderer;

// Floats "buoyant" objects on the ocean: heave, pitch and roll of a rigid body pushed by its
// pontoons (ocean/BuoyancyLayout.h) through the water surface the GPU sent back
// (ocean/OceanReadback.h). Yaw and horizontal position stay where the level put them -- the forces
// are vertical, and a moored boat that drifted off would be a level-design bug, not physics.
//
// NOTHING the GPU returns is applied directly. The copy is one to three frames old and arrives in
// steps, so each pontoon keeps the height it last read AND its rate of change and carries both
// forward to the current ocean time; the carried water then only FORCES a body with mass (the
// per-mesh "inertia", an added-mass multiplier), springs (the submerged pontoon volume) and
// damping. The pose is the integral of that, never a copy of a sample.
//
// The result is a world-space render offset on the object (RenderableObject::SetRenderOffset);
// its authored transform -- the editor's, the saved level's -- is never touched. Editing that
// transform (a gizmo drag) resets the body where it was put.
class OceanBuoyancy
{
public:
    // Once a frame from Scene::Tick: after the objects' Tick (the ocean clock is this frame's),
    // before their PostTick (which folds the offsets written here into the model matrices).
    // Works from the REGISTRY below, never a walk of the scene: a level where nothing floats pays one
    // empty check, and one where five boats float pays for five boats, not for 1400 objects.
    void Tick(OceanRenderable* ocean, Renderer& renderer, float deltaTime);

    // The floating objects: RenderableObject::SetBuoyant(true) registers, SetBuoyant(false) and the
    // destructor unregister. Thread-safe (a level load may build objects on workers).
    static void Register(RenderableObject* object);
    static void Unregister(RenderableObject* object);

    // World-space pontoons (xyz = centre, w = radius) of a floating object as they sit this frame.
    // False when the object is not floating. For the editor's overlay.
    bool GetWorldPontoons(const RenderableObject* object, std::vector<Math::float4>& out) const;
    size_t BodyCount() const { return bodies_.size(); }

private:
    struct Body
    {
        // What the body was built from; any change rebuilds it.
        bool built = false;
        bool valid = false;   // built, and the layout has pontoons that reach the water
        std::string geometry;
        std::uint32_t settingsRevision = 0;
        Math::mat4 authored;
        buoyancy::Layout layout;

        // The rest pose (the authored transform), world space.
        std::vector<Math::float3> restPoints;
        std::vector<float> radii;
        std::vector<float> restVolume;
        float totalRestVolume = 0.0f;
        Math::float3 comRest;
        Math::mat4 authoredRotation;      // rotation only: the body frame the inertia is diagonal in
        Math::float3 inertiaBody;         // per unit mass, x/y/z of the body frame, x inertia scale
        float lift = 0.0f;                // buoyant acceleration per m^3 submerged (g / rest volume)
        float dampingCoeff = 0.0f;        // heave damping per unit mass, spread over the pontoons

        // State.
        float heave = 0.0f;               // world y of the COM above its rest position
        float heaveVelocity = 0.0f;
        Math::float3 tilt;                // rotation vector about horizontal axes (y stays 0)
        Math::float3 angularVelocity;     // world, y stays 0

        // The water under each pontoon, carried forward between readbacks.
        std::vector<float> waterHeight;
        std::vector<float> waterRate;
        std::uint64_t waterFrame = 0;
        float waterTime = 0.0f;
    };

    bool Setup(Body& body, const RenderableObject& object, const OceanRenderable& ocean) const;
    void Step(Body& body, const OceanRenderable& ocean, float deltaTime) const;
    static Math::mat4 TiltMatrix(const Math::float3& tilt);
    static Math::mat4 RenderOffset(const Body& body);

    std::unordered_map<const RenderableObject*, Body> bodies_;
    // A different ocean (a level switch) drops every body: an object pointer reused by the new level
    // must not inherit the old one's water.
    const OceanRenderable* lastOcean_ = nullptr;
};
