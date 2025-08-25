#pragma once
#include "SceneObject.h"
#include "InstanceBuffer.h"
#include "Material.h"
#include "Texture2D.h"
#include <wrl.h>
#include <d3d12.h>
#include <string>
#include <memory>

class GpuInstancedModels : public SceneObject {
public:
    GpuInstancedModels(Renderer* renderer,
        std::string modelName,
        UINT numInstances,
        const std::string& cbLayout,
        const std::string& inputLayout,
        const std::wstring& graphicsShader,
        const std::wstring& computeShader);

    void Init(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive) override;

    void Tick(float deltaTime) override;
    bool IsSimpleRender() const override {return false;}

protected:
    // хук SceneObject
    void RecordCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl) override;
    void RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl) override;
    void PopulateContext(Renderer* renderer, ID3D12GraphicsCommandList* cl) override;
    void UpdateUniforms(Renderer* renderer, const mat4& view, const mat4& proj) override;
    void IssueDraw(Renderer* renderer, ID3D12GraphicsCommandList* cl) override;

private:
    // данные инстансинга
    InstanceBuffer instanceBuffer_;
    UINT instanceCount_ = 0;
    float deltaTime_ = 0.0f;
    float angularSpeed_ = DirectX::XM_PIDIV2;

    // compute
    std::shared_ptr<Material> computeMaterial_;
    RenderContext computeCtx_;
    std::wstring computeShader_;

    // модель/текстура
    std::string modelName_;
    Texture2D albedoTex_;
    Texture2D mrTex_;
    Texture2D normalTex_;
};