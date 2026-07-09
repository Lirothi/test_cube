#pragma once
#if WITH_EDITOR

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

#include "materials/Texture2D.h"
#include "rendering/meshes/MeshManager.h"
#include "materials/MaterialDataManager.h"

class Mesh;
class MaterialData;
class Renderer;
class UploadBatch;

// Self-contained offscreen renderer that produces Content Browser thumbnails for
// mesh and material assets (Step 12E). It owns a tiny forward pipeline (one PSO,
// one root signature, a shared depth target, a reusable constant buffer) plus a
// private MeshManager / MaterialDataManager so it never touches the edited scene.
//
// Lifetime / threading: everything runs inside the editor draw/tick window under a
// GPU wait, driven by AssetThumbnailCache. Asset loads use a caller-provided
// UploadBatch; each thumbnail render records into a caller-provided command list
// that the caller submits (one draw per list keeps the single-slot descriptor
// heaps correct). Generated color targets are handed to the cache, which owns
// their lifetime; this class owns only the shared pipeline objects.
class EditorPreviewRenderer
{
public:
    // Create the pipeline objects once (shaders, root signature, PSO, heaps,
    // shared depth target, reusable constant buffer, 1x1 white fallback). Returns
    // false if any step fails; callers then mark the affected thumbnails Failed.
    bool EnsureInitialized(Renderer& renderer);
    bool IsInitialized() const { return initialized_; }

    // Private asset caches used to build previews without touching the scene.
    MeshManager& Meshes() { return meshes_; }
    MaterialDataManager& Materials() { return materials_; }

    // Load material presets from data/materials.json once. Safe to call repeatedly.
    void EnsurePresets();

    // Ensure the shared unit sphere used for material previews is resident.
    // Records upload work into `load` on first use; returns null on failure.
    std::shared_ptr<Mesh> EnsureSphere(Renderer& renderer, UploadBatch& load);

    // Record one thumbnail draw of `mesh` into `cl` and return the freshly created
    // color target (sRGB, left in PIXEL_SHADER_RESOURCE, ready for ImGui). The
    // caller submits `cl`. `albedo` may be null for a neutral (mesh) preview.
    Microsoft::WRL::ComPtr<ID3D12Resource> RecordThumbnail(Renderer& renderer,
        ID3D12GraphicsCommandList* cl,
        const Mesh& mesh,
        ID3D12Resource* albedo,
        DXGI_FORMAT albedoSrvFormat,
        bool hasAlbedo,
        std::uint32_t size);

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> CreateColorTarget(ID3D12Device* device,
        std::uint32_t size);
    bool CreateSharedDepth(ID3D12Device* device, std::uint32_t size);

    MeshManager meshes_;
    MaterialDataManager materials_;
    std::shared_ptr<Mesh> sphere_;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap_; // shader-visible, 1 slot
    Microsoft::WRL::ComPtr<ID3D12Resource> depthTarget_;
    Microsoft::WRL::ComPtr<ID3D12Resource> constantBuffer_;
    Texture2D whiteFallback_;

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle_{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle_{};
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle_{};
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpuHandle_{};
    std::uint8_t* constantBufferMapped_ = nullptr;
    std::uint32_t depthSize_ = 0;
    bool initialized_ = false;
    bool presetsLoaded_ = false;
};

#endif // WITH_EDITOR
