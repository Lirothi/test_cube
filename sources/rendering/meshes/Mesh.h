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
// POSITION (float3), NORMAL (float3), TANGENT (float4), TEXCOORD (float2), COLOR (RGBA8)
struct VertexPNTUV {
    DirectX::XMFLOAT3 position;  // 0
    DirectX::XMFLOAT3 normal;    // 12  can be left as zeros—we will generate them
    DirectX::XMFLOAT4 tangent;   // 24  xyz = tangent, w = handedness (+1/-1)
    DirectX::XMFLOAT2 uv;        // 40
    // W7.1: baked per-vertex wind data (COLOR_0, sampled as R8G8B8A8_UNORM in the VS). R = geodesic
    // sway weight, G = per-limb id (phase), B = along-limb edge weight, A = spare. Default 0 => a
    // mesh with no bake is rigid and renders byte-identically. Appended at the END (offset 48) so the
    // hardcoded UV offset 40 (InputLayoutManager / EditorPreviewRenderer / PosUV_InstCasterId) and
    // every D3D12_APPEND_ALIGNED_ELEMENT stay valid.
    uint32_t color = 0;          // 48
};
static_assert(sizeof(VertexPNTUV) == 52, "VertexPNTUV must be 52 bytes (PNTUV + COLOR_0 wind bake)");

class Mesh {
public:
    Mesh() = default;

    // Part B: a contiguous run of the index buffer that shares one material slot. Every mesh has
    // at least one (covering the whole buffer). LODs carry their own tables (ranges re-simplified
    // per level, see MeshManager::GenerateLods). Draw() still draws the whole buffer in B1; the
    // render queue starts iterating submeshes in B2.
    struct Submesh {
        uint32_t indexOffset  = 0; // first index into the (lod-specific) index buffer
        uint32_t indexCount   = 0;
        uint32_t materialSlot = 0; // index into the owner object's material-slot array
    };

    struct LodDrawInfo {
        D3D12_VERTEX_BUFFER_VIEW vertexBufferView{};
        D3D12_INDEX_BUFFER_VIEW indexBufferView{};
        UINT indexCount = 0;
        const std::vector<Submesh>* submeshes = nullptr;
    };

    // Flexible upload of an arbitrary vertex format (specify the stride explicitly)
    void CreateGPUFlexible(ID3D12Device* device,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
        const void* vertices, UINT vertexCount, UINT vertexStride,
        const void* indices, UINT indexCount,
        DXGI_FORMAT indexFormat = DXGI_FORMAT_R16_UINT);

    // Upload the new format and optionally generate normals/tangents on the CPU.
    // submeshes: optional multi-material table over `indices` (B2 glTF path); null => a single
    // submesh spanning the whole buffer (materialSlot 0) — the behavior every current caller gets.
    void CreateGPU_PNTUV(ID3D12Device* device,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
        std::vector<VertexPNTUV>& verts,       // by reference so we can modify it
        const uint32_t* indices, UINT indexCount,
        bool generateTangentSpace = true,
        const std::vector<Submesh>* submeshes = nullptr);

    // CPU-only preprocessing used by asynchronous editor thumbnail preparation.
    static void GenerateNormalsTangents(std::vector<VertexPNTUV>& verts,
        const uint32_t* indices, UINT indexCount);

    // Rendering. lod 0 = full detail (the base buffers); higher indices select coarser LODs,
    // clamped to what's available (Step 6). With no extra LODs, any lod draws full detail.
    void Draw(ID3D12GraphicsCommandList* cmdList, UINT lod = 0) const;
    void DrawInstanced(ID3D12GraphicsCommandList* cmdList, UINT instanceCount, UINT lod = 0) const;
    // Part B2: draw one submesh (ordinal into SubmeshesForLod(lod)) — an offset ranged draw
    // over the same buffers. Out-of-range ordinals are skipped.
    void DrawSubmesh(ID3D12GraphicsCommandList* cmdList, UINT submeshOrdinal, UINT lod = 0) const;
    // B2b: instanced ranged draw of one submesh (multi-slot auto-instancing).
    void DrawSubmeshInstanced(ID3D12GraphicsCommandList* cmdList, UINT submeshOrdinal,
        UINT instanceCount, UINT lod = 0) const;

    UINT GetIndexCount() const { return indexCount_; }
    UINT GetLodCount() const { return 1u + static_cast<UINT>(extraLods_.size()); }

    // Resolve one explicit LOD without applying the runtime global LOD enable/force overrides.
    // Editor diagnostics use this to inspect every generated index buffer independently.
    LodDrawInfo GetLodDrawInfo(UINT lod) const;

    // Step 6: append a coarser LOD as a reduced 32-bit index buffer over the SAME vertex
    // buffer (meshopt_simplify output references the base verts). Call after the base
    // CreateGPU*, on the same upload command list. submeshes: this LOD's per-range table (ranges
    // into `indices`); pass the whole-buffer single submesh for single-material meshes.
    void AddLod(ID3D12Device* device, ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
        const uint32_t* indices, UINT indexCount,
        const std::vector<Submesh>& submeshes);

    // Part B plumbing (consumed by the queue in B2). lod 0 = base table; higher clamps like Draw.
    const std::vector<Submesh>& GetSubmeshes() const { return submeshes_; }

    const std::vector<Submesh>& SubmeshesForLod(UINT lod) const;
    size_t GetSubmeshCount() const { return submeshes_.size(); }

    // Shadow chunking (mesh.json "chunkGrid", baked by MeshManager's ChunkifyLod0). The LOD0
    // submeshes of a chunked mesh are SPATIAL tiles of one continuous surface, not material groups
    // scattered through it — so each carries its own mesh-local AABB and the shadow path registers
    // it as an INDEPENDENT caster. That is what lets a virtual-shadow-map page rasterize only the
    // tiles it overlaps instead of the whole terrain. Non-chunked meshes report false + an empty
    // table and keep sharing their object's bounds across slots.
    bool IsChunkedSubmeshes() const { return chunkedSubmeshes_; }
    const std::vector<AABB>& GetSubmeshBounds() const { return submeshBounds_; }

    // Compute one mesh-local AABB per LOD0 submesh and mark the mesh chunked. Call right after
    // CreateGPU_PNTUV, from the load path that knows mesh.json asked for chunking (the .bin carries
    // no flag of its own — see the format NOTE in MeshManager.cpp).
    void MarkChunkedSubmeshes(const std::vector<VertexPNTUV>& verts,
        const uint32_t* indices, UINT indexCount);

    // Clamp an explicit LOD request to the LODs this mesh actually has (0..GetLodCount()-1). Unlike
    // ResolveRuntimeLod this ignores the g_lodEnabled/g_forcedLod runtime overrides — the shadow LOD
    // bias is a deliberate fixed level, not the per-camera LOD.
    UINT ClampExplicitLod(UINT lod) const { const UINT last = GetLodCount() - 1u; return lod < last ? lod : last; }

    // Explicit-LOD resource + range accessors for the GPU-driven shadow mega-buffer (which sources a
    // coarser index buffer per the shadow LOD bias). LODs share the base vertex buffer, so only the
    // index buffer varies. `lod` is clamped to the available LODs.
    ID3D12Resource* GetLodIndexBufferResource(UINT lod) const {
        const UINT r = ClampExplicitLod(lod);
        return (r == 0 || extraLods_.empty()) ? indexBuffer_.Get() : extraLods_[r - 1u].indexBuffer.Get();
    }
    UINT GetLodIndexCount(UINT lod) const {
        const UINT r = ClampExplicitLod(lod);
        return (r == 0 || extraLods_.empty()) ? indexCount_ : extraLods_[r - 1u].indexCount;
    }

    ID3D12Resource* GetVertexBufferResource() const { return vertexBuffer_.Get(); }
    ID3D12Resource* GetIndexBufferResource()  const { return indexBuffer_.Get(); }

    UINT GetVertexStride() const { return vertexStride_; }
    DXGI_FORMAT GetIndexFormat() const { return indexFormat_; }

    // OBJECT-space metres of arc that COLOR_0.b == 1 stands for: the longest path from this mesh's
    // wood surface out to a leaf tip. The wind shader multiplies b by this (times the object's world
    // scale) to recover how far along its own leaf a vertex sits, which is what bounds the leaf's
    // streaming to its own length instead of to a global amplitude. 0 = the mesh has no wind bake.
    float GetWindLeafScale() const { return windLeafScale_; }

    const AABB& GetBoundingBox() const { return bounds_; }
    // Radius of the mesh's vertex-enclosing sphere, centered on its AABB center.
    // Unlike AABB::GetRadius(), this does not include empty box corners.
    float GetBoundingSphereRadius() const { return boundingSphereRadius_; }

#if WITH_EDITOR
    // Editor picking/placement narrow phase. Geometry stays in mesh-local space;
    // callers transform the ray so its parameter remains a world-ray distance.
    bool HasRaycastTriangles() const { return !raycastPositions_.empty() && raycastIndices_.size() >= 3; }
    bool RaycastLocal(const Math::float3& origin, const Math::float3& direction,
        float* outDistance) const;
#endif

private:
    // Step 6: a coarser LOD = a reduced index buffer over the SAME (base) vertex buffer.
    struct LodLevel {
        Microsoft::WRL::ComPtr<ID3D12Resource> indexBuffer;
        D3D12_INDEX_BUFFER_VIEW  indexBufferView = {};
        UINT indexCount = 0;
        std::vector<Submesh> submeshes; // ranges into this LOD's index buffer
    };

    UINT ResolveRuntimeLod(UINT lod) const;

    // Step 3: bind VB/IB/topology, skipping calls already matching the CL bind cache.
    void BindIA(ID3D12GraphicsCommandList* cmdList,
        const D3D12_VERTEX_BUFFER_VIEW& vbv, const D3D12_INDEX_BUFFER_VIEW& ibv) const;

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> vertexBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> indexBuffer_;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView_ = {};
    D3D12_INDEX_BUFFER_VIEW  indexBufferView_ = {};
    UINT  vertexStride_ = 0;      // explicit stride provided during upload
    DXGI_FORMAT indexFormat_ = DXGI_FORMAT_R16_UINT;
    UINT  indexCount_ = 0;
    AABB bounds_;
    float boundingSphereRadius_ = 0.0f;
    float windLeafScale_ = 0.0f; // object-space metres per unit of COLOR_0.b (see GetWindLeafScale)

    std::vector<Submesh> submeshes_;  // lod 0 submesh table (>=1 entry; whole buffer by default)
    std::vector<LodLevel> extraLods_; // lod 1+ (lod 0 is the base buffers above); empty = no LODs

    bool chunkedSubmeshes_ = false;   // submeshes are spatial chunks -> independent shadow casters
    std::vector<AABB> submeshBounds_; // mesh-local LOD0 AABB per submesh; empty unless chunked

#if WITH_EDITOR
    // Position-only CPU copy of base LOD geometry. This avoids GPU readback and retains
    // substantially less data than the complete PNTUV vertex stream.
    std::vector<Math::float3> raycastPositions_;
    std::vector<uint32_t> raycastIndices_;
#endif
};
