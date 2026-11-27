#pragma once

#include "core/math/Math.h"

namespace JPH { class Body; }

namespace physics
{
    // A body as the fixed step reaches it (architecture plan R14.2): the engine's types, no Jolt, so code above the physics
    // -- a boat's engine, its anchor -- can push a body between the steps. Handed out for the length of one call, on the
    // step's thread (the game's when the world steps in line), under the body's lock: never kept. Position and centre of
    // mass are world points; impulses are N s, at a world point or at the centre of mass.
    class StepBody
    {
    public:
        explicit StepBody(JPH::Body& body) : body_(&body) {}

        Math::float3 Position() const;     // the body's origin: the mesh's
        Math::quat Rotation() const;
        Math::float3 Rotate(const Math::float3& v) const; // Rotation() applied to `v`, as the physics turns it
        Math::float3 CenterOfMass() const;
        float Mass() const;                // kg; 0 for a body that does not move
        Math::float3 LinearVelocity() const;
        Math::float3 AngularVelocity() const;
        Math::float3 PointVelocity(const Math::float3& point) const;
        void SetLinearVelocityClamped(const Math::float3& velocity);
        void SetAngularVelocityClamped(const Math::float3& velocity);
        void AddImpulse(const Math::float3& impulse);
        void AddImpulse(const Math::float3& impulse, const Math::float3& point);
        void AddAngularImpulse(const Math::float3& impulse);
        // The shape's volume, and under a plane through `surface` with `normal` (Jolt's GetSubmergedVolume): the centre of
        // buoyancy relative to the centre of mass.
        void SubmergedVolume(const Math::float3& surface, const Math::float3& normal, float& total, float& submerged,
                             Math::float3& centreOfBuoyancy) const;
        // Jolt's buoyancy impulse over those volumes: `buoyancy` the share of the weight the whole volume carries.
        void ApplyBuoyancyImpulse(float total, float submerged, const Math::float3& centreOfBuoyancy, float buoyancy, float linearDrag,
                                  float angularDrag, const Math::float3& fluidVelocity, const Math::float3& gravity, float dt);

    private:
        JPH::Body* body_;
    };

    // The rotation by `radians` about `axis` (unit), as the physics makes it (Jolt's Quat::sRotation): a gate that wants the
    // physics' very numbers turns with this.
    Math::quat RotationAbout(const Math::float3& axis, float radians);
} // namespace physics
