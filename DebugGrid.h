#pragma once
#include <vector>
#include <wrl.h>
#include <DirectXMath.h>
#include "SceneObject.h"
#include "UploadManager.h"
#include "Math.h"

struct LineVertex {
    DirectX::XMFLOAT3 pos;
    DirectX::XMFLOAT4 col;
};

class DebugGrid : public SceneObject {
public:
    DebugGrid(Renderer* renderer,
        float halfSize = 10.0f, float step = 1.0f,
        float axisLen = 3.0f, float yPlane = 0.0f,
        float gridAlpha = 0.25f, float axisAlpha = 0.6f,
        float axisThicknessPx = 3.0f);

    void Init(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive) override;

    bool IsSimpleRender() const override {return true;}

protected:
    void PopulateContext(Renderer* renderer, ID3D12GraphicsCommandList* cl) override {}
	void UpdateUniforms(Renderer* renderer, const mat4& view, const mat4& proj) override;
    void IssueDraw(Renderer* renderer, ID3D12GraphicsCommandList* cl) override;

private:
    struct AxisVertex {
        DirectX::XMFLOAT3 a;           // 12
        DirectX::XMFLOAT3 b;           // 12  (24)
        DirectX::XMFLOAT3 cornerBias;  // 12  (36)  // xy = corner, z = edgeBiasPx
        DirectX::XMFLOAT4 col;         // 16  (52)
    };

    void BuildGridCPU(std::vector<LineVertex>& out);
    void BuildAxesScreenCPU(std::vector<AxisVertex>& out);

    // VB сетки (LINELIST)
    Microsoft::WRL::ComPtr<ID3D12Resource> vbGrid_;
    D3D12_VERTEX_BUFFER_VIEW vbvGrid_{};
    UINT gridVertexCount_ = 0;

    // VB осей (TRIANGLESTRIP, по 4 вершины на ось)
    Microsoft::WRL::ComPtr<ID3D12Resource> vbAxes_;
    D3D12_VERTEX_BUFFER_VIEW vbvAxes_{};
    UINT axesVertexCount_ = 0; // = 12 (3 оси * 4)

    // отдельный материал и CB для осей (axes.hlsl, input=AxisLine, b1=AxisParams)
    std::shared_ptr<Material> axisMat_;

    // параметры
    float halfSize_;
    float step_;
    float axisLen_;
    float yPlane_;
    float gridAlpha_;
    float axisAlpha_;
    float axisThicknessPx_;
};