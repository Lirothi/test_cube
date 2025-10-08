#include "rendering/meshes/StaticMesh.h"

#include "rendering/meshes/MeshManager.h"

using namespace Math;

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
