#pragma once

#include <array>
#include <memory>
#include <vector>

#include "core/Math.h"
#include "rendering/renderables/RenderableObject.h"
#include "ocean/OceanSimulation.h"

class Camera;
class SamplerManager;

class OceanRenderable : public RenderableObject
{
public:
    explicit OceanRenderable(Camera* camera);
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

    Math::float4 GetClipData(uint32_t index) const;
    Math::float4 GetSimulationParams() const;
    Math::float4 GetViewerParams() const;
    Math::float4 GetCascadeLengthScales() const;
    Math::float4 GetCascadeInvLengthScales() const;
    Math::float4 GetClipMapParams() const;
    Math::float4 GetClipMapViewer() const;

private:
    Camera* camera_ = nullptr;
    std::unique_ptr<OceanSimulation> simulation_;

    float elapsedTime_ = 0.0f;
    Math::float2 viewerXZ_ = Math::float2(0.0f, 0.0f);
    float viewerHeight_ = 0.0f;
    std::array<ClipLevel, OceanSimulation::kClipLevels> clipLevels_{};
    uint32_t activeClipLevels_ = OceanSimulation::kClipLevels;
    Math::float4 lengthScales_ = Math::float4(0.0f, 0.0f, 0.0f, 0.0f);
    Math::float4 invLengthScales_ = Math::float4(0.0f, 0.0f, 0.0f, 0.0f);

    uint32_t meshVertexDensity_ = 25u;
    float clipMapScale_ = 1.0f;
    float clipMapLevelHalfSize_ = 0.0f;
    Math::float3 clipMapViewer_ = Math::float3(0.0f, 0.0f, 0.0f);
    float cascadesFadeScale_ = 20.0f;
    float minMeshScale_ = 15.0f;
};

