#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
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
    uint32_t vbIndex = 0;
    uint32_t ibIndex = 0;
    uint32_t indexIs32 = 1;
    uint32_t albedoTexIndex = 0xFFFFFFFFu; // ~0 = no albedo texture (use baseColor)
    float    baseColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f }; // material tint / fallback
};

// Persistent bindless table (S9): a single shader-visible CBV_SRV_UAV heap that
// RT shaders index directly via SM6.6 ResourceDescriptorHeap[] (needs
// ResourceBindingTier 3 + SM >= 6.6). Holds each mesh's VB/IB as raw
// ByteAddressBuffer SRVs plus a geometry-info structured buffer, and reserves a
// small per-frame region the RT pass copies its scene SRVs/UAVs into.
//
//   heap[0]                                      geometry-info structured SRV
//   heap[kSceneBase + f*kScenePerFrame + i]      per-frame scene descriptor i
//   heap[kGeoBase + kDescPerGeom*g + {0,1,2}]    mesh g: VB raw, IB raw, albedo SRVs
class BindlessTable
{
public:
    static constexpr UINT kScenePerFrame = 8;  // TLAS, gbuffers, depth, ssr UAV, ...
    static constexpr UINT kDescPerGeom = 3;    // VB raw, IB raw, albedo texture
    static constexpr UINT kGeomInfoSlot = 0;
    static constexpr UINT kSceneBase = 1;
    static constexpr UINT kGeoBase = kSceneBase + kScenePerFrame * render::kFrameCount;
    static constexpr UINT kMaxDescriptors = 8192;

    void Init(ID3D12Device* device);
    void Reset();

    bool Ready() const { return heap_ != nullptr; }
    ID3D12DescriptorHeap* Heap() const { return heap_.Get(); }

    // Register a mesh (idempotent): creates its VB/IB raw SRVs (+ albedo texture
    // SRV if albedoSrv is valid) in the heap and a geometry-info record. Returns
    // the geometry index (used as TLAS InstanceID). baseColor4 may be null (white).
    uint32_t GetOrRegisterMesh(Mesh* mesh, D3D12_CPU_DESCRIPTOR_HANDLE albedoSrv, const float* baseColor4);

    // Absolute heap index of per-frame scene descriptor `which` for `frameIndex`.
    UINT SceneIndex(UINT frameIndex, UINT which) const
    {
        return kSceneBase + frameIndex * kScenePerFrame + which;
    }
    UINT GeomInfoIndex() const { return kGeomInfoSlot; }

    // Copy a CPU descriptor (SRV or UAV) into a per-frame scene slot.
    void WriteSceneDescriptor(UINT frameIndex, UINT which, D3D12_CPU_DESCRIPTOR_HANDLE srcCpu);

    // (Re)upload the geometry-info array and (re)create its SRV at slot 0. Call
    // after registering meshes for the frame (cheap; rebuilds the whole array).
    void UploadGeometryInfo();

    UINT GeometryCount() const { return static_cast<UINT>(geomInfo_.size()); }

private:
    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(UINT index) const;

    ID3D12Device* device_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_; // shader-visible CBV_SRV_UAV
    UINT incr_ = 0;

    robin_hood::unordered_map<Mesh*, uint32_t> meshToGeom_;
    std::vector<GeometryInfoGPU> geomInfo_;                  // CPU mirror
    Microsoft::WRL::ComPtr<ID3D12Resource> geomInfoBuffer_;  // UPLOAD, structured
    UINT geomInfoCapacity_ = 0;                              // in records
};

} // namespace rt
