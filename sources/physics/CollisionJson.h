#pragma once

#include "physics/PhysicsTypes.h"
#include "third_party/json/json.hpp"

// mesh.json / level object "collision" block:
//   "collision": { "shape": "hull", "lod": 1, "excludeSlots": [1, 3], "density": 400, "mass": 0, "friction": 0.5,
//                  "restitution": 0.1, "convexRadius": 0.01, "twoSided": true }
// Every key is optional, and WriteSettings leaves defaults OUT so an untouched asset stays byte-identical.
// The per-object choice of motion is a separate key, "physics": "dynamic" -- not this block, which ResolveMeshAsset folds from
// the asset into every placed instance.
namespace physics
{
    // False when `block` is not an object. *hasExcludeSlots says whether the list was written (absent = the foliage slots).
    inline bool ReadCollisionSettings(const nlohmann::json& block, CollisionSettings& out, bool* hasExcludeSlots = nullptr)
    {
        if (hasExcludeSlots) { *hasExcludeSlots = false; }
        if (!block.is_object()) { return false; }
        if (const auto it = block.find("shape"); it != block.end() && it->is_string())
        {
            ShapeKindFromName(it->get<std::string>(), out.shape);
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
        if (s.lod != d.lod) { block["lod"] = s.lod; }
        if (writeExcludeSlots) { block["excludeSlots"] = s.excludeSlots; }
        if (s.density != d.density) { block["density"] = s.density; }
        if (s.mass != d.mass) { block["mass"] = s.mass; }
        if (s.friction != d.friction) { block["friction"] = s.friction; }
        if (s.restitution != d.restitution) { block["restitution"] = s.restitution; }
        if (s.convexRadius != d.convexRadius) { block["convexRadius"] = s.convexRadius; }
        if (s.angularDamping != d.angularDamping) { block["angularDamping"] = s.angularDamping; }
        if (s.floatFactor != d.floatFactor) { block["float"] = s.floatFactor; }
        if (s.twoSided != d.twoSided) { block["twoSided"] = s.twoSided; }
        return block;
    }
} // namespace physics
