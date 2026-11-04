#include "physics/PhysicsInternal.h"

#include <algorithm>

#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>

namespace physics
{
    namespace
    {
        constexpr float kHeight = 1.8f;
        constexpr float kRadius = 0.3f;
        constexpr float kJumpSpeed = 3.6f;
    }

    // Who may push whom. The character pushes a body only while the world runs: a body it shoved in the editor would keep the
    // impulse and burst out on the next Simulate.
    class WalkingCharacter::Listener final : public JPH::CharacterContactListener
    {
    public:
        bool canPushBodies = false;

        void OnContactAdded(const JPH::CharacterVirtual*, const JPH::CharacterContact&, JPH::CharacterContactSettings& ioSettings) override
        {
            ioSettings.mCanReceiveImpulses = canPushBodies;
        }
        void OnContactPersisted(const JPH::CharacterVirtual*, const JPH::CharacterContact&, JPH::CharacterContactSettings& ioSettings) override
        {
            ioSettings.mCanReceiveImpulses = canPushBodies;
        }
    };

    WalkingCharacter::WalkingCharacter(JPH::PhysicsSystem& system, JPH::Vec3Arg feet)
        : system_(system)
        , listener_(std::make_unique<Listener>())
    {
        // Its origin at the feet: a capsule standing on the point it is given.
        JPH::RefConst<JPH::Shape> capsule = new JPH::CapsuleShape(0.5f * (kHeight - 2.0f * kRadius), kRadius);
        JPH::RotatedTranslatedShapeSettings standing(JPH::Vec3(0.0f, 0.5f * kHeight, 0.0f), JPH::Quat::sIdentity(), capsule.GetPtr());
        standing.SetEmbedded();

        JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
        settings->mShape = standing.Create().Get();
        settings->mMaxSlopeAngle = JPH::DegreesToRadians(50.0f);
        settings->mMass = 70.0f;
        settings->mMaxStrength = 400.0f;
        settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -kRadius); // only what is under the capsule's lower sphere holds it up
        settings->mUp = JPH::Vec3::sAxisY();
        character_ = new JPH::CharacterVirtual(settings, feet, JPH::Quat::sIdentity(), 0, &system);
        character_->SetListener(listener_.get());
    }

    WalkingCharacter::~WalkingCharacter() = default;

    void WalkingCharacter::Update(float dt, JPH::Vec3Arg desiredVelocity, bool jump, bool canPushBodies, JPH::TempAllocator& temp)
    {
        listener_->canPushBodies = canPushBodies;
        character_->UpdateGroundVelocity();

        const JPH::Vec3 up = character_->GetUp();
        const JPH::Vec3 vertical = character_->GetLinearVelocity().Dot(up) * up;
        const JPH::Vec3 ground = character_->GetGroundVelocity();
        const bool towardGround = vertical.GetY() - ground.GetY() < 0.1f;
        JPH::Vec3 velocity = vertical;
        if (character_->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround && towardGround)
        {
            velocity = ground;
            if (jump) { velocity += kJumpSpeed * up; }
        }
        velocity += system_.GetGravity() * dt;
        velocity += desiredVelocity;
        character_->SetLinearVelocity(velocity);

        // The stairs and the stick to the ground (Jolt's defaults: a 40 cm step, 50 cm down) and back faces: a sheet of cloth stops the
        // character from behind too (CharacterVirtualSettings' default).
        JPH::CharacterVirtual::ExtendedUpdateSettings update;
        character_->ExtendedUpdate(dt, system_.GetGravity(), update,
            system_.GetDefaultBroadPhaseLayerFilter(layers::kMoving), system_.GetDefaultLayerFilter(layers::kMoving), {}, {}, temp);
    }

    JPH::Vec3 WalkingCharacter::Feet() const { return character_->GetPosition(); }

    void WalkingCharacter::SetFeet(JPH::Vec3Arg feet)
    {
        character_->SetPosition(feet);
        character_->SetLinearVelocity(JPH::Vec3::sZero());
    }

    bool WalkingCharacter::OnGround() const
    {
        return character_->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
    }
} // namespace physics
