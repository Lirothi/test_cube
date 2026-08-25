#pragma once
#include <memory>
#include <string>
#include "third_party/robin_hood.h"
#include <vector>
#include <wrl.h>
#include "rendering/meshes/Mesh.h"

class Renderer;

struct MeshLoadOptions {
    bool generateTangentSpace = true; // Compute normals/tangents if the file does not provide them
    bool wantCW = false;               // Convert triangles to CW (for D3D12 FrontCounterClockwise = FALSE)
    int  iBase  = 0;                  // Index base used in "i a b c"
    // Material/submesh slots whose authored vertex normals are discarded before tangent-space
    // generation. Tangents are regenerated as well so the resulting TBN stays coherent.
    std::vector<uint32_t> recomputeNormalSlots;
    // mesh.json "windFoliage": per material slot, 0 = wood, >0 = foliage. Only the bake reads it (to
    // seed the along-limb distance field from the wood surface); the runtime still applies the values
    // themselves, so changing a weight does not need a re-bake -- only changing a slot between 0 and
    // non-zero does, because that moves the wood/foliage boundary. Empty = classification unknown,
    // bake falls back to the per-component ramp.
    std::vector<float> slotFoliage;

    // LOD3 foliage prune (bake path only). Alpha-card foliage cannot be edge-collapsed -- the
    // shape lives in the silhouette and UV island, so meshopt either stalls (~1.4x on a palm
    // crown) or shreds blades into spikes with Permissive. Instead LOD3 removes whole leaf
    // components and scales the survivors by 1/sqrt(keep) about their centroid so the crown's
    // silhouette DENSITY is preserved (Cook et al., stochastic aggregate simplification).
    // Fraction of leaf components kept; 0 disables (foliage LOD3 falls back to meshopt).
    // Only the BAKE can prune: the scaled survivors are APPENDED vertex copies (LODs are index
    // buffers over one shared VB), and the runtime-fallback path uploads the VB before LODs.
    float foliagePruneKeep = 0.35f;

    // LOD3 solid budget: true = the harsh level (target 5%, error 0.25 — user decision
    // 2026-08-21, the last level cuts hard instead of adding a fifth). false = keep the classic
    // 0.12/0.12 — what TERRAIN wants: its far-clipmap shadow casters are tuned against low-sun
    // banding and a 0.25 error deforms dune silhouettes. mesh.json "lod3Aggressive".
    bool lod3Aggressive = true;
    // LOD3's OWN budget multipliers over the harsh base (0.05 target / 0.25 error) — separate
    // from the whole-chain lodRatioScale/lodErrorScale so the last level can be pushed without
    // touching LODs 1-2. SOLID slots only; the kept-leaf interior decimation has its own pair
    // below. mesh.json "lod3RatioScale"/"lod3ErrorScale".
    float lod3RatioScale = 1.0f;
    float lod3ErrorScale = 1.0f;
    // Interior decimation of the KEPT leaves at LOD3 (LockBorder+Permissive over the pruned
    // subset): target fraction of the kept triangles and the error budget (relative to mesh
    // extent). mesh.json "foliageInnerRatio"/"foliageInnerError".
    float foliageInnerRatio = 0.5f;
    float foliageInnerError = 0.15f;
    // Material slots that VANISH at LOD3 entirely (empty submesh keeps the table aligned).
    // The surgical answer for trims that survive every metric — a date palm's husk scales are
    // welded to the trunk, so neither Prune nor the error budget can separate them from the
    // silhouette-critical cylinder. mesh.json "lod3DropSlots": [..].
    std::vector<uint32_t> lod3DropSlots;
    // Same contract for the earlier levels — each level's list is independent (a slot dropped
    // at LOD1 is NOT implicitly dropped later; every LOD builds from the BASE indices).
    // mesh.json "lod1DropSlots"/"lod2DropSlots".
    std::vector<uint32_t> lod1DropSlots;
    std::vector<uint32_t> lod2DropSlots;
    // Grow factor over the prune's automatic 1/sqrt(keep) survivor inflation: 1 = full area
    // compensation (holds silhouette DENSITY but can read fluffier than the source crown),
    // 0 = survivors stay authored-size. mesh.json "foliageGrow".
    float foliageGrow = 1.0f;
    // UV weight for attribute-aware simplification of FOLIAGE slots
    // (meshopt_simplifyWithAttributes). Position-only collapse slides vertices freely along a
    // flat alpha card — zero geometric error, but the leaf texture smears into streaks by
    // LOD1. A non-zero weight makes UV distortion cost like position error (UE's simplifier
    // is attribute-aware for the same reason). 0 = plain meshopt_simplify, byte-identical to
    // the old bake. mesh.json "foliageUvWeight".
    float foliageUvWeight = 0.0f;

    // NORMAL weight for attribute-aware simplification of EVERY slot
    // (meshopt_simplifyWithAttributes). LODs are index buffers over ONE shared vertex buffer, so a
    // surviving vertex keeps the normal that was computed for the LOD0 surface around it. A
    // position-only collapse is blind to that: it will happily remove the vertices that made the
    // stored normals correct, and the shading normal then stops matching the geometry it is drawn
    // on. Measured on atoll_island's current bake, the area-weighted angle between a triangle's
    // geometric normal and its interpolated shading normal grows 0.18 deg (LOD0) -> 0.25 -> 0.40 ->
    // 1.57 (LOD3), with p99 reaching 23 deg -- which at this scene's ~3 deg sun elevation is a large
    // change in N.L, i.e. the lighting visibly shifts between levels even where the silhouette does
    // not. A non-zero weight makes normal distortion cost like position error, so the simplifier
    // spends its budget on collapses that keep shading intact.
    //
    // Fixing the normals themselves would need meshopt_simplifyWithUpdate, which rewrites the
    // attribute values -- but it does so DESTRUCTIVELY on the shared vertex buffer, so LOD3's
    // rewrite would corrupt LOD0. That needs per-LOD vertex data first; this knob is the part that
    // works within one VB.
    //
    // 0 = plain position-only metric, byte-identical to the old bake. mesh.json "lodNormalWeight".
    float lodNormalWeight = 0.0f;

    // Uniform factor applied to VERTEX POSITIONS at bake time, so an asset authored in the wrong
    // unit (centimetres is the common one -- the tent bakes at 418 x 227 x 284) comes out of the
    // bake already in metres. The alternative the importer used to offer, a `spawnScale` on the
    // asset, only ever corrected the SPAWN: every instance carried scale 0.0107, the mesh's own
    // numbers stayed in centimetres, and anything reading model space (bounds, the editor's size
    // readout, a hand-placed instance) still saw the wrong unit.
    //
    // Applied before the wind bake and before LOD generation, because both are unit-aware: the
    // sway extent is in metres and the LOD error budgets are taken against mesh extents.
    // Positions only -- normals and tangents are directions and a uniform POSITIVE scale leaves
    // them alone. Non-positive values are ignored rather than flipping winding silently.
    float bakeScale = 1.0f;

    // --- LOD generation knobs (exposed per-import in the mesh import window) ---------------------
    // Defaults reproduce the shipped chain exactly: per-level target ratios 0.5/0.25/0.12 at error
    // budgets 0.02/0.05/0.12 (relative to mesh extents), with meshopt's safe options.
    //
    // `lodSimplifyOptions` takes meshopt_Simplify* flags. Handle with care on MASKED FOLIAGE:
    // meshopt_SimplifyPermissive removes the topological floor that alpha-card leaves hit (a palm's
    // foliage slot otherwise barely reduces across the whole chain) and even reports a LOWER geometric
    // error — but it visibly shreds leaf blades into spikes, because a position-only error metric
    // cannot see that a leaf card's shape lives in its silhouette and UV island. Judge foliage LODs by
    // the wireframe, not by the reported error. Off by default for that reason.
    float lodRatioScale = 1.0f;         // scales the per-level triangle targets (<1 = more aggressive)
    float lodErrorScale = 1.0f;         // scales the per-level error budgets (<1 = preserve shape more)
    unsigned int lodSimplifyOptions = 0; // meshopt_Simplify* flags; 0 = safe default

    // --- Mesh chunking (mesh.json "chunkGrid", 0/1 = off) -----------------------------------------
    // Bake time: split a SINGLE-submesh mesh's LOD0 triangles into an N x N grid over its XZ extent
    // and emit each non-empty cell as its own submesh (same material slot, same triangles, merely
    // reordered). 0 or 1 means one tile, i.e. not chunked.
    //
    // Runtime, BOTH paths — this stopped being shadow-only:
    //   CAMERA  each tile picks its own LOD from its own distance (render::SelectChunkLodTier),
    //           and the mesh draws per submesh at those tiers.
    //   SHADOWS each tile is an independent caster with its own bounds, so a virtual-shadow-map
    //           page rasterizes only the tiles that reach it instead of the whole surface — and it
    //           casts at the SAME per-chunk tier the camera drew, so the surface cannot shadow
    //           itself with geometry it is not showing.
    //
    // Chunks are simplified independently, so their shared borders have to be pinned or adjacent
    // chunks crack apart at LOD >= 1 — see BuildLodsCpu's LockBorder/Sparse/ErrorAbsolute block.
    // LOD0 is a loss-free reorder, so the partition itself changes no pixels.
    unsigned int chunkGrid = 0;
};

// CPU-side geometry prepared independently of D3D12. Thumbnail jobs parse and
// preprocess this on worker threads, then only enqueue GPU uploads in the editor.
struct MeshCpuData {
    std::vector<VertexPNTUV> vertices;
    std::vector<uint32_t> indices;
    std::vector<Mesh::Submesh> submeshes;
};

// A3: plain (cgltf-free) description of the glTF material for a given selector, resolved with the
// SAME group ordering as the geometry load (so "#N" addresses one consistent group). Texture
// paths are resolved relative to the glTF file (URI-decoded); empty when the channel is absent.
struct GltfMaterialDesc {
    bool  valid = false;          // false => no material (null-material group) or unresolved
    std::string albedoPath;
    std::string mrPath;
    std::string normalPath;
    std::string emissivePath;
    float baseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // baseColorFactor (multiplies albedo texture)
    float metallic     = 1.0f;    // metallicFactor  (multiplies MR.B)
    float roughness    = 1.0f;    // roughnessFactor (multiplies MR.G)
    float normalScale  = 1.0f;
    float emissive[3]  = {0.0f, 0.0f, 0.0f};
    bool  alphaMask    = false;   // alphaMode == MASK
    float alphaCutoff  = 0.5f;
    bool  doubleSided  = false;
};

class MeshManager {
public:
    // Parse a supported mesh without creating GPU resources. glTF fragment
    // selectors are honored and tangent generation is CPU-only.
    bool ParseFileCpu(const std::string& path, MeshCpuData& out,
        const MeshLoadOptions& opt = {});

    // Upload already prepared geometry without reparsing or generating LODs.
    std::shared_ptr<Mesh> CreateFromCpuData(const std::string& key,
        Renderer* renderer,
        const MeshCpuData& data,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);

    // A3/B2: parse a glTF/GLB material (CPU-only, no GPU, no buffer load). groupOrdinal >= 0
    // picks that group within the selector's resolved, material-index-ordered group list
    // (submesh i of a multi-submesh load == ordinal i); -1 = the selector's own group choice.
    // Returns desc.valid=false if the group has no material.
    // Drops the cached glTF material descriptions (a re-import writes new files).
    static void InvalidateGltfMaterialCache();
    static GltfMaterialDesc DescribeGltfMaterial(const std::string& pathWithFragment,
                                                 int groupOrdinal = -1);

    // Per-LOD, per-submesh triangle counts of a baked .mesh.bin, CPU-only (no GPU, no upload).
    // CountSubmeshes answers "how many", this answers "how big is each, at every level" — which is
    // what a CHUNKED mesh needs shown, since its submeshes are spatial tiles rather than material
    // slots and their LOD reduction is the thing that tells you the grid is too fine.
    // Returns false for a missing/unreadable/non-.mesh.bin path.
    struct BinaryLodInfo
    {
        std::vector<uint32_t> submeshTris; // triangles per submesh at this LOD
        uint32_t totalTris = 0;
        float error = 0.0f; // v2: worst-case object-space deviation of this level from LOD0
    };
    struct BinaryInfo
    {
        uint32_t vertexCount = 0;
        std::vector<BinaryLodInfo> lods; // lods[0] = LOD0
    };
    static bool DescribeMeshBinary(const std::string& binPath, BinaryInfo& out);

    // Fill the BAKE-affecting fields of `opt` from a mesh.json manifest. Engine-side and
    // authoritative on purpose: the headless bake used to mirror a hand-picked SUBSET of these as
    // --reimport-* flags, so re-baking from the command line silently dropped whatever had no flag
    // (lod3Aggressive, lodRatioScale, the per-level drop slots, foliageGrow, foliageUvWeight...) and
    // produced geometry that disagreed with the manifest it came from. One reader, one meaning.
    // Leaves every field alone that the manifest does not mention. Returns false if the file is
    // missing or is not an object.
    static bool ApplyManifestOptions(const std::string& meshJsonPath, MeshLoadOptions& opt);

    // J: number of material slots (submeshes) a geometry resolves to (CPU-only, no GPU). glTF =
    // the resolved group count (#N selector = 1); non-glTF (.obj/.mesh.txt) = 1. Used by the Mesh
    // Editor to show exactly one material picker per submesh. Returns 1 on any parse failure.
    static size_t CountSubmeshes(const std::string& pathWithFragment);

    // Auto-detect by extension (.obj | .mesh.txt | .txt)
    std::shared_ptr<Mesh> Load(const std::string& path,
                               Renderer* renderer,
                               ID3D12GraphicsCommandList* uploadCmdList,
                               std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
                               const MeshLoadOptions& opt = {});

    // Explicit options
    std::shared_ptr<Mesh> LoadText(const std::string& path,
                                   Renderer* renderer,
                                   ID3D12GraphicsCommandList* uploadCmdList,
                                   std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
                                   const MeshLoadOptions& opt = {});

    std::shared_ptr<Mesh> LoadOBJ(const std::string& path,
                                  Renderer* renderer,
                                  ID3D12GraphicsCommandList* uploadCmdList,
                                  std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
                                  const MeshLoadOptions& opt = {});

    // glTF/GLB (cgltf). Path may carry a fragment selector:
    //   "models/foo.glb"            -> whole file, material group 0 (+ warning until Part B)
    //   "models/foo.glb#2"          -> material group index 2 (across the whole file)
    //   "models/foo.glb#node:Rock_1"-> only that node's subtree, merged by material
    std::shared_ptr<Mesh> LoadGltf(const std::string& path,
                                   Renderer* renderer,
                                   ID3D12GraphicsCommandList* uploadCmdList,
                                   std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
                                   const MeshLoadOptions& opt = {});

    // From memory (when verts/indices are already available)
    std::shared_ptr<Mesh> CreateFromMemory(const std::string& key,
                                           Renderer* renderer,
                                           const std::vector<VertexPNTUV>& verts,
                                           const std::vector<uint32_t>& indices,
                                           ID3D12GraphicsCommandList* uploadCmdList,
                                           std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
                                           bool generateTangentSpace = true);

    std::shared_ptr<Mesh> Get(const std::string& key) const;
    void Clear();

    // W7.1b reimport: bake `srcPath` (a glTF in import_staging/) to an EXPLICIT output path
    // (models/<name>/<name>.mesh.bin, referenced by mesh.json "geometry"). Runs the full CPU import
    // off the load path (parse glTF, regen normals/tangents, [W7.2 wind-color bake], simplify LODs)
    // and serializes verts+LODs. CPU-only (no GPU/device), so the `--reimport` CLI runs it headless
    // without booting the app. Pass the SAME opt the runtime loads with (wantCW=false, the mesh's
    // recomputeNormalSlots) or the baked winding/normals won't match.
    bool BakeToBinary(const std::string& srcPath, const std::string& outBinPath,
                      const MeshLoadOptions& opt);

    // True when binPath is missing, unreadable, or was baked under a different wood/foliage
    // classification — its per-vertex wind weights no longer match `opt` and it must be re-baked.
    // Cheap: reads the 32-byte header only.
    static bool BinaryNeedsRebake(const std::string& binPath, const MeshLoadOptions& opt);

private:
    // W7.1b: geometry referenced directly as our committed .mesh.bin (the glTF source
    // lives in import_staging/, never loaded at runtime). Reads the binary at `binPath` (version
    // check only — it IS the asset, no source to hash) and uploads GPU buffers + LODs. nullptr on
    // failure (a bad/absent .mesh.bin has no glTF fallback — the source isn't in the shipped tree).
    std::shared_ptr<Mesh> LoadBinaryDirect(const std::string& binPath,
                                           Renderer* renderer,
                                           ID3D12GraphicsCommandList* uploadCmdList,
                                           std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
                                           const MeshLoadOptions& opt = {});

    // Internal parsers
    bool ParseTextFile(const std::string& path,
                       std::vector<VertexPNTUV>& outVerts,
                       std::vector<uint32_t>& outIndices,
                       const MeshLoadOptions& opt);

    bool ParseOBJFile(const std::string& path,
                      std::vector<VertexPNTUV>& outVerts,
                      std::vector<uint32_t>& outIndices,
                      const MeshLoadOptions& opt);

    // Parses a glTF/GLB (fragment selector honored) into one flat vertex/index buffer. B2: the
    // whole-file and #node paths load ALL material groups (concatenated in material-index order)
    // and fill outSubmeshes with one range per group (materialSlot = group ordinal); "#N" loads
    // that single group (outSubmeshes left empty => default whole-buffer submesh). Bakes node
    // world transforms; flips winding for mirrored nodes.
    bool ParseGltfFile(const std::string& fullPath,
                       std::vector<VertexPNTUV>& outVerts,
                       std::vector<uint32_t>& outIndices,
                       std::vector<Mesh::Submesh>& outSubmeshes,
                       const MeshLoadOptions& opt);

private:
    robin_hood::unordered_map<std::string, std::shared_ptr<Mesh>> cache_;
};
