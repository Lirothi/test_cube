#include "OceanRenderable.h"

#include <algorithm>
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

    struct MeshData
    {
        std::vector<OceanVertex> vertices;
        std::vector<uint32_t> indices;
    };

    constexpr int kOverlap = 2;

    int ClipLevelHalfSize(uint32_t vertexDensity)
    {
        return static_cast<int>((vertexDensity + 1u) * 4u - 1u);
    }

    void AppendMesh(MeshData& dst, const MeshData& src,
        const Math::float3& translation, const Math::float3& scale)
    {
        const uint32_t baseVertex = static_cast<uint32_t>(dst.vertices.size());
        dst.vertices.reserve(dst.vertices.size() + src.vertices.size());
        dst.indices.reserve(dst.indices.size() + src.indices.size());

        for (const auto& v : src.vertices)
        {
            OceanVertex out = v;
            out.px = v.px * scale.x + translation.x;
            out.py = v.py * scale.y + translation.y;
            out.pz = v.pz * scale.z + translation.z;
            dst.vertices.push_back(out);
        }

        for (uint32_t idx : src.indices)
        {
            dst.indices.push_back(baseVertex + idx);
        }
    }

    MeshData BuildPlane(int width, int height, const Math::float3& pivot,
        bool geomorphOffsetInUv, bool morphShiftX = false, bool morphShiftZ = false,
        int trianglesShift = 0)
    {
        MeshData mesh;
        const int vertCount = (width + 1) * (height + 1);
        mesh.vertices.resize(static_cast<size_t>(vertCount));
        mesh.indices.resize(static_cast<size_t>(width * height * 6));

        for (int i = 0; i <= height; ++i)
        {
            for (int j = 0; j <= width; ++j)
            {
                const int index = j + i * (width + 1);
                int x = j;
                int z = i;

                const Math::float3 normalPos(static_cast<float>(x), 1.0f, static_cast<float>(z));

                if ((x & 1) != 0)
                {
                    const bool cond = morphShiftX ^ ((x & 3) == 3);
                    x += cond ? 1 : -1;
                }
                if ((z & 1) != 0)
                {
                    const bool cond = morphShiftZ ^ ((z & 3) == 3);
                    z += cond ? 1 : -1;
                }

                OceanVertex v{};
                v.px = normalPos.x - pivot.x;
                v.py = normalPos.y - pivot.y;
                v.pz = normalPos.z - pivot.z;
                if (geomorphOffsetInUv)
                {
                    v.u = static_cast<float>(x) - normalPos.x;
                    v.v = static_cast<float>(z) - normalPos.z;
                }
                else
                {
                    v.u = 0.0f;
                    v.v = 0.0f;
                }
                mesh.vertices[static_cast<size_t>(index)] = v;
            }
        }

        size_t tri = 0;
        for (int i = 0; i < height; ++i)
        {
            for (int j = 0; j < width; ++j)
            {
                const int k = j + i * (width + 1);
                if (((i + j + trianglesShift) & 1) == 0)
                {
                    mesh.indices[tri++] = static_cast<uint32_t>(k);
                    mesh.indices[tri++] = static_cast<uint32_t>(k + width + 1);
                    mesh.indices[tri++] = static_cast<uint32_t>(k + width + 2);

                    mesh.indices[tri++] = static_cast<uint32_t>(k);
                    mesh.indices[tri++] = static_cast<uint32_t>(k + width + 2);
                    mesh.indices[tri++] = static_cast<uint32_t>(k + 1);
                }
                else
                {
                    mesh.indices[tri++] = static_cast<uint32_t>(k);
                    mesh.indices[tri++] = static_cast<uint32_t>(k + width + 1);
                    mesh.indices[tri++] = static_cast<uint32_t>(k + 1);

                    mesh.indices[tri++] = static_cast<uint32_t>(k + 1);
                    mesh.indices[tri++] = static_cast<uint32_t>(k + width + 1);
                    mesh.indices[tri++] = static_cast<uint32_t>(k + width + 2);
                }
            }
        }

        return mesh;
    }

    MeshData BuildRing(int clipLevelHalfSize)
    {
        MeshData ring;
        const int k = clipLevelHalfSize;
        const int shortSide = (k + 1) / 2 + kOverlap;
        const int longSide = k - 1;
        const int sum = longSide + shortSide;
        const bool shortMorphShift = ((shortSide / 2) % 2) == 1;

        const Math::float3 pivot = (Math::float3(1.0f, 0.0f, 0.0f) + Math::float3(0.0f, 0.0f, 1.0f)) * static_cast<float>(k + 1);

        const MeshData bottomLeft = BuildPlane(shortSide, shortSide, pivot, true, false, false);
        AppendMesh(ring, bottomLeft, Math::float3(0.0f, 0.0f, 0.0f), Math::float3(1.0f, 1.0f, 1.0f));

        const MeshData middleLeft = BuildPlane(shortSide, longSide, pivot, true, false, shortMorphShift);
        AppendMesh(ring, middleLeft, Math::float3(0.0f, 0.0f, static_cast<float>(shortSide)), Math::float3(1.0f, 1.0f, 1.0f));

        const MeshData topLeft = BuildPlane(shortSide, shortSide, pivot, true, false, !shortMorphShift);
        AppendMesh(ring, topLeft, Math::float3(0.0f, 0.0f, static_cast<float>(sum)), Math::float3(1.0f, 1.0f, 1.0f));

        const MeshData topMiddle = BuildPlane(longSide, shortSide, pivot, true, shortMorphShift, !shortMorphShift);
        AppendMesh(ring, topMiddle,
            Math::float3(static_cast<float>(shortSide), 0.0f, static_cast<float>(sum)),
            Math::float3(1.0f, 1.0f, 1.0f));

        const MeshData topRight = BuildPlane(shortSide, shortSide, pivot, true, !shortMorphShift, !shortMorphShift);
        AppendMesh(ring, topRight,
            Math::float3(static_cast<float>(sum), 0.0f, static_cast<float>(sum)),
            Math::float3(1.0f, 1.0f, 1.0f));

        const MeshData middleRight = BuildPlane(shortSide, longSide, pivot, true, !shortMorphShift, shortMorphShift);
        AppendMesh(ring, middleRight,
            Math::float3(static_cast<float>(sum), 0.0f, static_cast<float>(shortSide)),
            Math::float3(1.0f, 1.0f, 1.0f));

        const MeshData bottomRight = BuildPlane(shortSide, shortSide, pivot, true, !shortMorphShift, false);
        AppendMesh(ring, bottomRight,
            Math::float3(static_cast<float>(sum), 0.0f, 0.0f),
            Math::float3(1.0f, 1.0f, 1.0f));

        const MeshData bottomMiddle = BuildPlane(longSide, shortSide, pivot, true, shortMorphShift, false);
        AppendMesh(ring, bottomMiddle,
            Math::float3(static_cast<float>(shortSide), 0.0f, 0.0f),
            Math::float3(1.0f, 1.0f, 1.0f));

        return ring;
    }

    MeshData BuildSkirt(int clipLevelHalfSize, float outerBorderScale)
    {
        MeshData skirt;
        const int borderVertCount = clipLevelHalfSize + kOverlap;
        const int scale = 2;

        Math::float3 pivot = Math::float3(-1.0f, 0.0f, -1.0f)
            * static_cast<float>(borderVertCount) * (1.0f + 2.0f * outerBorderScale)
            + Math::float3(1.0f, 0.0f, 1.0f);

        const MeshData quad = BuildPlane(1, 1, Math::float3(0.0f, 0.0f, 0.0f), false);
        const MeshData hStrip = BuildPlane(borderVertCount, 1, Math::float3(0.0f, 0.0f, 0.0f), false);
        const MeshData vStrip = BuildPlane(1, borderVertCount, Math::float3(0.0f, 0.0f, 0.0f), false);

        outerBorderScale *= static_cast<float>(borderVertCount * scale);
        const Math::float3 cornerQuadScale(outerBorderScale, 1.0f, outerBorderScale);
        const Math::float3 stripScaleVert(static_cast<float>(scale), 1.0f, outerBorderScale);
        const Math::float3 stripScaleHor(outerBorderScale, 1.0f, static_cast<float>(scale));

        AppendMesh(skirt, quad, pivot + Math::float3(0.0f, 0.0f, 0.0f), cornerQuadScale);
        AppendMesh(skirt, hStrip, pivot + Math::float3(outerBorderScale, 0.0f, 0.0f), stripScaleVert);
        AppendMesh(skirt, quad,
            pivot + Math::float3(outerBorderScale + borderVertCount * scale, 0.0f, 0.0f), cornerQuadScale);
        AppendMesh(skirt, vStrip,
            pivot + Math::float3(0.0f, 0.0f, outerBorderScale), stripScaleHor);
        AppendMesh(skirt, vStrip,
            pivot + Math::float3(outerBorderScale + borderVertCount * scale, 0.0f, outerBorderScale), stripScaleHor);
        AppendMesh(skirt, quad,
            pivot + Math::float3(0.0f, 0.0f, outerBorderScale + borderVertCount * scale), cornerQuadScale);
        AppendMesh(skirt, hStrip,
            pivot + Math::float3(outerBorderScale, 0.0f, outerBorderScale + borderVertCount * scale), stripScaleVert);
        AppendMesh(skirt, quad,
            pivot + Math::float3(outerBorderScale + borderVertCount * scale, 0.0f,
                outerBorderScale + borderVertCount * scale), cornerQuadScale);

        return skirt;
    }
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
            simulationParamsHandle_ = material->ComputeCBFieldHandle(0, "simulationParams");
            viewerParamsHandle_ = material->ComputeCBFieldHandle(0, "viewerParams");
            cascadeLengthScalesHandle_ = material->ComputeCBFieldHandle(0, "cascadeLengthScales");
            inverseCascadeLengthScalesHandle_ = material->ComputeCBFieldHandle(0, "inverseCascadeLengthScales");
            clipMapParamsHandle_ = material->ComputeCBFieldHandle(0, "clipMapParams");
            clipMapViewerHandle_ = material->ComputeCBFieldHandle(0, "clipMapViewer");
        }
        else
        {
            modelHandle_ = {};
            viewHandle_ = {};
            projHandle_ = {};
            simulationParamsHandle_ = {};
            viewerParamsHandle_ = {};
            cascadeLengthScalesHandle_ = {};
            inverseCascadeLengthScalesHandle_ = {};
            clipMapParamsHandle_ = {};
            clipMapViewerHandle_ = {};
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

        UpdateUniform(owner, simulationParamsHandle_, material, owner_.GetSimulationParams(), cbData);
        UpdateUniform(owner, viewerParamsHandle_, material, owner_.GetViewerParams(), cbData);
        UpdateUniform(owner, cascadeLengthScalesHandle_, material, owner_.GetCascadeLengthScales(), cbData);
        UpdateUniform(owner, inverseCascadeLengthScalesHandle_, material, owner_.GetCascadeInvLengthScales(), cbData);
        UpdateUniform(owner, clipMapParamsHandle_, material, owner_.GetClipMapParams(), cbData);
        UpdateUniform(owner, clipMapViewerHandle_, material, owner_.GetClipMapViewer(), cbData);
    }

private:
    OceanRenderable& owner_;
    Material::CBFieldHandle modelHandle_{};
    Material::CBFieldHandle viewHandle_{};
    Material::CBFieldHandle projHandle_{};
    Material::CBFieldHandle simulationParamsHandle_{};
    Material::CBFieldHandle viewerParamsHandle_{};
    Material::CBFieldHandle cascadeLengthScalesHandle_{};
    Material::CBFieldHandle inverseCascadeLengthScalesHandle_{};
    Material::CBFieldHandle clipMapParamsHandle_{};
    Material::CBFieldHandle clipMapViewerHandle_{};
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
    gd.dsvFormat = renderer->GetDsvFormat();
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
    UpdateClipLevels();
}

void OceanRenderable::Tick(float deltaTime)
{
    elapsedTime_ += deltaTime;
    if (camera_)
    {
        const auto pos = camera_->GetPosition();
        viewerXZ_ = Math::float2(pos.x, pos.z);
        viewerHeight_ = pos.y;
    }
    UpdateClipLevels();
}

void OceanRenderable::RecordCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    CPU_SCOPE(ProfilerScopes::kOceanRender);
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

    const auto samplers = std::array{ *SamplerManager::LinearWrap() };
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
    const uint32_t levels = OceanSimulation::kClipLevels;
    const int clipHalfSize = ClipLevelHalfSize(meshVertexDensity_);

    MeshData combined;
    combined.vertices.reserve(1024);
    combined.indices.reserve(1024);

    const MeshData center = BuildPlane(2 * clipHalfSize + kOverlap, 2 * clipHalfSize + kOverlap,
        Math::float3(static_cast<float>(clipHalfSize + 1), 0.0f, static_cast<float>(clipHalfSize + 1)), true);
    AppendMesh(combined, center, Math::float3(0.0f, 0.0f, 0.0f), Math::float3(1.0f, 1.0f, 1.0f));

    const MeshData ring = BuildRing(clipHalfSize);
    for (uint32_t level = 1; level <= levels; ++level)
    {
        const float scale = std::pow(2.0f, static_cast<float>(level));
        AppendMesh(combined, ring, Math::float3(0.0f, 0.0f, 0.0f), Math::float3(scale, scale, scale));
    }

    const MeshData skirt = BuildSkirt(clipHalfSize, 10.0f);
    const float skirtScale = std::pow(2.0f, static_cast<float>(levels));
    AppendMesh(combined, skirt, Math::float3(0.0f, 0.0f, 0.0f), Math::float3(skirtScale, skirtScale, skirtScale));

    mesh_->CreateGPUFlexible(renderer->GetDevice(), uploadCmdList, uploadKeepAlive,
        combined.vertices.data(), static_cast<UINT>(combined.vertices.size()), sizeof(OceanVertex),
        combined.indices.data(), static_cast<UINT>(combined.indices.size()), DXGI_FORMAT_R32_UINT);
}

void OceanRenderable::UpdateClipLevels()
{
    const float patchLength = simulation_ ? simulation_->GetPatchLength() : 200.0f;

    clipMapLevelHalfSize_ = static_cast<float>(ClipLevelHalfSize(meshVertexDensity_));
    clipMapViewer_ = Math::float3(viewerXZ_.x, viewerHeight_, viewerXZ_.y);
    const float absHeight = std::abs(clipMapViewer_.y);
    int meshExponent = 0;
    if (absHeight > Math::EPS)
    {
        const float denom = std::max(2.0f * minMeshScale_, Math::EPS);
        const float ratio = absHeight / denom;
        if (ratio > Math::EPS)
        {
            meshExponent = static_cast<int>(std::floor(std::max(0.0f, std::log2(ratio) + 1.0f)));
        }
    }

    const float halfSize = std::max(1.0f, clipMapLevelHalfSize_);
    clipMapScale_ = (minMeshScale_ / halfSize) * std::pow(2.0f, static_cast<float>(meshExponent));
    clipMapScale_ = std::max(clipMapScale_, 1.0e-3f);
    for (uint32_t level = 0; level < clipLevels_.size(); ++level)
    {
        const float scale = patchLength * std::pow(2.0f, static_cast<float>(level));
        const float half = scale * 0.5f;
        const float step = scale / static_cast<float>(simulation_->GetResolution());

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
    return Math::float4(viewerXZ_.x, viewerXZ_.y, amplitude, cascadesFadeScale_);
}

Math::float4 OceanRenderable::GetCascadeLengthScales() const
{
    return lengthScales_;
}

Math::float4 OceanRenderable::GetCascadeInvLengthScales() const
{
    return invLengthScales_;
}

Math::float4 OceanRenderable::GetClipMapParams() const
{
    return Math::float4(clipMapScale_, clipMapLevelHalfSize_, static_cast<float>(meshVertexDensity_), cascadesFadeScale_);
}

Math::float4 OceanRenderable::GetClipMapViewer() const
{
    return Math::float4(clipMapViewer_.x, clipMapViewer_.y, clipMapViewer_.z, 0.0f);
}

void OceanRenderable::SetGridVertexDensity(uint32_t density)
{
    const uint32_t clamped = std::max<uint32_t>(1u, density);
    if (meshVertexDensity_ == clamped)
    {
        return;
    }
    meshVertexDensity_ = clamped;
    UpdateClipLevels();
}
