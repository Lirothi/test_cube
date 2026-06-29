#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include <wrl/client.h>

#include "core/math/Math.h"

#include "app/scene/SceneRenderConfig.h"
#include "materials/Material.h"

class Renderer;
class RenderableObjectBase;
class Skybox;
struct ID3D12GraphicsCommandList;

struct SceneLightingCBHandles
{
    Material::CBFieldHandle sunDir;
    Material::CBFieldHandle ambient;
    Material::CBFieldHandle lightRgb;
    Material::CBFieldHandle exposure;
    Material::CBFieldHandle camPos;
    Material::CBFieldHandle camDir;
    Material::CBFieldHandle invView;
    Material::CBFieldHandle invProj;
    Material::CBFieldHandle lightViewProj;
    Material::CBFieldHandle cascadeScaleBias;
    Material::CBFieldHandle cascadeSplits;
    Material::CBFieldHandle shadowAtlasSize;
    Material::CBFieldHandle shadowBiasNDC;
    Material::CBFieldHandle normalBiasWS;
    Material::CBFieldHandle screenSize;
    Material::CBFieldHandle invScreenSize;
    Material::CBFieldHandle sunMetalSpec;
    Material::CBFieldHandle sunAngularSize;

    void Populate(Material* material);
};

struct ScenePointLightCBHandles
{
    Material::CBFieldHandle invView;
    Material::CBFieldHandle invProj;
    Material::CBFieldHandle camPos;
    Material::CBFieldHandle lightCount;
    Material::CBFieldHandle screenSize;
    Material::CBFieldHandle invScreenSize;

    void Populate(Material* material);
};

struct SceneSpotLightCBHandles
{
    Material::CBFieldHandle invView;
    Material::CBFieldHandle invProj;
    Material::CBFieldHandle camPos;
    Material::CBFieldHandle lightCount;
    Material::CBFieldHandle screenSize;
    Material::CBFieldHandle invScreenSize;
    Material::CBFieldHandle invShadowSize;

    void Populate(Material* material);
};

struct SceneSsrCBHandles
{
    Material::CBFieldHandle view;
    Material::CBFieldHandle proj;
    Material::CBFieldHandle invView;
    Material::CBFieldHandle invProj;
    Material::CBFieldHandle depthA;
    Material::CBFieldHandle depthB;
    Material::CBFieldHandle zNear;
    Material::CBFieldHandle zFar;
    Material::CBFieldHandle screenSize;
    Material::CBFieldHandle invScreenSize;
    Material::CBFieldHandle technique;

    void Populate(Material* material);
};

struct SceneBlurCBHandles
{
    Material::CBFieldHandle dir;
    Material::CBFieldHandle radius;
    Material::CBFieldHandle glossyScale;

    void Populate(Material* material);
};

struct SceneComposeCBHandles
{
    Material::CBFieldHandle invView;
    Material::CBFieldHandle invProj;
    Material::CBFieldHandle skyboxIntensity;
    Material::CBFieldHandle camPos;
    Material::CBFieldHandle screenSize;
    Material::CBFieldHandle invScreenSize;

    void Populate(Material* material);
};

struct SceneFxaaCBHandles
{
    Material::CBFieldHandle invResolution;
    Material::CBFieldHandle subpix;
    Material::CBFieldHandle edgeThreshold;
    Material::CBFieldHandle edgeThresholdMin;

    void Populate(Material* material);
};

struct LightingPassConstants
{
    float3 sunDir{};
    float3 ambient{};
    float3 lightRgb{};
    float exposure = 1.0f;
    float3 camPos{};
    float3 camDir{};
    mat4 invView{};
    mat4 invProj{};
    std::array<mat4, 4> lightViewProj{};
    std::array<float4, 4> cascadeScaleBias{};
    float4 cascadeSplits{};
    float2 shadowAtlasSize{};
    float4 shadowBiasNDC{};
    float4 normalBiasWS{};
    float2 screenSize{};
    float2 invScreenSize{};
    float sunMetalSpec = 0.0f;
    float sunAngularSize = 0.0f;
};

struct PointLightPassConstants
{
    mat4 invView{};
    mat4 invProj{};
    float3 camPos{};
    uint32_t lightCount = 0;
    float2 screenSize{};
    float2 invScreenSize{};
};

struct SpotLightPassConstants
{
    mat4 invView{};
    mat4 invProj{};
    float3 camPos{};
    uint32_t lightCount = 0;
    float2 screenSize{};
    float2 invScreenSize{};
    float2 invShadowSize{};
};

struct SsrPassConstants
{
    mat4 view{};
    mat4 proj{};
    mat4 invView{};
    mat4 invProj{};
    float depthA = 0.0f;
    float depthB = 0.0f;
    float zNear = 0.1f;
    float zFar = 1000.0f;
    float2 screenSize{};
    float2 invScreenSize{};
    uint32_t technique = 0;
    float techniquePadding[3] = {};
};

struct BlurPassConstants
{
    float2 direction{};
    float radius = 1.0f;
    float glossyScale = 0.0f; // extra blur radius at full roughness (0 = sharp); drives glossy reflections
};

struct ComposePassConstants
{
    mat4 invView{};
    mat4 invProj{};
    float skyboxIntensity = 1.0f;
    float3 camPos{};
    float2 screenSize{};
    float2 invScreenSize{};
};

struct FxaaPassConstants
{
    float2 invResolution{};
    float subpix = 0.75f;
    float edgeThreshold = 0.166f;
    float edgeThresholdMin = 0.0625f;
};

class SceneResourceBootstrapper
{
public:
    using UploadList = std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>;

    void Initialize(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, UploadList* uploadKeepAlive);
    void Finalize(Renderer* renderer,
        const std::vector<std::unique_ptr<RenderableObjectBase>>& objects,
        ID3D12GraphicsCommandList* uploadCmdList, UploadList* uploadKeepAlive,
        Skybox* skybox);

    void RefreshMaterialHandles(Renderer* renderer,
        const std::vector<std::unique_ptr<RenderableObjectBase>>& objects,
        Skybox* skybox);

    void EnsureMaterials(Renderer* renderer);

    std::shared_ptr<Material> GetLightingMaterial() const { return matLighting_; }
    std::shared_ptr<Material> GetPointLightMaterial() const { return matPointLightCS_; }
    std::shared_ptr<Material> GetSpotLightMaterial() const { return matSpotLightCS_; }
    std::shared_ptr<Material> GetComposeMaterial() const { return matComposeCS_; }
    std::shared_ptr<Material> GetTonemapMaterial() const { return matTonemapCS_; }
    std::shared_ptr<Material> GetFxaaMaterial() const { return matFxaaCS_; }
    std::shared_ptr<Material> GetSsrMaterial() const { return matSSR_; }
    std::shared_ptr<Material> GetOceanReflectionMaterial() const { return matOceanReflection_; }
    std::shared_ptr<Material> GetBlurMaterial() const { return matBlur_; }
    std::shared_ptr<Material> GetDebugMaterial() const { return matDebug_; }
    std::shared_ptr<Material> GetRtDebugMaterial() const { return matRtDebug_; } // S6, null on non-RT HW
    std::shared_ptr<Material> GetRtReflectMaterial() const { return matRtReflect_; } // S7, null on non-RT HW
    std::shared_ptr<Material> GetRtDenoiseMaterial() const { return matRtDenoise_; } // S11, null on non-RT HW
    std::shared_ptr<Material> GetGlassReflPrepassMaterial() const { return matGlassReflPrepass_; } // S15b, null on non-RT HW

    const SceneLightingCBHandles& LightingHandles() const { return lightingHandles_; }
    const ScenePointLightCBHandles& PointHandles() const { return pointHandles_; }
    const SceneSpotLightCBHandles& SpotHandles() const { return spotHandles_; }
    const SceneSsrCBHandles& SsrHandles() const { return ssrHandles_; }
    const SceneBlurCBHandles& BlurHandles() const { return blurHandles_; }
    const SceneComposeCBHandles& ComposeHandles() const { return composeHandles_; }
    const SceneFxaaCBHandles& FxaaHandles() const { return fxaaHandles_; }

    UINT GetLightingCBSizeBytes() const;
    UINT GetPointLightCBSizeBytes() const;
    UINT GetSpotLightCBSizeBytes() const;
    UINT GetSsrCBSizeBytes() const;
    UINT GetOceanReflectionCBSizeBytes() const;
    UINT GetBlurCBSizeBytes() const;
    UINT GetComposeCBSizeBytes() const;
    UINT GetFxaaCBSizeBytes() const;

    void WriteLightingConstants(const LightingPassConstants& data, uint8_t* dest) const;
    void WritePointLightConstants(const PointLightPassConstants& data, uint8_t* dest) const;
    void WriteSpotLightConstants(const SpotLightPassConstants& data, uint8_t* dest) const;
    void WriteSsrConstants(const SsrPassConstants& data, uint8_t* dest) const;
    void WriteBlurConstants(const BlurPassConstants& data, uint8_t* dest) const;
    void WriteComposeConstants(const ComposePassConstants& data, uint8_t* dest) const;
    void WriteFxaaConstants(const FxaaPassConstants& data, uint8_t* dest) const;

private:
    void RefreshObjectMaterials(Renderer* renderer,
        const std::vector<std::unique_ptr<RenderableObjectBase>>& objects,
        Skybox* skybox);

    void RefreshHandles();

    std::shared_ptr<Material> matLighting_;
    std::shared_ptr<Material> matPointLightCS_;
    std::shared_ptr<Material> matSpotLightCS_;
    std::shared_ptr<Material> matComposeCS_;
    std::shared_ptr<Material> matTonemapCS_;
    std::shared_ptr<Material> matFxaaCS_;
    std::shared_ptr<Material> matSSR_;
    std::shared_ptr<Material> matOceanReflection_;
    std::shared_ptr<Material> matBlur_;
    std::shared_ptr<Material> matDebug_;
    std::shared_ptr<Material> matRtDebug_;   // S6 RT debug viz (RayQuery cs_6_5); only on RT HW
    std::shared_ptr<Material> matRtReflect_;  // S7 Tier-1 RT reflections (RayQuery cs_6_5); only on RT HW
    std::shared_ptr<Material> matRtDenoise_;  // S11 temporal reflection denoise; only on RT HW
    std::shared_ptr<Material> matGlassReflPrepass_; // S15b glass refl G-buffer prepass; only on RT HW

    SceneLightingCBHandles lightingHandles_{};
    ScenePointLightCBHandles pointHandles_{};
    SceneSpotLightCBHandles spotHandles_{};
    SceneSsrCBHandles ssrHandles_{};
    SceneBlurCBHandles blurHandles_{};
    SceneComposeCBHandles composeHandles_{};
    SceneFxaaCBHandles fxaaHandles_{};
};
