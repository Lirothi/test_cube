#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <array>
#include <vector>

#include "third_party/robin_hood.h"
#include "rendering/core/RenderConstants.h" // render::kFrameCount

class Mesh;

namespace rt {

// Per-geometry record consumed by RT shaders (must match the HLSL GeometryInfo).
// vbIndex/ibIndex are absolute indices into the bindless heap (SM6.6
// ResourceDescriptorHeap[]); indexIs32 selects 16- vs 32-bit index decode.
struct GeometryInfoGPU
{
    // Row 0 (16B)
    uint32_t vbIndex = 0;
    uint32_t ibIndex = 0;
    uint32_t indexIs32 = 1;
    uint32_t albedoTexIndex = 0xFFFFFFFFu; // ~0 = no albedo texture (use baseColor)
    // Row 1 (16B) — PBR params so RT hits shade like the base pass. metalRough are
    // the flat fallback; if mrTexIndex != ~0 the shader samples the MR texture instead.
    float    roughness = 1.0f;
    float    metalness = 0.0f;
    uint32_t mrTexIndex = 0xFFFFFFFFu; // ~0 = no MR texture (use flat roughness/metalness)
    uint32_t firstTri = 0u; // B3: this record's first triangle in the IB (submesh indexOffset/3)
    // Row 2 (16B)
    float    baseColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f }; // material tint / fallback
    // Row 3 (16B)
    uint32_t mrMultiply = 0u; // 0 = texture overrides values, 1 = texture * values
    // Byte stride of this record's vertex buffer, taken from Mesh::GetVertexStride(). The RT hit
    // shaders fetch normals/UVs out of a raw ByteAddressBuffer, so they need the stride; it used to
    // be a hardcoded 48 in rt_geometry.hlsli, which silently became WRONG the day VertexPNTUV grew
    // to 52 (W7.1's COLOR_0). Every vertex but #0 then decoded at the wrong offset and RT
    // reflections shaded off garbage normals/UVs. Sourcing it from the mesh removes the mirror.
    uint32_t vertexStride = 0u;
    uint32_t _pad[2]{};
};

// Persistent bindless table (S9): a single shader-visible CBV_SRV_UAV heap that
// RT shaders index directly via SM6.6 ResourceDescriptorHeap[] (needs
// ResourceBindingTier 3 + SM >= 6.6). Holds each mesh's VB/IB as raw
// ByteAddressBuffer SRVs plus a geometry-info structured buffer, and reserves a
// small per-frame region the RT pass copies its scene SRVs/UAVs into.
//
//   heap[f]                                      per-frame geometry-info structured SRV
//   heap[kSceneBase + f*kScenePerFrame + i]      per-frame scene descriptor i
//   heap[kGeoBase + kDescPerGeom*d + {0,1,2,3}]  immutable VB/IB/albedo/MR descriptor set d
class BindlessTable
{
public:
    static constexpr UINT kScenePerFrame = 32; // per-frame scene descriptors, partitioned
                                               // across the RT passes (reflections 0-7,
                                               // denoise 8-12, debug 13-16, glass refl 17-25)
                                               // so passes in the same frame never alias slots
    static constexpr UINT kDescPerGeom = 4;    // VB raw, IB raw, albedo texture, MR texture
    static constexpr UINT kSceneBase = render::kFrameCount;
    static constexpr UINT kGeoBase = kSceneBase + kScenePerFrame * render::kFrameCount;
    static constexpr UINT kMaxDescriptors = 8192;
    static constexpr uint32_t kInvalidGeometry = 0xFFFFFFFFu;

    void Init(ID3D12Device* device);
    void Reset();

    bool Ready() const { return heap_ != nullptr; }
    bool BuildFailed() const { return buildFailed_; }
    bool FrameReady(UINT frameIndex) const;
    ID3D12DescriptorHeap* Heap() const { return heap_.Get(); }

    // Per-slot material inputs for a multi-submesh registration (mirrors what the single-
    // material overload takes). baseColor4 may be null (white).
    struct SlotMaterial
    {
        D3D12_CPU_DESCRIPTOR_HANDLE albedoSrv{};
        D3D12_CPU_DESCRIPTOR_HANDLE mrSrv{};
        const float* baseColor4 = nullptr;
        float roughness = 1.0f;
        float metalness = 0.0f;
        bool mrMultiply = false;
    };

    // One stable, contiguous record run per owner + mesh (InstanceID + GeometryIndex).
    // Scalar edits update that owner's CPU records in place, never another object's material.
    // Submesh s uses slots[min(s, slotCount-1)]. Descriptor sets are shared separately and are
    // immutable until Reset (which requires GPU idle). Returns kInvalidGeometry on exhaustion.
    uint32_t GetOrUpdateMesh(const void* owner, Mesh* mesh, const SlotMaterial* slots, size_t slotCount);
    uint32_t GetOrUpdateMesh(const void* owner, Mesh* mesh, D3D12_CPU_DESCRIPTOR_HANDLE albedoSrv,
                               D3D12_CPU_DESCRIPTOR_HANDLE mrSrv,
                               const float* baseColor4, float roughness, float metalness,
                               bool mrMultiply);

    // Absolute heap index of per-frame scene descriptor `which` for `frameIndex`.
    UINT SceneIndex(UINT frameIndex, UINT which) const
    {
        return kSceneBase + frameIndex * kScenePerFrame + which;
    }
    UINT GeomInfoIndex(UINT frameIndex) const { return frameIndex; }

    // Copy a CPU descriptor (SRV or UAV) into a per-frame scene slot.
    void WriteSceneDescriptor(UINT frameIndex, UINT which, D3D12_CPU_DESCRIPTOR_HANDLE srcCpu);

    // Call only AFTER BeginFrame waited for this frame slot's fence, and before recording RT.
    // Neither the other frame buffers nor their SRVs are overwritten/released by this upload.
    bool UploadGeometryInfo(UINT frameIndex);

    UINT GeometryCount() const { return static_cast<UINT>(geomInfo_.size()); }
    UINT DescriptorSetCount() const { return static_cast<UINT>(descriptorCache_.size()); }

private:
    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(UINT index) const;
    uint32_t GetOrRegisterDescriptors(Mesh* mesh, const SlotMaterial& material);

    ID3D12Device* device_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_; // shader-visible CBV_SRV_UAV
    UINT incr_ = 0;

    struct GeometryKey
    {
        const void* owner;
        Mesh* mesh;
        bool operator==(const GeometryKey&) const = default;
    };
    struct DescriptorKey
    {
        Mesh* mesh;
        SIZE_T albedo;
        SIZE_T mr;
        bool operator==(const DescriptorKey&) const = default;
    };
    struct KeyHash
    {
        size_t operator()(const GeometryKey& key) const;
        size_t operator()(const DescriptorKey& key) const;
    };
    struct FrameGeometry
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer; // UPLOAD, structured
        UINT capacity = 0;
        uint64_t version = 0;
    };
    robin_hood::unordered_map<GeometryKey, uint32_t, KeyHash> geomCache_;
    robin_hood::unordered_map<DescriptorKey, uint32_t, KeyHash> descriptorCache_;
    std::vector<GeometryInfoGPU> geomInfo_; // latest CPU material state
    std::array<FrameGeometry, render::kFrameCount> frameGeometry_{};
    uint64_t geomVersion_ = 0;
    bool buildFailed_ = false; // sticky until Reset; renderer falls back to SSR
};

} // namespace rt
