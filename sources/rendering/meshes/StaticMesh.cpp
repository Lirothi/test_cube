#include "rendering/meshes/StaticMesh.h"

using namespace Math;

namespace
{
    bool NearlyEqualFloat3(const float3& a, const float3& b)
    {
        return Math::NearlyEqual(a.x, b.x) && Math::NearlyEqual(a.y, b.y) && Math::NearlyEqual(a.z, b.z);
    }
}

StaticMesh::StaticMesh(const std::string& modelName,
    const std::string& matPreset,
    const std::string& inputLayout,
    const std::wstring& graphicsShader)
    : GBufferRenderable(matPreset, inputLayout, graphicsShader),
    modelName_(modelName)
{
    pos_ = float3(0.0f, 0.0f, 0.0f);
    rotEuler_ = float3(0.0f, 0.0f, 0.0f);
    scale_ = float3(1);
    transformDirty_ = true;
}

void StaticMesh::Init(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    GBufferRenderable::Init(renderer, uploadCmdList, uploadKeepAlive);
    if (!modelName_.empty())
    {
        mesh_ = renderer->GetMeshManager()->Load(modelName_, renderer, uploadCmdList, uploadKeepAlive, { true, false, 0 });
    }
}

void StaticMesh::SetPosition(const float3& p)
{
    if (NearlyEqualFloat3(pos_, p))
    {
        return;
    }
    pos_ = p;
    MarkTransformDirty();
}

void StaticMesh::SetScale(const float3& s)
{
    if (NearlyEqualFloat3(scale_, s))
    {
        return;
    }
    scale_ = s;
    MarkTransformDirty();
}

void StaticMesh::SetRotationEulerRad(const float3& eulerXYZ)
{
    if (NearlyEqualFloat3(rotEuler_, eulerXYZ))
    {
        return;
    }
    rotEuler_ = eulerXYZ;
    MarkTransformDirty();
}

void StaticMesh::SetRotationEulerDeg(const float3& eulerDegXYZ)
{
    const float k = DEG2RAD;
    SetRotationEulerRad(float3(eulerDegXYZ.x * k, eulerDegXYZ.y * k, eulerDegXYZ.z * k));
}

void StaticMesh::PostTick(float /*deltaTime*/)
{
    if (!transformDirty_)
    {
        return;
    }

    RebuildModel();
    transformDirty_ = false;
}

void StaticMesh::RebuildModel()
{
    mat4 T = mat4::Translation(pos_);
    mat4 S = mat4::Scaling(scale_);
    mat4 R = mat4::RotationFromEulerXYZRad(rotEuler_);
    mat4 M = S * R * T;
    SetModelMatrix(M);
}

void StaticMesh::MarkTransformDirty()
{
    transformDirty_ = true;
}
