#pragma once

#include <vector>

#include "materials/Texture2D.h"
#include "rendering/renderables/RenderableObject.h"

class Scene;

class TransparentStaticMesh final : public RenderableObject
{
public:
    TransparentStaticMesh(Scene* scene,
        const std::string& modelName,
        const float3& position,
        const float3& scale,
        float rotationSpeedRad);

    void Init(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive) override;

    void Tick(float deltaTime) override;

    void SetTint(const float3& tint) { tint_ = tint; }
    void SetAbsorption(const float3& absorption) { absorption_ = absorption; }
    void SetThickness(float thickness) { thickness_ = thickness; }
    void SetReflectionStrength(float strength) { reflectionStrength_ = strength; }
    void SetRefractionDistortion(float distortion) { refractionDistortion_ = distortion; }
    void SetRoughness(float roughness) { roughness_ = roughness; }
    void SetIor(float ior) { ior_ = ior; }
    void SetNormalMap(const std::wstring& path, bool normalIsRG = false);

    bool HasNormalMap() const { return hasNormalMap_; }
    bool IsNormalMapRG() const { return normalMapIsRG_; }

    bool IsSimpleRender() const override { return false; }

protected:
    void PopulateContext(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx) override;
    void ConfigureGraphicsPipeline(Renderer* renderer, Material::GraphicsDesc& desc) const override;

private:
    class TransparentUniformBinder;

    Scene* scene_ = nullptr;
    std::string modelName_;
    float rotationSpeed_ = 0.0f;

    float ior_ = 1.52f;
    float3 absorption_ = float3(0.25f, 0.08f, 0.04f);
    float thickness_ = 0.6f;
    float reflectionStrength_ = 1.0f;
    float refractionDistortion_ = 0.015f;
    float3 tint_ = float3(0.85f, 0.93f, 1.0f);
    float roughness_ = 0.07f;

    Texture2D normalMap_{};
    std::wstring normalMapPath_{};
    bool normalMapIsRG_ = false;
    bool hasNormalMap_ = false;
};
