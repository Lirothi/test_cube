#pragma once
#include <memory>
#include <array>
#include <string>
#include "RenderableObject.h"
#include "TextureCube.h"
#include "SamplerManager.h"

class Skybox : public RenderableObject {
public:
    Skybox(const std::wstring& filePath): RenderableObject(/*matPreset*/"", /*inputLayout*/"PosOnly", /*graphicsShader*/L"shaders/skybox.hlsl"),
        path_(filePath)
    {
        allowWireframe_ = false;
    }

    ~Skybox() override = default;

    // Init: достраиваем PSO и геометрию (куб)
    void Init(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive) override;

    const TextureCube* GetTex() const { return &cube_; }

    bool IsSimpleRender() const { return true; }

protected:
    // пер-кадровые константы и дескрипторы
    void UpdateUniforms(Renderer* renderer, const mat4& view, const mat4& proj) override;
    void PopulateContext(Renderer* renderer, ID3D12GraphicsCommandList* cl) override;

    // рисуем обычным путём базового класса:
    // RecordGraphics/IssueDraw наследуем без изменений.

private:
    void BuildCubeMesh_(Renderer* r,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive);

private:
    TextureCube cube_;
    std::wstring path_;
};