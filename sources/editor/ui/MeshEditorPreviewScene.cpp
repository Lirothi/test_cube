#include "editor/ui/MeshEditorPreviewScene.h"
#if WITH_EDITOR

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>

#include <d3d12.h>
#include <wrl/client.h>

#include "editor/assets/EditorPreviewRenderer.h"
#include "materials/MaterialData.h"
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
            a.ambient == b.ambient;
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
    std::array<bool, render::kFrameCount> cameraValid{};
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
        const MeshEditorPreviewLight& light)
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
        Microsoft::WRL::ComPtr<ID3D12Resource> target =
            previewRenderer.RecordPreview(renderer,
                commands->CommandList(),
                *mesh,
                materials,
                renderWidth,
                renderHeight,
                orbit,
                previewLight,
                frameIndex,
                targets[frameIndex].Get());
        if (!target || !commands->Submit(&renderer))
        {
            error = "Could not submit the mesh preview render.";
            return false;
        }

        targets[frameIndex] = std::move(target);
        frameCommands[frameIndex] = std::move(commands);
        renderedCameras[frameIndex] = camera;
        renderedLights[frameIndex] = light;
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
    const MeshEditorPreviewLight& light)
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
    if (!impl_->targets[frameIndex] || !impl_->cameraValid[frameIndex] ||
        targetSizeChanged || !SameCamera(impl_->renderedCameras[frameIndex], camera) ||
        !SameLight(impl_->renderedLights[frameIndex], light))
    {
        if (!impl_->RenderFrame(renderer,
                frameIndex,
                renderWidth,
                renderHeight,
                camera,
                light))
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
    return view;
}

#endif // WITH_EDITOR
