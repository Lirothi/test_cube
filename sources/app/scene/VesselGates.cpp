#include "app/scene/VesselGates.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <tuple>
#include <memory>
#include <utility>

#include "physics/PhysicsBody.h"
#include "physics/VesselBench.h"
#include "app/scene/VesselModel.h"
#include "app/scene/VesselSystem.h" // the helm's road (R14.3b)
#include "core/task/TaskSystem.h"
#include "core/threading/RenderThread.h"
#include "core/threading/ThreadRole.h"
#include "physics/WaterSurface.h"
#include "rendering/renderables/RenderableObject.h"

// R14.3a: the boat's gate, moved out of physics/PhysicsSelfTest.cpp with the boat's model (physics/VesselModel.h): it runs a
// boat on physics::VesselBench -- a world of its own, no Jolt here -- through the same functions the world calls.
namespace
{
    using namespace physics;
    using namespace boats;

    Math::float3 AxisX() { return Math::float3(1.0f, 0.0f, 0.0f); }
    Math::float3 AxisY() { return Math::float3(0.0f, 1.0f, 0.0f); }
    Math::float3 UnitOf(const Math::float3& v) { return v / v.Length(); }
    float Degrees(float radians) { return radians * (180.0f / 3.14159265358979323846f); } // Jolt's RadiansToDegrees
    float Radians(float degrees) { return degrees * (3.14159265358979323846f / 180.0f); } // and DegreesToRadians

    // --------------------------------------------------------------------------------------------------------- vessel
    // A boat on the water, through the functions the world calls. On still water: it floats at its draft and level; full ahead climbs over the hump and planes, too little throttle stays under
    // it; it slows by itself; a rudder turns it to its own side; flat out it holds no more than its side grip and runs wider than eased; a boat lying still answers no rudder and one on its propeller's wash does; going astern it turns the other way. On waves: it
    // rides a long swell up and down and along its slope; a slope pushes it; a sea at an angle yaws it; at full speed into steep waves it slams and stays on its keel. The arms that must come
    // out wrong: no rudder keeps the course, without the grip lifting buys no tighter circle, a propeller out of the water pushes nothing, nothing in the water pushes nothing, the weight up top capsizes it, a boat that cannot see the waves
    // does not ride them, no wave force pushes nothing and yaws nothing, no slam slams nothing. At anchor a steady pull is held at the scope; weighed, it carries the boat off.
    struct TestBoat
    {
        VesselSettings settings;
        VesselBench bench;
        VesselState state;
        VesselWater water;
        float time = 0.0f;
        std::function<float(float, float, float)> sea = [](float, float, float) { return 0.0f; };
        // The anchor's rode on the stem of the box, at the draft (the world's bowLocal).
        bool anchored = false;
        Math::float3 anchorPoint = Math::float3(0.0f, 0.0f, 0.0f);
        Math::float3 bowLocal = Math::float3(5.0f, settings.draft, 0.0f);

        // Flat under, the keel at the origin, its upper works up to 2.7 m: the small_yacht's size and no more of it (VesselBench).
        explicit TestBoat(const VesselSettings& boat, const Math::quat& rotation = Math::quat::Identity(), float y = 0.0f)
            : settings(boat), bench(boat, rotation, y)
        {
        }
        float Factor() const { return bench.FloatFactor(); }
        template <typename OnStep>
        void Run(float seconds, float throttle, float rudder, OnStep&& onStep)
        {
            constexpr float kStep = 1.0f / 60.0f;
            const Math::float3 one(1.0f, 1.0f, 1.0f);
            const int steps = static_cast<int>(seconds * 60.0f + 0.5f);
            for (int i = 0; i < steps; ++i)
            {
                const std::function<float(float, float)> height = [this](float x, float z) { return sea(x, z, time); };
                MeasureVesselWater(water, height, bench.Position(), bench.CenterOfMass(), bench.Rotation(), settings, one, 5.0f, 1.17f, water.valid ? kStep : 0.0f);
                bench.Step([&](StepBody& body)
                {
                    PushVessel(body, settings, one, state, VesselInput{ throttle, rudder }, water, bench.FloatFactor(), bench.Gravity(), kStep);
                    if (anchored) { PullAnchor(body, settings, bowLocal, anchorPoint, kStep); }
                });
                time += kStep;
                onStep();
            }
        }
        void Run(float seconds, float throttle, float rudder) { Run(seconds, throttle, rudder, []() {}); }
        Math::quat Rotation() const { return bench.Rotation(); }
        Math::float3 Forward() const { return UnitOf(bench.Rotate(settings.forward)); }
        Math::float3 Position() const { return bench.Position(); }
        float UpY() const { return bench.Rotate(AxisY()).y; }
        float Speed() const { return bench.LinearVelocity().Dot(Forward()); }
        float HeadingDeg() const { const Math::float3 f = Forward(); return Degrees(std::atan2(f.x, f.z)); }
        float BowUpDeg() const { return Degrees(std::asin(std::clamp(Forward().y, -1.0f, 1.0f))); }
        float RollDeg() const { return Degrees(std::asin(std::clamp(bench.Rotate(AxisY()).Cross(Forward()).y, -1.0f, 1.0f))); }
        float YawRateDeg() const { return Degrees(bench.AngularVelocity().y); }
    };

    int GateVessel(std::string& detail)
    {
        const VesselSettings base;
        std::string failures;
        const auto expect = [&failures](bool ok, const std::string& what) { if (!ok) { failures += " [" + what + "]"; } };
        const auto num = [](float value) { return std::to_string(value); };
        const auto turned = [](float degrees) // a heading crosses 180
        {
            while (degrees > 180.0f) { degrees -= 360.0f; }
            while (degrees < -180.0f) { degrees += 360.0f; }
            return degrees;
        };

        // ---- still water
        TestBoat cruise(base, Math::quat::Identity(), 0.5f); // dropped in
        cruise.Run(30.0f, 0.0f, 0.0f);
        const float restY = cruise.Position().y;
        const float restUp = cruise.UpY();
        expect(std::fabs(restY + base.draft) < 0.02f, "the draft");
        expect(restUp > 0.9995f, "level at rest");

        float humpBow = 0.0f;
        float toPlane = -1.0f;
        float ran = 0.0f;
        cruise.Run(60.0f, 1.0f, 0.0f, [&]()
        {
            ran += 1.0f / 60.0f;
            humpBow = std::max(humpBow, cruise.BowUpDeg());
            if (toPlane < 0.0f && cruise.Speed() >= base.planeSpeed) { toPlane = ran; }
        });
        const float top = cruise.Speed();
        const float wantedTop = std::sqrt(base.thrust * (1.0f + base.planingLift) / base.surgeDrag);
        const float riseOnPlane = cruise.Position().y + base.draft;
        const float planeBow = cruise.BowUpDeg();
        expect(std::fabs(top - wantedTop) < 0.08f * wantedTop, "the top speed");
        expect(std::fabs(cruise.HeadingDeg() - 90.0f) < 0.5f, "no rudder keeps the course");
        expect(toPlane > 0.0f && toPlane < 20.0f, "it gets onto the plane");
        expect(planeBow > 0.5f && humpBow > planeBow + 1.0f, "the bow is highest at the hump");
        expect(riseOnPlane > 0.03f, "the speed lifts the hull");
        cruise.Run(20.0f, 0.0f, 0.0f);
        const float coasted = cruise.Speed();
        expect(coasted > 0.0f && coasted < 3.0f, "it slows by itself");

        TestBoat under(base, Math::quat::Identity(), -base.draft);
        under.Run(40.0f, 0.2f, 0.0f);
        const float underHump = under.Speed();
        expect(underHump < base.humpSpeed, "too little throttle stays under the hump");

        // Under the hump at part throttle, with more trim than the box's water can hold (2.5 x the asset's): the bow comes up only until it leaves the water. The trim is the water's push
        // on the fore part of the bottom, and a bow in the air has none under it; as a torque on a hull wet anywhere it stood the small_yacht on its stern at 0.35 throttle (2026-11-07).
        VesselSettings heavyTrim = base;
        heavyTrim.humpTrim = 2.5f * base.humpTrim;
        TestBoat trimmed(heavyTrim, Math::quat::Identity(), -base.draft);
        float worstTrim = 0.0f;
        trimmed.Run(30.0f, 0.35f, 0.0f, [&]() { worstTrim = std::max(worstTrim, trimmed.BowUpDeg()); });
        expect(worstTrim < 12.0f && trimmed.UpY() > 0.9f, "a bow out of the water takes no more trim (" + num(worstTrim) + " deg)");

        const auto turn = [&base, &turned](float rudder)
        {
            TestBoat boat(base, Math::quat::Identity(), -base.draft);
            boat.Run(25.0f, 1.0f, 0.0f);
            const float from = boat.HeadingDeg();
            boat.Run(4.0f, 1.0f, rudder);
            return std::make_tuple(turned(boat.HeadingDeg() - from), boat.RollDeg(), boat.UpY(), boat.Speed());
        };
        const auto [starboard, starboardRoll, starboardUp, starboardSpeed] = turn(1.0f);
        const auto [port, portRoll, portUp, portSpeed] = turn(-1.0f);
        expect(starboard > 30.0f, "hard to starboard turns to starboard");
        expect(port < -30.0f && std::fabs(starboard + port) < 0.1f * starboard, "hard to port the same way round");
        expect(starboardRoll < 0.0f && portRoll > 0.0f && starboardUp > 0.8f && portUp > 0.8f, "leans into the turn, on its keel"); // into a turn to starboard its starboard side goes down
        (void)portSpeed;

        // ---- the side grip (2026-11-12): flat out and hard over, the hull holds no more across its way than sideGrip x
        // sideGripPlane of g, so it runs a wider circle than with the throttle eased, where the rudder's own circle holds.
        // The arm: without the grip the circle hardly changes with the speed -- lifting for a bend bought nothing.
        const auto circle = [&base](float throttle, float sideGrip)
        {
            VesselSettings settings = base;
            settings.sideGrip = sideGrip;
            TestBoat boat(settings, Math::quat::Identity(), -base.draft);
            boat.Run(25.0f, 1.0f, 0.0f);
            boat.Run(12.0f, throttle, 1.0f);
            const float from = boat.HeadingDeg();
            float speedSum = 0.0f;
            int frames = 0;
            boat.Run(4.0f, throttle, 1.0f, [&]()
            {
                const Math::float3 v = boat.bench.LinearVelocity();
                speedSum += std::sqrt(v.x * v.x + v.z * v.z);
                ++frames;
            });
            const float rate = std::fabs(Radians(std::remainder(boat.HeadingDeg() - from, 360.0f))) / 4.0f;
            const float speed = speedSum / static_cast<float>(std::max(frames, 1));
            return std::make_pair(speed / std::max(rate, 1e-4f), speed * rate); // radius m, m/s2 across the way
        };
        const auto [gripFlatRadius, gripFlatAccel] = circle(1.0f, base.sideGrip);
        const auto [gripLiftRadius, gripLiftAccel] = circle(0.45f, base.sideGrip);
        const auto [freeFlatRadius, freeFlatAccel] = circle(1.0f, 0.0f);
        const auto [freeLiftRadius, freeLiftAccel] = circle(0.45f, 0.0f);
        (void)gripLiftAccel; (void)freeLiftAccel;
        const float gripMost = 9.81f * base.sideGrip * base.sideGripPlane;
        expect(gripFlatAccel < 1.15f * gripMost && gripFlatAccel > 0.7f * gripMost, "flat out the hull holds its grip across its way, no more (" + num(gripFlatAccel) + " m/s2)");
        expect(gripFlatRadius > 1.3f * gripLiftRadius, "flat out it runs wider than with the throttle eased");
        expect(freeFlatAccel > 1.3f * gripMost && freeFlatRadius < 1.15f * freeLiftRadius, "without the grip it turns its rudder's circle at any speed");

        TestBoat still(base, Math::quat::Identity(), -base.draft);
        still.Run(5.0f, 0.0f, 1.0f);
        const float stillTurn = still.HeadingDeg() - 90.0f;
        expect(std::fabs(stillTurn) < 0.5f, "a boat lying still answers no rudder");
        still.Run(3.0f, 0.2f, 1.0f);
        const float washTurn = still.HeadingDeg() - 90.0f;
        const float washSpeed = still.Speed();
        expect(washTurn > 5.0f && washSpeed < 5.0f, "on its propeller's wash it does");

        TestBoat astern(base, Math::quat::Identity(), -base.draft);
        astern.Run(30.0f, -1.0f, 0.0f);
        const float back = astern.Speed();
        const float wantedBack = -std::sqrt(base.thrust * base.reverseShare / base.surgeDrag);
        expect(std::fabs(back - wantedBack) < 0.07f * std::fabs(wantedBack), "the speed astern");
        const float fromAstern = astern.HeadingDeg();
        astern.Run(3.0f, -1.0f, 1.0f);
        const float asternTurn = turned(astern.HeadingDeg() - fromAstern);
        expect(asternTurn < -5.0f, "going astern it turns the other way");

        VesselSettings dryScrew = base;
        dryScrew.propeller.y = 2.0f;
        TestBoat dry(dryScrew, Math::quat::Identity(), -base.draft);
        dry.Run(10.0f, 1.0f, 0.0f);
        const float drySpeed = dry.Speed();
        expect(std::fabs(drySpeed) < 0.05f, "a propeller out of the water pushes nothing");

        TestBoat aloft(base, Math::quat::Identity(), 50.0f);
        aloft.bench.SetGravity(Math::float3(0.0f, 0.0f, 0.0f));
        aloft.Run(2.0f, 1.0f, 1.0f);
        const float aloftSpeed = aloft.Speed();
        const float aloftTurn = aloft.YawRateDeg();
        expect(aloftSpeed == 0.0f && aloftTurn == 0.0f, "no water, no push");

        const Math::quat heeled = RotationAbout(AxisX(), Radians(20.0f));
        TestBoat righting(base, heeled, -base.draft);
        righting.Run(15.0f, 0.0f, 0.0f);
        const float rightedUp = righting.UpY();
        expect(rightedUp > 0.9995f, "it rights itself from 20 degrees");
        VesselSettings topHeavy = base;
        topHeavy.centerOfMassY = 2.6f;
        topHeavy.rollStiffness = 0.0f;
        TestBoat capsized(topHeavy, heeled, -base.draft);
        capsized.Run(15.0f, 0.0f, 0.0f);
        const float capsizedUp = capsized.UpY();
        expect(capsizedUp < 0.9f, "the weight up top capsizes it");

        // ---- a long swell along the hull: 60 m, 0.6 m high either way. Deep water's speed.
        constexpr float kTwoPi = 6.2831853f;
        const auto swell = [kTwoPi](float wavelength, float amplitude, float directionX, float directionZ)
        {
            const float k = kTwoPi / wavelength;
            const float omega = std::sqrt(9.81f * k);
            return [=](float x, float z, float t) { return amplitude * std::sin(k * (x * directionX + z * directionZ) - omega * t); };
        };
        const auto ride = [&base](const std::function<float(float, float, float)>& sea, bool blind)
        {
            TestBoat boat(base, Math::quat::Identity(), -base.draft);
            if (!blind) { boat.sea = sea; }
            boat.Run(15.0f, 0.0f, 0.0f, []() {});
            float low = 1.0e6f, high = -1.0e6f, worstFollow = 0.0f, worstPitch = 0.0f;
            boat.Run(25.0f, 0.0f, 0.0f, [&]()
            {
                const float y = boat.Position().y;
                low = std::min(low, y);
                high = std::max(high, y);
                worstFollow = std::max(worstFollow, std::fabs(y + base.draft - boat.water.height));
                const float slope = Degrees(std::atan((boat.water.stations[2].height - boat.water.stations[0].height) / (boat.water.stations[2].offset - boat.water.stations[0].offset)));
                worstPitch = std::max(worstPitch, std::fabs(boat.BowUpDeg() - slope));
            });
            return std::make_tuple(high - low, worstFollow, worstPitch);
        };
        const auto longSwell = swell(60.0f, 0.6f, 1.0f, 0.0f);
        const auto [heave, follow, pitchLag] = ride(longSwell, false);
        const auto [blindHeave, blindFollow, blindPitch] = ride(longSwell, true);
        (void)blindFollow; (void)blindPitch;
        expect(heave > 0.95f, "it rides the swell up and down");
        expect(follow < 0.25f, "and stays at its draft on it");
        expect(pitchLag < 2.5f, "and along its slope");
        expect(blindHeave < 0.05f, "a boat that cannot see the waves does not ride them");

        // ---- a swell on its beam pushes it about, and a short sea at an angle yaws it. Its hull's own drag along its tilted axes moves it too, so without the slope's force it moves less, not not at all.
        const auto drift = [&base, &swell](float waveForce, bool blind)
        {
            VesselSettings settings = base;
            settings.waveForce = waveForce;
            TestBoat boat(settings, Math::quat::Identity(), -base.draft);
            if (!blind) { boat.sea = swell(40.0f, 0.6f, 0.0f, 1.0f); }
            boat.Run(10.0f, 0.0f, 0.0f);
            float low = 1.0e6f, high = -1.0e6f;
            boat.Run(20.0f, 0.0f, 0.0f, [&]()
            {
                const float z = boat.Position().z;
                low = std::min(low, z);
                high = std::max(high, z);
            });
            return high - low;
        };
        const float pushed = drift(1.0f, false);
        const float unpushed = drift(0.0f, false);
        const float stillWaterDrift = drift(1.0f, true);
        expect(pushed > 1.3f * unpushed, "a slope pushes it");
        expect(stillWaterDrift < 0.001f, "still water pushes nothing");

        const auto yawed = [&base, &swell](bool blind)
        {
            TestBoat boat(base, Math::quat::Identity(), -base.draft);
            if (!blind) { boat.sea = swell(15.0f, 0.4f, 0.7071f, 0.7071f); }
            float low = 1.0e6f, high = -1.0e6f;
            boat.Run(30.0f, 0.0f, 0.0f, [&]()
            {
                const float heading = boat.HeadingDeg();
                low = std::min(low, heading);
                high = std::max(high, heading);
            });
            return high - low;
        };
        const float yawSwing = yawed(false);
        const float noYawSwing = yawed(true);
        expect(yawSwing > 0.5f, "a sea at an angle yaws it");
        expect(noYawSwing < 0.01f, "still water yaws nothing");

        // ---- full speed into a steep sea, the worst of it. 30 m, 0.7 m either way.
        const auto headSea = [&base, &swell](float slam)
        {
            VesselSettings settings = base;
            settings.slam = slam;
            TestBoat boat(settings, Math::quat::Identity(), -base.draft);
            boat.sea = swell(30.0f, 0.7f, -1.0f, 0.0f);
            float lowestUp = 1.0f, bumpiest = 0.0f, biggestBow = 0.0f, speedSum = 0.0f;
            int frames = 0;
            boat.Run(40.0f, 1.0f, 0.0f, [&]()
            {
                lowestUp = std::min(lowestUp, boat.UpY());
                bumpiest = std::max(bumpiest, boat.state.impact);
                biggestBow = std::max(biggestBow, std::fabs(boat.BowUpDeg()));
                speedSum += boat.Speed();
                ++frames;
            });
            return std::make_tuple(lowestUp, bumpiest, biggestBow, speedSum / static_cast<float>(frames));
        };
        const auto [seaUp, seaSlam, seaBow, seaSpeed] = headSea(base.slam);
        const auto [calmUp, noSlam, calmBow, calmSpeed] = headSea(0.0f);
        (void)calmUp; (void)calmBow; (void)calmSpeed;
        expect(seaUp > 0.8f && seaBow < 35.0f, "into a steep sea it stays on its keel");
        expect(seaSlam > 0.05f, "it slams");
        expect(noSlam == 0.0f, "no slam slams nothing");
        expect(seaSpeed > 6.0f && seaSpeed < top, "the sea costs it speed");

        // ---- the anchor: dropped under the bow, then a steady pull astern (half astern, as a wind on the bow would) -- the
        // rode lies slack for its scope, takes up, and holds the bow at scope + pull / stiffness, the boat lying to it. The
        // arm: weighed, the same pull carries it off.
        const auto lieTo = [&](bool anchored)
        {
            TestBoat boat(base, Math::quat::Identity(), -base.draft);
            boat.Run(2.0f, 0.0f, 0.0f);
            boat.anchored = anchored;
            boat.anchorPoint = boat.Position() + boat.bench.Rotate(boat.bowLocal);
            float farthest = 0.0f;
            const auto reach = [&]()
            {
                const Math::float3 bow = boat.Position() + boat.bench.Rotate(boat.bowLocal);
                return Math::float3(bow.x - boat.anchorPoint.x, 0.0f, bow.z - boat.anchorPoint.z).Length();
            };
            boat.Run(60.0f, -0.5f, 0.0f, [&]() { farthest = std::max(farthest, reach()); });
            return std::make_tuple(reach(), farthest);
        };
        const auto [anchorReach, anchorFarthest] = lieTo(true);
        const auto [driftReach, driftFarthest] = lieTo(false);
        (void)driftFarthest;
        const float anchorPull = base.thrust * base.reverseShare * 0.5f;
        const float anchorHold = base.anchorScope + anchorPull / base.anchorStiffness;
        expect(std::fabs(anchorReach - anchorHold) < 0.5f && anchorFarthest < anchorHold + 4.0f, "the anchor holds it");
        expect(driftReach > 4.0f * anchorHold, "weighed, it drifts off");

        detail = "factor " + num(cruise.Factor()) + ", at rest y " + num(restY) + " (draft " + num(base.draft) + "), top " + num(top) + " m/s (wanted " + num(wantedTop) + "), on the plane after " + num(toPlane) +
            " s, risen " + num(riseOnPlane) + " m, bow up " + num(planeBow) + " deg, at the hump " + num(humpBow) + ", coasted to " + num(coasted) + ", a fifth of the throttle " + num(underHump) +
            " m/s; 4 s hard over " + num(starboard) + " / " + num(port) + " deg, roll " + num(starboardRoll) + " deg, speed " + num(starboardSpeed) +
            "; hard over flat out / at 0.45 r " + num(gripFlatRadius) + " / " + num(gripLiftRadius) + " m (" + num(gripFlatAccel) + " m/s2 of " + num(gripMost) + "), no grip " +
            num(freeFlatRadius) + " / " + num(freeLiftRadius) + " m (" + num(freeFlatAccel) + ")" + "; lying still " + num(stillTurn) + " deg, on the wash " + num(washTurn) +
            " deg at " + num(washSpeed) + " m/s; astern " + num(back) + " m/s, turning " + num(asternTurn) + " deg; dry propeller " + num(drySpeed) + " m/s; righted up.y " + num(rightedUp) + ", top-heavy " + num(capsizedUp) +
            "; anchored the bow at " + num(anchorReach) + " m (held at " + num(anchorHold) + ", farthest " + num(anchorFarthest) + "), weighed " + num(driftReach) + " m" +
            "; swell: heave " + num(heave) + " m (blind " + num(blindHeave) + "), off its draft by " + num(follow) + " m, off the slope by " + num(pitchLag) + " deg; on the beam " + num(pushed) + " / " + num(unpushed) +
            " m, at an angle " + num(yawSwing) + " / " + num(noYawSwing) + " deg; head sea: up.y " + num(seaUp) + ", bow " + num(seaBow) + " deg, slam " + num(seaSlam) + " of the weight, " + num(seaSpeed) + " m/s" +
            (failures.empty() ? "" : "; WRONG:" + failures);
        return failures.empty() ? 0 : 1;
    }

    // ----------------------------------------------------------------------------------------------------------- helm
    // The helm's road through a world (moved out of the physics' gate "world" by R14.3b with the boats): who is near a hull
    // (its rectangle, not a circle of its length), taking it, the numbers by name, and a boat gone with its helm held -- a
    // VesselSystem over a physics::World, a boat-shaped box as the object.
    class GateObject final : public RenderableObject
    {
    public:
        GateObject() : RenderableObject("", L"") {}
        const std::string& GetGeometryPath() const override { return path_; }
        bool IsSimpleRender() const override { return false; }

    private:
        std::string path_ = "gate"; // a custom shape reads no file; the world only wants a name
    };

    int GateHelm(std::string& detail)
    {
        const Settings saved = GameSettings();
        const bool savedRenderThread = threading::g_renderThreadEnabled;
        threading::g_renderThreadEnabled = true;
        threading::RoleScope role(threading::ThreadRole::Game);
        TaskSystem::Get().Start();
        const auto finish = [&](int verdict)
        {
            GameSettings() = saved;
            threading::g_renderThreadEnabled = savedRenderThread;
            return verdict;
        };
        Settings& settings = GameSettings();
        settings.async = true;
        settings.threads = 2;
        settings.simulate = true;
        settings.stepDelayMs = 0.0f;
        const auto frame = [](physics::World& world, float dt)
        {
            world.Tick(dt);
            world.KickStep();
            world.FinishStep();
            world.SyncPoses();
        };
        auto boat = std::make_unique<GateObject>();
        CollisionSettings shape;
        shape.shape = ShapeKind::Custom;
        CollisionPrimitive box;
        box.kind = PrimitiveKind::Box;
        box.halfExtents = Math::float3(5.0f, 1.35f, 1.17f);
        shape.primitives.push_back(box);
        physics::SetCollision(*boat, shape);
        physics::SetVessel(*boat, std::make_shared<VesselSettings>());
        physics::SetBuoyant(*boat, true);
        physics::SetMotion(*boat, Motion::Dynamic);
        physics::World world;
        VesselSystem vessels(world);
        frame(world, 1.0f / 60.0f);
        const Stats made = world.GetStats();
        const bool beside = vessels.FindVessel(Math::float3(0.0f, 0.0f, 3.5f), 3.0f) == boat.get();
        const bool past = vessels.FindVessel(Math::float3(0.0f, 0.0f, 4.5f), 3.0f) == nullptr;
        const bool taken = vessels.SetHelm(boat.get()) && vessels.GetHelm().exists;
        vessels.SetHelmInput(VesselInput{ 1.0f, 1.0f });
        frame(world, 1.0f / 60.0f);
        const bool tuned = vessels.TuneVessels("thrust", 1000.0f) && !vessels.TuneVessels("nonsense", 1.0f);
        boat.reset();
        frame(world, 1.0f / 60.0f);
        const bool gone = !vessels.GetHelm().exists && !vessels.SetHelm(nullptr);
        detail = "helm: floating " + std::to_string(made.floatingBodies) + ", beside " + std::to_string(beside) + ", past " + std::to_string(past) + ", taken " + std::to_string(taken) +
            ", tuned " + std::to_string(tuned) + ", gone " + std::to_string(gone);
        if (made.floatingBodies != 1 || !beside || !past || !taken || !tuned || !gone) { return finish(1); }

        // The boat's step through the system's hooks (the water measured, the step pushed, the pose shown): on a flat sea at
        // y 0 a boat let go at y 0 settles at its draft; the arm, no water -- nothing holds it up, it falls.
        struct FlatWater final : WaterSurface
        {
            bool Present() const override { return true; }
            bool HasWaves() const override { return false; }
            float Level() const override { return 0.0f; }
            float SampleHeight(float, float, float) const override { return 0.0f; }
        };
        const auto settle = [&](bool sea)
        {
            FlatWater water;
            auto hull = std::make_unique<GateObject>();
            physics::SetCollision(*hull, shape);
            physics::SetVessel(*hull, std::make_shared<VesselSettings>());
            physics::SetBuoyant(*hull, true);
            physics::SetMotion(*hull, Motion::Dynamic);
            physics::World afloat;
            VesselSystem boats(afloat);
            for (int i = 0; i < 300; ++i)
            {
                afloat.SetWater(sea ? &water : nullptr);
                frame(afloat, 1.0f / 60.0f);
            }
            std::vector<VesselHull> hulls;
            boats.CollectHulls(hulls);
            return hulls.size() == 1 ? hulls[0].position.y : 1.0e6f;
        };
        const float draft = VesselSettings{}.draft;
        const float floated = settle(true);
        const float sank = settle(false);
        detail += "; let go on a flat sea it rests at " + std::to_string(floated) + " m (draft " + std::to_string(draft) + "), without the sea " + std::to_string(sank) + " m";
        if (std::fabs(floated + draft) > 0.05f) { return finish(1); }
        return finish(sank < floated - 1.0f ? 0 : 1); // the arm
    }
} // namespace

std::vector<physics::ExtraGate> VesselGates()
{
    return { { "vessel", &GateVessel }, { "helm", &GateHelm } };
}
