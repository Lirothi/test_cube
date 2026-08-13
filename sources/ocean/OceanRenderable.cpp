#include "ocean/OceanRenderable.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

#include "app/camera/Camera.h"
#include "app/scene/Scene.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "rendering/lighting/DirectionalLight.h"
#include "rendering/core/Renderer.h"
#include "rendering/descriptors/SamplerManager.h"
#include "rendering/lighting/Skybox.h"
#include "vfx/WindState.h" // W8: g_windFreeze pins the shared wind/ocean clock

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
            prevModelHandle_ = material->ComputeCBFieldHandle(0, "prevModel");
            viewProjHandle_ = material->ComputeCBFieldHandle(0, "viewProj");
            viewProjNoJitterHandle_ = material->ComputeCBFieldHandle(0, "viewProjNoJitter");
            prevViewProjNoJitterHandle_ = material->ComputeCBFieldHandle(0, "prevViewProjNoJitter");
            invViewHandle_ = material->ComputeCBFieldHandle(0, "invView");
            invProjHandle_ = material->ComputeCBFieldHandle(0, "invProj");
            shoreViewParamsHandle_ = material->ComputeCBFieldHandle(0, "shoreViewParams");
            shoreSdfParamsHandle_ = material->ComputeCBFieldHandle(0, "shoreSdfParams");
            shoreDepthParamsHandle_ = material->ComputeCBFieldHandle(0, "shoreDepthParams");
            simulationParamsHandle_ = material->ComputeCBFieldHandle(0, "simulationParams");
            viewerParamsHandle_ = material->ComputeCBFieldHandle(0, "viewerParams");
            cascadeLengthScalesHandle_ = material->ComputeCBFieldHandle(0, "cascadeLengthScales");
            inverseCascadeLengthScalesHandle_ = material->ComputeCBFieldHandle(0, "inverseCascadeLengthScales");
            clipMapParamsHandle_ = material->ComputeCBFieldHandle(0, "clipMapParams");
            prevClipMapParamsHandle_ = material->ComputeCBFieldHandle(0, "prevClipMapParams");
            clipMapViewerHandle_ = material->ComputeCBFieldHandle(0, "clipMapViewer");
            prevClipMapViewerHandle_ = material->ComputeCBFieldHandle(0, "prevClipMapViewer");
            foamParams0Handle_ = material->ComputeCBFieldHandle(0, "foamParams0");
            foamParams1Handle_ = material->ComputeCBFieldHandle(0, "foamParams1");
            foamCascadeWeightsHandle_ = material->ComputeCBFieldHandle(0, "foamCascadeWeights");
            specularParamsHandle_ = material->ComputeCBFieldHandle(0, "specularParams");
            refractionParamsHandle_ = material->ComputeCBFieldHandle(0, "refractionParams");
            subsurfaceParamsHandle_ = material->ComputeCBFieldHandle(0, "subsurfaceParams");
            heightFogParamsHandle_ = material->ComputeCBFieldHandle(0, "heightFogParams");
            normalSamplingParamsHandle_ = material->ComputeCBFieldHandle(0, "normalSamplingParams");
            shoreBehaviorParams0Handle_ = material->ComputeCBFieldHandle(0, "shoreBehaviorParams0");
            shoreBehaviorParams1Handle_ = material->ComputeCBFieldHandle(0, "shoreBehaviorParams1");
            shoreNormalMinWeightsHandle_ = material->ComputeCBFieldHandle(0, "shoreNormalMinWeights");
            shoreFoamGeometryParamsHandle_ = material->ComputeCBFieldHandle(0, "shoreFoamGeometryParams");
            shoreFoamPatternParamsHandle_ = material->ComputeCBFieldHandle(0, "shoreFoamPatternParams");
            shoreFoamBreakupParamsHandle_ = material->ComputeCBFieldHandle(0, "shoreFoamBreakupParams");
            shoreFoamWindParamsHandle_ = material->ComputeCBFieldHandle(0, "shoreFoamWindParams");
            shoreFoamAlbedoParamsHandle_ = material->ComputeCBFieldHandle(0, "shoreFoamAlbedoParams");
            shoreSlopeParamsHandle_ = material->ComputeCBFieldHandle(0, "shoreSlopeParams");
            shoreSwashParamsHandle_ = material->ComputeCBFieldHandle(0, "shoreSwashParams");
            shoreLegacyDampParamsHandle_ = material->ComputeCBFieldHandle(0, "shoreLegacyDampParams");
            shoreLegacyFoamParamsHandle_ = material->ComputeCBFieldHandle(0, "shoreLegacyFoamParams");
            shoreLegacyFoamParams2Handle_ = material->ComputeCBFieldHandle(0, "shoreLegacyFoamParams2");
            shoreLegacyDissipationParamsHandle_ = material->ComputeCBFieldHandle(0, "shoreLegacyDissipationParams");
            shoreSamplingParamsHandle_ = material->ComputeCBFieldHandle(0, "shoreSamplingParams");
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
            prevModelHandle_ = {};
            viewProjHandle_ = {};
            viewProjNoJitterHandle_ = {};
            prevViewProjNoJitterHandle_ = {};
            invViewHandle_ = {};
            invProjHandle_ = {};
            shoreViewParamsHandle_ = {};
            shoreSdfParamsHandle_ = {};
            shoreDepthParamsHandle_ = {};
            simulationParamsHandle_ = {};
            viewerParamsHandle_ = {};
            cascadeLengthScalesHandle_ = {};
            inverseCascadeLengthScalesHandle_ = {};
            clipMapParamsHandle_ = {};
            prevClipMapParamsHandle_ = {};
            clipMapViewerHandle_ = {};
            prevClipMapViewerHandle_ = {};
            foamParams0Handle_ = {};
            foamParams1Handle_ = {};
            foamCascadeWeightsHandle_ = {};
            specularParamsHandle_ = {};
            refractionParamsHandle_ = {};
            subsurfaceParamsHandle_ = {};
            heightFogParamsHandle_ = {};
            normalSamplingParamsHandle_ = {};
            shoreBehaviorParams0Handle_ = {};
            shoreBehaviorParams1Handle_ = {};
            shoreNormalMinWeightsHandle_ = {};
            shoreFoamGeometryParamsHandle_ = {};
            shoreFoamPatternParamsHandle_ = {};
            shoreFoamBreakupParamsHandle_ = {};
            shoreFoamWindParamsHandle_ = {};
            shoreFoamAlbedoParamsHandle_ = {};
            shoreSlopeParamsHandle_ = {};
            shoreSwashParamsHandle_ = {};
            shoreLegacyDampParamsHandle_ = {};
            shoreLegacyFoamParamsHandle_ = {};
            shoreLegacyFoamParams2Handle_ = {};
            shoreLegacyDissipationParamsHandle_ = {};
            shoreSamplingParamsHandle_ = {};
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
        UpdateUniform(owner, prevModelHandle_, material, owner.GetPreviousModelMatrix(), cbData);
        UpdateUniform(owner, viewProjHandle_, material, camera.GetViewProjMatrix(), cbData);
        UpdateUniform(owner, viewProjNoJitterHandle_, material, camera.GetViewProjMatrixNoJitter(), cbData);
        UpdateUniform(owner, prevViewProjNoJitterHandle_, material, camera.GetPrevViewProjMatrixNoJitter(), cbData);
        UpdateUniform(owner, invViewHandle_, material, invView, cbData);
        UpdateUniform(owner, invProjHandle_, material, invProj, cbData);
        UpdateUniform(owner, shoreViewParamsHandle_, material, owner_.GetShoreViewParams(), cbData);
        UpdateUniform(owner, shoreSdfParamsHandle_, material, owner_.GetShoreSdfParams(), cbData);
        UpdateUniform(owner, shoreDepthParamsHandle_, material, owner_.GetShoreDepthParams(), cbData);

        UpdateUniform(owner, simulationParamsHandle_, material, owner_.GetSimulationParams(), cbData);
        UpdateUniform(owner, viewerParamsHandle_, material, owner_.GetViewerParams(), cbData);
        UpdateUniform(owner, cascadeLengthScalesHandle_, material, owner_.GetCascadeLengthScales(), cbData);
        UpdateUniform(owner, inverseCascadeLengthScalesHandle_, material, owner_.GetCascadeInvLengthScales(), cbData);
        UpdateUniform(owner, clipMapParamsHandle_, material, owner_.GetClipMapParams(), cbData);
        UpdateUniform(owner, prevClipMapParamsHandle_, material, owner_.GetPrevClipMapParams(), cbData);
        UpdateUniform(owner, clipMapViewerHandle_, material, owner_.GetClipMapViewer(), cbData);
        UpdateUniform(owner, prevClipMapViewerHandle_, material, owner_.GetPrevClipMapViewer(), cbData);
        UpdateUniform(owner, foamParams0Handle_, material, owner_.GetFoamParams0(), cbData);
        UpdateUniform(owner, foamParams1Handle_, material, owner_.GetFoamParams1(), cbData);
        UpdateUniform(owner, foamCascadeWeightsHandle_, material, owner_.GetFoamCascadeWeights(), cbData);
        UpdateUniform(owner, specularParamsHandle_, material, owner_.GetSpecularParams(), cbData);
        UpdateUniform(owner, refractionParamsHandle_, material, owner_.GetRefractionParams(), cbData);
        UpdateUniform(owner, subsurfaceParamsHandle_, material, owner_.GetSubsurfaceParams(), cbData);
        UpdateUniform(owner, heightFogParamsHandle_, material, owner_.GetHeightFogParams(), cbData);
        UpdateUniform(owner, normalSamplingParamsHandle_, material, owner_.GetNormalSamplingParams(renderer), cbData);
        UpdateUniform(owner, shoreBehaviorParams0Handle_, material, owner_.GetShoreBehaviorParams0(), cbData);
        UpdateUniform(owner, shoreBehaviorParams1Handle_, material, owner_.GetShoreBehaviorParams1(), cbData);
        UpdateUniform(owner, shoreNormalMinWeightsHandle_, material, owner_.GetShoreNormalMinWeights(), cbData);
        UpdateUniform(owner, shoreFoamGeometryParamsHandle_, material, owner_.GetShoreFoamGeometryParams(), cbData);
        UpdateUniform(owner, shoreFoamPatternParamsHandle_, material, owner_.GetShoreFoamPatternParams(), cbData);
        UpdateUniform(owner, shoreFoamBreakupParamsHandle_, material, owner_.GetShoreFoamBreakupParams(), cbData);
        UpdateUniform(owner, shoreFoamWindParamsHandle_, material, owner_.GetShoreFoamWindParams(), cbData);
        UpdateUniform(owner, shoreFoamAlbedoParamsHandle_, material, owner_.GetShoreFoamAlbedoParams(), cbData);
        UpdateUniform(owner, shoreSlopeParamsHandle_, material, owner_.GetShoreSlopeParams(), cbData);
        UpdateUniform(owner, shoreSwashParamsHandle_, material, owner_.GetShoreSwashParams(), cbData);
        UpdateUniform(owner, shoreLegacyDampParamsHandle_, material, owner_.GetShoreLegacyDampParams(), cbData);
        UpdateUniform(owner, shoreLegacyFoamParamsHandle_, material, owner_.GetShoreLegacyFoamParams(), cbData);
        UpdateUniform(owner, shoreLegacyFoamParams2Handle_, material, owner_.GetShoreLegacyFoamParams2(), cbData);
        UpdateUniform(owner, shoreLegacyDissipationParamsHandle_, material, owner_.GetShoreLegacyDissipationParams(), cbData);
        UpdateUniform(owner, shoreSamplingParamsHandle_, material, owner_.GetShoreSamplingParams(), cbData);
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
    Material::CBFieldHandle prevModelHandle_{};
    Material::CBFieldHandle viewProjHandle_{};
    Material::CBFieldHandle viewProjNoJitterHandle_{};
    Material::CBFieldHandle prevViewProjNoJitterHandle_{};
    Material::CBFieldHandle invViewHandle_{};
    Material::CBFieldHandle invProjHandle_{};
    Material::CBFieldHandle shoreViewParamsHandle_{};
    Material::CBFieldHandle shoreSdfParamsHandle_{};
    Material::CBFieldHandle shoreDepthParamsHandle_{};
    Material::CBFieldHandle simulationParamsHandle_{};
    Material::CBFieldHandle viewerParamsHandle_{};
    Material::CBFieldHandle cascadeLengthScalesHandle_{};
    Material::CBFieldHandle inverseCascadeLengthScalesHandle_{};
    Material::CBFieldHandle clipMapParamsHandle_{};
    Material::CBFieldHandle prevClipMapParamsHandle_{};
    Material::CBFieldHandle clipMapViewerHandle_{};
    Material::CBFieldHandle prevClipMapViewerHandle_{};
    Material::CBFieldHandle foamParams0Handle_{};
    Material::CBFieldHandle foamParams1Handle_{};
    Material::CBFieldHandle foamCascadeWeightsHandle_{};
    Material::CBFieldHandle specularParamsHandle_{};
    Material::CBFieldHandle refractionParamsHandle_{};
    Material::CBFieldHandle subsurfaceParamsHandle_{};
    Material::CBFieldHandle heightFogParamsHandle_{};
    Material::CBFieldHandle normalSamplingParamsHandle_{};
    Material::CBFieldHandle shoreBehaviorParams0Handle_{};
    Material::CBFieldHandle shoreBehaviorParams1Handle_{};
    Material::CBFieldHandle shoreNormalMinWeightsHandle_{};
    Material::CBFieldHandle shoreFoamGeometryParamsHandle_{};
    Material::CBFieldHandle shoreFoamPatternParamsHandle_{};
    Material::CBFieldHandle shoreFoamBreakupParamsHandle_{};
    Material::CBFieldHandle shoreFoamWindParamsHandle_{};
    Material::CBFieldHandle shoreFoamAlbedoParamsHandle_{};
    Material::CBFieldHandle shoreSlopeParamsHandle_{};
    Material::CBFieldHandle shoreSwashParamsHandle_{};
    Material::CBFieldHandle shoreLegacyDampParamsHandle_{};
    Material::CBFieldHandle shoreLegacyFoamParamsHandle_{};
    Material::CBFieldHandle shoreLegacyFoamParams2Handle_{};
    Material::CBFieldHandle shoreLegacyDissipationParamsHandle_{};
    Material::CBFieldHandle shoreSamplingParamsHandle_{};
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

OceanRenderable::OceanRenderable(Camera* camera, Scene* scene, OceanSimulation* simulation)
    : RenderableObject("PosLevelUV", L"shaders/ocean_surface.hlsl")
    , camera_(camera)
    , scene_(scene)
    , simulation_(simulation)
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
    if (simulation_)
    {
        simulation_->Initialize(renderer, uploadCmdList, uploadKeepAlive);
        lengthScales_ = simulation_->GetLengthScales();
        invLengthScales_ = simulation_->GetInvLengthScales();
    }
    clipMapHasHistory_ = false;
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
    loadTexture(shoreFoamBreakupMaskTexture_, L"textures/ocean/ContactFoam.dds", Texture2D::Usage::LinearData);
    loadTexture(shoreFoamAlbedoTexture_, L"textures/ocean/ShoreFoamAlbedo.png", Texture2D::Usage::AlbedoSRGB);
    // 8x8 flipbook of 128px caustic frames (BC4, per-frame mips) — see tools/gen_caustics.py.
    // Consumed by lighting_cs.hlsl, not by the ocean surface shader.
    loadTexture(causticsTexture_, L"textures/ocean/caustics_flipbook.dds", Texture2D::Usage::LinearData);

    UpdateFoamTrailState();
}

void OceanRenderable::Tick(float deltaTime)
{
    elapsedTime_ += deltaTime;
    // W8: the debug wind freeze is a freeze of the SHARED clock, and this is the clock — the wind
    // derives its time from elapsedTime_ precisely so waves and sway stay phase-coherent. Holding one
    // and not the other would desync them, and would leave the water animating under an otherwise
    // frozen frame (measured: the ocean alone put the run-to-run noise at 15.5 % of pixels, as large
    // as the wind signal being measured). PINNED to the same number rather than merely paused, so a
    // frozen frame is reproducible across runs instead of holding wherever wall-clock left it.
    if (vfx::g_windFreeze) { elapsedTime_ = vfx::g_windFrozenTime; }
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

void OceanRenderable::PrepareCompute(RenderGraphPassContext& ctx)
{
    if (!simulation_) { return; }
    simulation_->PrepareUpdate(ctx);
}

void OceanRenderable::PrepareRender(RenderGraphPassContext& ctx)
{
    if (!simulation_) { return; }
    const D3D12_RESOURCE_STATES srvState =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    // Guarded exactly as RecordGraphics guards them. Registering a resource the body will not
    // transition advances the compile past a barrier nobody emits, which is fatal under compiled
    // barriers; the previous-displacement pair was already asymmetric (registered unconditionally,
    // transitioned only when non-null).
    if (ID3D12Resource* displacement = simulation_->GetDisplacementResource())
    {
        ctx.Use(displacement, srvState);
    }
    if (ID3D12Resource* prevDisplacement = simulation_->GetPreviousDisplacementResource())
    {
        ctx.Use(prevDisplacement, srvState);
    }
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

    // MUST match numDescriptors in OCEAN_SURFACE_RS. Pushing past the end is a silent buffer
    // overrun that hands the table a garbage descriptor — it showed up as the ocean sampling sand.
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 16> srvs{};
    size_t srvCount = 0;

    auto pushSrv = [&](D3D12_CPU_DESCRIPTOR_HANDLE srv)
    {
        assert(srvCount < srvs.size() && "ocean SRV table overrun - grow srvs AND OCEAN_SURFACE_RS");
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

    D3D12_CPU_DESCRIPTOR_HANDLE prevDisplacementSrv = {};
    if (simulation_->HasPreviousDisplacement())
    {
        prevDisplacementSrv = simulation_->GetPreviousDisplacementSRV();
    }
    if (prevDisplacementSrv.ptr == 0)
    {
        prevDisplacementSrv = displacementSrv;
    }
    pushSrv(prevDisplacementSrv);

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

    pushTexture(distantRoughnessTexture_);
    pushTexture(foamDetailTexture_);
    pushTexture(foamAlbedoTexture_);
    pushTexture(foamUnderwaterTexture_);
    pushTexture(foamTrailTexture_);
    pushTexture(shoreFoamBreakupMaskTexture_);
    pushTexture(shoreFoamAlbedoTexture_);

    D3D12_CPU_DESCRIPTOR_HANDLE depthSrv = deferred.depthCopySRV.ptr != 0 ? deferred.depthCopySRV : deferred.depthSRV;
    if (depthSrv.ptr == 0)
    {
        depthSrv = fallbackSrv;
    }
    pushSrv(depthSrv);

    D3D12_CPU_DESCRIPTOR_HANDLE shoreDepthSrv = fallbackSrv;
    if (simulation_ && simulation_->GetShoreDepthSrv().ptr != 0)
    {
        shoreDepthSrv = simulation_->GetShoreDepthSrv();
    }
    else if (depthSrv.ptr != 0)
    {
        shoreDepthSrv = depthSrv;
    }
    pushSrv(shoreDepthSrv.ptr != 0 ? shoreDepthSrv : fallbackSrv);

    // Shore SDF. Falls back to the near depth map only so the slot is never null; the shader's
    // own bounds check keeps a wrong reading out of the result.
    D3D12_CPU_DESCRIPTOR_HANDLE shoreSdfSrv = shoreDepthSrv;
    if (simulation_ && simulation_->GetShoreSdfSrv().ptr != 0)
    {
        shoreSdfSrv = simulation_->GetShoreSdfSrv();
    }
    pushSrv(shoreSdfSrv.ptr != 0 ? shoreSdfSrv : fallbackSrv);

    D3D12_CPU_DESCRIPTOR_HANDLE oceanReflectionSrv = deferred.oceanReflectionSRV.ptr != 0 ? deferred.oceanReflectionSRV : fallbackSrv;
    pushSrv(oceanReflectionSrv.ptr != 0 ? oceanReflectionSrv : fallbackSrv);

    auto tbl = renderer->StageSrvUavTable(srvs, srvCount);
    ctx.srvTable[0] = tbl.gpu;

    const auto samplers = std::array{
        *SamplerManager::LinearWrap(),
        *SamplerManager::LinearClamp(),
        *SamplerManager::PointClamp(),
        *SamplerManager::AnisoWrap(16) };
    ctx.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, samplers);

    const D3D12_RESOURCE_STATES srvState =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (ID3D12Resource* displacement = simulation_->GetDisplacementResource())
    {
        renderer->Transition(cl, displacement, srvState);
    }
    if (auto* prevDisplacement = simulation_->GetPreviousDisplacementResource())
    {
        renderer->Transition(cl, prevDisplacement, srvState);
    }

    RenderableObject::RecordGraphics(renderer, cl, ctx, camera, cbData);
}

void OceanRenderable::Render(Renderer* renderer, ID3D12GraphicsCommandList* cl,
    const Camera& camera, D3D12_GPU_VIRTUAL_ADDRESS viewCB)
{
    // The whole point of this override: the transparent batch is ~88% this one draw, so it gets
    // a name in every trace. It wraps Render and NOT RecordGraphics because RecordGraphics is
    // binds only — the draw itself is issued by Render's DrawGeometry, and scoping the binds
    // measured 1 us of the ~300 that actually matter.
    GPU_SCOPE(cl, ProfilerScopes::kOceanSurface);
    RenderableObject::Render(renderer, cl, camera, viewCB);
}

void OceanRenderable::ConfigureGraphicsPipeline(Renderer* renderer, Material::GraphicsDesc& desc) const
{
    RenderableObject::ConfigureGraphicsPipeline(renderer, desc);

#if WITH_EDITOR
    desc.numRT = 3;
#else
    desc.numRT = 2;
#endif
    if (renderer)
    {
        desc.rtvFormats[0] = renderer->GetSceneColorFormat();
        desc.rtvFormats[1] = renderer->GetGBufferVelocityFormat();
#if WITH_EDITOR
        desc.rtvFormats[2] = renderer->GetObjectIdFormat();
#endif
        desc.dsvFormat = renderer->GetDsvFormat();
    }
    if (ocean::g_shoreSinkCut)
    {
        desc.defines.emplace_back("OCEAN_SHORE_SINK", "1");
    }
    if (ocean::g_foamDebug)
    {
        desc.defines.emplace_back("OCEAN_FOAM_DEBUG", "1");
    }
    if (ocean::g_vsDepthProbe)
    {
        desc.defines.emplace_back("OCEAN_VS_DEPTH_PROBE", "1");
    }
    if (!ocean::g_shoreRunup)
    {
        desc.defines.emplace_back("OCEAN_SHORE_RUNUP", "0");
    }
    desc.depth.DepthEnable = TRUE;
    desc.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.raster.CullMode = D3D12_CULL_MODE_NONE;
    desc.blend.RenderTarget[1].BlendEnable = FALSE;
    desc.blend.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
#if WITH_EDITOR
    desc.blend.RenderTarget[2].BlendEnable = FALSE;
    desc.blend.RenderTarget[2].RenderTargetWriteMask = 0;
#endif
}

void OceanRenderable::EnsureSimulationResources(Renderer* renderer)
{
    if (simulation_)
    {
        simulation_->EnsureFrameResources(renderer);
    }
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

    SetMesh(std::make_shared<Mesh>()); // 5b: own the mesh explicitly (no base default)
    mesh_->CreateGPUFlexible(renderer->GetDevice(), uploadCmdList, uploadKeepAlive,
        combined.vertices.data(), static_cast<UINT>(combined.vertices.size()), sizeof(OceanVertex),
        combined.indices.data(), static_cast<UINT>(combined.indices.size()), DXGI_FORMAT_R32_UINT);
}

void OceanRenderable::UpdateClipLevels()
{
    const float patchLength = simulation_ ? simulation_->GetPatchLength() : 200.0f;
    const OceanRenderConfig& render = GetRenderConfig();

    const Math::float3 previousViewer = clipMapViewer_;
    const float previousScale = clipMapScale_;
    const float previousHalfSize = clipMapLevelHalfSize_;
    const float previousFade = cascadesFadeScale_;
    const bool hadHistory = clipMapHasHistory_;

    cascadesFadeScale_ = render.cascadeFadeScale;
    clipMapLevelHalfSize_ = static_cast<float>(ClipLevelHalfSize(meshVertexDensity_));
    clipMapViewer_ = Math::float3(viewerXZ_.x, viewerHeight_, viewerXZ_.y);
    const float absHeight = std::abs(clipMapViewer_.y);
    int meshExponent = 0;
    if (absHeight > Math::EPS)
    {
        const float denom = std::max(2.0f * render.minMeshScale, Math::EPS);
        const float ratio = absHeight / denom;
        if (ratio > Math::EPS)
        {
            meshExponent = static_cast<int>(std::floor(std::max(0.0f, std::log2(ratio) + 1.0f)));
        }
    }

    const float halfSize = std::max(1.0f, clipMapLevelHalfSize_);
    clipMapScale_ = (render.minMeshScale / halfSize) * std::pow(2.0f, static_cast<float>(meshExponent));
    clipMapScale_ = std::max(clipMapScale_, 1.0e-3f);

    if (!hadHistory)
    {
        prevClipMapViewer_ = clipMapViewer_;
        prevClipMapScale_ = clipMapScale_;
        prevClipMapLevelHalfSize_ = clipMapLevelHalfSize_;
        prevCascadesFadeScale_ = cascadesFadeScale_;
        clipMapHasHistory_ = true;
    }
    else
    {
        prevClipMapViewer_ = previousViewer;
        prevClipMapScale_ = previousScale;
        prevClipMapLevelHalfSize_ = previousHalfSize;
        prevCascadesFadeScale_ = previousFade;
    }

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

Math::float4 OceanRenderable::GetPrevClipMapParams() const
{
    return Math::float4(prevClipMapScale_, prevClipMapLevelHalfSize_, static_cast<float>(meshVertexDensity_), prevCascadesFadeScale_);
}

Math::float4 OceanRenderable::GetPrevClipMapViewer() const
{
    return Math::float4(prevClipMapViewer_.x, prevClipMapViewer_.y, prevClipMapViewer_.z, 0.0f);
}

Math::float4 OceanRenderable::GetFoamParams0() const
{
    FoamParams foam = simulation_ ? simulation_->GetFoamParams() : FoamParams::GetDefault();
    return Math::float4(foam.coverage, foam.density, foam.sharpness, foam.persistence);
}

Math::float4 OceanRenderable::GetFoamParams1() const
{
    FoamParams foam = simulation_ ? simulation_->GetFoamParams() : FoamParams::GetDefault();
    return Math::float4(
        foam.trail,
        foam.trailTextureStrength,
        foam.underwater,
        GetRenderConfig().foamNormalStrength);
}

Math::float4 OceanRenderable::GetFoamCascadeWeights() const
{
    FoamParams foam = simulation_ ? simulation_->GetFoamParams() : FoamParams::GetDefault();
    return foam.cascadesWeights;
}

Math::float4 OceanRenderable::GetSpecularParams() const
{
    const OceanRenderConfig& render = GetRenderConfig();
    return Math::float4(
        render.specularStrength,
        render.roughnessScale,
        render.roughnessDistance,
        render.horizonFogStrength);
}

Math::float4 OceanRenderable::GetRefractionParams() const
{
    const OceanRenderConfig& render = GetRenderConfig();
    return Math::float4(
        render.surfaceRefractionStrength,
        render.underwaterRefractionStrength,
        render.absorptionDepthScale,
        render.fogDensity);
}

Math::float4 OceanRenderable::GetSubsurfaceParams() const
{
    const OceanRenderConfig& render = GetRenderConfig();
    return Math::float4(
        render.sunScatterStrength,
        render.skyScatterStrength,
        render.scatterSpread,
        render.viewAlignmentStrength);
}

Math::float4 OceanRenderable::GetHeightFogParams() const
{
    const OceanRenderConfig& render = GetRenderConfig();
    return Math::float4(
        render.sssHeightBias,
        render.sssFadeDistance,
        render.horizonFogDistanceScale,
        render.reflectionNormalStrength);
}

Math::float4 OceanRenderable::GetNormalSamplingParams(const Renderer* renderer) const
{
    const OceanRenderConfig& render = GetRenderConfig();
    const float macroMipBias = renderer && renderer->IsDlssActive()
        ? render.macroNormalMipBiasDlss
        : render.macroNormalMipBiasNative;
    return Math::float4(render.detailNormalMipBias, macroMipBias, 0.0f, 0.0f);
}

Math::float4 OceanRenderable::GetShoreBehaviorParams0() const
{
    const OceanRenderConfig& render = GetRenderConfig();
    return Math::float4(
        render.shoreVerticalFadeDepth,
        render.shoreHorizontalMin,
        render.shoreHorizontalFadeDepth,
        render.shoreNormalFadeDepth);
}

Math::float4 OceanRenderable::GetShoreBehaviorParams1() const
{
    const OceanRenderConfig& render = GetRenderConfig();
    return Math::float4(
        render.shoreRunupDepth,
        render.shoreRunupStrength,
        render.shoreRunupMaxWave,
        render.shoreBottomClearance);
}

Math::float4 OceanRenderable::GetShoreNormalMinWeights() const
{
    return GetRenderConfig().shoreNormalMinWeights;
}

Math::float4 OceanRenderable::GetShoreFoamGeometryParams() const
{
    const OceanRenderConfig& render = GetRenderConfig();
    return Math::float4(
        render.shoreContactFoamMainWidth,
        render.shoreContactFoamBreakupLength,
        render.shoreGeometryEdgeRefractionFadeDepth,
        render.shoreContactFoamOpacity);
}

Math::float4 OceanRenderable::GetShoreFoamPatternParams() const
{
    const OceanRenderConfig& render = GetRenderConfig();
    return Math::float4(
        render.shoreContactFoamPatternScale,
        render.shoreContactFoamPatternDensity,
        render.shoreContactFoamPatternScrollSpeed,
        render.shoreContactFoamDepthWarpStrength);
}

Math::float4 OceanRenderable::GetShoreFoamBreakupParams() const
{
    return Math::float4(
        GetRenderConfig().shoreContactFoamBreakupLengthVariation,
        GetRenderConfig().shoreContactFoamBreakupVariationScale,
        GetRenderConfig().shoreContactFoamNormalStrength,
        // Diagnostic view id; ignored unless the shader was built with OCEAN_FOAM_DEBUG.
        static_cast<float>(ocean::g_foamDebugView));
}

Math::float4 OceanRenderable::GetShoreFoamWindParams() const
{
    const OceanRenderConfig& render = GetRenderConfig();
    const float windForce = simulation_ ? simulation_->GetWindForce01() : 1.0f;
    return Math::float4(
        windForce,
        render.shoreContactFoamCalmAmount,
        render.shoreContactFoamFullWindForce,
        0.0f);
}

Math::float4 OceanRenderable::GetShoreFoamAlbedoParams() const
{
    const OceanRenderConfig& render = GetRenderConfig();
    return Math::float4(
        render.shoreContactFoamAlbedoScale,
        render.shoreContactFoamAlbedoScrollSpeed,
        render.shoreContactFoamDepthWarpRange,
        render.shoreContactFoamDepthWarpScale);
}

Math::float4 OceanRenderable::GetShoreSlopeParams() const
{
    constexpr float kDegreesToRadians = 0.01745329251994329577f;
    const OceanRenderConfig& render = GetRenderConfig();
    const float startDegrees = std::clamp(render.shoreRunupSlopeStartDegrees, 0.0f, 89.0f);
    const float endDegrees = std::clamp(
        std::max(render.shoreRunupSlopeEndDegrees, startDegrees + 0.1f),
        0.0f,
        89.0f);
    return Math::float4(
        std::tan(startDegrees * kDegreesToRadians),
        std::tan(endDegrees * kDegreesToRadians),
        render.shoreEdgeSoftDepth,
        ocean::g_geometryFadeOverride >= 0.0f ? ocean::g_geometryFadeOverride
                                              : render.shoreGeometryFadeDistance);
}

Math::float4 OceanRenderable::GetShoreSwashParams() const
{
    const OceanRenderConfig& render = GetRenderConfig();
    // z: the reference wave height the sea WOULD have at "full at wind" force. The shore run-up
    // uses it to freeze its wave drive past that wind: every other nearshore term saturates at
    // full via ContactFoamWindAmount, but the raw anchored wave keeps growing to wind 1 and was
    // dragging the push (and everything advected by it) into absurd surf. Read-only preset
    // evaluation — the live FFT is untouched.
    const float fullWind = std::clamp(render.shoreContactFoamFullWindForce, 0.0f, 1.0f);
    const float fullWindWaveHeight = simulation_
        ? simulation_->EvaluateInputsAt(fullWind).referenceWaveHeight
        : 0.0f;
    return Math::float4(
        std::clamp(render.shoreSwashAmplitude, 0.0f, 1.0f),
        std::clamp(render.shoreRunupSlopeSmoothing, 0.5f, 8.0f),
        fullWindWaveHeight,
        0.0f);
}

Math::float4 OceanRenderable::GetShoreLegacyDampParams() const
{
    const OceanRenderConfig& render = GetRenderConfig();
    // w: the shoreline normal fade depth — the SAME authored field the modern surface uses
    // (shoreBehaviorParams0.w there), so the two variants share the setting.
    return Math::float4(
        std::clamp(render.shoreLegacyVerticalDampStrength, 0.0f, 1.0f),
        std::clamp(render.shoreLegacyXzDampStrength, 0.0f, 1.0f),
        std::clamp(render.shoreLegacyDampFadeDepth, 0.01f, 50.0f),
        std::max(render.shoreNormalFadeDepth, 0.01f));
}

Math::float4 OceanRenderable::GetShoreLegacyFoamParams() const
{
    const OceanRenderConfig& render = GetRenderConfig();
    return Math::float4(
        std::clamp(render.shoreLegacyTailTextureScale, 0.001f, 10.0f),
        std::clamp(render.shoreLegacyTailDepth, 0.0f, 5.0f),
        std::clamp(render.shoreLegacyTailScrollSpeed, 0.0f, 10.0f),
        std::clamp(render.shoreLegacyTailDetile, 0.0f, 1.0f));
}

Math::float4 OceanRenderable::GetShoreLegacyFoamParams2() const
{
    const OceanRenderConfig& render = GetRenderConfig();
    return Math::float4(
        std::clamp(render.shoreLegacyTailEdgeFade, 0.001f, 2.0f),
        std::clamp(render.shoreLegacyWindThinning, 0.0f, 1.0f),
        std::clamp(render.shoreLegacyTailContrast, 0.0f, 4.0f),
        std::clamp(render.shoreLegacyTailBias, -1.0f, 1.0f));
}

Math::float4 OceanRenderable::GetShoreLegacyDissipationParams() const
{
    const OceanRenderConfig& render = GetRenderConfig();
    return Math::float4(
        std::clamp(render.shoreLegacyDissipationScale, 1.0f, 200.0f),
        std::clamp(render.shoreLegacyDissipationSpeed, 0.0f, 5.0f),
        std::clamp(render.shoreLegacyDissipationAmount, 0.0f, 1.0f),
        std::clamp(render.shoreLegacyDissipationContrast, 0.1f, 8.0f));
}

Math::float4 OceanRenderable::GetShoreSamplingParams() const
{
    const float width = simulation_
        ? static_cast<float>(std::max(simulation_->GetShoreDepthWidth(), 1u))
        : 1.0f;
    const float height = simulation_
        ? static_cast<float>(std::max(simulation_->GetShoreDepthHeight(), 1u))
        : 1.0f;
    const float extent = simulation_ ? simulation_->GetShoreDepthHalfExtent() * 2.0f : 500.0f;
    return Math::float4(1.0f / width, 1.0f / height, extent / width, extent / height);
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
    return GetRenderConfig().deepScatterColor;
}

Math::float4 OceanRenderable::GetSssColor() const
{
    return GetRenderConfig().sssColor;
}

Math::float4 OceanRenderable::GetDiffuseColor() const
{
    return GetRenderConfig().diffuseColor;
}

Math::float4 OceanRenderable::GetAbsorptionGradientParams() const
{
    const OceanRenderConfig& render = GetRenderConfig();
    const float colorCount = static_cast<float>(std::min<size_t>(render.absorptionColors.size(), 8u));
    return Math::float4(colorCount, render.absorptionGradientType, 0.0f, 0.0f);
}

Math::float4 OceanRenderable::GetAbsorptionColor(uint32_t index) const
{
    const std::vector<Math::float4>& colors = GetRenderConfig().absorptionColors;
    if (index >= colors.size() || index >= 8u)
    {
        return Math::float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    return colors[index];
}

uint32_t OceanRenderable::GetAbsorptionColorCount() const
{
    return static_cast<uint32_t>(std::min<size_t>(GetRenderConfig().absorptionColors.size(), 8u));
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
    const OceanRenderConfig& render = GetRenderConfig();
    return Math::float4(
        render.windSpeed,
        render.wavesScale,
        render.windAlignment,
        render.windUvWarpStrength);
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
    const float blendValue = Math::Clamp(foamTrailBlendValue_, 0.0f, 1.0f);
    const OceanRenderConfig& render = GetRenderConfig();
    // y: the LEGACY shader's contact-foam strength gate — the June-22 surface both scales and
    // `if (y > 0)`-gates its contact foam by this (its C++ of the day hardcoded 0.1; now an
    // authored knob). The modern surface does not read y at all, so it is zeroed there to keep
    // the intent visible.
    return Math::float4(
        blendValue,
        ocean::g_shoreRunup
            ? 0.0f
            : Math::Clamp(render.shoreLegacyContactFoamStrength, 0.0f, 1.0f),
        render.underwaterFoamParallax,
        0.0f);
}

Math::float4 OceanRenderable::GetFoamTint() const
{
    return GetRenderConfig().foamTint;
}

Math::float4 OceanRenderable::GetDepthTextureSize(const Renderer* renderer) const
{
    const float width = renderer ? static_cast<float>(std::max(renderer->GetRenderWidth(), 1u)) : 1.0f;
    const float height = renderer ? static_cast<float>(std::max(renderer->GetRenderHeight(), 1u)) : 1.0f;
    return Math::float4(1.0f / width, 1.0f / height, width, height);
}

Math::float2 OceanRenderable::GetDepthParams() const
{
    const Camera* camera = scene_ ? &scene_->CameraRef() : camera_;
    const float zNear = camera ? camera->GetZNear() : 0.01f;
    const float zFar = camera ? camera->GetZFar() : 10000.0f;
    return { zNear / (zNear - zFar), (zNear * zFar) / (zFar - zNear) };
}

Math::float4 OceanRenderable::GetShoreViewParams() const
{
    Math::float2 center = Math::float2(0.0f, 0.0f);
    float height = 0.0f;
    float kInvExtent = 1.0f / 500.0f;
    if (simulation_)
    {
        center = simulation_->GetShoreViewCenter();
        height = simulation_->GetShoreViewHeight();
        kInvExtent = 1.0f / (simulation_->GetShoreDepthHalfExtent() * 2.0f);
    }
     
    return { center.x, center.y, height, kInvExtent };
}

Math::float4 OceanRenderable::GetShoreSdfParams() const
{
    Math::float2 center = Math::float2(0.0f, 0.0f);
    float extent = 2000.0f;
    float resolution = 1024.0f;
    if (simulation_)
    {
        center = simulation_->GetShoreSdfCenter();
        extent = std::max(simulation_->GetShoreSdfHalfExtent() * 2.0f, 1.0f);
    }

    return { center.x, center.y, 1.0f / extent, extent / resolution };
}

Math::float4 OceanRenderable::GetShoreDepthParams() const
{
    if (simulation_)
    {
        const float2 nearFar = simulation_->GetShoreDepthRange();
        return { nearFar.x, nearFar.y, 0.0f, 0.0f };
    }

    return { 0.1f, 50.0f, 0.0f, 0.0f };
}

const OceanRenderConfig& OceanRenderable::GetRenderConfig() const
{
    static const OceanRenderConfig defaultRenderConfig;
    return simulation_ ? simulation_->GetRenderConfig() : defaultRenderConfig;
}

void OceanRenderable::SetGridVertexDensity(uint32_t density)
{
    const uint32_t clamped = std::max<uint32_t>(1u, density);
    if (meshVertexDensity_ == clamped)
    {
        return;
    }
    meshVertexDensity_ = clamped;
    clipMapHasHistory_ = false;
    UpdateClipLevels();
}
