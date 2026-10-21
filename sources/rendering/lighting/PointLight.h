#pragma once
#include <cstdint>
#include "core/math/Math.h"
#include "rendering/meshes/Mesh.h"
#include "rendering/lighting/NightLamps.h"
#include "materials/Material.h"

class Renderer;

// Authorable fire-like variation. A zero amplitude or frequency leaves the light
// steady, so existing point-light JSON keeps its exact current behavior.
struct PointLightFlickerDesc {
    float         amplitude = 0.0f;  // intensity multiplier varies by +/- this amount
    float         frequencyHz = 0.0f;
    std::uint32_t seed = 0;
};

struct PointLightDesc {
    Math::float3 position = Math::float3(0, 0, 0);
    float        radius   = 1.0f;
    Math::float3 color    = Math::float3(1, 1, 1);
    // P16.5: LUMINOUS FLUX, in lumens. 800 = a 60 W incandescent, 1600 = 100 W, 10000-50000 a
    // floodlight. Converted to candela with CandelaFromLumens; see PhotographicSettings.h.
    float        luminousFluxLm = 1000.0f;
    float        volumetricIntensity = 1.0f; // plan A4d: UE VolumetricScatteringIntensity -- this light's weight in the fog volume
    bool         shadowsEnabled = false; // if false, the point lights the scene but never casts a shadow (honored once Part B point shadows land)
    PointLightFlickerDesc flicker{};
    LampSwitch   onAt = LampSwitch::Always; // plan L: "onAt" -- comes on at night (or by day), NightLamps.h
    // Local-light shadow plan S1: UE's ShadowBias / ShadowSlopeBias (both 0.5). UE biases a point light at projection:
    // the compare distance moves toward the light by 0.02 x 512/Res x 2 x shadowBias of itself, times
    // (1 + 3 x shadowSlopeBias x (1 - NoL)) -- local_shadow_bias.hlsli. Under the texel bias (the atlas default) they
    // scale its normal / slope push instead, 0.5 = the global texels (render::LocalShadowNormalTexels, plan S8).
    float        shadowBias = 0.5f;
    float        shadowSlopeBias = 0.5f;
    // Plan S2: UE's ShadowSharpen (0): narrows the filtered penumbra, x (7 x this + 1).
    float        shadowSharpen = 0.0f;
};

class PointLight {
public:
    void Init(Renderer* r, ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive);

    void SetDesc(const PointLightDesc& d);
    const PointLightDesc& GetDesc() const { return desc_; }

    // Smooth, deterministic modulation for fire-like lights. This deliberately uses
    // layered sine waves rather than frame-random values, which would visibly strobe.
    void Tick(float deltaTime);

    // Plan L: the night lamps' level (NightLamps::Level), multiplied into the effective flux with the flicker.
    // GetDesc() is that effective desc; the authored one stays as SetDesc gave it.
    void SetLampLevel(float level);
    float GetLampLevel() const { return lampLevel_; }

    // Rung 1 (Step 11) foundation: monotonic version bumped on any SetDesc (conservative — a
    // shadow cache compares it to detect a moved/changed light). PointLight has no dirty_, so
    // it mirrors SpotLight's version. No consumer yet. Flicker changes intensity only, not
    // the shadow transform or coverage, so it deliberately does not bump this version.
    std::uint32_t GetTransformVersion() const { return transformVersion_; }

    // TWO SEPARATE FUNCTIONS:
    // 1) Z-FAIL stencil pre-pass (two passes: back then front)
    void RenderZFail(Renderer* r, ID3D12GraphicsCommandList* cl,
                     const Math::mat4& view, const Math::mat4& proj);

    // 2) Color pass (additive into the Light RT) with a stencil!=0 mask
    void RenderColor(Renderer* r, ID3D12GraphicsCommandList* cl,
                     const Math::mat4& view, const Math::mat4& proj,
                     const Math::mat4& invView, const Math::mat4& invProj,
                     const Math::float3& camPos);

    void OnMaterialHotReload();

private:
    void ApplyFlicker();
    Math::mat4 BuildModel() const;

    std::shared_ptr<Mesh> sphere_ = nullptr;

    // two PSOs for z-fail: one for back faces and one for front faces
    std::shared_ptr<Material> matZFail_;

    // color (additive) fullscreen pass
    std::shared_ptr<Material> matColorFS_;

    struct CBHandleCache {
        struct ZFailHandles {
            Material::CBFieldHandle world;
            Material::CBFieldHandle viewProj;
        } zFail;

        struct ColorHandles {
            struct PerFrame {
                Material::CBFieldHandle invView;
                Material::CBFieldHandle invProj;
                Material::CBFieldHandle camPos;
            } frame;

            struct PerLight {
                Material::CBFieldHandle position;
                Material::CBFieldHandle radius;
                Material::CBFieldHandle color;
                Material::CBFieldHandle intensity;
            } light;
        } color;
    } cbHandles_{};

    void RebuildHandleCache();

    PointLightDesc baseDesc_{};
    PointLightDesc desc_{}; // effective desc after flicker modulation
    float flickerTime_ = 0.0f;
    float lampLevel_ = 1.0f; // plan L
    std::uint32_t transformVersion_ = 0; // Step 11: bumped on SetDesc
};
