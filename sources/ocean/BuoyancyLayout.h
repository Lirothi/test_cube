#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/math/Math.h"

// Buoyancy of a mesh on the ocean: WHERE it floats (the pontoons) and HOW it moves (draft,
// inertia, damping). Model space throughout -- the baked .mesh.bin's metres, bakeScale already in
// the vertices; an instance's own scale is applied by the runtime (ocean/OceanBuoyancy).
//
// A pontoon is a sphere (UE's Water plugin calls them the same): its submerged volume pushes up.
// The automatic layout puts the sphere CENTRES on the resting waterline, each one's radius chosen
// so its waterplane section equals the hull waterplane it stands for -- see AutoLayout.
namespace buoyancy
{
    struct Pontoon
    {
        Math::float3 center;
        float radius = 0.0f;
    };

    // What an asset (mesh.json "buoyancy") or a level object may say. Every field has an
    // "automatic" value, so an empty block -- or no block at all -- still floats sensibly.
    struct Settings
    {
        // Resting waterline above the mesh's LOWEST point, in metres. < 0 = automatic (a quarter
        // of the hull's height, see AutoLayout).
        float draft = -1.0f;
        // Added mass: 1 = the calibrated hull; 3 = the same draft but heave, pitch and roll all
        // answer the waves sqrt(3) times slower. A heavy boat wallows, a float bobs.
        float inertia = 1.0f;
        // Damping ratio of the heave (0 = rings forever, 1 = critically damped).
        float damping = 0.25f;
        // Empty = automatic, from the geometry.
        std::vector<Pontoon> pontoons;
    };

    // Settings resolved against the geometry: what the runtime integrates.
    struct Layout
    {
        float draft = 0.0f;       // resolved (never automatic)
        float waterline = 0.0f;   // model-space y of the calm surface at rest = keel + draft
        float keel = 0.0f;        // model-space y of the lowest point
        float hullHeight = 0.0f;  // keel to gunwale (the auto draft's yardstick)
        float inertia = 1.0f;
        float damping = 0.25f;
        std::vector<Pontoon> pontoons;
        bool automatic = false;   // pontoons came from AutoLayout, not from the settings

        bool Valid() const { return !pontoons.empty(); }
    };

    // The automatic layout. Rasterises the hull's underside into a plan-view grid along its
    // principal axes, finds the WATERPLANE at the draft (cells whose underside is below the
    // waterline -- an oar blade above it, a mast, a cabin roof contribute nothing), splits it into
    // a lengthwise x crosswise grid of bins and puts one pontoon on each bin's centroid, radius
    // sqrt(area / pi). Heave stiffness is then the hull's own waterplane, and roll/pitch
    // stiffness its second moments -- a narrow hull rolls quicker than a wide one because it is
    // narrower, not because a constant said so. `positions`/`indices` are LOD0 triangles.
    // `draft` < 0 = automatic. Never throws; an empty result means "nothing floats here".
    Layout AutoLayout(const std::vector<Math::float3>& positions,
        const std::vector<std::uint32_t>& indices,
        float draft = -1.0f);

    // Settings -> Layout for a mesh on disk (a .mesh.bin or any MeshManager::ParseFileCpu path).
    // Explicit pontoons are kept as authored; the geometry still supplies keel and hull height.
    // Cached per (path, draft): every instance of a boat shares one analysis.
    Layout Resolve(const Settings& settings, const std::string& geometryPath);

    // Drop the cached analysis of a geometry that was just re-baked ("" = every geometry). The
    // cache is keyed by PATH, and a re-bake keeps the path: without this the next instance would
    // float on the old hull until a restart.
    void ForgetGeometry(const std::string& geometryPath);
}
