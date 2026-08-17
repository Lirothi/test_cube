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
    float GetExposure() const { return exposure_; }
    void SetExposure(float exp) { exposure_ = exp; }

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
};
