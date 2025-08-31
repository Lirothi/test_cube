#pragma once

#include <vector>

#include "RenderGraph.h"

class Renderer;

class RenderableObjectBase
{
public:
    virtual ~RenderableObjectBase() noexcept = default;
    virtual void Init(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive) = 0;
    virtual void Tick(float /*dt*/) = 0;
    virtual void Render(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& view, const mat4& proj) = 0;
    virtual void RenderShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& lightView, const mat4& lightProj) = 0;
    virtual bool IsTransparent() const = 0;
    virtual bool IsSimpleRender() const = 0;
    virtual bool CastsShadow() const = 0;
};