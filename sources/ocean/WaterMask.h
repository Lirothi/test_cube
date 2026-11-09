#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/math/Math.h"
#include "core/threading/Knob.h"

class RenderableObject;

// The water kept out of a hull (2026-11-09, owner: "the water floods the inside of the boat; make a mask for marked meshes so
// the water is not drawn inside them"). A mesh asset with mesh.json "waterMask": true keeps the ocean's surface out of a
// VOLUME made from its LOD 0: the surface's pixel shader takes its point into the model's space (through the pose the frame
// draws the object at: its render offset, the waves, the physics) and discards it inside.
//
// The volume is COLUMNS, the mesh rasterized from above (same day, owner on the first cut: "your method artefacts, better a
// mask by rasterization"). The first cut was the convex hull, and a convex hull bridges every concavity: behind the transom
// it closed a wedge from the swim platform up to the deck, and the sea over the platform was cut in a straight edge. Now,
// on a grid over the model's xz (kWaterMaskCellsAlong cells along its long side), each cell keeps water out between
//   bottom: the LOWEST surface over each of four points of the cell, the highest of the four -- a cell the mesh does not
//           cover whole keeps no water out, so the mask stops inside the hull's skin;
//   top:    in a DEPRESSION, the height water poured on the boat would stand at before it spilled over the lowest edge of
//           its rim (a priority flood from outside over the cells' rims, openings narrower than kWaterMaskGap closed
//           first): the cockpit to its gunwale, its walkthrough to the platform shut; elsewhere the column all four
//           points share (the highest surface over each, the lowest of the four), lifted to a filled neighbour's level --
//           the swim platform keeps only its own slab, open water over it stays, and a cell on the outer skin half over
//           a spray rail keeps the rail and not the air beside the topside.
// A view ray plays no part: it is the water's point that is tested, so any camera -- outside, at the helm -- sees the
// same volume, and a shape no convex volume follows (the platform aft of the transom, a flared bow) is kept as it is.
namespace ocean
{
// `--set ocean.waterMask:<0|1|2>` (transient): 0 no water kept out (the A/B), 1 the mask, 2 the kept-out water painted
// magenta instead of discarded (where the volume is).
inline int g_waterMask = 1;
inline int g_waterMask_game = 1;
inline const bool g_waterMask_knob = ::knob::Register(g_waterMask_game, g_waterMask, "ocean::g_waterMask");

// A mesh's columns in the model's space: cell (i, k) covers x [originX + i cell, + cell), z [originZ + k cell, + cell);
// columns[k * nx + i] = (bottom, the top of the water it keeps out, the top of its SOLID, 0) -- (+big, -big, -big, 0) for
// none. The solid top is the water's top but in a filled depression, where it is the cell's own highest surface (the
// cockpit's floor, a seat): a particle (EmitterDesc::collideHulls) dies inside the hull, not in the cockpit's air. A free
// cell rings the mesh.
struct WaterMaskField
{
    float originX = 0.0f;
    float originZ = 0.0f;
    float cell = 1.0f;
    std::uint32_t nx = 0;
    std::uint32_t nz = 0;
    float minY = 0.0f; // the mesh's: the quick reject
    float maxY = 0.0f;
    std::vector<Math::float4> columns;
};

// One a frame: a field and the world-to-model transform its object is drawn at.
struct WaterMaskInstance
{
    std::shared_ptr<const WaterMaskField> field;
    Math::mat4 worldToModel;
};

// The hulls a frame (shaders/ocean_surface_surf_sim.hlsli WaterMaskCB), the long side's cells (small_yacht's 10.2 m: 4 cm),
// the room every field together has on the GPU (OceanRenderable's buffer: 2 MB of float4, five small_yachts), and the
// widest opening in a rim that is closed before the depressions fill (m: a walkthrough from the cockpit to the platform).
constexpr std::uint32_t kWaterMaskMaxHulls = 4u;
constexpr std::uint32_t kWaterMaskCellsAlong = 256u;
constexpr std::uint32_t kWaterMaskMaxCells = 1u << 17;
constexpr float kWaterMaskGap = 1.0f;

// A frame's hulls as the shaders take them (shaders/water_mask_columns.hlsli): 5 rows each -- the world-to-model rows (the
// model's x y z = dot(row.xyz, world) + row.w), the grid (origin x, origin z, 1 / cell, asuint its first cell), the span
// (the mesh's lowest and highest y, asuint nx, asuint nz) -- and the columns' SRV. OceanRenderable fills it before the
// render graph (EnsureSimulationResources) whether the surface keeps water out or not (ocean.waterMask); the surface's
// WaterMaskCB and the particles' hull collision read it. The render half's alone.
struct WaterMaskFrame
{
    std::uint32_t hulls = 0;
    float rows[kWaterMaskMaxHulls * 5][4] = {};
    std::uint64_t columnsSrv = 0; // a CPU descriptor handle's ptr; 0 = no columns yet
};
inline WaterMaskFrame g_waterMaskFrame;

namespace watermask
{
    // The columns of a geometry's LOD 0 (above). Null when it cannot be read. Kept by the registry, one per geometry path.
    std::shared_ptr<const WaterMaskField> BuildField(const std::string& geometryPath);
    // RenderableObject::SetWaterMask: the registry the frame reads, never a scene walk. Register builds the field of the
    // object's geometry the first time a geometry is met (one per path, kept for the process).
    void Register(RenderableObject* object);
    void Unregister(RenderableObject* object);
    // At Scene::Sync, after the physics poses: every registered object that is visible and has a field, with the inverse of
    // the matrix the frame draws it at.
    void Collect(std::vector<WaterMaskInstance>& out);
}
} // namespace ocean
