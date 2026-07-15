#pragma once
#include <cstdint>
#include "core/math/Math.h"
#include "rendering/meshes/Mesh.h"
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
    float        intensity= 1.0f;
    bool         shadowsEnabled = false; // if false, the point lights the scene but never casts a shadow (honored once Part B point shadows land)
    PointLightFlickerDesc flicker{};
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
    std::uint32_t transformVersion_ = 0; // Step 11: bumped on SetDesc
};
