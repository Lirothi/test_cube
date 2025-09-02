#include "StaticMesh.h"

using namespace Math;

StaticMesh::StaticMesh(const std::string& modelName,
    const std::string& matPreset,
    const std::string& inputLayout,
    const std::wstring& graphicsShader)
    : RenderableObject(matPreset, inputLayout, graphicsShader),
    modelName_(modelName)
{
    pos_ = float3(0.0f, 0.0f, 0.0f);
    rot_ = mat4::Identity();
    scale_ = float3(1);
    RebuildModel();
}

void StaticMesh::Init(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    RenderableObject::Init(renderer, uploadCmdList, uploadKeepAlive);
    if (!modelName_.empty())
    {
        mesh_ = renderer->GetMeshManager()->Load(modelName_, renderer, uploadCmdList, uploadKeepAlive, { true, false, 0 });
    }
}

void StaticMesh::UpdateUniforms(Renderer* renderer, const Math::mat4& view, const Math::mat4& proj, uint8_t* cbData)
{
    if (!cbData) { return; }
    UpdateUniform("world", GetModelMatrix(), cbData);
    UpdateUniform("view", view, cbData);
    UpdateUniform("proj", proj, cbData);
    
    ApplyMaterialParamsToCB(cbData);
}

void StaticMesh::SetPosition(const float3& p)
{
    pos_ = p;
    RebuildModel();
}

void StaticMesh::SetScale(const float3& s)
{
    scale_ = s;
    RebuildModel();
}

void StaticMesh::SetRotationEulerRad(const float3& eulerXYZ)
{
    rot_ = mat4::RotationFromEulerXYZRad(eulerXYZ); // Rx * Ry * Rz
    RebuildModel();
}

void StaticMesh::SetRotationEulerDeg(const float3& eulerDegXYZ)
{
    const float k = DEG2RAD;
    SetRotationEulerRad(float3(eulerDegXYZ.x * k, eulerDegXYZ.y * k, eulerDegXYZ.z * k));
}

void StaticMesh::SetOrientationMatrix(const mat4& rotation3x3In4x4)
{
    rot_ = rotation3x3In4x4;
    RebuildModel();
}

void StaticMesh::RebuildModel()
{
    mat4 T = mat4::Translation(pos_);
    mat4 S = mat4::Scaling(scale_);
    mat4 M = S * rot_ * T;
    SetModelMatrix(M);
}