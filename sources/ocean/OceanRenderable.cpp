#include "ocean/OceanRenderable.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "app/Camera.h"
#include "app/Scene.h"
#include "app/DirectionalLight.h"
#include "app/Systems.h"
#include "rendering/core/Renderer.h"
#include "rendering/descriptors/SamplerManager.h"
#include "rendering/lighting/Skybox.h"

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

    const Math::float4 kDeepScatterColor(0.0f, 0.012745098f, 0.04019608f, 1.0f);
    const Math::float4 kSssColor(0.13333334f, 0.9411765f, 0.6039216f, 1.0f);
    const Math::float4 kDiffuseColor(0.0f, 0.025490196f, 0.02745098f, 1.0f);
    const Math::float4 kAbsorptionGradientParams(4.0f, 0.0f, 0.0f, 0.0f);
    const std::array<Math::float4, 4> kAbsorptionColors = {
        Math::float4(0.0f, 0.041025557f, 0.094412796f, 0.0f),
        Math::float4(0.0f, 0.17351386f, 0.43203577f, 0.2f),
        Math::float4(0.16198544f, 0.68352747f, 0.79865986f, 0.66608685f),
        Math::float4(1.0f, 1.0f, 1.0f, 1.0f)
    };

    const Math::float4 kFoamTintColor(1.0f, 1.0f, 1.0f, 1.0f);
    const Math::float4 kWindParams0(12.0f, 1.0f, 0.5f, 0.2f);

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
            invViewHandle_ = material->ComputeCBFieldHandle(0, "invView");
            invProjHandle_ = material->ComputeCBFieldHandle(0, "invProj");
            simulationParamsHandle_ = material->ComputeCBFieldHandle(0, "simulationParams");
            viewerParamsHandle_ = material->ComputeCBFieldHandle(0, "viewerParams");
            cascadeLengthScalesHandle_ = material->ComputeCBFieldHandle(0, "cascadeLengthScales");
            inverseCascadeLengthScalesHandle_ = material->ComputeCBFieldHandle(0, "inverseCascadeLengthScales");
            clipMapParamsHandle_ = material->ComputeCBFieldHandle(0, "clipMapParams");
            clipMapViewerHandle_ = material->ComputeCBFieldHandle(0, "clipMapViewer");
            foamParams0Handle_ = material->ComputeCBFieldHandle(0, "foamParams0");
            foamParams1Handle_ = material->ComputeCBFieldHandle(0, "foamParams1");
            foamCascadeWeightsHandle_ = material->ComputeCBFieldHandle(0, "foamCascadeWeights");
            specularParamsHandle_ = material->ComputeCBFieldHandle(0, "specularParams");
            refractionParamsHandle_ = material->ComputeCBFieldHandle(0, "refractionParams");
            subsurfaceParamsHandle_ = material->ComputeCBFieldHandle(0, "subsurfaceParams");
            heightFogParamsHandle_ = material->ComputeCBFieldHandle(0, "heightFogParams");
            sunDirAmbientHandle_ = material->ComputeCBFieldHandle(0, "sunDirAmbient");
            sunColorExposureHandle_ = material->ComputeCBFieldHandle(0, "sunColorExposure");
            deepScatterColorHandle_ = material->ComputeCBFieldHandle(0, "deepScatterColor");
            sssColorHandle_ = material->ComputeCBFieldHandle(0, "sssColor");
            diffuseColorHandle_ = material->ComputeCBFieldHandle(0, "diffuseColor");
            absorptionGradientParamsHandle_ = material->ComputeCBFieldHandle(0, "absorptionGradientParams");
            absorptionColorsHandle_ = material->ComputeCBFieldHandle(0, "absorptionColors");
            worldToWindHandle_ = material->ComputeCBFieldHandle(0, "worldToWind");
            windParams0Handle_ = material->ComputeCBFieldHandle(0, "windParams0");
            windParams1Handle_ = material->ComputeCBFieldHandle(0, "windParams1");
            foamTrailParams0Handle_ = material->ComputeCBFieldHandle(0, "foamTrailParams0");
            foamTrailParams1Handle_ = material->ComputeCBFieldHandle(0, "foamTrailParams1");
            foamParams2Handle_ = material->ComputeCBFieldHandle(0, "foamParams2");
            foamTintHandle_ = material->ComputeCBFieldHandle(0, "foamTint");
            depthTextureSizeHandle_ = material->ComputeCBFieldHandle(0, "depthTextureSize");
            depthParamsHandle_ = material->ComputeCBFieldHandle(0, "depthParams");
        }
        else
        {
            modelHandle_ = {};
            viewHandle_ = {};
            projHandle_ = {};
            invViewHandle_ = {};
            invProjHandle_ = {};
            simulationParamsHandle_ = {};
            viewerParamsHandle_ = {};
            cascadeLengthScalesHandle_ = {};
            inverseCascadeLengthScalesHandle_ = {};
            clipMapParamsHandle_ = {};
            clipMapViewerHandle_ = {};
            foamParams0Handle_ = {};
            foamParams1Handle_ = {};
            foamCascadeWeightsHandle_ = {};
            specularParamsHandle_ = {};
            refractionParamsHandle_ = {};
            subsurfaceParamsHandle_ = {};
            heightFogParamsHandle_ = {};
            sunDirAmbientHandle_ = {};
            sunColorExposureHandle_ = {};
            deepScatterColorHandle_ = {};
            sssColorHandle_ = {};
            diffuseColorHandle_ = {};
            absorptionGradientParamsHandle_ = {};
            absorptionColorsHandle_ = {};
            worldToWindHandle_ = {};
            windParams0Handle_ = {};
            windParams1Handle_ = {};
            foamTrailParams0Handle_ = {};
            foamTrailParams1Handle_ = {};
            foamParams2Handle_ = {};
            foamTintHandle_ = {};
            depthTextureSizeHandle_ = {};
            depthParamsHandle_ = {};
        }
    }

    void UpdateMainCB(RenderableObject& owner, Renderer* renderer, const Camera& camera, uint8_t* cbData) override
    {
        Material* material = owner.GetGraphicsMaterial();
        if (!material)
        {
            return;
        }

        const mat4& view = camera.GetViewMatrix();
        const mat4& proj = camera.GetProjMatrix();
        const mat4& invView = camera.GetInvViewMatrix();
        const mat4& invProj = camera.GetInvProjMatrix();

        UpdateUniform(owner, modelHandle_, material, owner.GetModelMatrix(), cbData);
        UpdateUniform(owner, viewHandle_, material, view, cbData);
        UpdateUniform(owner, projHandle_, material, proj, cbData);
        UpdateUniform(owner, invViewHandle_, material, invView, cbData);
        UpdateUniform(owner, invProjHandle_, material, invProj, cbData);

        UpdateUniform(owner, simulationParamsHandle_, material, owner_.GetSimulationParams(), cbData);
        UpdateUniform(owner, viewerParamsHandle_, material, owner_.GetViewerParams(), cbData);
        UpdateUniform(owner, cascadeLengthScalesHandle_, material, owner_.GetCascadeLengthScales(), cbData);
        UpdateUniform(owner, inverseCascadeLengthScalesHandle_, material, owner_.GetCascadeInvLengthScales(), cbData);
        UpdateUniform(owner, clipMapParamsHandle_, material, owner_.GetClipMapParams(), cbData);
        UpdateUniform(owner, clipMapViewerHandle_, material, owner_.GetClipMapViewer(), cbData);
        UpdateUniform(owner, foamParams0Handle_, material, owner_.GetFoamParams0(), cbData);
        UpdateUniform(owner, foamParams1Handle_, material, owner_.GetFoamParams1(), cbData);
        UpdateUniform(owner, foamCascadeWeightsHandle_, material, owner_.GetFoamCascadeWeights(), cbData);
        UpdateUniform(owner, specularParamsHandle_, material, owner_.GetSpecularParams(), cbData);
        UpdateUniform(owner, refractionParamsHandle_, material, owner_.GetRefractionParams(), cbData);
        UpdateUniform(owner, subsurfaceParamsHandle_, material, owner_.GetSubsurfaceParams(), cbData);
        UpdateUniform(owner, heightFogParamsHandle_, material, owner_.GetHeightFogParams(), cbData);
        UpdateUniform(owner, sunDirAmbientHandle_, material, owner_.GetSunDirAmbient(), cbData);
        UpdateUniform(owner, sunColorExposureHandle_, material, owner_.GetSunColorExposure(), cbData);
        UpdateUniform(owner, deepScatterColorHandle_, material, owner_.GetDeepScatterColor(), cbData);
        UpdateUniform(owner, sssColorHandle_, material, owner_.GetSssColor(), cbData);
        UpdateUniform(owner, diffuseColorHandle_, material, owner_.GetDiffuseColor(), cbData);
        UpdateUniform(owner, absorptionGradientParamsHandle_, material, owner_.GetAbsorptionGradientParams(), cbData);
        const uint32_t absorptionCount = owner_.GetAbsorptionColorCount();
        for (uint32_t i = 0; i < absorptionCount; ++i)
        {
            UpdateUniform(owner, absorptionColorsHandle_, material, owner_.GetAbsorptionColor(i), cbData, i);
        }
        UpdateUniform(owner, worldToWindHandle_, material, owner_.GetWorldToWindMatrix(), cbData);
        UpdateUniform(owner, windParams0Handle_, material, owner_.GetWindParams0(), cbData);
        UpdateUniform(owner, windParams1Handle_, material, owner_.GetWindParams1(), cbData);
        UpdateUniform(owner, foamTrailParams0Handle_, material, owner_.GetFoamTrailParams0(), cbData);
        UpdateUniform(owner, foamTrailParams1Handle_, material, owner_.GetFoamTrailParams1(), cbData);
        UpdateUniform(owner, foamParams2Handle_, material, owner_.GetFoamParams2(), cbData);
        UpdateUniform(owner, foamTintHandle_, material, owner_.GetFoamTint(), cbData);
        UpdateUniform(owner, depthTextureSizeHandle_, material, owner_.GetDepthTextureSize(renderer), cbData);
        UpdateUniform(owner, depthParamsHandle_, material, owner_.GetDepthParams(), cbData);
    }

private:
    OceanRenderable& owner_;
    Material::CBFieldHandle modelHandle_{};
    Material::CBFieldHandle viewHandle_{};
    Material::CBFieldHandle projHandle_{};
    Material::CBFieldHandle invViewHandle_{};
    Material::CBFieldHandle invProjHandle_{};
    Material::CBFieldHandle simulationParamsHandle_{};
    Material::CBFieldHandle viewerParamsHandle_{};
    Material::CBFieldHandle cascadeLengthScalesHandle_{};
    Material::CBFieldHandle inverseCascadeLengthScalesHandle_{};
    Material::CBFieldHandle clipMapParamsHandle_{};
    Material::CBFieldHandle clipMapViewerHandle_{};
    Material::CBFieldHandle foamParams0Handle_{};
    Material::CBFieldHandle foamParams1Handle_{};
    Material::CBFieldHandle foamCascadeWeightsHandle_{};
    Material::CBFieldHandle specularParamsHandle_{};
    Material::CBFieldHandle refractionParamsHandle_{};
    Material::CBFieldHandle subsurfaceParamsHandle_{};
    Material::CBFieldHandle heightFogParamsHandle_{};
    Material::CBFieldHandle sunDirAmbientHandle_{};
    Material::CBFieldHandle sunColorExposureHandle_{};
    Material::CBFieldHandle deepScatterColorHandle_{};
    Material::CBFieldHandle sssColorHandle_{};
    Material::CBFieldHandle diffuseColorHandle_{};
    Material::CBFieldHandle absorptionGradientParamsHandle_{};
    Material::CBFieldHandle absorptionColorsHandle_{};
    Material::CBFieldHandle worldToWindHandle_{};
    Material::CBFieldHandle windParams0Handle_{};
    Material::CBFieldHandle windParams1Handle_{};
    Material::CBFieldHandle foamTrailParams0Handle_{};
    Material::CBFieldHandle foamTrailParams1Handle_{};
    Material::CBFieldHandle foamParams2Handle_{};
    Material::CBFieldHandle foamTintHandle_{};
    Material::CBFieldHandle depthTextureSizeHandle_{};
    Material::CBFieldHandle depthParamsHandle_{};
};

OceanRenderable::OceanRenderable(Camera* camera, Scene* scene)
    : RenderableObject("PosLevelUV", L"shaders/ocean_surface.hlsl")
    , camera_(camera)
    , scene_(scene)
    , simulation_(std::make_unique<OceanSimulation>())
{
    const FoamParams defaultFoam = FoamParams::GetDefault();
    foamTrailTextureSize0_ = defaultFoam.trailTextureSize;
    foamTrailTextureSize1_ = defaultFoam.trailTextureSize;
    foamTrailDirection0_ = Math::float2(1.0f, 0.0f);
    foamTrailDirection1_ = Math::float2(1.0f, 0.0f);
    foamTrailBlendValue_ = 0.0f;
    foamTrailBlendStartTime_ = 0.0f;
    foamTrailBlendDuration_ = 0.0f;
    foamTrailBlendActive_ = false;
    foamTrailHasHistory_ = false;
    SetRenderLayerMask(RenderLayerMask(RenderLayer::Transparent));
}

void OceanRenderable::Init(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
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

    auto loadTexture = [&](Texture2D& tex, const wchar_t* path, Texture2D::Usage usage)
    {
        Texture2D::CreateDesc desc{};
        desc.path = path;
        desc.usage = usage;
        tex.CreateFromFile(renderer, uploadCmdList, desc, uploadKeepAlive);
    };

    loadTexture(distantRoughnessTexture_, L"textures/ocean/wind_gusts.png", Texture2D::Usage::LinearData);
    loadTexture(foamDetailTexture_, L"textures/ocean/wind_gusts.png", Texture2D::Usage::LinearData);
    loadTexture(foamAlbedoTexture_, L"textures/ocean/FoamAlbedo.png", Texture2D::Usage::AlbedoSRGB);
    loadTexture(foamUnderwaterTexture_, L"textures/ocean/UnderwaterFoam.png", Texture2D::Usage::AlbedoSRGB);
    loadTexture(foamTrailTexture_, L"textures/ocean/FoamTrail.png", Texture2D::Usage::LinearData);
    loadTexture(contactFoamTexture_, L"textures/ocean/ContactFoam.png", Texture2D::Usage::AlbedoSRGB);

    UpdateFoamTrailState();
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
    UpdateFoamTrailState();
}

void OceanRenderable::RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx, const Camera& camera, uint8_t* cbData)
{
    if (!renderer || !simulation_)
    {
        return;
    }

    const auto& deferred = renderer->GetDeferredForFrame();
    Skybox* sky = scene_ ? scene_->GetSkybox() : nullptr;

    auto fallbackSrv = deferred.sceneSRV;

    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 12> srvs{};
    size_t srvCount = 0;

    auto pushSrv = [&](D3D12_CPU_DESCRIPTOR_HANDLE srv)
    {
        srvs[srvCount++] = srv;
    };

    auto pushTexture = [&](Texture2D& tex)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE srv = tex.GetSRVCPU();
        if (srv.ptr == 0)
        {
            srv = fallbackSrv;
        }
        pushSrv(srv);
    };

    D3D12_CPU_DESCRIPTOR_HANDLE displacementSrv = simulation_->GetDisplacementSRV();
    if (displacementSrv.ptr == 0)
    {
        displacementSrv = fallbackSrv;
    }
    pushSrv(displacementSrv);

    D3D12_CPU_DESCRIPTOR_HANDLE foamSrv = simulation_->GetFoamTurbulenceSRV();
    if (foamSrv.ptr == 0)
    {
        foamSrv = fallbackSrv;
    }
    pushSrv(foamSrv);

    D3D12_CPU_DESCRIPTOR_HANDLE sceneSrv = deferred.sceneOpaqueSRV.ptr != 0 ? deferred.sceneOpaqueSRV : fallbackSrv;
    pushSrv(sceneSrv.ptr != 0 ? sceneSrv : fallbackSrv);

    D3D12_CPU_DESCRIPTOR_HANDLE skySrv{};
    if (sky && sky->GetTex())
    {
        skySrv = sky->GetTex()->GetSRVCPU();
    }
    if (skySrv.ptr == 0)
    {
        skySrv = fallbackSrv;
    }
    pushSrv(skySrv);

    D3D12_CPU_DESCRIPTOR_HANDLE ssrSrv = deferred.ssrSRV.ptr != 0 ? deferred.ssrSRV : fallbackSrv;
    pushSrv(ssrSrv.ptr != 0 ? ssrSrv : fallbackSrv);

    pushTexture(distantRoughnessTexture_);
    pushTexture(foamDetailTexture_);
    pushTexture(foamAlbedoTexture_);
    pushTexture(foamUnderwaterTexture_);
    pushTexture(foamTrailTexture_);
    pushTexture(contactFoamTexture_);

    D3D12_CPU_DESCRIPTOR_HANDLE depthSrv = deferred.depthCopySRV.ptr != 0 ? deferred.depthCopySRV : deferred.depthSRV;
    if (depthSrv.ptr == 0)
    {
        depthSrv = fallbackSrv;
    }
    pushSrv(depthSrv);

    auto tbl = renderer->StageSrvUavTable(srvs, srvCount);
    ctx.table[0] = tbl.gpu;

    const auto samplers = std::array{ *SamplerManager::LinearWrap(), *SamplerManager::LinearClamp(), *SamplerManager::PointClamp() };
    ctx.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, samplers);

    const D3D12_RESOURCE_STATES srvState =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    renderer->Transition(cl, simulation_->GetDisplacementResource(), srvState);

    RenderableObject::RecordGraphics(renderer, cl, ctx, camera, cbData);
}

void OceanRenderable::ConfigureGraphicsPipeline(Renderer* renderer, Material::GraphicsDesc& desc) const
{
    RenderableObject::ConfigureGraphicsPipeline(renderer, desc);

    desc.numRT = 1;
    if (renderer)
    {
        desc.rtvFormats[0] = renderer->GetSceneColorFormat();
        desc.dsvFormat = renderer->GetDsvFormat();
    }
    desc.depth.DepthEnable = TRUE;
    desc.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.raster.CullMode = D3D12_CULL_MODE_NONE;
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
    const uint32_t levels = kClipLevels;
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
}

void OceanRenderable::UpdateFoamTrailState()
{
    FoamParams foam = simulation_ ? simulation_->GetFoamParams() : FoamParams::GetDefault();

    Math::float2 windDir(1.0f, 0.0f);
    if (simulation_)
    {
        windDir = simulation_->GetLocalWindDirectionVector();
    }
    if (windDir.Length() > Math::EPS)
    {
        windDir = windDir.Normalized();
    }
    else
    {
        windDir = Math::float2(1.0f, 0.0f);
    }

    const float updateTime = simulation_ ? simulation_->GetFoamTrailUpdateTime() : 0.0f;

    if (updateTime <= Math::EPS)
    {
        foamTrailTextureSize0_ = foam.trailTextureSize;
        foamTrailTextureSize1_ = foam.trailTextureSize;
        foamTrailDirection0_ = windDir;
        foamTrailDirection1_ = windDir;
        foamTrailBlendValue_ = 0.0f;
        foamTrailBlendStartTime_ = elapsedTime_;
        foamTrailBlendDuration_ = 0.0f;
        foamTrailBlendActive_ = false;
        foamTrailHasHistory_ = false;
        return;
    }

    if (!foamTrailHasHistory_)
    {
        foamTrailTextureSize0_ = foam.trailTextureSize;
        foamTrailTextureSize1_ = foam.trailTextureSize;
        foamTrailDirection0_ = windDir;
        foamTrailDirection1_ = windDir;
        foamTrailBlendStartTime_ = elapsedTime_;
        foamTrailBlendValue_ = 0.0f;
        foamTrailBlendDuration_ = updateTime;
        foamTrailHasHistory_ = true;
        foamTrailBlendActive_ = true;
        return;
    }

    const float timeSinceStart = elapsedTime_ - foamTrailBlendStartTime_;
    if (!foamTrailBlendActive_ || timeSinceStart >= updateTime)
    {
        foamTrailTextureSize0_ = foamTrailTextureSize1_;
        foamTrailDirection0_ = foamTrailDirection1_;
        foamTrailTextureSize1_ = foam.trailTextureSize;
        foamTrailDirection1_ = windDir;
        foamTrailBlendStartTime_ = elapsedTime_;
        foamTrailBlendValue_ = 0.0f;
        foamTrailBlendDuration_ = updateTime;
        foamTrailBlendActive_ = true;
        return;
    }

    const float denom = std::max(updateTime, Math::EPS);
    foamTrailBlendValue_ = Math::Clamp(timeSinceStart / denom, 0.0f, 1.0f);
    foamTrailBlendDuration_ = updateTime;
}

Math::float4 OceanRenderable::GetSimulationParams() const
{
    const float patchLength = simulation_ ? simulation_->GetPatchLength() : 200.0f;
    const float invPatch = (patchLength > Math::EPS) ? (1.0f / patchLength) : 0.0f;
    return Math::float4(patchLength, invPatch, elapsedTime_, (float)simulation_->GetCascadeCount());
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

Math::float4 OceanRenderable::GetFoamParams0() const
{
    FoamParams foam = simulation_ ? simulation_->GetFoamParams() : FoamParams::GetDefault();
    return Math::float4(foam.coverage, foam.density, foam.sharpness, foam.persistence);
}

Math::float4 OceanRenderable::GetFoamParams1() const
{
    FoamParams foam = simulation_ ? simulation_->GetFoamParams() : FoamParams::GetDefault();
    return Math::float4(foam.trail, foam.trailTextureStrength, foam.underwater, 0.6f);
}

Math::float4 OceanRenderable::GetFoamCascadeWeights() const
{
    FoamParams foam = simulation_ ? simulation_->GetFoamParams() : FoamParams::GetDefault();
    return foam.cascadesWeights;
}

Math::float4 OceanRenderable::GetSpecularParams() const
{
    // spec strength, roughness scale, roughness distance, horizon fog strength
    return Math::float4(1.1f, 0.7f, 150.0f, 0.55f);
}

Math::float4 OceanRenderable::GetRefractionParams() const
{
    // refraction strengths and absorption parameters
    return Math::float4(0.28f, 0.75f, 10.0f, 0.1f);
}

Math::float4 OceanRenderable::GetSubsurfaceParams() const
{
    // subsurface scattering controls
    // x: sun scatter strength, y: sky scatter strength, z: scatter spread, w: view alignment strength
    return Math::float4(0.35f, 0.35f, 0.2f, 0.8f);
}

Math::float4 OceanRenderable::GetHeightFogParams() const
{
    // sss height bias, fade distance, horizon fog distance scale, reflection normal strength
    return Math::float4(0.0f, 6.0f, 2.5f, 0.05f);
}

Math::float4 OceanRenderable::GetSunDirAmbient() const
{
    Math::float3 dir = Math::float3(0.0f, -1.0f, 0.0f);
    float ambient = 0.1f;
    if (scene_)
    {
        const auto& light = scene_->GetDirectionalLight();
        dir = light.GetDirection();
        ambient = light.GetAmbient();
    }
    return Math::float4(dir.x, dir.y, dir.z, ambient);
}

Math::float4 OceanRenderable::GetSunColorExposure() const
{
    Math::float3 color = Math::float3(1.0f, 1.0f, 1.0f);
    float exposure = 1.0f;
    if (scene_)
    {
        const auto& light = scene_->GetDirectionalLight();
        color = light.GetColor();
        exposure = light.GetExposure();
    }
    return Math::float4(color.x, color.y, color.z, exposure);
}

Math::float4 OceanRenderable::GetDeepScatterColor() const
{
    return kDeepScatterColor;
}

Math::float4 OceanRenderable::GetSssColor() const
{
    return kSssColor;
}

Math::float4 OceanRenderable::GetDiffuseColor() const
{
    return kDiffuseColor;
}

Math::float4 OceanRenderable::GetAbsorptionGradientParams() const
{
    return kAbsorptionGradientParams;
}

Math::float4 OceanRenderable::GetAbsorptionColor(uint32_t index) const
{
    if (index >= kAbsorptionColors.size())
    {
        return Math::float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    return kAbsorptionColors[index];
}

uint32_t OceanRenderable::GetAbsorptionColorCount() const
{
    return static_cast<uint32_t>(kAbsorptionColors.size());
}

mat4 OceanRenderable::GetWorldToWindMatrix() const
{
    float radians = 0.0f;
    if (simulation_)
    {
        radians = simulation_->GetLocalWindDirectionRadians();
    }

    return mat4::RotationY(-radians);
}

Math::float4 OceanRenderable::GetWindParams0() const
{
    return kWindParams0;
}

Math::float4 OceanRenderable::GetWindParams1() const
{
    Math::float2 dir(1.0f, 0.0f);
    if (simulation_)
    {
        dir = simulation_->GetLocalWindDirectionVector().Normalized();
    }
    const float amplitude = simulation_ ? simulation_->GetDisplacementAmplitude() : 1.0f;
    return Math::float4(dir.x, dir.y, amplitude, 0.0f);
}

Math::float4 OceanRenderable::GetFoamTrailParams0() const
{
    return Math::float4(foamTrailTextureSize0_.x, foamTrailTextureSize0_.y,
        foamTrailTextureSize1_.x, foamTrailTextureSize1_.y);
}

Math::float4 OceanRenderable::GetFoamTrailParams1() const
{
    return Math::float4(foamTrailDirection0_.x, foamTrailDirection0_.y,
        foamTrailDirection1_.x, foamTrailDirection1_.y);
}

Math::float4 OceanRenderable::GetFoamParams2() const
{
    FoamParams foam = simulation_ ? simulation_->GetFoamParams() : FoamParams::GetDefault();
    const float blendValue = Math::Clamp(foamTrailBlendValue_, 0.0f, 1.0f);
    const float contactFoam = 0.1f;
    const float underwaterParallax = 1.6f;
    return Math::float4(blendValue, contactFoam, underwaterParallax, 0.0f);
}

Math::float4 OceanRenderable::GetFoamTint() const
{
    return kFoamTintColor;
}

Math::float4 OceanRenderable::GetDepthTextureSize(const Renderer* renderer) const
{
    const float width = renderer ? static_cast<float>(std::max(renderer->GetWidth(), 1u)) : 1.0f;
    const float height = renderer ? static_cast<float>(std::max(renderer->GetHeight(), 1u)) : 1.0f;
    return Math::float4(1.0f / width, 1.0f / height, width, height);
}

Math::float2 OceanRenderable::GetDepthParams() const
{
    auto& scene = Systems::GetScene();
    float zNear = scene.CameraRef().GetZNear();
	float zFar = scene.CameraRef().GetZFar();
    return { zNear / (zNear - zFar), (zNear * zFar) / (zFar - zNear) };
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
