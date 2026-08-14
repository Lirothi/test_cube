#pragma once
#include <memory>
#include <array>
#include <string>
#include "rendering/renderables/RenderableObject.h"
#include "materials/TextureCube.h"
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
    std::wstring path_;
    float exposure_ = 1.0f;
};
