#pragma once
#include <memory>
#include <vector>
#include <wrl/client.h>

#include "RenderableObjectBase.h"
#include "Math.h"

class Renderer;
struct ID3D12GraphicsCommandList;

class DebugGrid final : public RenderableObjectBase {
public:
    DebugGrid(float halfSize = 10.0f, float step = 1.0f,
        float axisLen = 3.0f, float yPlane = 0.0f,
        float gridAlpha = 0.1f, float axisAlpha = 0.6f,
        float axisThicknessPx = 3.0f);

    ~DebugGrid();

    void Init(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive) override;

    void Tick(float dt) override;

    void Render(Renderer* renderer,
        ID3D12GraphicsCommandList* cl,
        const mat4& view,
        const mat4& proj) override;

    bool IsTransparent() const override { return true; }
    bool IsSimpleRender() const override { return true; }

private:
    // два внутренних рендерабла
    class GridRO;
    class AxesRO;
    std::unique_ptr<GridRO> grid_;
    std::unique_ptr<AxesRO> axes_;

    // параметры
    float halfSize_;
    float step_;
    float axisLen_;
    float yPlane_;
    float gridAlpha_;
    float axisAlpha_;
    float axisThicknessPx_;
};