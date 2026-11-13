#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/math/Math.h"
#include "core/threading/Knob.h"

class RenderableObject;

// The water kept out of a hull (2026-11-09, owner: "the water floods the inside of the boat; make a mask for marked meshes so
// the water is not drawn inside them"). A mesh asset with mesh.json "waterMask": true keeps the ocean's surface out of its
// cockpits -- whatever water poured on the boat would hold before it spilled.
//
// THE LID, rasterized (2026-11-11, owner: "the water cutting is still so-so, artefacts all the time -- go classic, mask
// rasterization"; then, on an authored mask mesh: "you would be needed at every import -- make a smart heuristic that
// generates the mask mesh"). Until then the test was the water's point against COLUMNS -- the mesh rasterized from above on a
// 4 cm grid, each cell keeping the water out from the hull's skin up to a level --, and every wall the grid crossed was up
// to a cell wrong one way or the other: water cut outside a single-skinned hull at the waterline, or a line of water inside
// it. A lid takes the walls from the HULL ITSELF:
//   - every BASIN (a depression of the columns' flood: water poured on the boat stands there up to its rim's lowest edge,
//     openings narrower than kWaterMaskGap shut) gets a CAP, the plane of that level cut through the mesh: its outline is
//     where the plane meets the hull's own walls (exact: the cross-section's segments), and it reaches through a door into an
//     enclosed space under a deck or a roof (a cabin) unless that space opens on the outside;
//   - where its outline crosses an opening instead of a wall (a walkthrough to the swim platform, a notch in the rim) a
//     vertical CURTAIN hangs from it down to the sill.
// The cap and the curtains with the hull's opaque walls enclose the basin: a view ray that reaches the water through an odd
// number of them reached it inside (shaders/water_mask_lid.hlsl draws them into the two nearest crossings of every pixel
// before the water, the surface counts the ones in front of its point -- odd = kept out). The hull's walls are never
// approximated, so nothing is cut outside the hull however its skin leans, and nothing is left inside it.
namespace ocean
{
// `--set ocean.waterMask:<0|1|2|3>` (transient): 0 no water kept out (the A/B), 1 the mask, 2 the kept-out water painted
// magenta instead of discarded (where the lid keeps it out), 3 the mask, and the water still drawn over a hull's plan painted
// red (a leak, or a wave over the deck).
inline int g_waterMask = 1;
inline int g_waterMask_game = 1;
inline const bool g_waterMask_knob = ::knob::Register(g_waterMask_game, g_waterMask, "ocean::g_waterMask");

// One basin's part of a lid: the cap's plane, its triangles in the field's lid (the camera's own test), its box and, per
// field cell, the FLOOR under it (the highest surface below the plane; -big = none): a camera is in the basin between the two.
struct WaterMaskBasin
{
    float capY = 0.0f;
    std::uint32_t firstIndex = 0; // the cap's triangles, in lidIndices
    std::uint32_t capIndexCount = 0;
    float minX = 0.0f, minZ = 0.0f, maxX = 0.0f, maxZ = 0.0f;
    std::vector<float> floor;     // nx * nz
};

// A mesh's mask in the model's space. The COLUMNS (cell (i, k) covers x [originX + i cell, + cell), z [originZ + k cell,
// + cell); columns[k * nx + i].w = the PLAN: the lowest surface over the cell wherever the mesh touches it at all, +big where
// it does not; .xyz unused) are what a particle is tested against (EmitterDesc::collideHulls: it dies anywhere over the plan,
// from its lowest surface up) and what ocean.waterMask:3 paints. The LID (above) is what keeps the water out: its
// triangles, every basin's cap and then its curtains.
struct WaterMaskField
{
    float originX = 0.0f;
    float originZ = 0.0f;
    float cell = 1.0f;
    std::uint32_t nx = 0;
    std::uint32_t nz = 0;
    float minY = 0.0f; // the mesh's: the quick reject
    float maxY = 0.0f;
    std::vector<Math::float4> columns; // nx * nz
    std::vector<Math::float3> lidVertices;
    std::vector<std::uint32_t> lidIndices;
    std::vector<WaterMaskBasin> basins;
};

// One a frame: a field and the transforms its object is drawn at.
struct WaterMaskInstance
{
    std::shared_ptr<const WaterMaskField> field;
    Math::mat4 worldToModel;
    Math::mat4 modelToWorld;
};

// The hulls a frame (shaders/ocean_surface_surf_sim.hlsli WaterMaskCB), the long side's cells (small_yacht's 10.2 m: 4 cm),
// the room every field's columns together have on the GPU (OceanRenderable's buffer, in float4, one a cell), the room of the
// lids (vertices, indices), the widest opening in a rim that is closed before the depressions fill (m: a walkthrough from
// the cockpit to the platform), and a lid's raster: kWaterMaskLidSub pixels a cell side (small_yacht: 1 cm).
constexpr std::uint32_t kWaterMaskMaxHulls = 4u;
constexpr std::uint32_t kWaterMaskCellsAlong = 256u;
constexpr std::uint32_t kWaterMaskMaxCells = 1u << 18;
constexpr std::uint32_t kWaterMaskLidMaxVertices = 1u << 17;
constexpr std::uint32_t kWaterMaskLidMaxIndices = 1u << 19;
constexpr float kWaterMaskGap = 1.0f;
constexpr std::uint32_t kWaterMaskLidSub = 4u;
// How far over its still waterline the sea reaches into a boat (m): a cap lies no higher than that, and a basin whose floor
// is over it gets none -- a foredeck in its rails, a roof, a helm under a hardtop 1.4 m up never meet the sea, and a cap
// there only has more to go wrong (glass, frames, rails at the plane).
constexpr float kWaterMaskReach = 1.0f;

// A frame's hulls as the shaders take them (shaders/water_mask_columns.hlsli): 5 rows each -- the world-to-model rows (the
// model's x y z = dot(row.xyz, world) + row.w), the grid (origin x, origin z, 1 / cell, asuint its first cell), the span
// (the mesh's lowest and highest y, asuint nx, asuint nz) -- and the columns' SRV. OceanRenderable fills it before the
// render graph (EnsureSimulationResources) whether the surface keeps water out or not (ocean.waterMask); the surface's
// WaterMaskCB (ocean.waterMask:3) and the particles' hull collision read it. The render half's alone.
struct WaterMaskFrame
{
    std::uint32_t hulls = 0;
    float rows[kWaterMaskMaxHulls * 5][4] = {};
    std::uint64_t columnsSrv = 0; // a CPU descriptor handle's ptr; 0 = no columns yet
};
inline WaterMaskFrame g_waterMaskFrame;

// THE BAKE (2026-11-11, owner: "why can't this mesh be generated at import?"). A field depends on the geometry and on what is
// set after the import -- the vessel's draft, which slots are glass -- so it is made once and kept beside its geometry,
// `models/<name>/<name>.watermask.bin`, with what it was made from: the .mesh.bin's size and time, the draft, the glass slots and
// kWaterMaskBakeVersion. The frame reads it when all of them match (~1 ms); otherwise it builds the field on a worker
// (the hull keeps no water out until it is done) and, in a build with the editor, writes the file -- after a Mesh Editor Save
// (its placed copies come back with the new draft or glass), after a re-bake of the geometry, or the first time. A Release
// build only reads. BUMP kWaterMaskBakeVersion WITH ANY CHANGE TO WHAT BuildField MAKES: every bake on disk is then stale.
constexpr std::uint32_t kWaterMaskBakeVersion = 1u;

// What a field was made from (THE BAKE, above).
struct WaterMaskBakeKey
{
    std::uint64_t geometryBytes = 0;
    std::int64_t geometryTime = 0; // the file's last write, std::filesystem's ticks
    float waterline = 3.0e38f;
    float draftOverLowest = -3.0e38f;
    std::vector<int> glassSlots;
};

namespace watermask
{
    // The bake file of a geometry ("<x>.watermask.bin" beside "<x>.mesh.bin"); empty when the geometry is no .mesh.bin.
    std::string BakePathOf(const std::string& geometryPath);
    // The key of a geometry as it is on disk now; false = the geometry cannot be stat'ed.
    bool MakeBakeKey(const std::string& geometryPath, const std::vector<int>& seeThroughSlots, float waterline, float draftOverLowest,
                     WaterMaskBakeKey& out);
    // A field and its key as the bake file holds them, and back. Deserialize returns null (and says why) when the bytes are
    // no bake, another version, or -- with `expect` -- made from anything else.
    std::vector<std::uint8_t> SerializeField(const WaterMaskField& field, const WaterMaskBakeKey& key);
    std::shared_ptr<WaterMaskField> DeserializeField(const std::uint8_t* bytes, std::size_t size, const WaterMaskBakeKey* expect,
                                                     std::string& why);
    // The content cook (docs/distribution_build_plan.md §8.1 item 4): a bake made for the loose geometry (`looseBytes`,
    // `looseVersion` = its vfs::Stat) is re-keyed for the same geometry in a package, whose vfs::Stat::version is its content
    // hash -- only the key's geometry version changes, so the frame reads it there as it reads it from the files. False (and
    // why) when the bytes are no bake of this version or were made from another bake of the geometry.
    bool RekeyBakeForPackage(std::vector<std::uint8_t>& bytes, std::uint64_t looseBytes, std::uint64_t looseVersion,
                             std::uint64_t packagedVersion, std::string& why);
    // A bake's key as it stands in the file (the draft, the glass slots the frame built it for), without its field.
    bool ReadBakeKey(const std::uint8_t* bytes, std::size_t size, WaterMaskBakeKey& out, std::string& why);
    // The bytes the GPU and the tests read of a field (the plan, the lid, the basins), to compare two fields exactly.
    bool SameField(const WaterMaskField& a, const WaterMaskField& b);

    // The columns and the lid of a geometry's LOD 0 (above). `seeThroughSlots`: its glass (RenderableObjectBase::
    // SeeThroughSlots) -- a wall for the flood, and where it parts a basin from the outside below the cap, faces of the lid (a
    // view ray passes it as it passes a curtain). Null when the geometry cannot be read. Thread-safe (Collect runs it on a
    // worker); the registry keeps one per geometry and key. `waterline`: where still water stands on it (the model's y: physics::VesselSettings::draft) -- caps lie
    // no higher than kWaterMaskReach over it; or, not known, `draftOverLowest` (buoyancy::Settings::draft: over the mesh's
    // lowest point, < 0 = a quarter of its height); neither = caps at their basins' levels.
    std::shared_ptr<const WaterMaskField> BuildField(const std::string& geometryPath, const std::vector<int>& seeThroughSlots = {},
                                                     float waterline = 3.0e38f, float draftOverLowest = -3.0e38f);
    // Is a point (the model's space) inside a basin of the lid: under its cap, over its floor? The camera's: the surface's
    // count of the lid crossings in front of its point starts from it.
    bool InsideLid(const WaterMaskField& field, const Math::float3& modelPos);
    // RenderableObject::SetWaterMask: the registry the frame reads, never a scene walk. Collect finds the field of an object
    // the first time it meets it -- one per geometry and key, kept for the process: the bake when it matches, else built on a
    // worker (THE BAKE, above).
    void Register(RenderableObject* object);
    void Unregister(RenderableObject* object);
    // At Scene::Sync, after the physics poses: every registered object that is visible and has a field, with the matrix the
    // frame draws it at and its inverse.
    void Collect(std::vector<WaterMaskInstance>& out);
}
} // namespace ocean
