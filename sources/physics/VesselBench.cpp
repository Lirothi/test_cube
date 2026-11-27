#include "physics/PhysicsInternal.h"
#include "physics/VesselBench.h"
#include "physics/PhysicsBody.h"

#include <string>

namespace physics
{
    // The gates' TestWorld of one thread (PhysicsSelfTest.cpp), and a boat in it: the members in its order, so they go in its.
    struct VesselBench::Impl
    {
        LayerTables tables;
        JPH::PhysicsSystem system;
        JPH::TempAllocatorImpl temp{ 32u * 1024u * 1024u };
        JPH::JobSystemThreadPool jobs;
        JPH::BodyID body;
        float factor = 1.0f;
    };

    VesselBench::VesselBench(const VesselSettings& settings, const Math::quat& rotation, float y)
    {
        AcquireJolt();
        impl_ = std::make_unique<Impl>();
        Impl& m = *impl_;
        m.jobs.Init(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, 1);
        m.system.Init(4096, 0, 65536, 10240, m.tables.broadPhase, *m.tables.objectVsBroadPhase, m.tables.pairs);
        m.system.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
        // Flat under, the keel at the origin, its upper works up to 2.7 m: the small_yacht's size and no more of it.
        JPH::Array<JPH::Vec3> corners;
        for (const float cx : { -5.0f, 5.0f })
        {
            for (const float cy : { 0.0f, 2.7f })
            {
                for (const float cz : { -1.17f, 1.17f }) { corners.push_back(JPH::Vec3(cx, cy, cz)); }
            }
        }
        JPH::ConvexHullShapeSettings hull(corners, 0.0f);
        hull.SetEmbedded();
        const JPH::RefConst<JPH::Shape> shape = hull.Create().Get();
        std::string error;
        const JPH::RefConst<JPH::Shape> floating = MakeVesselShape(*shape, settings, Math::float3(1.0f, 1.0f, 1.0f), m.factor, error);
        JPH::BodyCreationSettings creation(floating.GetPtr(), JPH::Vec3(0.0f, y, 0.0f), ToJolt(rotation), JPH::EMotionType::Dynamic, layers::kMoving);
        SetVesselBody(creation, settings);
        m.body = m.system.GetBodyInterface().CreateAndAddBody(creation, JPH::EActivation::Activate);
    }

    VesselBench::~VesselBench()
    {
        impl_.reset();
        ReleaseJolt();
    }

    float VesselBench::FloatFactor() const { return impl_->factor; }
    Math::float3 VesselBench::Gravity() const { return FromJolt(impl_->system.GetGravity()); }
    void VesselBench::SetGravity(const Math::float3& gravity) { impl_->system.SetGravity(ToJolt(gravity)); }

    void VesselBench::Step(const std::function<void(StepBody&)>& push)
    {
        Impl& m = *impl_;
        {
            JPH::BodyLockWrite lock(m.system.GetBodyLockInterface(), m.body);
            StepBody body(lock.GetBody());
            push(body);
        }
        m.system.Update(1.0f / 60.0f, 1, &m.temp, &m.jobs);
    }

    Math::float3 VesselBench::Position() const { return FromJolt(JPH::Vec3(impl_->system.GetBodyInterface().GetPosition(impl_->body))); }
    Math::float3 VesselBench::CenterOfMass() const { return FromJolt(JPH::Vec3(impl_->system.GetBodyInterface().GetCenterOfMassPosition(impl_->body))); }
    Math::quat VesselBench::Rotation() const { return FromJolt(impl_->system.GetBodyInterface().GetRotation(impl_->body)); }
    Math::float3 VesselBench::Rotate(const Math::float3& v) const { return FromJolt(impl_->system.GetBodyInterface().GetRotation(impl_->body) * ToJolt(v)); }
    Math::float3 VesselBench::LinearVelocity() const { return FromJolt(impl_->system.GetBodyInterface().GetLinearVelocity(impl_->body)); }
    Math::float3 VesselBench::AngularVelocity() const { return FromJolt(impl_->system.GetBodyInterface().GetAngularVelocity(impl_->body)); }
} // namespace physics
