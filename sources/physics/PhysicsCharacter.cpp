#include "physics/PhysicsInternal.h"

#include <algorithm>
#include <cmath>

#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>

namespace physics
{
    namespace
    {
        constexpr float kJumpBuffer = 0.1f;   // a jump pressed this long before landing still jumps
        constexpr float kCoyoteTime = 0.1f;   // and this long after walking off an edge
        constexpr float kStepEyeTau = 0.08f;  // a step up, for the eye
        constexpr float kCrouchEyeTau = 0.08f;
        constexpr float kSwimFriction = 2.0f;
        constexpr float kSwimBraking = 4.0f;
        constexpr float kSwimAccelerationShare = 0.4f;
        constexpr float kSwimEyeOverWater = 0.15f;
        constexpr float kSwimHysteresis = 0.05f;
        constexpr float kSwimFollowTau = 0.25f; // how fast it follows the swell
        constexpr float kSwimVerticalTau = 0.15f; // how fast a fall into the water is taken out
        constexpr float kSwimMaxVertical = 3.0f;
        constexpr float kBrakeToStop = 0.1f;    // UE's BRAKE_TO_STOP_VELOCITY, 10 cm/s
        constexpr float kOverMaxSpeed = 1.01f;  // UE's IsExceedingMaxSpeed

        JPH::RefConst<JPH::Shape> MakeCapsule(float height, float radius)
        {
            const float cylinder = std::max(0.5f * (height - 2.0f * radius), 0.01f);
            JPH::RefConst<JPH::Shape> capsule = new JPH::CapsuleShape(cylinder, radius);
            JPH::RotatedTranslatedShapeSettings atTheFeet(JPH::Vec3(0.0f, cylinder + radius, 0.0f), JPH::Quat::sIdentity(), capsule.GetPtr());
            atTheFeet.SetEmbedded();
            return atTheFeet.Create().Get();
        }

        // UE's ApplyVelocityBraking (CharacterMovementComponent.cpp:4377-4427): friction and a constant deceleration, in substeps so a low frame rate brakes alike.
        JPH::Vec3 ApplyBraking(JPH::Vec3 velocity, float friction, float braking, float dt)
        {
            if (velocity.IsNearZero() || (friction <= 0.0f && braking <= 0.0f)) { return velocity; }
            const JPH::Vec3 old = velocity;
            const JPH::Vec3 deceleration = braking > 0.0f ? -braking * velocity.Normalized() : JPH::Vec3::sZero();
            constexpr float kMaxTimeStep = 1.0f / 33.0f; // UE's BrakingSubStepTime
            float remaining = dt;
            while (remaining >= 1.0e-6f)
            {
                const float step = (remaining > kMaxTimeStep && friction > 0.0f) ? std::min(kMaxTimeStep, remaining * 0.5f) : remaining;
                remaining -= step;
                velocity = velocity + (-friction * velocity + deceleration) * step;
                if (velocity.Dot(old) <= 0.0f) { return JPH::Vec3::sZero(); }
            }
            if (braking > 0.0f && velocity.LengthSq() <= kBrakeToStop * kBrakeToStop) { return JPH::Vec3::sZero(); }
            return velocity;
        }

        // UE's CalcVelocity (CharacterMovementComponent.cpp:3863-3950), along the ground. `wish` has a length up to 1, UE's Acceleration / MaxAcceleration.
        JPH::Vec3 CalcVelocity(JPH::Vec3 velocity, JPH::Vec3Arg wish, float maxSpeed, float friction, float braking, float acceleration, float dt)
        {
            const float wishLength = wish.Length();
            const bool hasWish = wishLength > 1.0e-4f;
            const bool overMax = velocity.LengthSq() > (maxSpeed * kOverMaxSpeed) * (maxSpeed * kOverMaxSpeed);
            if (!hasWish || overMax)
            {
                const JPH::Vec3 old = velocity;
                velocity = ApplyBraking(velocity, friction, braking, dt);
                // Braking does not take it under the limit it started over, if it is still going that way.
                if (overMax && velocity.LengthSq() < maxSpeed * maxSpeed && wish.Dot(old) > 0.0f) { velocity = old.Normalized() * maxSpeed; }
            }
            else
            {
                // Friction is what turns it: the velocity swings toward where the keys point.
                const JPH::Vec3 direction = wish / wishLength;
                velocity = velocity - (velocity - direction * velocity.Length()) * std::min(dt * friction, 1.0f);
            }
            if (hasWish)
            {
                const float inputSpeed = maxSpeed * std::min(wishLength, 1.0f);
                const float limit = velocity.LengthSq() > (inputSpeed * kOverMaxSpeed) * (inputSpeed * kOverMaxSpeed) ? velocity.Length() : inputSpeed;
                velocity = velocity + wish * (acceleration * dt);
                if (velocity.LengthSq() > limit * limit) { velocity = velocity.Normalized() * limit; }
            }
            return velocity;
        }
    }

    const char* PlayerModeName(PlayerMode mode)
    {
        switch (mode)
        {
        case PlayerMode::Walking: return "walking";
        case PlayerMode::Swimming: return "swimming";
        default: return "falling";
        }
    }

    PlayerBody::PlayerBody(JPH::PhysicsSystem& system, JPH::Vec3Arg feet, const PlayerSettings& settings)
        : system_(system)
        , standing_(MakeCapsule(settings.height, settings.radius))
        , crouching_(MakeCapsule(std::min(settings.crouchHeight, settings.height), settings.radius))
        , eyeFeetY_(feet.GetY())
    {
        JPH::Ref<JPH::CharacterVirtualSettings> character = new JPH::CharacterVirtualSettings();
        character->mShape = standing_;
        character->mMaxSlopeAngle = JPH::DegreesToRadians(settings.maxSlopeDeg);
        character->mMass = 70.0f;
        character->mMaxStrength = 400.0f;
        character->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -settings.radius); // only what is under the capsule's lower sphere holds it up
        character->mUp = JPH::Vec3::sAxisY();
        character_ = new JPH::CharacterVirtual(character, feet, JPH::Quat::sIdentity(), 0, &system);
    }

    PlayerBody::~PlayerBody() = default;

    void PlayerBody::Update(float dt, const PlayerInput& input, const PlayerSettings& settings, float waterHeight, JPH::TempAllocator& temp)
    {
        const auto broadPhaseFilter = system_.GetDefaultBroadPhaseLayerFilter(layers::kMoving);
        const auto layerFilter = system_.GetDefaultLayerFilter(layers::kMoving);
        const JPH::Vec3 up = character_->GetUp();
        const JPH::Vec3 gravity = system_.GetGravity();
        const JPH::Vec3 before = character_->GetPosition();

        character_->SetMaxSlopeAngle(JPH::DegreesToRadians(settings.maxSlopeDeg));
        character_->UpdateGroundVelocity();
        const JPH::Vec3 ground = character_->GetGroundVelocity();
        const JPH::Vec3 groundFlat(ground.GetX(), 0.0f, ground.GetZ());

        // The water: deep enough, and it floats.
        const float depth = waterHeight - before.GetY();
        const float swimFeetDepth = std::max(settings.eyeHeight - kSwimEyeOverWater, settings.swimDepth + 2.0f * kSwimHysteresis);
        if (swimming_) { swimming_ = depth >= settings.swimDepth; }
        else { swimming_ = depth > settings.swimDepth + kSwimHysteresis; }

        // Crouch. Getting up can be refused, by what is overhead: then it stays down and asks again next frame.
        const bool wantCrouch = input.crouch && !swimming_;
        if (wantCrouch != crouched_)
        {
            const JPH::Shape* shape = wantCrouch ? crouching_.GetPtr() : standing_.GetPtr();
            if (character_->SetShape(shape, 1.5f * system_.GetPhysicsSettings().mPenetrationSlop, broadPhaseFilter, layerFilter, {}, {}, temp))
            {
                crouched_ = wantCrouch;
            }
        }

        // On the ground: it holds it up, and it is not leaving it.
        const bool supported = character_->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
        const bool onGround = supported && !swimming_ && vertical_ - ground.GetY() < 0.1f;
        if (onGround && !onGround_) { horizontal_ = horizontal_ - groundFlat; }    // landed: from here on over this ground
        if (!onGround && onGround_) { horizontal_ = horizontal_ + lastGroundFlat_; } // left it, at its speed
        onGround_ = onGround;
        lastGroundFlat_ = groundFlat;

        sinceGround_ = onGround ? 0.0f : sinceGround_ + dt;
        sinceJumpPress_ = input.jump ? 0.0f : sinceJumpPress_ + dt;

        // Along the ground.
        const JPH::Vec3 wish(input.move.x, 0.0f, input.move.z);
        if (swimming_)
        {
            horizontal_ = CalcVelocity(horizontal_, wish, settings.swimSpeed, kSwimFriction, kSwimBraking, settings.acceleration * kSwimAccelerationShare, dt);
        }
        else if (onGround)
        {
            const float speed = crouched_ ? settings.crouchSpeed : (input.sprint ? settings.sprintSpeed : settings.walkSpeed);
            horizontal_ = CalcVelocity(horizontal_, wish, speed, settings.friction, settings.braking, settings.acceleration, dt);
        }
        else
        {
            // UE's PhysFalling: no friction, no braking, a share of the acceleration; what it took off with it keeps.
            const float speed = crouched_ ? settings.crouchSpeed : settings.walkSpeed;
            horizontal_ = CalcVelocity(horizontal_, wish, speed, 0.0f, 0.0f, settings.acceleration * settings.airControl, dt);
        }

        // Up and down.
        if (swimming_)
        {
            const float wanted = std::clamp((waterHeight - swimFeetDepth - before.GetY()) / kSwimFollowTau, -kSwimMaxVertical, kSwimMaxVertical);
            vertical_ += (wanted - vertical_) * (1.0f - std::exp(-dt / kSwimVerticalTau));
        }
        else
        {
            if (onGround) { vertical_ = ground.GetY(); }
            if (sinceJumpPress_ < kJumpBuffer && sinceGround_ < kCoyoteTime && !crouched_)
            {
                vertical_ = ground.GetY() + settings.jumpSpeed;
                sinceGround_ = kCoyoteTime;     // one jump
                sinceJumpPress_ = kJumpBuffer;  // for one press
            }
            vertical_ += gravity.GetY() * dt;
        }

        const JPH::Vec3 flat = onGround_ ? horizontal_ + groundFlat : horizontal_;
        character_->SetLinearVelocity(JPH::Vec3(flat.GetX(), vertical_, flat.GetZ()));

        if (swimming_)
        {
            character_->Update(dt, gravity, broadPhaseFilter, layerFilter, {}, {}, temp);
        }
        else
        {
            // The stairs, and the stick to the ground on the way down (Jolt's 50 cm). A sheet of cloth stops the character from behind too (CharacterVirtualSettings' default).
            JPH::CharacterVirtual::ExtendedUpdateSettings update;
            update.mWalkStairsStepUp = settings.stepHeight * up;
            character_->ExtendedUpdate(dt, gravity, update, broadPhaseFilter, layerFilter, {}, {}, temp);
        }

        // Jolt never takes anything out of the velocity it is given (UE: "make velocity reflect actual move", PhysWalking). What a wall stopped must not stay in it, or a jump
        // at a wall flies off over its top at full speed; a head against a ceiling starts to fall. Only what was really stopped: a walkable slope also moves less along the
        // ground than asked, and the walk must not take that out of the speed, each frame again.
        const JPH::Vec3 moved = (character_->GetPosition() - before) / dt;
        const float wantedLength = flat.Length();
        if (wantedLength > 1.0e-3f && moved.Dot(flat) / wantedLength < 0.5f * wantedLength)
        {
            for (const JPH::CharacterContact& contact : character_->GetActiveContacts())
            {
                if (!contact.mHadCollision || contact.mWasDiscarded || !character_->IsSlopeTooSteep(contact.mSurfaceNormal)) { continue; }
                JPH::Vec3 normal = contact.mContactNormal - contact.mContactNormal.Dot(up) * up;
                const float lengthSq = normal.LengthSq();
                if (lengthSq < 1.0e-6f) { continue; }
                normal = normal / std::sqrt(lengthSq);
                const float into = horizontal_.Dot(normal);
                if (into < 0.0f) { horizontal_ = horizontal_ - into * normal; }
            }
        }
        if (vertical_ > 0.0f && moved.GetY() < 0.5f * vertical_)
        {
            for (const JPH::CharacterContact& contact : character_->GetActiveContacts())
            {
                if (contact.mHadCollision && !contact.mWasDiscarded && contact.mContactNormal.GetY() < -0.1f)
                {
                    vertical_ = 0.0f;
                    break;
                }
            }
        }

        // The eye: a step up eases in, everything else it follows at once.
        const float feetY = character_->GetPosition().GetY();
        if (onGround_ && feetY > eyeFeetY_) { eyeFeetY_ += (feetY - eyeFeetY_) * (1.0f - std::exp(-dt / kStepEyeTau)); }
        else { eyeFeetY_ = feetY; }
        crouchShare_ += ((crouched_ ? 1.0f : 0.0f) - crouchShare_) * (1.0f - std::exp(-dt / kCrouchEyeTau));
    }

    JPH::Vec3 PlayerBody::Feet() const { return character_->GetPosition(); }

    void PlayerBody::SetFeet(JPH::Vec3Arg feet)
    {
        character_->SetPosition(feet);
        character_->SetLinearVelocity(JPH::Vec3::sZero());
        horizontal_ = JPH::Vec3::sZero();
        vertical_ = 0.0f;
        eyeFeetY_ = feet.GetY();
        onGround_ = false;
        swimming_ = false;
    }

    JPH::Vec3 PlayerBody::Eye(const PlayerSettings& settings) const
    {
        const JPH::Vec3 feet = character_->GetPosition();
        const float height = settings.eyeHeight + (settings.crouchEyeHeight - settings.eyeHeight) * crouchShare_;
        return JPH::Vec3(feet.GetX(), eyeFeetY_ + height, feet.GetZ());
    }

    PlayerMode PlayerBody::Mode() const
    {
        if (swimming_) { return PlayerMode::Swimming; }
        return onGround_ ? PlayerMode::Walking : PlayerMode::Falling;
    }

    float PlayerBody::HorizontalSpeed() const { return horizontal_.Length(); }
} // namespace physics
