#pragma once
#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <cstdint>

using namespace Microsoft::WRL;

#include "core/math/AABB.h"

// NEW "full" format for textures/lighting
// order matches the layout preset "PosNormTanUV":
// POSITION (float3), NORMAL (float3), TANGENT (float4), TEXCOORD (float2)
struct VertexPNTUV {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;    // can be left as zeros—we will generate them
    DirectX::XMFLOAT4 tangent;   // xyz = tangent, w = handedness (+1/-1)
    DirectX::XMFLOAT2 uv;
};

class Mesh {
public:
    Mesh() = default;
    
    // Flexible upload of an arbitrary vertex format (specify the stride explicitly)
    void CreateGPUFlexible(ID3D12Device* device,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
        const void* vertices, UINT vertexCount, UINT vertexStride,
        const void* indices, UINT indexCount,
        DXGI_FORMAT indexFormat = DXGI_FORMAT_R16_UINT);

    // Upload the new format and optionally generate normals/tangents on the CPU
    void CreateGPU_PNTUV(ID3D12Device* device,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
        std::vector<VertexPNTUV>& verts,       // by reference so we can modify it
        const uint32_t* indices, UINT indexCount,
        bool generateTangentSpace = true);

    // Rendering. lod 0 = full detail (the base buffers); higher indices select coarser LODs,
    // clamped to what's available (Step 6). With no extra LODs, any lod draws full detail.
    void Draw(ID3D12GraphicsCommandList* cmdList, UINT lod = 0) const;
    void DrawInstanced(ID3D12GraphicsCommandList* cmdList, UINT instanceCount, UINT lod = 0) const;

    UINT GetIndexCount() const { return indexCount_; }
    UINT GetLodCount() const { return 1u + static_cast<UINT>(extraLods_.size()); }

    // Step 6: append a coarser LOD as a reduced 32-bit index buffer over the SAME vertex
    // buffer (meshopt_simplify output references the base verts). Call after the base
    // CreateGPU*, on the same upload command list.
    void AddLod(ID3D12Device* device, ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
        const uint32_t* indices, UINT indexCount);

    ID3D12Resource* GetVertexBufferResource() const { return vertexBuffer_.Get(); }
    ID3D12Resource* GetIndexBufferResource()  const { return indexBuffer_.Get(); }

    UINT GetVertexStride() const { return vertexStride_; }
    DXGI_FORMAT GetIndexFormat() const { return indexFormat_; }

    const AABB& GetBoundingBox() const { return bounds_; }

private:
    // Step 6: a coarser LOD = a reduced index buffer over the SAME (base) vertex buffer.
    struct LodLevel {
        Microsoft::WRL::ComPtr<ID3D12Resource> indexBuffer;
        D3D12_INDEX_BUFFER_VIEW  indexBufferView = {};
        UINT indexCount = 0;
    };

    // Resolve a lod index to its buffer views + index count (lod 0 -> base members;
    // out-of-range clamps to the coarsest available).
    void SelectLod(UINT lod, const D3D12_VERTEX_BUFFER_VIEW*& vbv,
        const D3D12_INDEX_BUFFER_VIEW*& ibv, UINT& indexCount) const;

    // Step 3: bind VB/IB/topology, skipping calls already matching the CL bind cache.
    void BindIA(ID3D12GraphicsCommandList* cmdList,
        const D3D12_VERTEX_BUFFER_VIEW& vbv, const D3D12_INDEX_BUFFER_VIEW& ibv) const;

    // Generate normals/tangents (simple: per triangle with vertex averaging)
    static void GenerateNormalsTangents(std::vector<VertexPNTUV>& verts,
        const uint32_t* indices, UINT indexCount);

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> vertexBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> indexBuffer_;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView_ = {};
    D3D12_INDEX_BUFFER_VIEW  indexBufferView_ = {};
    UINT  vertexStride_ = 0;      // explicit stride provided during upload
    DXGI_FORMAT indexFormat_ = DXGI_FORMAT_R16_UINT;
    UINT  indexCount_ = 0;
    AABB bounds_;

    std::vector<LodLevel> extraLods_; // lod 1+ (lod 0 is the base buffers above); empty = no LODs
};
