#include "rendering/meshes/StaticMesh.h"

#include "rendering/meshes/MeshManager.h"

#include <algorithm>
#include <cctype>

using namespace Math;

namespace
{
    bool IsGltfPath(const std::string& path)
    {
        const size_t frag = path.find('#');
        std::string ext = (frag == std::string::npos) ? path : path.substr(0, frag);
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto endsWith = [&](const char* s, size_t n) {
            return ext.size() >= n && ext.compare(ext.size() - n, n, s) == 0;
        };
        return endsWith(".gltf", 5) || endsWith(".glb", 4);
    }
}

std::string StaticMesh::GetGltfMaterialSourcePath() const
{
    // B2: this is a SOURCE path, not a decision — per-slot "auto vs named preset" is resolved in
    // GBufferRenderable::ResolveMaterialSlots (slots whose preset is "auto"/absent pull from the
    // glTF; named presets win for their slot).
    return IsGltfPath(modelName_) ? modelName_ : std::string{};
}

StaticMesh::StaticMesh(const std::string& modelName,
    const std::string& matPreset,
    const std::string& inputLayout,
    const std::wstring& graphicsShader)
    : GBufferRenderable(matPreset, inputLayout, graphicsShader)
    , modelName_(modelName)
{
}

void StaticMesh::SetRecomputeNormalSlots(std::vector<uint32_t> slots)
{
    std::sort(slots.begin(), slots.end());
    slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
    recomputeNormalSlots_ = std::move(slots);
}

void StaticMesh::Init(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    // B2: load the mesh BEFORE the base Init — material-slot resolution needs the submesh count
    // (one slot per glTF material group).
    if (renderer && !modelName_.empty())
    {
        MeshLoadOptions options;
        options.generateTangentSpace = true;
        options.wantCW = false;
        options.recomputeNormalSlots = recomputeNormalSlots_;
        options.chunkGrid = chunkGrid_;
        SetMesh(renderer->GetMeshManager()->Load(
            modelName_, renderer, uploadCmdList, uploadKeepAlive, options));
    }

    GBufferRenderable::Init(renderer, uploadCmdList, uploadKeepAlive);
}
