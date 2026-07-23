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
    static GltfMaterialDesc DescribeGltfMaterial(const std::string& pathWithFragment,
                                                 int groupOrdinal = -1);

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
                                           std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);

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
