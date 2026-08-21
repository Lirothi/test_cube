#pragma once
#include <memory>
#include <array>
#include <string>
#include "rendering/renderables/RenderableObject.h"
#include "materials/TextureCube.h"
#include "materials/Texture2D.h"
#include "rendering/descriptors/SamplerManager.h"

class Skybox : public RenderableObject {
public:
    Skybox(const std::wstring& filePath): RenderableObject(/*inputLayout*/"PosOnly", /*graphicsShader*/L"shaders/skybox.hlsl"),
        path_(filePath)
    {
        allowWireframe_ = false;
        SetRenderLayer(RenderLayer::Sky);
    }

    ~Skybox() override = default;

    // Init: finalize the PSO and cube geometry
    void Init(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive) override;

    const TextureCube* GetTex() const { return &cube_; }

    // F8 split-sum IBL. Loaded from siblings of `path_` produced by F7:
    //   <stem>_spec.dds     GGX-prefiltered radiance, mip m <-> roughness m/(mips-1)
    //   <stem>_diffuse.dds  cosine-convolved irradiance (already divided by PI)
    //   textures/brdf_lut.dds  the shared, scene-independent environment BRDF
    // All three are OPTIONAL: a sky imported before F7 simply has none, `HasIbl()` stays false and
    // every consumer keeps the pre-F8 behaviour. That fallback is the compatibility story, so it
    // must stay cheap to check and impossible to half-enter -- hence one flag for all three.
    bool HasIbl() const { return hasIbl_; }
    const TextureCube* GetSpecTex() const { return &specCube_; }
    const TextureCube* GetIrradianceTex() const { return &irradianceCube_; }
    const Texture2D* GetBrdfLut() const { return &brdfLut_; }
    // Real mip count of the prefiltered cube. This is what retires the hardcoded
    // `kSkyRoughMaxMip = 5` that both compose and the ocean used to guess with.
    UINT GetSpecMips() const { return specCube_.GetMips(); }
    const std::wstring& GetPath() const { return path_; }

    // The multiplier every consumer of this sky applies -- the authored appearance trim TIMES the
    // physical calibration below. Folded together on purpose: compose, the lighting pass, the ocean
    // and glass all already multiply by this, so the calibration reaches every one of them without
    // a single new constant-buffer field.
    float GetExposure() const { return exposure_ * PhysicalScale(); }
    // The authored trim alone -- for the editor to round-trip, AND for every consumer that is not
    // on the physical path. The editor's preview pass (thumbnails, the Mesh Editor viewport) writes
    // linear light straight into an sRGB target with no tonemapper, so handing it GetExposure()
    // multiplies the environment by the calibration (x10985 for a 12,000 lx sky) and saturates the
    // whole image to white. Anything without an exposure stage wants THIS one.
    float GetIntensity() const { return exposure_; }
    void SetExposure(float exp) { exposure_ = exp; }

    // P16.3b -- HOW MUCH LIGHT THIS SKY PUTS ON A HORIZONTAL SURFACE, IN LUX. 0 = not authored,
    // which reproduces the pre-P16.3b behaviour exactly.
    //
    // A clear sky delivers roughly 12,000 lx with the sun 30 degrees up (and ~20,000 with it
    // overhead); a heavy overcast, where the whole sky IS the light, 10,000-20,000.
    //
    // The scale that realises it is DERIVED, never authored: `MeasuredUpIlluminance()` is this
    // sky's own irradiance cube integrated for the up direction, so the same 12,000 means the same
    // thing whether the cube came out of the importer bright or dim.
    float GetIlluminanceLux() const { return illuminanceLux_; }
    void SetIlluminanceLux(float lux) { illuminanceLux_ = lux; }
    // The sky's horizontal illuminance in CUBE UNITS, measured from `_diffuse.dds` at load. 0 when
    // this sky has no derivatives, which is also what disables the calibration.
    float MeasuredUpIlluminance() const { return measuredUpIlluminance_; }
    float PhysicalScale() const
    {
        return (illuminanceLux_ > 0.0f && measuredUpIlluminance_ > 1e-8f)
            ? illuminanceLux_ / measuredUpIlluminance_
            : 1.0f;
    }

    bool IsSimpleRender() const { return true; }
    bool CastsShadow() const { return false; }

protected:
    void RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx, const Camera& camera, uint8_t* cbData) override;
    void ConfigureGraphicsPipeline(Renderer* renderer, Material::GraphicsDesc& desc) const override;

private:
    void BuildCubeMesh_(Renderer* r,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive);

private:
    TextureCube cube_;
    TextureCube specCube_;
    TextureCube irradianceCube_;
    Texture2D brdfLut_;
    bool hasIbl_ = false;
    std::wstring path_;
    float exposure_ = 1.0f;
    float illuminanceLux_ = 0.0f;          // P16.3b, 0 = not authored
    float measuredUpIlluminance_ = 0.0f;   // from _diffuse.dds at load; 0 = no derivatives
};
