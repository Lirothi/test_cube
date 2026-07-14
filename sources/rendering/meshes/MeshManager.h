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
    bool wantCW = true;               // Convert triangles to CW (for D3D12 FrontCounterClockwise = FALSE)
    int  iBase  = 0;                  // Index base used in "i a b c"
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
    // A3: parse a glTF/GLB material for the selector's group (CPU-only, no GPU, no buffer load).
    // Consistent with LoadGltf's "#N" group ordering. Returns desc.valid=false if none.
    static GltfMaterialDesc DescribeGltfMaterial(const std::string& pathWithFragment);

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

private:
    // Internal parsers
    bool ParseTextFile(const std::string& path,
                       std::vector<VertexPNTUV>& outVerts,
                       std::vector<uint32_t>& outIndices,
                       const MeshLoadOptions& opt);

    bool ParseOBJFile(const std::string& path,
                      std::vector<VertexPNTUV>& outVerts,
                      std::vector<uint32_t>& outIndices,
                      const MeshLoadOptions& opt);

    // Parses a glTF/GLB (fragment selector honored) into one flat vertex/index buffer for the
    // selected material group. Bakes node world transforms; flips winding for mirrored nodes.
    bool ParseGltfFile(const std::string& fullPath,
                       std::vector<VertexPNTUV>& outVerts,
                       std::vector<uint32_t>& outIndices,
                       const MeshLoadOptions& opt);

private:
    robin_hood::unordered_map<std::string, std::shared_ptr<Mesh>> cache_;
};