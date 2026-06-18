#pragma once

#include <vector>

#include "core/math/AABB.h"
#include "rendering/RenderLayers.h"
#include "rendering/core/RenderGraph.h"

class Renderer;
class Camera;

class RenderableObjectBase
{
public:
    RenderableObjectBase() = default;
    virtual ~RenderableObjectBase() noexcept = default;
    virtual void Init(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive) = 0;
    virtual void Tick(float /*dt*/) = 0;
    virtual void PostTick(float /*dt*/) {}
    // viewCB: GPU VA of the per-pass shared view/frame constant buffer bound at b1
    // (camera matrices for the gbuffer/transparent pass, light viewProj for shadows).
    virtual void Render(Renderer* renderer, ID3D12GraphicsCommandList* cl, const Camera& camera, D3D12_GPU_VIRTUAL_ADDRESS viewCB) = 0;
    virtual void ExecuteCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl) { (void)renderer; (void)cl; }
    virtual void RenderShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& lightView, const mat4& lightProj, D3D12_GPU_VIRTUAL_ADDRESS viewCB) = 0;
    virtual bool IsTransparent() const = 0;
    virtual bool IsSimpleRender() const = 0;
    virtual bool CastsShadow() const = 0;
    virtual void OnMaterialHotReload(Renderer* renderer) {}

    virtual const AABB& GetWorldBounds() const
    {
        static const AABB kInvalidBounds = AABB::Empty();
        return kInvalidBounds;
    }

    uint32_t GetRenderLayerMask() const { return renderLayerMask_; }
    void SetRenderLayerMask(uint32_t mask) { renderLayerMask_ = mask; }
    void SetRenderLayer(RenderLayer layer) { renderLayerMask_ = RenderLayerMask(layer); }
    void AddRenderLayer(RenderLayer layer) { EnableLayer(renderLayerMask_, layer); }
    void RemoveRenderLayer(RenderLayer layer) { DisableLayer(renderLayerMask_, layer); }

protected:
    uint32_t renderLayerMask_ = RenderLayerMask(RenderLayer::Default);
};
