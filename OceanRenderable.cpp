#include "OceanRenderable.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Camera.h"
#include "Renderer.h"
#include "SamplerManager.h"

using Microsoft::WRL::ComPtr;

namespace
{
    struct OceanVertex
    {
        float px;
        float py;
        float pz;
        float u;
        float v;
    };
}

class OceanRenderable::OceanUniformBinder final : public RenderableObject::UniformBinder
{
public:
    explicit OceanUniformBinder(OceanRenderable& owner) : owner_(owner) {}

    void RebuildHandles(RenderableObject& owner) override
    {
        if (Material* material = owner.GetGraphicsMaterial())
        {
            modelHandle_ = material->ComputeCBFieldHandle(0, "model");
            viewHandle_ = material->ComputeCBFieldHandle(0, "view");
            projHandle_ = material->ComputeCBFieldHandle(0, "proj");
            clipHandle_ = material->ComputeCBFieldHandle(0, "clipData");
            simulationParamsHandle_ = material->ComputeCBFieldHandle(0, "simulationParams");
            viewerParamsHandle_ = material->ComputeCBFieldHandle(0, "viewerParams");
            cascadeLengthScalesHandle_ = material->ComputeCBFieldHandle(0, "cascadeLengthScales");
            inverseCascadeLengthScalesHandle_ = material->ComputeCBFieldHandle(0, "inverseCascadeLengthScales");
        }
        else
        {
            modelHandle_ = {};
            viewHandle_ = {};
            projHandle_ = {};
            clipHandle_ = {};
            simulationParamsHandle_ = {};
            viewerParamsHandle_ = {};
            cascadeLengthScalesHandle_ = {};
            inverseCascadeLengthScalesHandle_ = {};
        }
    }

    void UpdateMainCB(RenderableObject& owner, Renderer* /*renderer*/, const mat4& view, const mat4& proj, uint8_t* cbData) override
    {
        Material* material = owner.GetGraphicsMaterial();
        if (!material)
        {
            return;
        }

        UpdateUniform(owner, modelHandle_, material, owner.GetModelMatrix(), cbData);
        UpdateUniform(owner, viewHandle_, material, view, cbData);
        UpdateUniform(owner, projHandle_, material, proj, cbData);

        for (uint32_t i = 0; i < OceanSimulation::kClipLevels; ++i)
        {
            const Math::float4 clip = owner_.GetClipData(i);
            material->UpdateCBField(clipHandle_, clip, cbData, i);
        }

        UpdateUniform(owner, simulationParamsHandle_, material, owner_.GetSimulationParams(), cbData);
        UpdateUniform(owner, viewerParamsHandle_, material, owner_.GetViewerParams(), cbData);
        UpdateUniform(owner, cascadeLengthScalesHandle_, material, owner_.GetCascadeLengthScales(), cbData);
        UpdateUniform(owner, inverseCascadeLengthScalesHandle_, material, owner_.GetCascadeInvLengthScales(), cbData);
    }

private:
    OceanRenderable& owner_;
    Material::CBFieldHandle modelHandle_{};
    Material::CBFieldHandle viewHandle_{};
    Material::CBFieldHandle projHandle_{};
    Material::CBFieldHandle clipHandle_{};
    Material::CBFieldHandle simulationParamsHandle_{};
    Material::CBFieldHandle viewerParamsHandle_{};
    Material::CBFieldHandle cascadeLengthScalesHandle_{};
    Material::CBFieldHandle inverseCascadeLengthScalesHandle_{};
};

OceanRenderable::OceanRenderable(Camera* camera)
    : RenderableObject("PosLevelUV", L"shaders/ocean_surface.hlsl")
    , camera_(camera)
    , simulation_(std::make_unique<OceanSimulation>())
{
}

void OceanRenderable::Init(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    auto& gd = GetGraphicsDesc();
    gd.numRT = 1;
    gd.rtvFormats[0] = renderer->GetSceneColorFormat();
    gd.dsvFormat = DXGI_FORMAT_D32_FLOAT;
    gd.depth.DepthEnable = TRUE;
    gd.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    gd.raster.CullMode = D3D12_CULL_MODE_NONE;

    if (!GetUniformBinder())
    {
        SetUniformBinder(std::make_unique<OceanUniformBinder>(*this));
    }

    RenderableObject::Init(renderer, uploadCmdList, uploadKeepAlive);

    BuildMesh(renderer, uploadCmdList, uploadKeepAlive);
    simulation_->Initialize(renderer, uploadCmdList, uploadKeepAlive);
    lengthScales_ = simulation_->GetLengthScales();
    invLengthScales_ = simulation_->GetInvLengthScales();
}

void OceanRenderable::Tick(float deltaTime)
{
    elapsedTime_ += deltaTime;
    if (camera_)
    {
        const auto pos = camera_->GetPosition();
        viewerXZ_ = Math::float2(pos.x, pos.z);
    }
    UpdateClipLevels();
}

void OceanRenderable::RecordCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (!renderer || !cl)
    {
        return;
    }
    if (!simulation_)
    {
        return;
    }
    simulation_->Update(renderer, cl, elapsedTime_);
    lengthScales_ = simulation_->GetLengthScales();
    invLengthScales_ = simulation_->GetInvLengthScales();
}

void OceanRenderable::PopulateContext(Renderer* renderer, ID3D12GraphicsCommandList* /*cl*/, RenderContext& ctx)
{
    if (!renderer || !simulation_)
    {
        return;
    }

    auto tbl = renderer->StageSrvUavTable({ simulation_->GetDisplacementSRV() });
    ctx.table[0] = tbl.gpu;

    const auto samplers = std::array{ SamplerManager::LinearWrap() };
    ctx.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, samplers);
}

void OceanRenderable::RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx)
{
    if (!renderer || !cl)
    {
        return;
    }
    if (simulation_)
    {
        const D3D12_RESOURCE_STATES srvState =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        renderer->Transition(cl, simulation_->GetDisplacementResource(), srvState);
    }
    RenderableObject::RecordGraphics(renderer, cl, ctx);
}

void OceanRenderable::OnMaterialHotReload(Renderer* renderer)
{
    RenderableObject::OnMaterialHotReload(renderer);
    if (simulation_)
    {
        simulation_->OnHotReload(renderer);
    }
}

void OceanRenderable::BuildMesh(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    const uint32_t resolution = OceanSimulation::kResolution;
    const uint32_t levels = OceanSimulation::kClipLevels;
    const uint32_t vertsPerLevel = (resolution + 1u) * (resolution + 1u);

    std::vector<OceanVertex> vertices;
    vertices.reserve(size_t(vertsPerLevel) * size_t(levels));

    std::vector<uint32_t> indices;
    indices.reserve(size_t(levels) * size_t(resolution) * size_t(resolution) * 6u);

    for (uint32_t level = 0; level < levels; ++level)
    {
        for (uint32_t y = 0; y <= resolution; ++y)
        {
            const float fy = static_cast<float>(y) / static_cast<float>(resolution);
            const float ly = fy * 2.0f - 1.0f;
            for (uint32_t x = 0; x <= resolution; ++x)
            {
                const float fx = static_cast<float>(x) / static_cast<float>(resolution);
                const float lx = fx * 2.0f - 1.0f;
                vertices.push_back({ lx, ly, static_cast<float>(level), fx, fy });
            }
        }

        const uint32_t baseVertex = level * vertsPerLevel;
        for (uint32_t y = 0; y < resolution; ++y)
        {
            for (uint32_t x = 0; x < resolution; ++x)
            {
                const uint32_t i0 = baseVertex + y * (resolution + 1u) + x;
                const uint32_t i1 = i0 + 1u;
                const uint32_t i2 = i0 + (resolution + 1u);
                const uint32_t i3 = i2 + 1u;

                indices.push_back(i0);
                indices.push_back(i2);
                indices.push_back(i1);

                indices.push_back(i1);
                indices.push_back(i2);
                indices.push_back(i3);
            }
        }
    }

    mesh_->CreateGPUFlexible(renderer->GetDevice(), uploadCmdList, uploadKeepAlive,
        vertices.data(), static_cast<UINT>(vertices.size()), sizeof(OceanVertex),
        indices.data(), static_cast<UINT>(indices.size()), DXGI_FORMAT_R32_UINT);
}

void OceanRenderable::UpdateClipLevels()
{
    const float patchLength = simulation_ ? simulation_->GetPatchLength() : 200.0f;

    for (uint32_t level = 0; level < clipLevels_.size(); ++level)
    {
        const float scale = patchLength * std::pow(2.0f, static_cast<float>(level));
        const float half = scale * 0.5f;
        const float step = scale / static_cast<float>(OceanSimulation::kResolution);

        const float snappedX = std::floor(viewerXZ_.x / step) * step;
        const float snappedZ = std::floor(viewerXZ_.y / step) * step;

        clipLevels_[level].halfExtent = half;
        clipLevels_[level].offset = Math::float2(snappedX, snappedZ);
        clipLevels_[level].step = step;
    }

    activeClipLevels_ = static_cast<uint32_t>(clipLevels_.size());
}

Math::float4 OceanRenderable::GetClipData(uint32_t index) const
{
    if (index >= activeClipLevels_ || index >= clipLevels_.size())
    {
        return Math::float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    const auto& lvl = clipLevels_[index];
    return Math::float4(lvl.offset.x, lvl.offset.y, lvl.halfExtent, lvl.step);
}

Math::float4 OceanRenderable::GetSimulationParams() const
{
    const float patchLength = simulation_ ? simulation_->GetPatchLength() : 200.0f;
    const float invPatch = (patchLength > Math::EPS) ? (1.0f / patchLength) : 0.0f;
    return Math::float4(patchLength, invPatch, elapsedTime_, static_cast<float>(activeClipLevels_));
}

Math::float4 OceanRenderable::GetViewerParams() const
{
    const float amplitude = simulation_ ? simulation_->GetDisplacementAmplitude() : 1.0f;
    return Math::float4(viewerXZ_.x, viewerXZ_.y, amplitude, 0.0f);
}

Math::float4 OceanRenderable::GetCascadeLengthScales() const
{
    return lengthScales_;
}

Math::float4 OceanRenderable::GetCascadeInvLengthScales() const
{
    return invLengthScales_;
}

