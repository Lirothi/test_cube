#pragma once
#include <cstdint>
#include <vector>

#include "rendering/renderables/GBufferRenderable.h"
#include "core/math/Math.h"

class StaticMesh : public GBufferRenderable
{
public:
    StaticMesh(const std::string& modelName,
        const std::string& matPreset,
        const std::string& inputLayout,
        const std::wstring& graphicsShader);

    virtual void Init(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive) override;

    void SetRecomputeNormalSlots(std::vector<uint32_t> slots);

    // mesh.json "chunkGrid": the geometry was baked as an N x N grid of submesh chunks, so the
    // shadow path should treat each chunk as its own caster. Must be set BEFORE Init (that is where
    // the mesh is loaded and the flag reaches MeshLoadOptions), same channel as the normal slots.
    void SetChunkGrid(unsigned int grid) { chunkGrid_ = grid; }

    bool IsSimpleRender() const override { return true; }
    bool CastsShadow() const override { return true; }

protected:
    // A3: a glTF model with "material":"auto" (or no preset) sources its material from the glTF.
    std::string GetGltfMaterialSourcePath() const override;

private:
    std::string modelName_;
    std::vector<uint32_t> recomputeNormalSlots_;
    unsigned int chunkGrid_ = 0;
};
