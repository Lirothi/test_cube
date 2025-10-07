#pragma once

#include <vector>

#include "rendering/renderables/RenderableObject.h"

class Scene;

class GlassCube final : public RenderableObject
{
public:
    GlassCube(Scene* scene,
        const std::string& modelName,
        float3 position,
        float3 scale,
        float rotationSpeedRad);

    void Init(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive) override;

    void Tick(float deltaTime) override;
    void PostTick(float deltaTime) override;

    void SetTint(const float3& tint) { tint_ = tint; }
    void SetAbsorption(const float3& absorption) { absorption_ = absorption; }
    void SetThickness(float thickness) { thickness_ = thickness; }
    void SetReflectionStrength(float strength) { reflectionStrength_ = strength; }
    void SetRefractionDistortion(float distortion) { refractionDistortion_ = distortion; }
    void SetRoughness(float roughness) { roughness_ = roughness; }
    void SetIor(float ior) { ior_ = ior; }

    bool IsSimpleRender() const override { return false; }

protected:
    void PopulateContext(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx) override;

private:
    class GlassUniformBinder;

    void MarkTransformDirty();
    void RebuildModel();

    Scene* scene_ = nullptr;
    std::string modelName_;
    float3 pos_{};
    float3 scale_{};
    float3 rotEuler_{};
    bool transformDirty_ = false;
    float rotationSpeed_ = 0.0f;

    float ior_ = 1.52f;
    float3 absorption_ = float3(0.25f, 0.08f, 0.04f);
    float thickness_ = 0.6f;
    float reflectionStrength_ = 1.0f;
    float refractionDistortion_ = 0.015f;
    float3 tint_ = float3(0.85f, 0.93f, 1.0f);
    float roughness_ = 0.07f;
};
