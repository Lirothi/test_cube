#include "editor/ui/MeshEditorPreviewScene.h"
#if WITH_EDITOR

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <sstream>

#include <d3d12.h>
#include <wrl/client.h>

#include "editor/assets/EditorPreviewRenderer.h"
#include "materials/MaterialData.h"
#include "materials/TextureCube.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/meshes/Mesh.h"

namespace
{
    constexpr std::uint32_t kMaxRenderSize = 1024;
    constexpr DXGI_FORMAT kPreviewFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

    std::string BuildSourceSignature(const std::string& assetKey,
        const std::string& geometry,
        const std::vector<std::string>& materialSlots,
        const std::vector<std::uint32_t>& recomputeNormalSlots,
        std::uint64_t revision)
    {
        std::ostringstream signature;
        signature << assetKey << '\n' << geometry << '\n' << revision << '\n';
        for (const std::string& material : materialSlots)
        {
            signature << material.size() << ':' << material << ';';
        }
        signature << '\n';
        for (const std::uint32_t slot : recomputeNormalSlots)
        {
            signature << slot << ',';
        }
        return signature.str();
    }

    bool SameCamera(const MeshEditorPreviewCamera& a,
        const MeshEditorPreviewCamera& b)
    {
        return a.yaw == b.yaw && a.pitch == b.pitch && a.zoom == b.zoom &&
            a.panX == b.panX && a.panY == b.panY;
    }

    // The absent override is identity tiling, so a mesh with no texOffsScale never counts as dirty.
bool SameTexOffsScale(const Math::float4& cached, const Math::float4* current)
{
    const Math::float4 v = current ? *current : Math::float4(0.0f, 0.0f, 1.0f, 1.0f);
    return cached.x == v.x && cached.y == v.y && cached.z == v.z && cached.w == v.w;
}

bool SameLight(const MeshEditorPreviewLight& a,
        const MeshEditorPreviewLight& b)
    {
        return a.direction.x == b.direction.x &&
            a.direction.y == b.direction.y &&
            a.direction.z == b.direction.z &&
            a.color.x == b.color.x &&
            a.color.y == b.color.y &&
            a.color.z == b.color.z &&
            a.exposure == b.exposure &&
            a.ambient == b.ambient &&
            a.showPosition == b.showPosition &&
            a.positionDistance == b.positionDistance;
    }

    bool SameHandle(D3D12_CPU_DESCRIPTOR_HANDLE a, D3D12_CPU_DESCRIPTOR_HANDLE b)
    {
        return a.ptr == b.ptr;
    }

    // Field by field: the struct has padding, so comparing its bytes would compare garbage.
    bool SamePhysical(const std::optional<EditorPreviewRenderer::PhysicalLighting>& cached,
        const EditorPreviewRenderer::PhysicalLighting* current)
    {
        if (!cached || !current)
        {
            return !cached && !current;
        }
        const EditorPreviewRenderer::PhysicalLighting& a = *cached;
        const EditorPreviewRenderer::PhysicalLighting& b = *current;
        return SameHandle(a.sky, b.sky) && a.skyMips == b.skyMips &&
            SameHandle(a.specular, b.specular) && a.specularMips == b.specularMips &&
            SameHandle(a.irradiance, b.irradiance) && SameHandle(a.brdfLut, b.brdfLut) &&
            a.skyIntensity == b.skyIntensity && a.skyFill == b.skyFill &&
            a.sunIlluminance.x == b.sunIlluminance.x &&
            a.sunIlluminance.y == b.sunIlluminance.y &&
            a.sunIlluminance.z == b.sunIlluminance.z &&
            a.sunHalfApex == b.sunHalfApex && a.flatAmbient == b.flatAmbient &&
            a.lightExposure == b.lightExposure &&
            a.groundAlbedo.x == b.groundAlbedo.x &&
            a.groundAlbedo.y == b.groundAlbedo.y &&
            a.groundAlbedo.z == b.groundAlbedo.z &&
            a.exposure == b.exposure && a.toneCurve == b.toneCurve &&
            a.gradeSaturation == b.gradeSaturation && a.gradeContrast == b.gradeContrast &&
            a.gradeGamma == b.gradeGamma && a.gradeGain == b.gradeGain &&
            a.gradeOffset == b.gradeOffset &&
            a.agxSlope == b.agxSlope && a.agxPower == b.agxPower &&
            a.agxSaturation == b.agxSaturation &&
            a.filmSlope == b.filmSlope && a.filmToe == b.filmToe &&
            a.filmShoulder == b.filmShoulder && a.filmBlackClip == b.filmBlackClip &&
            a.filmWhiteClip == b.filmWhiteClip;
    }
}

struct MeshEditorPreviewScene::Impl
{
    EditorPreviewRenderer previewRenderer;
    std::shared_ptr<Mesh> mesh;
    std::vector<std::shared_ptr<MaterialData>> materials;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, render::kFrameCount> targets;
    std::array<std::unique_ptr<UploadBatch>, render::kFrameCount> frameCommands;
    std::array<MeshEditorPreviewCamera, render::kFrameCount> renderedCameras{};
    std::array<MeshEditorPreviewLight, render::kFrameCount> renderedLights{};
    std::array<EditorPreviewMode, render::kFrameCount> renderedModes{};
    std::array<std::uint32_t, render::kFrameCount> renderedLods{};
    // The preview only re-renders a frame when something it draws changed. The mesh-asset tiling is
    // one of those things, so it has to take part in the check or dragging Tex Offset/Scale would
    // leave the cached image on screen.
    std::array<Math::float4, render::kFrameCount> renderedTexOffsScale{};
    std::array<int, render::kFrameCount> renderedHighlights{ -1, -1, -1 };
    std::array<int, render::kFrameCount> renderedHighlightOrdinals{ -1, -1, -1 };
    std::array<ID3D12Resource*, render::kFrameCount> renderedEnvironments{};
    std::array<float, render::kFrameCount> renderedEnvironmentExposures{};
    std::array<std::optional<EditorPreviewRenderer::PhysicalLighting>, render::kFrameCount>
        renderedPhysical{};
    std::array<bool, render::kFrameCount> cameraValid{};
    std::array<Math::mat4, render::kFrameCount> renderedViewProj{};
    std::string sourceSignature;
    std::string error;
    bool loaded = false;

    bool Load(Renderer& renderer,
        const std::string& geometry,
        const std::vector<std::string>& materialSlots,
        const std::vector<std::uint32_t>& recomputeNormalSlots)
    {
        loaded = false;
        error.clear();
        if (geometry.empty())
        {
            error = "Mesh asset has no geometry.";
            return false;
        }
        if (!previewRenderer.EnsureInitialized(renderer.GetDevice(), kMaxRenderSize))
        {
            error = "Could not initialize the mesh preview renderer.";
            return false;
        }

        previewRenderer.Meshes().Clear();
        previewRenderer.ReloadPresets();

        UploadBatch uploads;
        if (!uploads.Begin(&renderer))
        {
            error = "Could not begin the mesh preview upload.";
            return false;
        }

        MeshLoadOptions options;
        options.wantCW = false;
        options.recomputeNormalSlots = recomputeNormalSlots;
        mesh = previewRenderer.Meshes().Load(geometry,
            &renderer,
            uploads.CommandList(),
            uploads.KeepAlive(),
            options);
        if (!mesh)
        {
            error = "Referenced mesh geometry could not be loaded.";
            return false;
        }

        // The directional light has no physical position. Its optional editor marker reuses the
        // standard material-preview sphere and places it along the inverse light-ray direction.
        previewRenderer.EnsureSphere(renderer, uploads);

        std::size_t materialSlotCount = 1;
        for (const Mesh::Submesh& submesh : mesh->GetSubmeshes())
        {
            materialSlotCount = std::max(materialSlotCount,
                static_cast<std::size_t>(submesh.materialSlot) + 1);
        }

        materials.clear();
        materials.reserve(materialSlotCount);
        for (std::size_t slot = 0; slot < materialSlotCount; ++slot)
        {
            const std::string materialName = slot < materialSlots.size()
                ? materialSlots[slot]
                : "auto";
            if (materialName.empty() || materialName == "auto")
            {
                materials.push_back(previewRenderer.Materials().GetOrCreateFromGltf(
                    &renderer,
                    uploads.CommandList(),
                    uploads.KeepAlive(),
                    geometry,
                    static_cast<int>(slot)));
            }
            else
            {
                materials.push_back(previewRenderer.Materials().GetOrCreate(
                    &renderer,
                    uploads.CommandList(),
                    uploads.KeepAlive(),
                    materialName));
            }
        }

        uploads.SubmitAndWait(&renderer);
        cameraValid.fill(false);
        loaded = true;
        return true;
    }

    bool RenderFrame(Renderer& renderer,
        std::uint32_t frameIndex,
        std::uint32_t renderWidth,
        std::uint32_t renderHeight,
        const MeshEditorPreviewCamera& camera,
        const MeshEditorPreviewLight& light,
        EditorPreviewMode mode,
        std::uint32_t lod,
        const Math::float4* texOffsScaleOverride,
        int highlightMaterialSlot,
        int highlightSubmeshOrdinal,
        const TextureCube* environment,
        float environmentExposure,
        const EditorPreviewRenderer::PhysicalLighting* physical)
    {
        if (!loaded || !mesh || frameIndex >= render::kFrameCount)
        {
            return false;
        }

        // BeginFrame has already waited for this swapchain slot. Replacing its
        // allocator and rewriting its private preview resources is therefore safe.
        frameCommands[frameIndex].reset();
        if (targets[frameIndex])
        {
            const D3D12_RESOURCE_DESC desc = targets[frameIndex]->GetDesc();
            if (desc.Width != renderWidth || desc.Height != renderHeight)
            {
                renderer.ReleaseImGuiTextureDescriptors(targets[frameIndex].Get());
                targets[frameIndex].Reset();
                cameraValid[frameIndex] = false;
            }
        }
        std::unique_ptr<UploadBatch> commands = std::make_unique<UploadBatch>();
        if (!commands->Begin(&renderer))
        {
            error = "Could not record the mesh preview.";
            return false;
        }

        EditorPreviewRenderer::OrbitCamera orbit;
        orbit.yaw = camera.yaw;
        orbit.pitch = camera.pitch;
        orbit.zoom = camera.zoom;
        orbit.panX = camera.panX;
        orbit.panY = camera.panY;
        EditorPreviewRenderer::PreviewLight previewLight;
        previewLight.direction = light.direction;
        previewLight.color = light.color;
        previewLight.exposure = light.exposure;
        previewLight.ambient = light.ambient;
        previewLight.showPosition = light.showPosition;
        previewLight.positionDistance = light.positionDistance;
        Microsoft::WRL::ComPtr<ID3D12Resource> target =
            previewRenderer.RecordPreview(renderer,
                commands->CommandList(),
                *mesh,
                materials,
                renderWidth,
                renderHeight,
                orbit,
                previewLight,
                mode,
                lod,
                frameIndex,
                targets[frameIndex].Get(),
                texOffsScaleOverride,
                highlightMaterialSlot,
                highlightSubmeshOrdinal,
                environment,
                environmentExposure,
                physical);
        if (!target || !commands->Submit(&renderer))
        {
            error = "Could not submit the mesh preview render.";
            return false;
        }
        renderedViewProj[frameIndex] = previewRenderer.LastViewProj();

        targets[frameIndex] = std::move(target);
        frameCommands[frameIndex] = std::move(commands);
        renderedCameras[frameIndex] = camera;
        renderedLights[frameIndex] = light;
        renderedModes[frameIndex] = mode;
        renderedLods[frameIndex] = lod;
        renderedTexOffsScale[frameIndex] = texOffsScaleOverride
            ? *texOffsScaleOverride : Math::float4(0.0f, 0.0f, 1.0f, 1.0f);
        renderedHighlights[frameIndex] = highlightMaterialSlot;
        renderedHighlightOrdinals[frameIndex] = highlightSubmeshOrdinal;
        renderedEnvironments[frameIndex] = environment ? environment->GetResource() : nullptr;
        renderedEnvironmentExposures[frameIndex] = environmentExposure;
        renderedPhysical[frameIndex] = physical
            ? std::optional<EditorPreviewRenderer::PhysicalLighting>(*physical)
            : std::nullopt;
        cameraValid[frameIndex] = true;
        error.clear();
        return true;
    }
};

MeshEditorPreviewScene::MeshEditorPreviewScene()
    : impl_(std::make_unique<Impl>())
{
}

MeshEditorPreviewScene::~MeshEditorPreviewScene() = default;

void MeshEditorPreviewScene::Reset(Renderer& renderer)
{
    bool hasGpuResources = impl_->mesh != nullptr;
    for (const auto& target : impl_->targets)
    {
        hasGpuResources = hasGpuResources || target != nullptr;
    }
    if (hasGpuResources)
    {
        renderer.WaitForPreviousFrame();
    }

    for (std::size_t frame = 0; frame < impl_->targets.size(); ++frame)
    {
        impl_->frameCommands[frame].reset();
        if (impl_->targets[frame])
        {
            renderer.ReleaseImGuiTextureDescriptors(impl_->targets[frame].Get());
            impl_->targets[frame].Reset();
        }
    }
    impl_->materials.clear();
    impl_->mesh.reset();
    impl_->previewRenderer.Meshes().Clear();
    impl_->previewRenderer.Materials().ClearAll();
    impl_->cameraValid.fill(false);
    impl_->renderedEnvironments.fill(nullptr);
    impl_->renderedEnvironmentExposures.fill(0.0f);
    impl_->renderedPhysical.fill(std::nullopt);
    impl_->sourceSignature.clear();
    impl_->error.clear();
    impl_->loaded = false;
}

MeshEditorPreviewScene::View MeshEditorPreviewScene::Update(Renderer& renderer,
    const std::string& assetKey,
    const std::string& geometry,
    const std::vector<std::string>& materialSlots,
    const std::vector<std::uint32_t>& recomputeNormalSlots,
    std::uint64_t assetRegistryRevision,
    std::uint32_t renderWidth,
    std::uint32_t renderHeight,
    const MeshEditorPreviewCamera& camera,
    const MeshEditorPreviewLight& light,
    EditorPreviewMode mode,
    std::uint32_t lod,
    const Math::float4* texOffsScaleOverride,
    int highlightMaterialSlot,
    int highlightSubmeshOrdinal,
    const TextureCube* environment,
    float environmentExposure,
    const EditorPreviewRenderer::PhysicalLighting* physical)
{
    View view;
    const std::string signature = BuildSourceSignature(assetKey,
        geometry,
        materialSlots,
        recomputeNormalSlots,
        assetRegistryRevision);
    if (signature != impl_->sourceSignature)
    {
        Reset(renderer);
        impl_->sourceSignature = signature;
        if (!impl_->Load(renderer, geometry, materialSlots, recomputeNormalSlots))
        {
            view.state = State::Failed;
            view.error = impl_->error.c_str();
            return view;
        }
    }

    if (!impl_->loaded)
    {
        view.state = State::Failed;
        view.error = impl_->error.c_str();
        return view;
    }

    view.lodCount = std::max(1u, impl_->mesh->GetLodCount());
    lod = std::min(lod, view.lodCount - 1u);
    view.indexCount = impl_->mesh->GetLodDrawInfo(lod).indexCount;
    view.triangleCount = view.indexCount / 3u;

    const std::uint32_t frameIndex = renderer.GetCurrentFrameIndex();
    if (frameIndex >= render::kFrameCount)
    {
        view.state = State::Failed;
        view.error = "Invalid renderer frame index.";
        return view;
    }

    renderWidth = std::clamp(renderWidth, 1u, kMaxRenderSize);
    renderHeight = std::clamp(renderHeight, 1u, kMaxRenderSize);
    const bool targetSizeChanged = impl_->targets[frameIndex] &&
        (impl_->targets[frameIndex]->GetDesc().Width != renderWidth ||
         impl_->targets[frameIndex]->GetDesc().Height != renderHeight);
    ID3D12Resource* const environmentResource = environment
        ? environment->GetResource()
        : nullptr;
    if (!impl_->targets[frameIndex] || !impl_->cameraValid[frameIndex] ||
        targetSizeChanged || !SameCamera(impl_->renderedCameras[frameIndex], camera) ||
        !SameLight(impl_->renderedLights[frameIndex], light) ||
        impl_->renderedModes[frameIndex] != mode ||
        impl_->renderedLods[frameIndex] != lod ||
        !SameTexOffsScale(impl_->renderedTexOffsScale[frameIndex], texOffsScaleOverride) ||
        impl_->renderedHighlights[frameIndex] != highlightMaterialSlot ||
        impl_->renderedHighlightOrdinals[frameIndex] != highlightSubmeshOrdinal ||
        impl_->renderedEnvironments[frameIndex] != environmentResource ||
        impl_->renderedEnvironmentExposures[frameIndex] != environmentExposure ||
        !SamePhysical(impl_->renderedPhysical[frameIndex], physical))
    {
        if (!impl_->RenderFrame(renderer,
                frameIndex,
                renderWidth,
                renderHeight,
                camera,
                light,
                mode,
                lod,
                texOffsScaleOverride,
                highlightMaterialSlot,
                highlightSubmeshOrdinal,
                environment,
                environmentExposure,
                physical))
        {
            view.state = State::Failed;
            view.error = impl_->error.c_str();
            return view;
        }
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = kPreviewFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    renderer.MarkImGuiTextureShaderReadable(impl_->targets[frameIndex].Get());
    view.texture = renderer.CreateImGuiTextureId(impl_->targets[frameIndex].Get(), srv);
    view.state = view.texture != ImTextureID_Invalid ? State::Ready : State::Loading;
    view.viewProj = impl_->renderedViewProj[frameIndex];
    return view;
}

#endif // WITH_EDITOR
