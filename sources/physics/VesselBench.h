#pragma once

#include <functional>
#include <memory>

#include "core/math/Math.h"
#include "physics/PhysicsTypes.h" // VesselSettings

namespace physics
{
    class StepBody; // physics/PhysicsBody.h

    // One boat in a world of its own, no scene and no water but what its stepper does (architecture plan R14.3a): the boat
    // gate's bench, for a layer above the physics without Jolt. The hull: flat under, the keel at the origin, up to 2.7 m, 10 x
    // 2.34 m (the small_yacht's size), made a vessel's shape and body by its settings (MakeVesselShape, SetVesselBody), at
    // (0, y, 0) turned by `rotation`. Gravity -9.81 m/s2; a step is 1/60 s, one collision step, one thread.
    class VesselBench
    {
    public:
        VesselBench(const VesselSettings& settings, const Math::quat& rotation, float y);
        ~VesselBench();
        VesselBench(const VesselBench&) = delete;
        VesselBench& operator=(const VesselBench&) = delete;

        float FloatFactor() const; // what Jolt's buoyancy takes for the shape (MakeVesselShape)
        Math::float3 Gravity() const;
        void SetGravity(const Math::float3& gravity);
        // `push` with the body under its lock, then one step.
        void Step(const std::function<void(StepBody&)>& push);

        Math::float3 Position() const;
        Math::float3 CenterOfMass() const;
        Math::quat Rotation() const;
        Math::float3 Rotate(const Math::float3& v) const; // by the body's rotation, as the physics turns it
        Math::float3 LinearVelocity() const;
        Math::float3 AngularVelocity() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace physics
