#pragma once
#include "rendering/renderables/GBufferRenderable.h"
#include "core/Math.h"

class StaticMesh : public GBufferRenderable
{
public:
    StaticMesh(const std::string& modelName,
        const std::string& matPreset,
        const std::string& inputLayout,
        const std::wstring& graphicsShader);

    virtual void Init(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive) override;
    void PostTick(float deltaTime) override;

    // --- Transforms ---
    void SetPosition(const float3& p);
    void SetScale(const float3& s);
    void SetRotationEulerRad(const float3& eulerXYZ);   // (pitch=X, yaw=Y, roll=Z), radians
    void SetRotationEulerDeg(const float3& eulerDegXYZ);

    float3 GetPosition() const { return pos_; }
    float3 GetScale() const { return scale_; }
    mat4 GetOrientationMatrix() const { return mat4::RotationFromEulerXYZRad(rotEuler_); }
    float3 GetRotationEulerRad() const { return rotEuler_; }

    bool IsSimpleRender() const { return true; }
    bool CastsShadow() const override { return true; }

private:
    void RebuildModel(); // M = R * T (for the row-vector convention)
    void MarkTransformDirty();

private:
    float3 rotEuler_;
    float3 pos_;
    float3 scale_;
    bool transformDirty_ = true;

    std::string modelName_;
};
