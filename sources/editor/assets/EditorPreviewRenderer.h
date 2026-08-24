#pragma once
#if WITH_EDITOR

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

#include "core/math/Math.h"
#include "editor/assets/EditorPreviewMode.h"
#include "rendering/meshes/MeshManager.h"
#include "materials/MaterialDataManager.h"
#include "rendering/core/RenderConstants.h"

class Mesh;
class MaterialData;
class Renderer;
class TextureCube;
class UploadBatch;

// Self-contained offscreen renderer for Content Browser thumbnails and editor
// mini-scenes. It owns a tiny forward pipeline, per-frame render resources, and
// private mesh/material caches, so it never touches the edited scene or its lights.
//
// Lifetime / threading: thumbnail draws are recorded on the editor thread by
// AssetThumbnailCache, which polls a dedicated fence instead of waiting. Asset
// loads use a caller-provided UploadBatch; each thumbnail render records into a
// caller-provided command list. Generated color targets are handed to the cache, which owns
// their lifetime; this class owns only the shared pipeline objects.
class EditorPreviewRenderer
{
public:
    struct OrbitCamera
    {
        float yaw = 0.674741f;
        float pitch = 0.500180f;
        float zoom = 1.0f;
        float panX = 0.0f;
        float panY = 0.0f;
    };

    struct PreviewLight
    {
        Math::float3 direction{ -0.390360f, -0.780720f, 0.487950f };
        Math::float3 color{ 1.0f, 1.0f, 1.0f };
        float exposure = 1.0f;
        float ambient = 0.3f;
        bool showPosition = false;
        float positionDistance = 1.5f;
    };

    // Create the pipeline objects and independent per-frame render slots once.
    // Returns false if any step fails; callers then mark the preview Failed.
    bool EnsureInitialized(ID3D12Device* device, std::uint32_t maxRenderSize = 256);
    bool IsInitialized() const { return initialized_; }

    // Private asset caches used to build previews without touching the scene.
    MeshManager& Meshes() { return meshes_; }
    MaterialDataManager& Materials() { return materials_; }

    // Load material assets (data/materials/*.json + legacy monolith) once. Safe to call repeatedly.
    void EnsurePresets();

    // Drop the preview-only material cache after a material file or one of its
    // referenced maps changed. The next EnsurePresets/GetOrCreate reloads them.
    void ReloadPresets();

    // Same, but ONLY when the preset sources actually changed on disk.
    //
    // AssetThumbnailCache::StartGpuJob used to call ReloadPresets() unconditionally for every mesh
    // and material thumbnail. That is a directory rescan plus a full JSON reparse per thumbnail —
    // and, because ClearAll() drops the cached MaterialData, it also re-loads every referenced
    // TEXTURE from disk for each one. Scrolling a folder of meshes did that once per icon, on the
    // main thread. The stamp is a cheap mtime fold, and the scan behind it is throttled, so the
    // steady-state cost of "did anything change" is nothing.
    //
    // Detects edits to `data/materials.json` and `data/materials/*`. It does NOT see a texture
    // edited in place under an unchanged .json — that case needs an explicit refresh.
    void ReloadPresetsIfChanged();

    // Ensure the shared unit sphere used for material previews is resident.
    // Records upload work into `load` on first use; returns null on failure.
    std::shared_ptr<Mesh> EnsureSphere(Renderer& renderer, UploadBatch& load);

    // Record one mesh draw into `cl` and return its color target (sRGB, left in
    // PIXEL_SHADER_RESOURCE, ready for ImGui). Passing an existing target updates
    // it in place; renderSlot selects the per-frame descriptors/depth/constants.
    // A material entry is selected by each submesh's material slot.
    Microsoft::WRL::ComPtr<ID3D12Resource> RecordThumbnail(Renderer& renderer,
        ID3D12GraphicsCommandList* cl,
        const Mesh& mesh,
        const std::vector<std::shared_ptr<MaterialData>>& materials,
        std::uint32_t size,
        const OrbitCamera& camera = {},
        std::uint32_t renderSlot = 0,
        ID3D12Resource* existingColorTarget = nullptr,
        const TextureCube* environment = nullptr,
        float environmentExposure = 1.0f);

    // Rectangular variant used by resizable editor mini-scenes.
    Microsoft::WRL::ComPtr<ID3D12Resource> RecordPreview(Renderer& renderer,
        ID3D12GraphicsCommandList* cl,
        const Mesh& mesh,
        const std::vector<std::shared_ptr<MaterialData>>& materials,
        std::uint32_t width,
        std::uint32_t height,
        const OrbitCamera& camera,
        const PreviewLight& light,
        EditorPreviewMode mode,
        std::uint32_t lod,
        std::uint32_t renderSlot,
        ID3D12Resource* existingColorTarget = nullptr,
        // Mesh-asset level UV tiling (mesh.json "texOffsScale"). It is NOT part of the material, so
        // the preview cannot read it from MaterialData — the Mesh Editor passes its live value here
        // so dragging the control retiles immediately. Null = use the material's own value.
        const Math::float4* texOffsScaleOverride = nullptr,
        // Mesh Editor hover highlight: index of the MATERIAL SLOT to tint, or -1 for none.
        // Matched against each submesh's materialSlot, so a slot spanning several submeshes
        // lights all of them.
        int highlightMaterialSlot = -1,
        // Mesh Editor hover highlight by submesh ORDINAL (-1 = none). A CHUNKED mesh's submeshes
        // are spatial tiles that all share material slot 0, so the slot highlight above would tint
        // every tile at once; picking out ONE tile is exactly what the chunk list needs.
        int highlightSubmeshOrdinal = -1,
        // Optional scene environment used for metallic reflections and the preview background.
        const TextureCube* environment = nullptr,
        float environmentExposure = 1.0f);

    // Render the +X face of a cube texture into the standard 2D thumbnail
    // target. The caller submits `cl` and owns the returned color target.
    Microsoft::WRL::ComPtr<ID3D12Resource> RecordCubeThumbnail(Renderer& renderer,
        ID3D12GraphicsCommandList* cl,
        const TextureCube& cube,
        std::uint32_t size);

private:
    struct RenderSlot
    {
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
        Microsoft::WRL::ComPtr<ID3D12Resource> depthTarget;
        Microsoft::WRL::ComPtr<ID3D12Resource> constantBuffer;
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{};
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle{};
        D3D12_GPU_DESCRIPTOR_HANDLE srvGpuHandle{};
        std::uint8_t* constantBufferMapped = nullptr;
        std::uint32_t depthSize = 0;
    };

    Microsoft::WRL::ComPtr<ID3D12Resource> CreateColorTarget(ID3D12Device* device,
        std::uint32_t width,
        std::uint32_t height);
    bool CreateSharedDepth(ID3D12Device* device,
        std::uint32_t size,
        RenderSlot& slot);

    MeshManager meshes_;
    MaterialDataManager materials_;
    std::shared_ptr<Mesh> sphere_;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> doubleSidedPipeline_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> wireframePipeline_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> vertexNormalsPipeline_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> lightMarkerPipeline_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> skyboxPipeline_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> cubePipeline_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> cubeArrayPipeline_;
    // Each swapchain frame gets independent descriptors, constants, and depth.
    // This lets the Mesh Editor update its mini-scene every frame without
    // overwriting resources still consumed by an older GPU frame. Thumbnail
    // generation continues to use slot zero under its existing fence.
    std::array<RenderSlot, render::kFrameCount> renderSlots_;
    std::uint32_t srvDescriptorSize_ = 0;
    bool initialized_ = false;
    bool presetsLoaded_ = false;
    std::uint64_t PresetSourceStamp() const;              // fold of the preset sources' mtimes
    std::uint64_t presetStamp_ = 0;
    std::chrono::steady_clock::time_point presetStampCheckedAt_{}; // throttles the scan itself
};

#endif // WITH_EDITOR
