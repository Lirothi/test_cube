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

    inline void ReadVec3(const nlohmann::json& block, const char* key, Math::float3& out)
    {
        const auto it = block.find(key);
        if (it == block.end() || !it->is_array() || it->size() != 3) { return; }
        if ((*it)[0].is_number() && (*it)[1].is_number() && (*it)[2].is_number())
        {
            out = Math::float3((*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>());
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
