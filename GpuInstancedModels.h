#pragma once
#include "RenderableObject.h"
#include "InstanceBuffer.h"
#include "Material.h"
#include "Texture2D.h"
#include <wrl.h>
#include <d3d12.h>
#include <string>
#include <memory>

class GpuInstancedModels : public RenderableObject {
public:
    GpuInstancedModels(
        std::string modelName,
        UINT numInstances,
        const std::string& matPreset,
        const std::string& inputLayout,
        const std::wstring& graphicsShader,
        const std::wstring& computeShader);

    void Init(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive) override;

    void Tick(float deltaTime) override;
    bool IsSimpleRender() const { return false; }
    bool CastsShadow() const override { return true; }

protected:
    void RecordCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl) override;
    void RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx) override;
    void PopulateContext(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx) override;
    void UpdateUniforms(Renderer* renderer, const mat4& view, const mat4& proj, uint8_t* cbData) override;
    void IssueDraw(Renderer* renderer, ID3D12GraphicsCommandList* cl) override;
    void RecordShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& lightView, const mat4& lightProj, RenderContext& ctx, uint8_t* cbData) override;

private:
    // данные инстансинга
    InstanceBuffer instanceBuffer_;
    UINT instanceCount_ = 0;
    float deltaTime_ = 0.0f;
    float angularSpeed_ = DirectX::XM_PIDIV2;

    // compute
    std::shared_ptr<Material> computeMaterial_;
    std::wstring computeShader_;

    // модель/текстура
    std::string modelName_;
};