#pragma once
#if WITH_EDITOR

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/math/Math.h"
#include "third_party/json/json.hpp"

// A mesh asset's REST POSE: how a NEW copy of it lies when nothing says otherwise.
//
//   mesh.json "restRotationDeg": [pitch, yaw, roll]   (the level's rotationDeg convention)
//
// Absent = as authored, which for a tree or a post is upright and right. For a loose prop the
// authoring is whatever the source file happened to hold -- the seashells came in standing on
// their hinge, 37-56 degrees off flat, and every scatter of them stood like that.
//
// It is a SPAWN default, applied by every path that creates an object from the asset (spawn,
// place, the Content Browser, a drop into the viewport), and the height comes with it: the pivot
// is raised so the posed geometry's lowest point rests where the ground is. Placed copies carry
// their own rotation in the level and never follow a later change here.
namespace restpose
{
    std::optional<Math::float3> Read(const nlohmann::json& asset);
    std::optional<Math::float3> ReadFile(const std::string& meshJsonPath);

    // The pose that lays the geometry on its broadest side: the axis its SURFACE is thinnest along
    // (area-weighted, so a dense rim does not outvote a sparse face) turned to world up, and a
    // cupped shape rim-down, dome-up -- how a wave leaves a shell. An elongated shape lies along
    // its length for the same reason: its thinnest axis is across it. Empty when the geometry
    // cannot be read or is degenerate.
    std::optional<Math::float3> Auto(const std::vector<Math::float3>& positions,
        const std::vector<std::uint32_t>& indices);
    std::optional<Math::float3> Auto(const std::string& geometryPath);

    // How far the pivot has to rise for the geometry, turned by `rotationDeg` at scale 1, to rest
    // ON the pivot's height: minus its lowest point. A yaw on top changes nothing here. The
    // file form is cached per file (by write time) and pose; 0 when the geometry cannot be read.
    float Lift(const std::vector<Math::float3>& positions, const Math::float3& rotationDeg);
    float Lift(const std::string& geometryPath, const Math::float3& rotationDeg);

    // `restDeg` followed by a turn of `yawDeg` about world up, as rotationDeg.
    Math::float3 ComposeYaw(const Math::float3& restDeg, float yawDeg);

    // Lay a new object (IEditorObjectFactory::BuildDefaultJson's "mesh" + placement) in its
    // asset's rest pose: rotationDeg = the pose turned by `yawDeg`, position.y = `groundY` plus the
    // lift times the object's scale (the mean of three for a non-uniform one -- a scale applied
    // before the turn is not exact there). Returns false, touching nothing, for an asset without
    // a rest pose.
    bool ApplyToNewObject(nlohmann::json& objectJson, float yawDeg, float groundY);
}

#endif
