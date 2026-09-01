#pragma once

#include <cstdint>
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

    TransparentStaticMesh* AsTransparentStaticMesh() override { return this; }

    void SetTint(const float3& tint) { tint_ = tint; }
    void SetAbsorption(const float3& absorption) { absorption_ = absorption; }
    void SetThickness(float thickness) { thickness_ = thickness; }
    void SetReflectionStrength(float strength) { reflectionStrength_ = strength; }
    void SetRefractionDistortion(float distortion) { refractionDistortion_ = distortion; }
    void SetRoughness(float roughness) { roughness_ = roughness; }
    void SetIor(float ior) { ior_ = ior; }
    void SetNormalMap(const std::wstring& path, bool normalIsRG = false);
    void SetRecomputeNormalSlots(std::vector<uint32_t> slots);

    bool HasNormalMap() const { return hasNormalMap_; }
    bool IsNormalMapRG() const { return normalMapIsRG_; }

    bool IsSimpleRender() const override { return false; }
    // Rung 1 (Step 10): dynamic only while it spins (rotationSpeed != 0); a still glass pane is
    // a static caster.
    bool IsDynamicCaster() const override { return rotationSpeed_ != 0.0f; }
    // S15b: glass samples the off-screen glass reflection, so it (and only it, not the ocean
    // or other transparent renderables) is rasterized into the glass-reflection G-buffer.
    bool UsesGlassReflection() const override { return true; }

protected:
    bool RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx, const Camera& camera, uint8_t* cbData) override;
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
    std::vector<uint32_t> recomputeNormalSlots_;
};
