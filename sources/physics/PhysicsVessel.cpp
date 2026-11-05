#include "physics/PhysicsInternal.h"

#include <algorithm>
#include <cmath>

#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>

// A powered boat on the waves (docs/play_mode_plan.md): every step, what the sea and the helm do to a Dynamic body. Jolt's exact buoyancy over the plane that fits the water under the hull holds it
// up at the draft the asset names; a propeller pushes at the stern and comes out of the water on a crest's back; a rudder needs water moving past it; the hull slides easily ahead and hardly at all
// sideways, climbs over its hump and planes; the surface's slope pushes it down the slope, and differently at the bow and the stern; what falls into the water slams. The damping is against the
// WATER's own motion, not against the world, so a boat rides a swell instead of fighting it. In the hull's frame: u along the bow, sway to starboard, the yaw rate about its up. Numbers cross as they
// are: a positive yaw turns to starboard (the level's yaw).
namespace physics
{
    namespace
    {
        constexpr float kPropellerBite = 0.1f;    // m of water over the propeller at which it has its full thrust
        constexpr float kStationAlong = 0.7f;     // of the half length: where the stern's and the bow's stations stand
        constexpr float kSampleAcross = 0.8f;     // of the half beam
        constexpr float kRateTau = 0.08f;         // s: the water's heights come from readbacks at 30 Hz, its rates are followed, not differenced
        constexpr float kMaxSurfaceSpeed = 8.0f;  // m/s: a jump (the first copy, the preset changed) is no wave
        constexpr float kMaxSurfaceSpin = 2.0f;   // rad/s
        constexpr float kSlamFrom = 0.5f;         // m/s: slower than this a hull just settles
        constexpr float kImpactFade = 0.25f;      // s
        constexpr float kCalmTau = 0.3f;          // s: VesselWater::calmAcross

        float MoveToward(float value, float target, float step)
        {
            if (value < target) { return std::min(value + step, target); }
            return std::max(value - step, target);
        }

        float SmoothStep(float from, float to, float x)
        {
            if (to <= from) { return x >= to ? 1.0f : 0.0f; }
            const float t = std::clamp((x - from) / (to - from), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }
    }

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

    void MeasureVesselWater(VesselWater& water, const std::function<float(float, float)>& height, JPH::RVec3Arg origin, JPH::RVec3Arg centre, JPH::QuatArg rotation,
        const VesselSettings& settings, const Math::float3& scale, float halfLength, float halfBeam, float elapsed)
    {
        const JPH::Vec3 heading = rotation * ToJolt(settings.forward);
        const JPH::Vec3 forward = JPH::Vec3(heading.GetX(), 0.0f, heading.GetZ()).NormalizedOr(JPH::Vec3::sAxisX());
        const JPH::Vec3 starboard = JPH::Vec3::sAxisY().Cross(forward);
        const float along = std::max(kStationAlong * halfLength, 0.05f);
        const float across = std::max(kSampleAcross * halfBeam, 0.05f);

        // Stern, middle, bow by port, middle, starboard.
        float grid[3][3];
        float mean[3];
        for (int i = 0; i < 3; ++i)
        {
            mean[i] = 0.0f;
            for (int j = 0; j < 3; ++j)
            {
                const JPH::Vec3 point = centre + forward * (along * static_cast<float>(i - 1)) + starboard * (across * static_cast<float>(j - 1));
                grid[i][j] = height(point.GetX(), point.GetZ());
                mean[i] += grid[i][j] / 3.0f;
            }
        }
        // The plane that fits them, least squares: on a symmetric grid each term by itself.
        const float planeHeight = (mean[0] + mean[1] + mean[2]) / 3.0f;
        const float slopeAlong = (mean[2] - mean[0]) / (2.0f * along);
        const float slopeAcross = ((grid[0][2] - grid[0][0]) + (grid[1][2] - grid[1][0]) + (grid[2][2] - grid[2][0])) / (3.0f * 2.0f * across);
        const JPH::Vec3 normal = (JPH::Vec3::sAxisY() - forward * slopeAlong - starboard * slopeAcross).Normalized();

        const bool hasRates = water.valid && elapsed > 0.0f;
        const float follow = hasRates ? 1.0f - std::exp(-elapsed / kRateTau) : 0.0f;
        const auto followRate = [&](float now, float before, float followed)
        {
            if (!hasRates) { return 0.0f; }
            const float raw = std::clamp((now - before) / elapsed, -kMaxSurfaceSpeed, kMaxSurfaceSpeed);
            return followed + (raw - followed) * follow;
        };
        for (int i = 0; i < 3; ++i)
        {
            VesselStation& station = water.stations[i];
            station.speed = followRate(mean[i], station.height, station.speed);
            station.offset = along * static_cast<float>(i - 1);
            station.height = mean[i];
            const int ahead = std::min(i + 1, 2);
            const int behind = std::max(i - 1, 0);
            station.slopeAlong = (mean[ahead] - mean[behind]) / (along * static_cast<float>(ahead - behind));
            station.slopeAcross = (grid[i][2] - grid[i][0]) / (2.0f * across);
        }
        water.speed = followRate(planeHeight, water.height, water.speed);
        JPH::Vec3 spin = JPH::Vec3::sZero();
        if (hasRates)
        {
            const JPH::Vec3 raw = water.normal.Cross(normal) / elapsed;
            const float rate = raw.Length();
            spin = water.spin + ((rate > kMaxSurfaceSpin ? raw * (kMaxSurfaceSpin / rate) : raw) - water.spin) * follow;
        }
        water.spin = spin;
        water.calmAcross = hasRates ? water.calmAcross + (slopeAcross - water.calmAcross) * (1.0f - std::exp(-elapsed / kCalmTau)) : slopeAcross;
        water.height = planeHeight;
        water.normal = normal;
        water.forward = forward;
        water.starboard = starboard;
        const JPH::Vec3 propellerLocal(settings.propeller.x * scale.x, settings.propeller.y * scale.y, settings.propeller.z * scale.z);
        const JPH::RVec3 propeller = origin + rotation * propellerLocal;
        water.propellerHeight = height(propeller.GetX(), propeller.GetZ());
        water.valid = true;
    }

    void PushVessel(JPH::Body& body, const VesselSettings& settings, const Math::float3& scale, VesselState& state, const VesselInput& input, const VesselWater& water, float buoyancy, JPH::Vec3Arg gravity, float dt)
    {
        // The helm answers at its own pace: an engine spools, a rudder swings.
        state.throttle = MoveToward(state.throttle, std::clamp(input.throttle, -1.0f, 1.0f), settings.throttleRate * dt);
        const float rudderStep = settings.rudderDeg > 0.0f ? settings.rudderRate / settings.rudderDeg * dt : 1.0f;
        state.rudder = MoveToward(state.rudder, std::clamp(input.rudder, -1.0f, 1.0f), rudderStep);

        const JPH::RVec3 centre = body.GetCenterOfMassPosition();
        const float weight = settings.mass * -gravity.GetY();
        float totalVolume = 0.0f;
        float submergedVolume = 0.0f;
        JPH::Vec3 centerOfBuoyancy = JPH::Vec3::sZero();
        body.GetSubmergedVolume(JPH::RVec3(centre.GetX(), water.height, centre.GetZ()), water.normal, totalVolume, submergedVolume, centerOfBuoyancy);
        // 1 = as much of it under as it floats at at rest. Out of the water nothing below has anything to push against.
        state.immersion = totalVolume > 0.0f ? submergedVolume * buoyancy / totalVolume : 0.0f;
        const float wet = std::clamp(state.immersion, 0.0f, 1.0f);

        const JPH::Quat rotation = body.GetRotation();
        const JPH::Vec3 up = rotation * JPH::Vec3::sAxisY();
        const JPH::Vec3 hullAxis = ToJolt(settings.forward).Normalized();
        const JPH::Vec3 forward = rotation * hullAxis;
        const JPH::Vec3 starboard = up.Cross(forward);

        // What holds the heave, the pitch and the roll down: the water's mass going with the hull, far more than the air's. Against the water's own rise and tilting, or a boat lags behind every swell
        // and shoulders through it. On the velocities themselves, so no setting can make it unstable, and before this step's impulses: after them it takes a share of the buoyancy too, and the boat
        // sits under its draft.
        if (wet > 0.0f)
        {
            const float keepHeave = std::exp(-settings.heaveDamping * wet * dt);
            const float keepPitch = std::exp(-settings.pitchDamping * wet * dt);
            const float keepRoll = std::exp(-settings.rollDamping * wet * dt);
            const JPH::Vec3 linear = body.GetLinearVelocity();
            const float rise = linear.GetY() - water.speed;
            body.SetLinearVelocityClamped(linear - JPH::Vec3::sAxisY() * (rise * (1.0f - keepHeave)));
            const JPH::Vec3 angular = body.GetAngularVelocity();
            const float pitching = (angular - water.spin).Dot(starboard);
            const float rolling = (angular - water.spin).Dot(forward);
            body.SetAngularVelocityClamped(angular - starboard * (pitching * (1.0f - keepPitch)) - forward * (rolling * (1.0f - keepRoll)));
        }
        if (submergedVolume > 0.0f)
        {
            const float lifting = std::min(submergedVolume, settings.reserveLift * totalVolume / buoyancy);
            body.ApplyBuoyancyImpulse(totalVolume, lifting, centerOfBuoyancy, buoyancy, 0.0f, 0.0f, JPH::Vec3::sZero(), gravity, dt);
        }

        const JPH::Vec3 velocity = body.GetLinearVelocity();
        const float u = velocity.Dot(forward);
        const float sway = velocity.Dot(starboard);
        const float yawRate = body.GetAngularVelocity().Dot(up);
        state.speed = u;
        state.waterHeight = water.height;

        // The propeller: at the stern, under the centre of mass, so full throttle lifts the bow by itself. Out of the water it spins free -- on the water that stands over it, not on the plane: a
        // long sea lifts the stern clear, and the boat loses its push and its rudder together.
        const JPH::Vec3 propellerLocal(settings.propeller.x * scale.x, settings.propeller.y * scale.y, settings.propeller.z * scale.z);
        const JPH::RVec3 propeller = body.GetPosition() + rotation * propellerLocal;
        const float bite = std::clamp((water.propellerHeight - static_cast<float>(propeller.GetY())) / kPropellerBite, 0.0f, 1.0f);
        state.bite = bite;
        const float thrust = (state.throttle >= 0.0f ? settings.thrust : settings.thrust * settings.reverseShare) * state.throttle * bite;
        if (thrust != 0.0f) { body.AddImpulse(forward * (thrust * dt), propeller); }

        // The rudder: a force across the stern by the water moving past it, the boat's own speed and the propeller's wash. A boat lying still with its engine off cannot be steered; going astern
        // it turns the other way round.
        const float wash = settings.wash * std::sqrt(std::fabs(state.throttle)) * (state.throttle >= 0.0f ? 1.0f : -1.0f);
        const float flow = u + wash;
        const float rudderSine = std::sin(state.rudder * JPH::DegreesToRadians(settings.rudderDeg));
        // At the centre of mass's height: it turns the boat, and the lean into a turn is the form stability's below.
        const float across = -settings.rudderForce * flow * std::fabs(flow) * rudderSine * bite; // the stern to port turns the bow to starboard
        if (across != 0.0f) { body.AddImpulse(starboard * (across * dt), centre + forward * JPH::Vec3(propeller - centre).Dot(forward)); }

        state.impact *= std::exp(-dt / kImpactFade);
        if (wet <= 0.0f) { return; }

        // The hull: quadratic, as water is, with the hump of the waves it makes on top; sideways a little linear too, so the last of a drift dies away. The speed then carries part of the weight:
        // the hull rises, less of it is in the water (wet) and the drag falls. Too little throttle never gets over the hump, and the bow is highest there.
        const float ahead = std::max(u, 0.0f);
        const float rising = SmoothStep(0.0f, settings.humpSpeed, ahead);
        const float planing = SmoothStep(settings.humpSpeed, settings.planeSpeed, ahead);
        const float humpOff = (ahead - settings.humpSpeed) / std::max(0.5f * settings.humpSpeed, 0.01f);
        const float surgeForce = -settings.surgeDrag * u * std::fabs(u) - settings.humpDrag * rising * std::exp(-humpOff * humpOff);
        const float swayForce = -(settings.swayDrag * std::fabs(sway) + settings.swayLinear) * sway;
        body.AddImpulse(forward * (surgeForce * wet * dt));
        body.AddImpulse(starboard * (swayForce * wet * dt), centre - forward * (settings.swayLever * scale.x));
        body.AddImpulse(JPH::Vec3::sAxisY() * (settings.planingLift * planing * weight * wet * dt));
        const float yawTorque = -(settings.yawDrag * std::fabs(yawRate) + settings.yawLinear) * yawRate;
        const float trimTorque = settings.humpTrim * rising * (1.0f - planing) + settings.planeTrim * planing;
        // The hull lies along the surface, and leans into a turn by what the turn asks. A roll is its starboard side rising.
        const float roll = std::asin(std::clamp(starboard.GetY(), -1.0f, 1.0f));
        const float lean = JPH::DegreesToRadians(std::clamp(settings.bank * u * yawRate, -settings.maxBank, settings.maxBank));
        const float rollTorque = settings.rollStiffness * (std::atan(water.calmAcross) - lean - roll);
        body.AddAngularImpulse((up * yawTorque + forward * rollTorque - starboard * trimTorque) * (wet * dt));

        // The waves, each station for its third of the hull. The pressure under a sloping surface leans with it: a share of the weight pushes the hull down the slope, at the height of the centre of
        // mass (the roll and the pitch are the buoyancy's). A swell surfs it; a slope that differs at the bow and the stern yaws it. And a part of the hull falling into the water faster than the
        // water rises there takes the water's mass at once: the slam, on top of what its volume says.
        float worstSlam = 0.0f;
        for (const VesselStation& station : water.stations)
        {
            const JPH::RVec3 atCentre = centre + water.forward * station.offset;
            if (settings.waveForce > 0.0f)
            {
                const float share = settings.waveForce * weight * wet / 3.0f;
                const JPH::Vec3 push = -(water.forward * station.slopeAlong + water.starboard * station.slopeAcross) * share;
                body.AddImpulse(push * dt, atCentre);
            }
            const JPH::RVec3 keel = body.GetPosition() + rotation * (hullAxis * station.offset);
            const float depth = station.height - static_cast<float>(keel.GetY());
            const float fall = station.speed - body.GetPointVelocity(keel).GetY();
            if (settings.slam > 0.0f && depth > 0.0f && fall > kSlamFrom)
            {
                const float reach = std::clamp(1.0f - depth / std::max(2.0f * settings.draft * scale.y, 0.01f), 0.0f, 1.0f); // the first of the water: past that the volume has it
                const float over = fall - kSlamFrom;
                const float force = std::min(settings.slam * over * over * reach, settings.slamMax * weight);
                if (force > 0.0f)
                {
                    body.AddImpulse(JPH::Vec3::sAxisY() * (force * dt), keel);
                    worstSlam = std::max(worstSlam, force);
                }
            }
        }
        if (weight > 0.0f) { state.impact = std::max(state.impact, worstSlam / weight); }
    }
} // namespace physics
