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
    OceanRenderable(Camera* camera, Scene* scene, OceanSimulation* simulation);
    ~OceanRenderable() override = default;

    void Init(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive) override;

    void Tick(float deltaTime) override;

    void RecordCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl) override;
    void PrepareCompute(RenderGraphPassContext& ctx) override;
    void PrepareRender(RenderGraphPassContext& ctx) override;
    void RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx, const Camera& camera, uint8_t* cbData) override;
    // Only to give the surface draw a GPU scope — see the note on the definition.
    void Render(Renderer* renderer, ID3D12GraphicsCommandList* cl, const Camera& camera,
        D3D12_GPU_VIRTUAL_ADDRESS viewCB) override;

    bool IsTransparent() const override { return true; }
    bool IsSimpleRender() const override { return false; }
    bool CastsShadow() const override { return false; }
    // Rung 1 (Step 10): the surface is compute-simulated every frame — dynamic (moot while it
    // isn't a shadow caster, but correct if that ever changes).
    bool IsDynamicCaster() const override { return true; }

    void OnMaterialHotReload(Renderer* renderer) override;
    OceanRenderable* AsOceanRenderable() override { return this; }

    OceanSimulation* GetSimulation() { return simulation_; }
    const OceanSimulation* GetSimulation() const { return simulation_; }

    // W1: the ocean's simulation clock (seconds), accumulated in Tick and fed to the FFT sim. The
    // wind system reads this as its shared clock so waves and foliage sway stay phase-coherent.
    float GetElapsedTime() const { return elapsedTime_; }

    // Caustics: the flipbook lives with the ocean because it IS a water effect, but it is consumed
    // by the deferred lighting pass (see SceneRenderer::Pass_Lighting), which needs the CPU-side
    // SRV handle to stage into its own descriptor table. Null until Initialize has run.
    D3D12_CPU_DESCRIPTOR_HANDLE GetCausticsSrvCPU() const { return causticsTexture_.GetSRVCPU(); }
    // World-space Y of the still water plane. Everything below it receives caustics.
    float GetWaterLevel() const { return GetPosition().y; }

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
    Math::float4 GetPrevClipMapParams() const;
    Math::float4 GetPrevClipMapViewer() const;
    Math::float4 GetFoamParams0() const;
    Math::float4 GetFoamParams1() const;
    Math::float4 GetFoamCascadeWeights() const;
    Math::float4 GetSpecularParams() const;
    Math::float4 GetRefractionParams() const;
    Math::float4 GetSubsurfaceParams() const;
    Math::float4 GetHeightFogParams() const;
    Math::float4 GetNormalSamplingParams(const Renderer* renderer) const;
    Math::float4 GetShoreBehaviorParams0() const;
    Math::float4 GetShoreBehaviorParams1() const;
    Math::float4 GetShoreNormalMinWeights() const;
    Math::float4 GetShoreFoamGeometryParams() const;
    Math::float4 GetShoreFoamPatternParams() const;
    Math::float4 GetShoreFoamBreakupParams() const;
    Math::float4 GetShoreFoamWindParams() const;
    Math::float4 GetShoreFoamAlbedoParams() const;
    Math::float4 GetShoreSlopeParams() const;
    Math::float4 GetShoreSamplingParams() const;
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
    Math::float4 GetShoreViewParams() const;
    Math::float4 GetShoreDepthParams() const;
    const OceanRenderConfig& GetRenderConfig() const;
    void UpdateFoamTrailState();

private:
    Camera* camera_ = nullptr;
    Scene* scene_ = nullptr;
    OceanSimulation* simulation_ = nullptr;

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
    float prevClipMapScale_ = 1.0f;
    float prevClipMapLevelHalfSize_ = 0.0f;
    Math::float3 prevClipMapViewer_ = Math::float3(0.0f, 0.0f, 0.0f);
    float prevCascadesFadeScale_ = 20.0f;
    bool clipMapHasHistory_ = false;
    float cascadesFadeScale_ = 20.0f;

    Texture2D foamDetailTexture_;
    Texture2D foamAlbedoTexture_;
    Texture2D foamUnderwaterTexture_;
    Texture2D foamTrailTexture_;
    Texture2D shoreFoamBreakupMaskTexture_;
    Texture2D shoreFoamAlbedoTexture_;
    Texture2D distantRoughnessTexture_;
    Texture2D causticsTexture_;

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

