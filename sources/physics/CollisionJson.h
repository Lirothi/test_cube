#pragma once

#include <string_view>
#include <vector>

#include "physics/PhysicsTypes.h"
#include "third_party/json/json.hpp"

// mesh.json / level object "collision" block:
//   "collision": { "shape": "custom", "shapes": [{ "type": "capsule", "center": [0, 2, 0], "rotationDeg": [0, 0, 0], "radius": 0.2, "halfHeight": 1.5 }],
//                  "lod": 1, "excludeSlots": [1, 3], "density": 400, "mass": 0, "friction": 0.5,
//                  "restitution": 0.1, "convexRadius": 0.01, "twoSided": true }
// Every key is optional, and WriteSettings leaves defaults OUT so an untouched asset stays byte-identical.
// The per-object choice of motion is a separate key, "physics": "dynamic" -- not this block, which ResolveMeshAsset folds from
// the asset into every placed instance.
namespace physics
{
    inline nlohmann::json WriteVec3(const Math::float3& v) { return nlohmann::json::array({ v.x, v.y, v.z }); }

    // Where a number of a boat belongs: the Mesh Editor heads its rows by it, and a placed copy may say its own only for the
    // race's groups (VesselGroupPerCopy).
    enum class VesselGroup : unsigned char { Engine, Hull, Helm, Turn, Sea, Hydrostatics, Anchor, Look };
    inline const char* VesselGroupName(VesselGroup group)
    {
        switch (group)
        {
        case VesselGroup::Engine: return "Engine";
        case VesselGroup::Hull: return "Hull and planing";
        case VesselGroup::Helm: return "Helm";
        case VesselGroup::Turn: return "Turning";
        case VesselGroup::Sea: return "In a sea";
        case VesselGroup::Hydrostatics: return "How it floats";
        case VesselGroup::Anchor: return "Anchor";
        case VesselGroup::Look: return "Look";
        }
        return "";
    }
    // What a placed copy may say for itself (a level object's "vessel" block over its asset's: the Inspector's Boat
    // section, MCP setVessel): how it races. How it floats stays the hull's -- the draft is what the water mask is baked
    // for, the centre of mass where the hull lies --, and the anchor, the spray and the cameras are no race's.
    inline bool VesselGroupPerCopy(VesselGroup group) { return group <= VesselGroup::Sea; }

    // The numbers of a mesh.json "vessel" block: ONE table for the file, the Mesh Editor, the Inspector (a copy's own),
    // MCP setVessel and --set vessel.<key>. The hint says what the hand at the helm feels and which way to turn.
    struct VesselKey
    {
        const char* key;
        const char* label;
        float VesselSettings::* member;
        float speed;
        float lo;
        float hi;
        VesselGroup group;
        const char* hint;
    };
    inline const std::vector<VesselKey>& VesselKeys()
    {
        using S = VesselSettings;
        using G = VesselGroup;
        static const std::vector<VesselKey> keys = {
            { "thrust", "Thrust (N)", &S::thrust, 100.0f, 0.0f, 10000000.0f, G::Engine,
              "The propeller's push, full ahead. With the drag ahead it is the top speed: thrust = 0.77 x drag x speed^2, so\n"
              "+21 % thrust = +10 % speed. More also climbs the hump onto the plane sooner." },
            { "reverseShare", "Astern share", &S::reverseShare, 0.01f, 0.0f, 1.0f, G::Engine,
              "Of the thrust, full astern: how hard it backs and how fast a bot brakes with the engine." },
            { "throttleRate", "Throttle rate (1/s)", &S::throttleRate, 0.01f, 0.01f, 20.0f, G::Engine,
              "How fast the engine answers the throttle: 1 = idle to full in a second. Lower = a heavy boat's slow pull away\n"
              "(20 kn later), higher = a light boat's kick." },
            { "mass", "Mass (kg)", &S::mass, 10.0f, 1.0f, 1000000.0f, G::Hull,
              "Its weight, the body's (the Physics row's mass has no say on a boat). Heavier: slower to speed up and to slow\n"
              "down, it slides more in a bend (a wider circle), and a wave throws it about less. The top speed does not change." },
            { "surgeDrag", "Drag ahead", &S::surgeDrag, 1.0f, 0.1f, 100000.0f, G::Hull,
              "The water's drag along the hull, N per (m/s)^2. Lower = a higher top speed: half the drag, +41 %." },
            { "humpSpeed", "Hump speed (m/s)", &S::humpSpeed, 0.05f, 0.1f, 50.0f, G::Hull,
              "Where the bow stands highest and the drag bumps. Too little throttle stays under it." },
            { "planeSpeed", "Planing speed (m/s)", &S::planeSpeed, 0.05f, 0.2f, 100.0f, G::Hull,
              "Past this it is up on the plane: less of it in the water, less drag." },
            { "humpDrag", "Hump drag (N)", &S::humpDrag, 50.0f, 0.0f, 10000000.0f, G::Hull,
              "The extra drag at the hump. Higher = a longer climb onto the plane (20 kn later); 0 = none." },
            { "humpTrim", "Bow up at the hump", &S::humpTrim, 500.0f, 0.0f, 100000000.0f, G::Hull,
              "N m of bow up at the hump. Too much stands it on its stern at part throttle." },
            { "planeTrim", "Bow up on the plane", &S::planeTrim, 500.0f, 0.0f, 100000000.0f, G::Hull,
              "N m of bow up on the plane." },
            { "planingLift", "Planing lift", &S::planingLift, 0.01f, 0.0f, 2.0f, G::Hull,
              "How much of its weight the speed carries on the plane: the hull rises and less of it drags." },
            { "rudderForce", "Rudder force", &S::rudderForce, 1.0f, 0.0f, 100000.0f, G::Helm,
              "The rudder's push per (m/s)^2 of water past it -- THE handling number: more = a tighter circle and faster\n"
              "bends (a bot takes its bends by the boat's own circle)." },
            { "rudderDeg", "Rudder hard over (deg)", &S::rudderDeg, 0.25f, 0.0f, 80.0f, G::Helm,
              "How far the rudder goes. More = a tighter circle (by its sine)." },
            { "rudderRate", "Rudder rate (deg/s)", &S::rudderRate, 1.0f, 1.0f, 1000.0f, G::Helm,
              "How fast the rudder swings: lower = lazier into a turn and out of it, a slalom gets hard." },
            { "wash", "Propeller wash (m/s)", &S::wash, 0.05f, 0.0f, 50.0f, G::Helm,
              "Water the propeller throws past the rudder at full throttle: what steers a boat that hardly moves, and why\n"
              "easing the throttle in a bend widens it." },
            { "swayDrag", "Drag across", &S::swayDrag, 10.0f, 0.0f, 1000000.0f, G::Turn,
              "The water's drag on the hull sliding sideways, per (m/s)^2. Higher = it slides less in a bend, its bow nearer\n"
              "its way, and keeps more of its speed through it." },
            { "swayLinear", "Drift damping", &S::swayLinear, 10.0f, 0.0f, 1000000.0f, G::Turn,
              "The same at a slow drift sideways, per m/s." },
            { "swayLever", "Keel aft of the centre (m)", &S::swayLever, 0.01f, -50.0f, 50.0f, G::Turn,
              "Where that sideways push acts, aft of the centre of mass: the keel's. Further aft = it holds a course on\n"
              "the waves and turns wider." },
            { "yawDrag", "Turn drag", &S::yawDrag, 1000.0f, 0.0f, 100000000.0f, G::Turn,
              "The water's resistance to the hull turning, per (rad/s)^2. Lower = it turns faster." },
            { "yawLinear", "Turn damping", &S::yawLinear, 100.0f, 0.0f, 100000000.0f, G::Turn,
              "The same at a slow turn: how soon a turn dies once the rudder is amidships." },
            { "bank", "Lean into a turn (deg per m/s2)", &S::bank, 0.01f, 0.0f, 20.0f, G::Turn,
              "How far it leans in, per m/s2 across its way." },
            { "maxBank", "Most lean (deg)", &S::maxBank, 0.1f, 0.0f, 80.0f, G::Turn, "The most it leans into a turn." },
            { "reserveLift", "Reserve lift (of the weight)", &S::reserveLift, 0.01f, 1.0f, 50.0f, G::Sea,
              "The most the water lifts it, in its weights. Lower = it ploughs through a crest instead of flying off it (a\n"
              "light hull in the air has no keel and no rudder)." },
            { "heaveDamping", "Heave damping (1/s)", &S::heaveDamping, 0.01f, 0.0f, 50.0f, G::Sea,
              "How soon its rise and fall on the waves settles." },
            { "pitchDamping", "Pitch damping (1/s)", &S::pitchDamping, 0.01f, 0.0f, 50.0f, G::Sea,
              "How soon bow up / bow down settles. Too little and it rocks in step with a sea met at speed." },
            { "rollDamping", "Roll damping (1/s)", &S::rollDamping, 0.01f, 0.0f, 50.0f, G::Sea, "How soon a roll settles." },
            { "rollStiffness", "Form stability", &S::rollStiffness, 100.0f, 0.0f, 100000000.0f, G::Sea,
              "How hard it rights itself from a lean, N m per radian. Higher = flatter in a sea and in a turn." },
            { "waveForce", "Wave slope force", &S::waveForce, 0.01f, 0.0f, 5.0f, G::Sea,
              "The waves' slope pushing the hull down it: 1 = physics, 0 = none. A swell surfs it, a sea at an angle yaws it." },
            { "slam", "Slam", &S::slam, 10.0f, 0.0f, 1000000.0f, G::Sea,
              "The blow when the hull falls into a wave, N per (m/s)^2 of the fall." },
            { "slamMax", "Slam limit (of the weight)", &S::slamMax, 0.01f, 0.0f, 10.0f, G::Sea, "The most one slam takes." },
            { "centerOfMassY", "Centre of mass height (m)", &S::centerOfMassY, 0.01f, -10.0f, 20.0f, G::Hydrostatics,
              "Over the mesh's origin. Low, or it rolls over." },
            { "draft", "Draft (m)", &S::draft, 0.005f, 0.01f, 20.0f, G::Hydrostatics,
              "Where still water stands on it, over the mesh's origin: the hull's hydrostatics at its weight. The water mask\n"
              "is baked for it." },
            { "anchorScope", "Anchor's slack (m)", &S::anchorScope, 0.1f, 0.0f, 500.0f, G::Anchor, "The rode lies slack this far round the anchor." },
            { "anchorStiffness", "Anchor rode stiffness (N/m)", &S::anchorStiffness, 50.0f, 0.0f, 10000000.0f, G::Anchor, "Its pull per m past the slack." },
            { "anchorDamping", "Anchor rode damping (N s/m)", &S::anchorDamping, 50.0f, 0.0f, 10000000.0f, G::Anchor,
              "Its pull per m/s of the bow running out along it." },
            { "sprayScale", "Spray size (1 = small_yacht's)", &S::sprayScale, 0.01f, 0.1f, 4.0f, G::Look,
              "The spray's sprites times it, their throw times its root: the presets were made for a 10 m hull." },
            { "cameraDistance", "Chase camera distance (m)", &S::cameraDistance, 0.1f, 1.0f, 500.0f, G::Look, "Behind the boat, at the helm." },
            { "cameraHeight", "Chase camera height (m)", &S::cameraHeight, 0.05f, -10.0f, 100.0f, G::Look, "Over the mesh's origin." },
            { "giProbeSpeed", "Out of the GI probes past (m/s, 0 = never)", &S::giProbeSpeed, 0.05f, 0.0f, 100.0f, G::Look,
              "Faster than this the hull leaves the GI probes' rays (they stand in the world and it flies through them)." },
        };
        return keys;
    }
    inline const VesselKey* FindVesselKey(std::string_view key)
    {
        for (const VesselKey& entry : VesselKeys())
        {
            if (key == entry.key) { return &entry; }
        }
        return nullptr;
    }

    inline void ReadVec3(const nlohmann::json& block, const char* key, Math::float3& out)
    {
        const auto it = block.find(key);
        if (it == block.end() || !it->is_array() || it->size() != 3) { return; }
        if ((*it)[0].is_number() && (*it)[1].is_number() && (*it)[2].is_number())
        {
            out = Math::float3((*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>());
        }
    }

    inline void ReadVesselSettings(const nlohmann::json& block, VesselSettings& out)
    {
        ReadVec3(block, "forward", out.forward);
        ReadVec3(block, "propeller", out.propeller);
        ReadVec3(block, "helmEye", out.helmEye);
        for (const VesselKey& entry : VesselKeys())
        {
            const auto it = block.find(entry.key);
            if (it != block.end() && it->is_number()) { out.*(entry.member) = it->get<float>(); }
        }
    }

    // False when `block` is not an object. *hasExcludeSlots says whether the list was written (absent = the foliage slots).
    inline bool ReadCollisionSettings(const nlohmann::json& block, CollisionSettings& out, bool* hasExcludeSlots = nullptr)
    {
        if (hasExcludeSlots) { *hasExcludeSlots = false; }
        if (!block.is_object()) { return false; }
        if (const auto it = block.find("shape"); it != block.end() && it->is_string())
        {
            ShapeKindFromName(it->get<std::string>(), out.shape);
        }
        out.primitives.clear();
        if (const auto it = block.find("shapes"); it != block.end() && it->is_array())
        {
            for (const nlohmann::json& part : *it)
            {
                if (!part.is_object()) { continue; }
                CollisionPrimitive p;
                const std::string type = part.value("type", std::string("box"));
                p.kind = type == "sphere" ? PrimitiveKind::Sphere : type == "capsule" ? PrimitiveKind::Capsule : type == "cylinder" ? PrimitiveKind::Cylinder : PrimitiveKind::Box;
                ReadVec3(part, "center", p.center);
                ReadVec3(part, "rotationDeg", p.rotationDeg);
                ReadVec3(part, "halfExtents", p.halfExtents);
                p.radius = part.value("radius", p.radius);
                p.halfHeight = part.value("halfHeight", p.halfHeight);
                out.primitives.push_back(p);
            }
        }
        if (const auto it = block.find("lod"); it != block.end() && it->is_number())
        {
            out.lod = it->get<int>();
        }
        if (const auto it = block.find("excludeSlots"); it != block.end() && it->is_array())
        {
            out.excludeSlots.clear();
            for (const nlohmann::json& v : *it)
            {
                if (v.is_number()) { out.excludeSlots.push_back(v.get<int>()); }
            }
            if (hasExcludeSlots) { *hasExcludeSlots = true; }
        }
        if (const auto it = block.find("density"); it != block.end() && it->is_number())
        {
            out.density = it->get<float>();
        }
        if (const auto it = block.find("mass"); it != block.end() && it->is_number())
        {
            out.mass = it->get<float>();
        }
        if (const auto it = block.find("friction"); it != block.end() && it->is_number())
        {
            out.friction = it->get<float>();
        }
        if (const auto it = block.find("restitution"); it != block.end() && it->is_number())
        {
            out.restitution = it->get<float>();
        }
        if (const auto it = block.find("convexRadius"); it != block.end() && it->is_number())
        {
            out.convexRadius = it->get<float>();
        }
        if (const auto it = block.find("rollingResistance"); it != block.end() && it->is_number())
        {
            out.rollingResistance = it->get<float>();
        }
        if (const auto it = block.find("angularDamping"); it != block.end() && it->is_number())
        {
            out.angularDamping = it->get<float>();
        }
        if (const auto it = block.find("float"); it != block.end() && it->is_number())
        {
            out.floatFactor = it->get<float>();
        }
        if (const auto it = block.find("twoSided"); it != block.end() && it->is_boolean())
        {
            out.twoSided = it->get<bool>();
        }
        return true;
    }

    // `writeExcludeSlots`: the list as an authored choice. Empty result = no key at all (the caller erases "collision").
    inline nlohmann::json WriteCollisionSettings(const CollisionSettings& s, bool writeExcludeSlots)
    {
        const CollisionSettings d;
        nlohmann::json block = nlohmann::json::object();
        if (s.shape != d.shape) { block["shape"] = ShapeKindName(s.shape); }
        if (!s.primitives.empty())
        {
            nlohmann::json parts = nlohmann::json::array();
            for (const CollisionPrimitive& p : s.primitives)
            {
                nlohmann::json part = { { "center", WriteVec3(p.center) } };
                if (p.rotationDeg.x != 0.0f || p.rotationDeg.y != 0.0f || p.rotationDeg.z != 0.0f) { part["rotationDeg"] = WriteVec3(p.rotationDeg); }
                switch (p.kind)
                {
                case PrimitiveKind::Box:
                    part["type"] = "box";
                    part["halfExtents"] = WriteVec3(p.halfExtents);
                    break;
                case PrimitiveKind::Sphere:
                    part["type"] = "sphere";
                    part["radius"] = p.radius;
                    break;
                case PrimitiveKind::Capsule:
                case PrimitiveKind::Cylinder:
                    part["type"] = p.kind == PrimitiveKind::Capsule ? "capsule" : "cylinder";
                    part["radius"] = p.radius;
                    part["halfHeight"] = p.halfHeight;
                    break;
                }
                parts.push_back(std::move(part));
            }
            block["shapes"] = std::move(parts);
        }
        if (s.lod != d.lod) { block["lod"] = s.lod; }
        if (writeExcludeSlots) { block["excludeSlots"] = s.excludeSlots; }
        if (s.density != d.density) { block["density"] = s.density; }
        if (s.mass != d.mass) { block["mass"] = s.mass; }
        if (s.friction != d.friction) { block["friction"] = s.friction; }
        if (s.restitution != d.restitution) { block["restitution"] = s.restitution; }
        if (s.convexRadius != d.convexRadius) { block["convexRadius"] = s.convexRadius; }
        if (s.angularDamping != d.angularDamping) { block["angularDamping"] = s.angularDamping; }
        if (s.rollingResistance != d.rollingResistance) { block["rollingResistance"] = s.rollingResistance; }
        if (s.floatFactor != d.floatFactor) { block["float"] = s.floatFactor; }
        if (s.twoSided != d.twoSided) { block["twoSided"] = s.twoSided; }
        return block;
    }
} // namespace physics
