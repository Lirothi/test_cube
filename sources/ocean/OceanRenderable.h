#pragma once

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include "core/math/Math.h"
#include "rendering/renderables/RenderableObject.h"
#include "materials/Texture2D.h"
#include "ocean/OceanSimulation.h"
#include "ocean/OceanSurfSim.h" // surf sim injection (docs/ocean_surf_sim_plan.md)
#include "ocean/OceanWetness.h"

class Camera;
class SamplerManager;

namespace ocean
{
// "--ocean-shore-sink": cut the run-up sheet on dry land by SINKING it under the terrain in the
// vertex shader and letting the ordinary depth test draw the waterline, instead of `clip()`-ing it
// in the pixel shader.
//
// It has to be a SHADER VARIANT, not a runtime branch: a `clip` behind an `if` is still a discard
// in the compiled shader, so the hardware still disables early-Z for the whole draw and the branch
// buys nothing. Measured cost of that discard: 38 us, ~14% of the ocean pass.
//
// Boot-only for the same reason — flipping it rebuilds the material. Compare two runs.
inline bool g_shoreSinkCut = true;

// OCEAN_SHORE_RUNUP variant switch. true = the modern shore stack (run-up sheet with a travelling
// front, anchored swash, contact foam with the torn dither edge, SDF, sink). false = the CLASSIC
// surface, byte-faithful from commit 3e54d5d (2026-06-22, pre-rework) via
// ocean_surface_legacy.hlsli — classic depth-map damping and the old contact foam. Toggle at boot
// with "--ocean-runup-shore" / "--ocean-classic-shore" (variant = material rebuild, so boot-only).
inline bool g_shoreRunup = false;

// "--ocean-foam-debug": compile the contact-foam diagnostic views into the ocean surface shader.
// Boot-only and a VARIANT for the same reason as above — the views need per-pixel intermediates
// (sweep position, feather, tear noise) kept alive, and a runtime `if` would make every water pixel
// pay for them. With the variant compiled in, the VIEW itself is a plain uniform, so switching
// views in the ocean window is free and needs no rebuild.
inline bool g_foamDebug = false;

// Which diagnostic view the shader renders (0 = normal shading). Deliberately NOT part of
// OceanRenderConfig: it is a debugging knob, and putting it there would serialize it into the
// user's ocean settings. Rides shoreFoamBreakupParams.w.
inline int g_foamDebugView = 0;

// surf sim injection: which surf-sim channel the LEGACY surface tints with (0 = off, 1 = sim
// height, 2 = sim foam, 3 = shore SDF isolines, 4 = shore depth). A plain runtime uniform, not
// a variant, and not serialized — same reasoning as g_foamDebugView. Rides surfSimParams.w.
inline int g_surfSimDebugView = 0;

// "--ocean-surf-sim": force-enable the surf sim regardless of the level's surfSimEnabled, so a
// headless capture can exercise it without editing authored level JSON. Runtime OR with the
// config flag, not a variant.
inline bool g_surfSimForce = false;


// "--ocean-vs-depth-probe": A/B experiment, OFF. Replaces the world-space shore SDF with a
// screen-space probe of the depth buffer, taken in the VERTEX shader, as the thing that decides
// where the wave's vertical motion gets damped. Boot-only and a variant, because it changes which
// texture the vertex shader reads; everything else is identical, so a pair of runs isolates exactly
// that choice.
//
// COMPARED SIDE BY SIDE AND THE SDF WON, decisively — it stays the default. The probe's weaknesses
// are structural, not tuning: a vertex off-screen gets no answer at all so the damping pops as the
// camera turns, the along-ray depth gap degenerates at a grazing angle (tiny over genuinely deep
// water, so it damps the open sea), and the buffer holds palms and props as well as terrain. It is
// also the more expensive of the two in the ways that matter: the SDF is one texture read shared
// with the sink and is built once per level, while the probe makes the vertex shader depend on a
// buffer another pass writes in the same frame. Kept compilable because re-running the comparison
// is easier than re-arguing it.
inline bool g_vsDepthProbe = false;
}

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
    // surf sim injection (pass-flow S3): the surf sim's own pass, authored with AddPass2 —
    // SceneRenderer calls this as the pass builder. Empty return = the sim is off this frame.
    std::function<void(RenderGraphPassContext)> BuildSurfSimPass(RenderGraphPassContext& ctx);
    std::function<void(RenderGraphPassContext)> BuildWetnessPass(RenderGraphPassContext& ctx);
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

    // Runs before the render graph — see OceanSimulation::EnsureFrameResources.
    void EnsureSimulationResources(Renderer* renderer);
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

    bool IsWetnessReady() const { return wetness_ && wetness_->IsReady(); }
    ID3D12Resource* GetWetnessResource() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetWetnessSrv() const;
    Math::float4 GetWetnessComposeWindow() const;
    Math::float4 GetWetnessComposeAppearance() const;
    Math::float4 GetWetnessComposeFallback() const;
    Math::float4 GetWetnessComposeBreakup() const;

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
    Math::float4 GetShoreLegacyDampParams() const;
    Math::float4 GetShoreLegacyFoamParams() const;
    Math::float4 GetShoreLegacyFoamParams2() const;
    Math::float4 GetShoreLegacyDissipationParams() const;
    Math::float4 GetSurfSimParams() const;  // surf sim injection
    Math::float4 GetSurfSimParams2() const; // surf sim injection (S4): x = front breakup
    Math::float4 GetSurfSimParams3() const; // surf sim injection: x = chopness, y = cap width
    Math::float4 GetShoreWetnessParams() const;
    Math::float4 GetShoreWetnessParams2() const;
    bool SurfSimActive() const;            // surf sim injection: config flag OR the boot force
    Math::float4 GetShoreSwashParams() const;
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
    // Shore SDF placement: xy centre, z inverse extent, w texel world size.
    Math::float4 GetShoreSdfParams() const;
    Math::float4 GetShoreDepthParams() const;
    const OceanRenderConfig& GetRenderConfig() const;
    void UpdateFoamTrailState();

private:
    Camera* camera_ = nullptr;
    Scene* scene_ = nullptr;
    OceanSimulation* simulation_ = nullptr;
    std::unique_ptr<OceanSurfSim> surfSim_; // surf sim injection
    std::unique_ptr<OceanWetness> wetness_;

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

