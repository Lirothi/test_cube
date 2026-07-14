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
    // Use the glTF's own material when the model is a glTF and no explicit preset was requested
    // ("auto" or empty). A named preset in the level JSON overrides (placeholder/testing).
    const std::string& preset = MatPreset();
    if (IsGltfPath(modelName_) && (preset.empty() || preset == "auto"))
    {
        return modelName_;
    }
    return {};
}

StaticMesh::StaticMesh(const std::string& modelName,
    const std::string& matPreset,
    const std::string& inputLayout,
    const std::wstring& graphicsShader)
    : GBufferRenderable(matPreset, inputLayout, graphicsShader)
    , modelName_(modelName)
{
}

void StaticMesh::Init(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    GBufferRenderable::Init(renderer, uploadCmdList, uploadKeepAlive);
    if (!renderer)
    {
        return;
    }

    if (!modelName_.empty())
    {
        SetMesh(renderer->GetMeshManager()->Load(modelName_, renderer, uploadCmdList, uploadKeepAlive, { true, false, 0 }));
    }
}
