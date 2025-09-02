#pragma once
#include "RenderableObject.h"
#include "Math.h"

class StaticMesh : public RenderableObject
{
public:
    StaticMesh(const std::string& modelName,
        const std::string& matPreset,
        const std::string& inputLayout,
        const std::wstring& graphicsShader);

    virtual void Init(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive) override;
    virtual void UpdateUniforms(Renderer* renderer, const Math::mat4& view, const Math::mat4& proj, uint8_t* cbData) override;

    // --- Трансформы ---
    void SetPosition(const float3& p);
    void SetScale(const float3& s);
    void SetRotationEulerRad(const float3& eulerXYZ);   // (pitch=X, yaw=Y, roll=Z), радианы
    void SetRotationEulerDeg(const float3& eulerDegXYZ);
    void SetOrientationMatrix(const mat4& rotation3x3In4x4);

    float3 GetPosition() const { return pos_; }
    float3 GetScale() const { return scale_; }
    const mat4& GetOrientationMatrix() const { return rot_; }

    bool IsSimpleRender() const { return true; }
    bool CastsShadow() const override { return true; }

private:
    void RebuildModel(); // M = R * T (под твою row-vector семантику)

private:
    mat4   rot_;   // ортонормальная 3x3 в верхнем левом блоке
    float3 pos_;
    float3 scale_;

    std::string modelName_;
};