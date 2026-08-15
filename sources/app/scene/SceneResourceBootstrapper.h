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
    Material::CBFieldHandle useVsm;           // Step 24f: directional clipmap sampling
    Material::CBFieldHandle vsmDepthBias;
    Material::CBFieldHandle clipmapBaseExtent;
    Material::CBFieldHandle clipmapNormalBias;
    Material::CBFieldHandle clipmapViewProj;
    Material::CBFieldHandle causticsTint;      // rgb = tint, w = master enable
    Material::CBFieldHandle causticsParams0;
    Material::CBFieldHandle causticsParams1;
    Material::CBFieldHandle causticsParams2;
    Material::CBFieldHandle csmDebugMode;      // S0.3: Legacy CSM cascade-tint visualization

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
    Material::CBFieldHandle invPointShadowSize;
    Material::CBFieldHandle useVsm;
    Material::CBFieldHandle vsmRefDist;
    Material::CBFieldHandle localLateralTexels;
    Material::CBFieldHandle localDepthPushTexels;

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
    Material::CBFieldHandle useVsm;
    Material::CBFieldHandle vsmRefDist;
    Material::CBFieldHandle localLateralTexels;
    Material::CBFieldHandle localDepthPushTexels;

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
    Material::CBFieldHandle enableSkySpecular;
    Material::CBFieldHandle screenSize;
    Material::CBFieldHandle invScreenSize;
    Material::CBFieldHandle shoreWetnessWindow;
    Material::CBFieldHandle shoreWetnessAppearance;
    Material::CBFieldHandle shoreWetnessFallback;
    Material::CBFieldHandle shoreWetnessBreakup;

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

// P2 photographic camera.
struct SceneTonemapCBHandles
{
    Material::CBFieldHandle exposureEnabled;

    void Populate(Material* material);
};

struct SceneExposureHistogramCBHandles
{
    Material::CBFieldHandle sampleGridX;
    Material::CBFieldHandle sampleGridY;
    Material::CBFieldHandle minLogLum;
    Material::CBFieldHandle invLogLumRange;

    void Populate(Material* material);
};

struct SceneExposureSolveCBHandles
{
    Material::CBFieldHandle minLogLum;
    Material::CBFieldHandle logLumRange;
    Material::CBFieldHandle lowPercentile;
    Material::CBFieldHandle highPercentile;
    Material::CBFieldHandle compensationEv;
    Material::CBFieldHandle minEv100;
    Material::CBFieldHandle maxEv100;
    Material::CBFieldHandle deltaTime;
    Material::CBFieldHandle speedUp;
    Material::CBFieldHandle speedDown;
    Material::CBFieldHandle manualEv100;
    Material::CBFieldHandle autoExposure;
    Material::CBFieldHandle resetHistory;

    void Populate(Material* material);
};

#if WITH_EDITOR
struct SceneSelectionOutlineCBHandles
{
    Material::CBFieldHandle screenSize;
    Material::CBFieldHandle selectedBit;
    Material::CBFieldHandle outlineRadius;
    Material::CBFieldHandle outlineColor;

    void Populate(Material* material);
};
#endif

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
    uint32_t useVsm = 0;                          // Step 24f: 1 = sample the directional VSM clipmap
    float vsmDepthBias = 0.0f;
    float clipmapBaseExtent = 0.0f;               // finest clipmap level's world extent
    float clipmapNormalBias = 0.0f;               // normal offset in texels
    std::array<mat4, 8> clipmapViewProj{};        // camera-centered ortho viewProj per clipmap level
    // Underwater caustics (see shaders/caustics.hlsli). causticsTint.w == 0 disables the block,
    // which is what a level without an ocean produces.
    float4 causticsTint{};
    float4 causticsParams0{};   // intensity, metres per tile, frames/sec, water level Y
    float4 causticsParams1{};   // depth fade, surface fade, up-facing gate, bias
    float4 causticsParams2{};   // dispersion, second-layer blend, time, world metres per pixel
    uint32_t csmDebugMode = 0;  // S0.3: 0 = off, 1 = cascade tint (Legacy CSM only)
};

struct PointLightPassConstants
{
    mat4 invView{};
    mat4 invProj{};
    float3 camPos{};
    uint32_t lightCount = 0;
    float2 screenSize{};
    float2 invScreenSize{};
    float invPointShadowSize = 0.0f; // 1 / pointShadowRes (cube face texel, for PCF)
    uint32_t useVsm = 0;      // Rung 2 / Step 21
    float vsmRefDist = 10.0f;
    float localLateralTexels = 1.0f;   // VSM local-light bias (texels) — mirrors HLSL PointLightFrame
    float localDepthPushTexels = 0.5f;
    float _vsmPad0 = 0.0f;
    float _vsmPad1 = 0.0f;
    float _vsmPad2 = 0.0f;
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
    uint32_t useVsm = 0;      // Rung 2 / Step 21
    float vsmRefDist = 10.0f;
    float localLateralTexels = 1.0f;   // VSM local-light bias (texels) — mirrors HLSL SpotLightFrame
    float localDepthPushTexels = 0.5f;
    float _vsmPad0 = 0.0f;
    float _vsmPad1 = 0.0f;
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
    uint32_t enableSkySpecular = 1u;
    float2 screenSize{};
    float2 invScreenSize{};
    float4 shoreWetnessWindow{};
    float4 shoreWetnessAppearance{};
    float4 shoreWetnessFallback{};
    float4 shoreWetnessBreakup{};
};

struct FxaaPassConstants
{
    float2 invResolution{};
    float subpix = 0.75f;
    float edgeThreshold = 0.166f;
    float edgeThresholdMin = 0.0625f;
};

// P2 photographic camera. The log-luminance window is a compile-time constant of the metering,
// not an authored setting: it only has to be wide enough to contain any scene the histogram will
// ever see, and moving it would silently reinterpret every stored bin.
struct ExposureMeteringConstants
{
    static constexpr uint32_t kSampleGridX = 256;
    static constexpr uint32_t kSampleGridY = 144;
    static constexpr float kMinLogLum = -10.0f; // log2 luminance
    static constexpr float kMaxLogLum = 14.0f;  // 24 stops over 256 bins = 0.094 stops per bin

    float compensationEv = 0.0f;
    float minEv100 = -6.0f;
    float maxEv100 = 16.0f;
    float lowPercentile = 0.02f;
    float highPercentile = 0.80f;
    float speedUp = 3.0f;
    float speedDown = 1.0f;
    float manualEv100 = 0.0f;
    float deltaTime = 0.0f;
    uint32_t autoExposure = 1;
    uint32_t resetHistory = 0;
};

#if WITH_EDITOR
struct SelectionOutlinePassConstants
{
    float2 screenSize{};
    uint32_t selectedBit = 0;
    uint32_t outlineRadius = 1;
    float4 outlineColor{};
};
#endif

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
    std::shared_ptr<Material> GetExposureClearMaterial() const { return matExposureClearCS_; }
    std::shared_ptr<Material> GetExposureBuildMaterial() const { return matExposureBuildCS_; }
    std::shared_ptr<Material> GetExposureSolveMaterial() const { return matExposureSolveCS_; }
    std::shared_ptr<Material> GetSsrMaterial() const { return matSSR_; }
    std::shared_ptr<Material> GetOceanReflectionMaterial() const { return matOceanReflection_; }
    std::shared_ptr<Material> GetBlurMaterial() const { return matBlur_; }
    std::shared_ptr<Material> GetDebugMaterial() const { return matDebug_; }
    std::shared_ptr<Material> GetRtDebugMaterial() const { return matRtDebug_; } // S6, null on non-RT HW
    std::shared_ptr<Material> GetRtReflectMaterial() const { return matRtReflect_; } // S7, null on non-RT HW
    std::shared_ptr<Material> GetRtDenoiseMaterial() const { return matRtDenoise_; } // S11, null on non-RT HW
    std::shared_ptr<Material> GetGlassReflPrepassMaterial() const { return matGlassReflPrepass_; } // S15b, null on non-RT HW
#if WITH_EDITOR
    std::shared_ptr<Material> GetSelectionOutlineMaterial() const { return matSelectionOutlineCS_; }
    std::shared_ptr<Material> GetSelectionStencilMaterial() const { return matSelectionStencil_; }
#endif

    const SceneLightingCBHandles& LightingHandles() const { return lightingHandles_; }
    const ScenePointLightCBHandles& PointHandles() const { return pointHandles_; }
    const SceneSpotLightCBHandles& SpotHandles() const { return spotHandles_; }
    const SceneSsrCBHandles& SsrHandles() const { return ssrHandles_; }
    const SceneBlurCBHandles& BlurHandles() const { return blurHandles_; }
    const SceneComposeCBHandles& ComposeHandles() const { return composeHandles_; }
    const SceneFxaaCBHandles& FxaaHandles() const { return fxaaHandles_; }
#if WITH_EDITOR
    const SceneSelectionOutlineCBHandles& SelectionOutlineHandles() const { return selectionOutlineHandles_; }
#endif

    UINT GetLightingCBSizeBytes() const;
    UINT GetPointLightCBSizeBytes() const;
    UINT GetSpotLightCBSizeBytes() const;
    UINT GetSsrCBSizeBytes() const;
    UINT GetOceanReflectionCBSizeBytes() const;
    UINT GetBlurCBSizeBytes() const;
    UINT GetComposeCBSizeBytes() const;
    UINT GetFxaaCBSizeBytes() const;
    UINT GetTonemapCBSizeBytes() const;
    UINT GetExposureHistogramCBSizeBytes() const;
    UINT GetExposureSolveCBSizeBytes() const;
#if WITH_EDITOR
    UINT GetSelectionOutlineCBSizeBytes() const;
#endif

    void WriteLightingConstants(const LightingPassConstants& data, uint8_t* dest) const;
    void WritePointLightConstants(const PointLightPassConstants& data, uint8_t* dest) const;
    void WriteSpotLightConstants(const SpotLightPassConstants& data, uint8_t* dest) const;
    void WriteSsrConstants(const SsrPassConstants& data, uint8_t* dest) const;
    void WriteBlurConstants(const BlurPassConstants& data, uint8_t* dest) const;
    void WriteComposeConstants(const ComposePassConstants& data, uint8_t* dest) const;
    void WriteFxaaConstants(const FxaaPassConstants& data, uint8_t* dest) const;
    void WriteTonemapConstants(bool exposureEnabled, uint8_t* dest) const;
    void WriteExposureHistogramConstants(uint8_t* dest) const;
    void WriteExposureSolveConstants(const ExposureMeteringConstants& data, uint8_t* dest) const;
#if WITH_EDITOR
    void WriteSelectionOutlineConstants(const SelectionOutlinePassConstants& data, uint8_t* dest) const;
#endif

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
    std::shared_ptr<Material> matExposureClearCS_;
    std::shared_ptr<Material> matExposureBuildCS_;
    std::shared_ptr<Material> matExposureSolveCS_;
    std::shared_ptr<Material> matSSR_;
    std::shared_ptr<Material> matOceanReflection_;
    std::shared_ptr<Material> matBlur_;
    std::shared_ptr<Material> matDebug_;
    std::shared_ptr<Material> matRtDebug_;   // S6 RT debug viz (RayQuery cs_6_5); only on RT HW
    std::shared_ptr<Material> matRtReflect_;  // S7 Tier-1 RT reflections (RayQuery cs_6_5); only on RT HW
    std::shared_ptr<Material> matRtDenoise_;  // S11 temporal reflection denoise; only on RT HW
    std::shared_ptr<Material> matGlassReflPrepass_; // S15b glass refl G-buffer prepass; only on RT HW
#if WITH_EDITOR
    std::shared_ptr<Material> matSelectionOutlineCS_;
    std::shared_ptr<Material> matSelectionStencil_;
#endif

    SceneLightingCBHandles lightingHandles_{};
    ScenePointLightCBHandles pointHandles_{};
    SceneSpotLightCBHandles spotHandles_{};
    SceneSsrCBHandles ssrHandles_{};
    SceneBlurCBHandles blurHandles_{};
    SceneComposeCBHandles composeHandles_{};
    SceneFxaaCBHandles fxaaHandles_{};
    SceneTonemapCBHandles tonemapHandles_{};
    SceneExposureHistogramCBHandles exposureHistogramHandles_{};
    SceneExposureSolveCBHandles exposureSolveHandles_{};
#if WITH_EDITOR
    SceneSelectionOutlineCBHandles selectionOutlineHandles_{};
#endif
};
