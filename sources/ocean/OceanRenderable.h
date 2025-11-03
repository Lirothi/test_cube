#pragma once

#include <array>
#include <memory>
#include <vector>

#include "core/math/Math.h"
#include "rendering/renderables/RenderableObject.h"
#include "materials/Texture2D.h"
#include "ocean/OceanSimulation.h"

class Camera;
class SamplerManager;

class Scene;

class OceanRenderable : public RenderableObject
{
public:
    explicit OceanRenderable(Camera* camera, Scene* scene);
    ~OceanRenderable() override = default;

    void Init(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive) override;

    void Tick(float deltaTime) override;

    void RecordCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl) override;
    void PopulateContext(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx) override;
    void RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx) override;

    bool IsTransparent() const override { return true; }
    bool IsSimpleRender() const override { return false; }
    bool CastsShadow() const override { return false; }

    void OnMaterialHotReload(Renderer* renderer) override;

    void SetGridVertexDensity(uint32_t density);

private:
    struct ClipLevel
    {
        float halfExtent = 1.0f;
        Math::float2 offset = Math::float2(0.0f, 0.0f);
        float step = 1.0f;
    };

    class OceanUniformBinder;

    void BuildMesh(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
    void UpdateClipLevels();

    void ConfigureGraphicsPipeline(Renderer* renderer, Material::GraphicsDesc& desc) const override;

    Math::float4 GetSimulationParams() const;
    Math::float4 GetViewerParams() const;
    Math::float4 GetCascadeLengthScales() const;
    Math::float4 GetCascadeInvLengthScales() const;
    Math::float4 GetClipMapParams() const;
    Math::float4 GetClipMapViewer() const;
    Math::float4 GetFoamParams0() const;
    Math::float4 GetFoamParams1() const;
    Math::float4 GetFoamCascadeWeights() const;
    Math::float4 GetSpecularParams() const;
    Math::float4 GetRefractionParams() const;
    Math::float4 GetSubsurfaceParams() const;
    Math::float4 GetHeightFogParams() const;
    Math::float4 GetSunDirAmbient() const;
    Math::float4 GetSunColorExposure() const;
    Math::float4 GetDeepScatterColor() const;
    Math::float4 GetSssColor() const;
    Math::float4 GetDiffuseColor() const;
    Math::float4 GetAbsorptionGradientParams() const;
    Math::float4 GetAbsorptionColor(uint32_t index) const;
    uint32_t GetAbsorptionColorCount() const;
    mat4 GetWorldToWindMatrix() const;
    Math::float4 GetWindParams0() const;
    Math::float4 GetWindParams1() const;
    Math::float4 GetFoamTrailParams0() const;
    Math::float4 GetFoamTrailParams1() const;
    Math::float4 GetFoamParams2() const;
    Math::float4 GetFoamTint() const;
    Math::float4 GetDepthTextureSize(const Renderer* renderer) const;
    Math::float2 GetDepthParams() const;
    void UpdateFoamTrailState();

private:
    Camera* camera_ = nullptr;
    Scene* scene_ = nullptr;
    std::unique_ptr<OceanSimulation> simulation_;

    float elapsedTime_ = 0.0f;
    Math::float2 viewerXZ_ = Math::float2(0.0f, 0.0f);
    float viewerHeight_ = 0.0f;

    static constexpr UINT kClipLevels = 7;
    std::array<ClipLevel, kClipLevels> clipLevels_{};
    Math::float4 lengthScales_ = Math::float4(0.0f, 0.0f, 0.0f, 0.0f);
    Math::float4 invLengthScales_ = Math::float4(0.0f, 0.0f, 0.0f, 0.0f);

    uint32_t meshVertexDensity_ = 25u;
    float clipMapScale_ = 1.0f;
    float clipMapLevelHalfSize_ = 0.0f;
    Math::float3 clipMapViewer_ = Math::float3(0.0f, 0.0f, 0.0f);
    float cascadesFadeScale_ = 20.0f;
    float minMeshScale_ = 15.0f;

    Texture2D foamDetailTexture_;
    Texture2D foamAlbedoTexture_;
    Texture2D foamUnderwaterTexture_;
    Texture2D foamTrailTexture_;
    Texture2D contactFoamTexture_;
    Texture2D distantRoughnessTexture_;

    Math::float2 foamTrailTextureSize0_ = Math::float2(100.0f, 50.0f);
    Math::float2 foamTrailTextureSize1_ = Math::float2(100.0f, 50.0f);
    Math::float2 foamTrailDirection0_ = Math::float2(1.0f, 0.0f);
    Math::float2 foamTrailDirection1_ = Math::float2(1.0f, 0.0f);
    float foamTrailBlendValue_ = 0.0f;
    float foamTrailBlendStartTime_ = 0.0f;
    float foamTrailBlendDuration_ = 0.0f;
    bool foamTrailBlendActive_ = false;
    bool foamTrailHasHistory_ = false;
};

