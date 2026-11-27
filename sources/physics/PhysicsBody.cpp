// Jolt's defines and flags come from the project's sources/physics/**/*.cpp item (no PCH).
#include "physics/PhysicsInternal.h" // Jolt, ToJolt / FromJolt
#include "physics/PhysicsBody.h"

#include <Jolt/Physics/Body/Body.h>

namespace physics
{
    Math::float3 StepBody::Position() const { return FromJolt(JPH::Vec3(body_->GetPosition())); }
    Math::quat StepBody::Rotation() const { return FromJolt(body_->GetRotation()); }
    Math::float3 StepBody::Rotate(const Math::float3& v) const { return FromJolt(body_->GetRotation() * ToJolt(v)); }
    Math::float3 StepBody::CenterOfMass() const { return FromJolt(JPH::Vec3(body_->GetCenterOfMassPosition())); }
    float StepBody::Mass() const
    {
        if (body_->IsStatic()) { return 0.0f; }
        const float inverse = body_->GetMotionProperties()->GetInverseMassUnchecked();
        return inverse > 0.0f ? 1.0f / inverse : 0.0f;
    }
    Math::float3 StepBody::LinearVelocity() const { return FromJolt(body_->GetLinearVelocity()); }
    Math::float3 StepBody::AngularVelocity() const { return FromJolt(body_->GetAngularVelocity()); }
    Math::float3 StepBody::PointVelocity(const Math::float3& point) const { return FromJolt(body_->GetPointVelocity(ToJolt(point))); }
    void StepBody::SetLinearVelocityClamped(const Math::float3& velocity) { body_->SetLinearVelocityClamped(ToJolt(velocity)); }
    void StepBody::SetAngularVelocityClamped(const Math::float3& velocity) { body_->SetAngularVelocityClamped(ToJolt(velocity)); }
    void StepBody::AddImpulse(const Math::float3& impulse) { body_->AddImpulse(ToJolt(impulse)); }
    void StepBody::AddImpulse(const Math::float3& impulse, const Math::float3& point) { body_->AddImpulse(ToJolt(impulse), ToJolt(point)); }
    void StepBody::AddAngularImpulse(const Math::float3& impulse) { body_->AddAngularImpulse(ToJolt(impulse)); }

    void StepBody::SubmergedVolume(const Math::float3& surface, const Math::float3& normal, float& total, float& submerged,
                                   Math::float3& centreOfBuoyancy) const
    {
        JPH::Vec3 relative = JPH::Vec3::sZero();
        body_->GetSubmergedVolume(ToJolt(surface), ToJolt(normal), total, submerged, relative);
        centreOfBuoyancy = FromJolt(relative);
    }

    void StepBody::ApplyBuoyancyImpulse(float total, float submerged, const Math::float3& centreOfBuoyancy, float buoyancy, float linearDrag,
                                        float angularDrag, const Math::float3& fluidVelocity, const Math::float3& gravity, float dt)
    {
        body_->ApplyBuoyancyImpulse(total, submerged, ToJolt(centreOfBuoyancy), buoyancy, linearDrag, angularDrag, ToJolt(fluidVelocity),
                                    ToJolt(gravity), dt);
    }

    Math::quat RotationAbout(const Math::float3& axis, float radians) { return FromJolt(JPH::Quat::sRotation(ToJolt(axis), radians)); }
} // namespace physics
