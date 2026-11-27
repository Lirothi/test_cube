#include "physics/PhysicsInternal.h"

#include <algorithm>
#include <cmath>

#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>

// The boat's body (architecture plan R14.3b): its shape over the hull and what it is made with. The boat itself -- the
// water under it, the helm, the hull and the waves -- is app/scene/VesselModel.cpp, run by app/scene/VesselSystem.
namespace physics
{
    JPH::RefConst<JPH::Shape> MakeVesselShape(const JPH::Shape& hull, const VesselSettings& settings, const Math::float3& scale, float& floatFactor, std::string& error)
    {
        if (hull.MustBeStatic())
        {
            error = "a triangle mesh has no volume to float on";
            return nullptr;
        }
        // Where still water cuts the hull, in the frame of its centre of mass, upright.
        const JPH::Vec3 centre = hull.GetCenterOfMass();
        const JPH::Plane surface = JPH::Plane::sFromPointAndNormal(JPH::Vec3(0.0f, settings.draft * scale.y - centre.GetY(), 0.0f), JPH::Vec3::sAxisY());
        float total = 0.0f;
        float under = 0.0f;
        JPH::Vec3 centerOfBuoyancy = JPH::Vec3::sZero();
        hull.GetSubmergedVolume(JPH::Mat44::sIdentity(), JPH::Vec3::sOne(), surface, total, under, centerOfBuoyancy JPH_IF_DEBUG_RENDERER(, JPH::RVec3::sZero()));
        if (total <= 0.0f || under <= 1.0e-4f)
        {
            error = "the draft does not reach the hull";
            return nullptr;
        }
        // Jolt's buoyancy is a share of the shape's whole volume, not a density: it balances the weight with total / factor under water.
        floatFactor = total / under;
        // A uniform hull with its superstructure would roll over and sit by the stern: the weight is low, and over the water's lift.
        JPH::OffsetCenterOfMassShapeSettings moved(JPH::Vec3(centerOfBuoyancy.GetX(), settings.centerOfMassY * scale.y - centre.GetY(), centerOfBuoyancy.GetZ()), &hull);
        moved.SetEmbedded();
        const JPH::ShapeSettings::ShapeResult result = moved.Create();
        if (result.HasError())
        {
            error = result.GetError().c_str();
            return nullptr;
        }
        return result.Get();
    }

    void SetVesselBody(JPH::BodyCreationSettings& creation, const VesselSettings& settings)
    {
        creation.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        creation.mMassPropertiesOverride.mMass = settings.mass;
        creation.mAllowSleeping = false; // waves move under it
        creation.mLinearDamping = 0.0f;  // the water's hold is PushVessel's
        creation.mAngularDamping = 0.05f;
    }

} // namespace physics
