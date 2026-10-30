#pragma once
#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <cstdint>

using namespace Microsoft::WRL;

#include "core/math/AABB.h"
#include "rendering/meshes/LodSelect.h" // render::kLodScreenSizes (GetLodScreenSizes)

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

    // Tessy (docs/tessy_plan.md N1): a CLUSTER is a contiguous run of at most kClusterMaxTris triangles
    // of ONE LOD's index buffer, inside one submesh -- the unit the VSM page cull hands the rasterizer
    // instead of the whole mesh (UE Nanite's cluster limits, NaniteDefinitions.h:21-25). The bake
    // reorders every (LOD x submesh) range into cluster order, so the classic paths draw the very same
    // triangles in another order and need nothing of this. Bounds are object space; the two wind maxima
    // (the vertices' COLOR_0.r and .b, 0..1) bound how far the sway can carry the cluster
    // (tessy_vsm_cull_cs.hlsl). Written to the .mesh.bin as is: the layout is the file format (v4).
    struct Cluster {
        uint32_t firstIndex = 0;           // into this LOD's index buffer
        uint32_t triCount = 0;             // 1..kClusterMaxTris
        DirectX::XMFLOAT3 boxCenter{};
        DirectX::XMFLOAT3 boxExtent{};
        DirectX::XMFLOAT4 sphere{};        // meshopt_computeSphereBounds: centre, radius
        float windAlongMax = 0.0f;         // max COLOR_0.r over the cluster's vertices
        float windLeafMax = 0.0f;          // max COLOR_0.b
    };
    static_assert(sizeof(Cluster) == 56, "Mesh::Cluster is the .mesh.bin v4 record");
    static constexpr uint32_t kClusterMaxTris = 128;  // UE NANITE_MAX_CLUSTER_TRIANGLES
    static constexpr uint32_t kClusterMaxVerts = 256; // UE NANITE_MAX_CLUSTER_VERTICES

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
        const std::vector<Submesh>& submeshes,
        // Worst-case OBJECT-SPACE deviation of this level from LOD0 (meshopt, absolute).
        // 0 = unknown. Drives GetLodScreenSizes below.
        float geometricError = 0.0f);

    // Part B plumbing (consumed by the queue in B2). lod 0 = base table; higher clamps like Draw.
    const std::vector<Submesh>& GetSubmeshes() const { return submeshes_; }

    // Baked deviation of `lod` from LOD0 in object-space units; 0 for LOD0 and for anything the
    // bake could not measure.
    float GetLodError(UINT lod) const
    {
        return (lod == 0 || lod - 1u >= extraLods_.size()) ? 0.0f : extraLods_[lod - 1u].error;
    }

    // UE's AUTO screen size of every LOD (FStaticMeshRenderData::ResolveSectionInfo with
    // bAutoComputeLODScreenSize; render::AutoLodScreenSizes): the screen size at which each level's
    // baked deviation projects to `pixelError` pixels at 1080p, 90 degrees. out[0] = 2, levels the mesh
    // lacks = 0. Pure function of the bake and the argument -- safe from any thread.
    void GetLodScreenSizes(float pixelError, float (&out)[render::kLodScreenSizes]) const;

    const std::vector<Submesh>& SubmeshesForLod(UINT lod) const;
    size_t GetSubmeshCount() const { return submeshes_.size(); }

    // Tessy: the clusters of `lod` (clamped like ClampExplicitLod), sorted by firstIndex; every
    // submesh's run is contiguous. Empty for a mesh without clusters (a v3 .mesh.bin): it stays on the
    // classic path for every consumer (plan section 4.8).
    const std::vector<Cluster>& GetClusters(UINT lod) const;
    bool HasClusters() const { return !clusters_.empty(); }
    size_t GetClusterCount() const;
    // Tessy N6: a cluster as MESHLETS -- runs of its triangles (in order) whose unique vertices (mesh-VB indices,
    // first-use order) stay within kMeshletMaxVerts, each triangle as its corners' places in that list (three bytes
    // packed in a uint). Built at load from the index buffer, not stored in the .mesh.bin: the mesh shader computes
    // each vertex once from these, one group per meshlet. 64 and not the cluster's 256: the DECLARED output decides
    // how many groups an SM holds -- at 256 x 52 bytes the mesh shader was slower than the VS path (a leaf card's
    // cluster has ~2 vertices a triangle, so its 128 triangles reach 256), at 128 it drew the grove's shadows in
    // 0.078 ms per million triangles, at 64 in 0.063-0.072 (the VS path 0.12; 2026-10-29). Parallel to GetClusters(lod).
    static constexpr uint32_t kMeshletMaxVerts = 64;
    struct ClusterMeshlet { uint32_t vertOffset = 0, vertCount = 0, triOffset = 0, triCount = 0; };
    struct ClusterLocal { uint32_t firstMeshlet = 0, meshletCount = 0; };
    struct ClusterLocalTables
    {
        std::vector<ClusterLocal> clusters;
        std::vector<ClusterMeshlet> meshlets;
        std::vector<uint32_t> verts; // mesh-VB vertex indices
        std::vector<uint32_t> tris;  // local indices a | b << 8 | c << 16
    };
    const ClusterLocalTables& GetClusterLocal(UINT lod) const;
    // The meshlet tables of one LOD's clusters over its index buffer; false (and `out` empty) unless the clusters
    // cover `idx` exactly. SetClusters' builder, public for the gate.
    static bool BuildClusterLocal(const std::vector<Cluster>& clusters, const std::vector<uint32_t>& idx,
                                  ClusterLocalTables& out);
    // One table per LOD, in LOD order, after the LODs exist (AddLod), with each LOD's index buffer as it was
    // uploaded (the meshlet tables are made from it). A table whose triangle total does not match its LOD's
    // index count is refused -- the clusters would describe another buffer.
    void SetClusters(std::vector<std::vector<Cluster>> perLod, const std::vector<const std::vector<uint32_t>*>& lodIndices);

    // Tessy N7: the CLUSTER DAG (rendering/meshes/TessyDag.h) -- continuous LOD over LOD0, one index buffer of its own
    // over this mesh's VB holding every level of every submesh. Each cluster's ClusterLod says which simplification
    // made it (lod*) and which consumes it (parent*); a view draws a cluster when its error projects within the
    // threshold and its parent's does not. Empty unless the asset asked for it (mesh.json "tessyLod", or
    // --tessy-lod-build); the discrete LODs stay beside it for every classic path.
    struct ClusterLod
    {
        DirectX::XMFLOAT4 lodSphere{};    // the group simplification that made the cluster (a leaf: its own sphere)
        DirectX::XMFLOAT4 parentSphere{}; // the one that consumes it (a root: its lodSphere)
        float lodError = 0.0f;            // object space; 0 for a leaf
        float parentError = 0.0f;         // object space; FLT_MAX for a root
        uint32_t level = 0;               // 0 = the leaves (LOD0's clusters)
        uint32_t pad = 0;
    };
    static_assert(sizeof(ClusterLod) == 48, "Mesh::ClusterLod");
    struct DagLevel { uint32_t firstCluster = 0, clusterCount = 0; float minLodError = 0.0f, maxParentError = 0.0f; };
    struct DagSubmesh { uint32_t firstCluster = 0, clusterCount = 0, firstLevel = 0, levelCount = 0; };
    struct Dag
    {
        std::vector<uint32_t> indices;     // the DAG's index buffer (freed by SetDag once uploaded)
        std::vector<Cluster> clusters;     // firstIndex into `indices`; per submesh contiguous, by level
        std::vector<ClusterLod> lod;       // parallel to `clusters`
        std::vector<DagLevel> levels;      // DagSubmesh::firstLevel..: one per level of each submesh
        std::vector<DagSubmesh> submeshes; // per LOD0 submesh, in LOD0's order
    };
    bool HasDag() const { return !dag_.clusters.empty(); }
    const Dag& GetDag() const { return dag_; }
    const ClusterLocalTables& GetDagLocal() const { return dagLocal_; } // its meshlets (N6), parallel to the clusters
    ID3D12Resource* GetDagIndexBufferResource() const { return dagIndexBuffer_.Get(); }
    UINT GetDagIndexCount() const { return dagIndexCount_; }
    // Uploads the DAG's index buffer (COMMON, R32, like AddLod's) and builds its meshlet tables; then frees the CPU
    // indices. Refused (no DAG) when the clusters do not cover the indices.
    bool SetDag(ID3D12Device* device, ID3D12GraphicsCommandList* uploadCmdList,
                std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive, Dag dag);

    // Mesh chunking (mesh.json "chunkGrid", baked by MeshManager's ChunkifyLod0). The LOD0
    // submeshes of a chunked mesh are SPATIAL tiles of one continuous surface, not material groups
    // scattered through it — so each carries its own mesh-local AABB. Two consumers: the CAMERA
    // LODs and draws each tile independently (RenderableObject::chunkLods_), and the shadow path
    // registers each as an INDEPENDENT caster so a page rasterizes only the tiles it overlaps.
    // Non-chunked meshes report false + an empty table and keep sharing their object's bounds.
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
    // The same mesh-local positions the narrow phase tests, exposed so a placement tool can ask
    // about EVERY vertex rather than about the AABB -- burying a mesh under a sloped surface is
    // a per-vertex question, and an AABB corner is not a point on the mesh.
    const std::vector<Math::float3>& RaycastPositions() const { return raycastPositions_; }
    // The triangles too, for a caller that wants to sweep the whole surface rather than
    // probe it point by point. RaycastLocal is a LINEAR walk of this list -- there is no
    // acceleration structure behind it -- so asking it nine thousand questions about one
    // mesh costs nine thousand walks. Tracing a shoreline did exactly that and took 71
    // seconds with the frame thread held; rasterising these triangles into the same grid
    // is one pass and answers the same question.
    const std::vector<uint32_t>& RaycastIndices() const { return raycastIndices_; }
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
        float error = 0.0f;             // object-space deviation from LOD0; 0 = unknown
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

    std::vector<std::vector<Cluster>> clusters_; // Tessy: per LOD; empty = no clusters
    std::vector<ClusterLocalTables> clusterLocal_; // Tessy N6: per LOD, parallel to clusters_
    Dag dag_;                                      // Tessy N7: the cluster DAG (indices freed after upload)
    ClusterLocalTables dagLocal_;
    Microsoft::WRL::ComPtr<ID3D12Resource> dagIndexBuffer_;
    UINT dagIndexCount_ = 0;

#if WITH_EDITOR
    // Position-only CPU copy of base LOD geometry. This avoids GPU readback and retains
    // substantially less data than the complete PNTUV vertex stream.
    std::vector<Math::float3> raycastPositions_;
    std::vector<uint32_t> raycastIndices_;
#endif
};
