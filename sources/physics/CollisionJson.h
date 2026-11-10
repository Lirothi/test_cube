#pragma once

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

    // The numbers of a mesh.json "vessel" block: ONE table for the file, the Mesh Editor and --set vessel.<key>.
    struct VesselKey
    {
        const char* key;
        const char* label;
        float VesselSettings::* member;
        float speed;
        float lo;
        float hi;
    };
    inline const std::vector<VesselKey>& VesselKeys()
    {
        using S = VesselSettings;
        static const std::vector<VesselKey> keys = {
            { "mass", "Mass (kg)", &S::mass, 10.0f, 1.0f, 1000000.0f },
            { "centerOfMassY", "Centre of mass height (m)", &S::centerOfMassY, 0.01f, -10.0f, 20.0f },
            { "draft", "Draft (m)", &S::draft, 0.005f, 0.01f, 20.0f },
            { "reserveLift", "Reserve lift (of the weight)", &S::reserveLift, 0.01f, 1.0f, 50.0f },
            { "thrust", "Thrust (N)", &S::thrust, 100.0f, 0.0f, 10000000.0f },
            { "reverseShare", "Astern share", &S::reverseShare, 0.01f, 0.0f, 1.0f },
            { "throttleRate", "Throttle rate (1/s)", &S::throttleRate, 0.01f, 0.01f, 20.0f },
            { "rudderDeg", "Rudder hard over (deg)", &S::rudderDeg, 0.25f, 0.0f, 80.0f },
            { "rudderRate", "Rudder rate (deg/s)", &S::rudderRate, 1.0f, 1.0f, 1000.0f },
            { "rudderForce", "Rudder force", &S::rudderForce, 1.0f, 0.0f, 100000.0f },
            { "wash", "Propeller wash (m/s)", &S::wash, 0.05f, 0.0f, 50.0f },
            { "surgeDrag", "Drag ahead", &S::surgeDrag, 1.0f, 0.1f, 100000.0f },
            { "swayDrag", "Drag across", &S::swayDrag, 10.0f, 0.0f, 1000000.0f },
            { "swayLinear", "Drift damping", &S::swayLinear, 10.0f, 0.0f, 1000000.0f },
            { "swayLever", "Keel aft of the centre (m)", &S::swayLever, 0.01f, -50.0f, 50.0f },
            { "yawDrag", "Turn drag", &S::yawDrag, 1000.0f, 0.0f, 100000000.0f },
            { "yawLinear", "Turn damping", &S::yawLinear, 100.0f, 0.0f, 100000000.0f },
            { "heaveDamping", "Heave damping (1/s)", &S::heaveDamping, 0.01f, 0.0f, 50.0f },
            { "pitchDamping", "Pitch damping (1/s)", &S::pitchDamping, 0.01f, 0.0f, 50.0f },
            { "rollDamping", "Roll damping (1/s)", &S::rollDamping, 0.01f, 0.0f, 50.0f },
            { "rollStiffness", "Form stability", &S::rollStiffness, 100.0f, 0.0f, 100000000.0f },
            { "bank", "Lean into a turn (deg per m/s2)", &S::bank, 0.01f, 0.0f, 20.0f },
            { "maxBank", "Most lean (deg)", &S::maxBank, 0.1f, 0.0f, 80.0f },
            { "humpSpeed", "Hump speed (m/s)", &S::humpSpeed, 0.05f, 0.1f, 50.0f },
            { "planeSpeed", "Planing speed (m/s)", &S::planeSpeed, 0.05f, 0.2f, 100.0f },
            { "humpDrag", "Hump drag (N)", &S::humpDrag, 50.0f, 0.0f, 10000000.0f },
            { "humpTrim", "Bow up at the hump", &S::humpTrim, 500.0f, 0.0f, 100000000.0f },
            { "planeTrim", "Bow up on the plane", &S::planeTrim, 500.0f, 0.0f, 100000000.0f },
            { "planingLift", "Planing lift", &S::planingLift, 0.01f, 0.0f, 2.0f },
            { "waveForce", "Wave slope force", &S::waveForce, 0.01f, 0.0f, 5.0f },
            { "slam", "Slam", &S::slam, 10.0f, 0.0f, 1000000.0f },
            { "slamMax", "Slam limit (of the weight)", &S::slamMax, 0.01f, 0.0f, 10.0f },
            { "anchorScope", "Anchor's slack (m)", &S::anchorScope, 0.1f, 0.0f, 500.0f },
            { "anchorStiffness", "Anchor rode stiffness (N/m)", &S::anchorStiffness, 50.0f, 0.0f, 10000000.0f },
            { "anchorDamping", "Anchor rode damping (N s/m)", &S::anchorDamping, 50.0f, 0.0f, 10000000.0f },
            { "sprayScale", "Spray size (1 = small_yacht's)", &S::sprayScale, 0.01f, 0.1f, 4.0f },
            { "cameraDistance", "Chase camera distance (m)", &S::cameraDistance, 0.1f, 1.0f, 500.0f },
            { "cameraHeight", "Chase camera height (m)", &S::cameraHeight, 0.05f, -10.0f, 100.0f },
            { "giProbeSpeed", "Out of the GI probes past (m/s, 0 = never)", &S::giProbeSpeed, 0.05f, 0.0f, 100.0f },
        };
        return keys;
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
