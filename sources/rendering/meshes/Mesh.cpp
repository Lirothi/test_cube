#include "rendering/meshes/Mesh.h"
#include "core/Helpers.h"
#include "rendering/core/UploadManager.h"
#include "rendering/core/CommandListBindState.h"
#include "rendering/core/RenderStats.h"
#include "rendering/meshes/LodSelect.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

using namespace DirectX;

// Flexible upload for an arbitrary format
void Mesh::CreateGPUFlexible(ID3D12Device* device,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
    const void* vertices, UINT vertexCount, UINT vertexStride,
    const void* indices, UINT indexCount,
    DXGI_FORMAT indexFormat)
{
    indexCount_ = indexCount;
    vertexStride_ = vertexStride;
    indexFormat_ = indexFormat;

#if WITH_EDITOR
    raycastPositions_.clear();
    raycastIndices_.clear();
#endif

    // Default single submesh spanning the whole buffer (material slot 0). CreateGPU_PNTUV may
    // replace this with a multi-material table; every other caller keeps the single submesh.
    submeshes_ = { Submesh{ 0u, indexCount, 0u } };

    bounds_.Reset();

    UploadManager up(device, uploadCmdList);

    // VB. Left in COMMON (not VERTEX_AND_CONSTANT_BUFFER): buffers decay to COMMON after every
    // ExecuteCommandLists regardless, and all readers (IA binds, RT BLAS builds, the Rung-2
    // mega-buffer copy) implicitly promote from COMMON — so COMMON is the one initial state every
    // consumer can start from without a barrier (needed by the mega-buffer's copy-source promotion).
    const UINT vbSize = vertexStride_ * vertexCount;
    vertexBuffer_ = up.CreateBufferWithData(vertices, vbSize, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COMMON);
    vertexBufferView_.BufferLocation = vertexBuffer_->GetGPUVirtualAddress();
    vertexBufferView_.StrideInBytes = vertexStride_;
    vertexBufferView_.SizeInBytes = vbSize;

    // Index buffer (supports 16/32-bit indices)
    UINT ibSize = 0;
    if (indexFormat_ == DXGI_FORMAT_R16_UINT) {
        ibSize = sizeof(uint16_t) * indexCount_;
    }
    else {
        ibSize = sizeof(uint32_t) * indexCount_;
    }

    indexBuffer_ = up.CreateBufferWithData(indices, ibSize, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COMMON); // see VB note: COMMON + implicit promotion for every reader
    indexBufferView_.BufferLocation = indexBuffer_->GetGPUVirtualAddress();
    indexBufferView_.SizeInBytes = ibSize;
    indexBufferView_.Format = indexFormat_;

    up.StealKeepAlive(uploadKeepAlive);
}

// Batch path for the new format plus TBN generation
void Mesh::CreateGPU_PNTUV(ID3D12Device* device,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
    std::vector<VertexPNTUV>& verts,
    const uint32_t* indices, UINT indexCount,
    bool generateTangentSpace,
    const std::vector<Submesh>* submeshes)
{
    if (generateTangentSpace) {
        GenerateNormalsTangents(verts, indices, indexCount);
    }

    CreateGPUFlexible(device, uploadCmdList, uploadKeepAlive,
        verts.data(), (UINT)verts.size(), sizeof(VertexPNTUV),
        indices, indexCount, DXGI_FORMAT_R32_UINT);

    // Override the default single submesh with the caller's multi-material table (B2 glTF path).
    if (submeshes && !submeshes->empty()) {
        submeshes_ = *submeshes;
    }

    AABB bounds = AABB::Empty();
    for (const auto& vert : verts)
    {
        bounds.Expand(Math::float3(vert.position.x, vert.position.y, vert.position.z));
    }
    bounds_ = bounds;

#if WITH_EDITOR
    raycastPositions_.reserve(verts.size());
    for (const VertexPNTUV& vert : verts)
    {
        raycastPositions_.emplace_back(vert.position.x, vert.position.y, vert.position.z);
    }
    if (indices && indexCount >= 3)
    {
        raycastIndices_.assign(indices, indices + indexCount);
    }
#endif
}

#if WITH_EDITOR
bool Mesh::RaycastLocal(const Math::float3& origin, const Math::float3& direction,
    float* outDistance) const
{
    float bestDistance = FLT_MAX;
    if (outDistance)
    {
        *outDistance = bestDistance;
    }
    if (!HasRaycastTriangles() || direction.Dot(direction) < 1.0e-16f)
    {
        return false;
    }

    constexpr float kDeterminantEpsilon = 1.0e-8f;
    constexpr float kBarycentricEpsilon = 1.0e-6f;
    for (size_t i = 0; i + 2 < raycastIndices_.size(); i += 3)
    {
        const uint32_t i0 = raycastIndices_[i];
        const uint32_t i1 = raycastIndices_[i + 1];
        const uint32_t i2 = raycastIndices_[i + 2];
        if (i0 >= raycastPositions_.size() ||
            i1 >= raycastPositions_.size() ||
            i2 >= raycastPositions_.size())
        {
            continue;
        }

        const Math::float3& v0 = raycastPositions_[i0];
        const Math::float3 edge1 = raycastPositions_[i1] - v0;
        const Math::float3 edge2 = raycastPositions_[i2] - v0;
        const Math::float3 p = direction.Cross(edge2);
        const float determinant = edge1.Dot(p);
        if (std::fabs(determinant) < kDeterminantEpsilon)
        {
            continue;
        }

        const float inverseDeterminant = 1.0f / determinant;
        const Math::float3 fromV0 = origin - v0;
        const float u = fromV0.Dot(p) * inverseDeterminant;
        if (u < -kBarycentricEpsilon || u > 1.0f + kBarycentricEpsilon)
        {
            continue;
        }

        const Math::float3 q = fromV0.Cross(edge1);
        const float v = direction.Dot(q) * inverseDeterminant;
        if (v < -kBarycentricEpsilon || u + v > 1.0f + kBarycentricEpsilon)
        {
            continue;
        }

        const float distance = edge2.Dot(q) * inverseDeterminant;
        if (distance >= 0.0f && distance < bestDistance)
        {
            bestDistance = distance;
        }
    }

    if (bestDistance == FLT_MAX)
    {
        return false;
    }
    if (outDistance)
    {
        *outDistance = bestDistance;
    }
    return true;
}
#endif

void Mesh::SelectLod(UINT lod, const D3D12_VERTEX_BUFFER_VIEW*& vbv,
    const D3D12_INDEX_BUFFER_VIEW*& ibv, UINT& indexCount) const {
    vbv = &vertexBufferView_; // LODs share the base vertex buffer (simplify only cuts indices)

    // g_lodEnabled off -> full detail; else g_forcedLod (>=0) overrides per-object selection.
    UINT effectiveLod = lod;
    if (!render::g_lodEnabled) { effectiveLod = 0u; }
    else if (render::g_forcedLod >= 0) { effectiveLod = static_cast<UINT>(render::g_forcedLod); }

    if (effectiveLod == 0 || extraLods_.empty()) {
        ibv = &indexBufferView_;
        indexCount = indexCount_;
        return;
    }
    const UINT idx = std::min(effectiveLod - 1u, static_cast<UINT>(extraLods_.size()) - 1u);
    ibv = &extraLods_[idx].indexBufferView;
    indexCount = extraLods_[idx].indexCount;
}

void Mesh::AddLod(ID3D12Device* device, ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
    const uint32_t* indices, UINT indexCount,
    const std::vector<Submesh>& submeshes) {
    if (!indices || indexCount == 0) { return; }
    UploadManager up(device, uploadCmdList);
    LodLevel lod;
    const UINT ibSize = sizeof(uint32_t) * indexCount;
    lod.indexBuffer = up.CreateBufferWithData(indices, ibSize, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_INDEX_BUFFER);
    lod.indexBufferView.BufferLocation = lod.indexBuffer->GetGPUVirtualAddress();
    lod.indexBufferView.SizeInBytes = ibSize;
    lod.indexBufferView.Format = DXGI_FORMAT_R32_UINT;
    lod.indexCount = indexCount;
    lod.submeshes = submeshes.empty() ? std::vector<Submesh>{ Submesh{ 0u, indexCount, 0u } } : submeshes;
    extraLods_.push_back(std::move(lod));
    up.StealKeepAlive(uploadKeepAlive);
}

const std::vector<Mesh::Submesh>& Mesh::SubmeshesForLod(UINT lod) const {
    // Mirror SelectLod's resolution: 0 (or LODs disabled/none) -> base table; else clamp to the
    // coarsest available extra LOD.
    UINT effectiveLod = lod;
    if (!render::g_lodEnabled) { effectiveLod = 0u; }
    else if (render::g_forcedLod >= 0) { effectiveLod = static_cast<UINT>(render::g_forcedLod); }

    if (effectiveLod == 0 || extraLods_.empty()) { return submeshes_; }
    const UINT idx = std::min(effectiveLod - 1u, static_cast<UINT>(extraLods_.size()) - 1u);
    return extraLods_[idx].submeshes;
}

// Step 3: skip redundant IA binds when this mesh's buffers/topology already match the
// per-command-list bind cache (e.g. a sorted run of the same mesh). IA state persists on
// the command list and is unaffected by root-signature changes.
void Mesh::BindIA(ID3D12GraphicsCommandList* cmdList,
    const D3D12_VERTEX_BUFFER_VIEW& vbv, const D3D12_INDEX_BUFFER_VIEW& ibv) const {
    render::CommandListBindState& cache = render::g_clBindState;
    const bool batch = render::g_bindBatchingEnabled;
    if (!batch || cache.vb != vbv.BufferLocation) {
        cmdList->IASetVertexBuffers(0, 1, &vbv);
        if (batch) { cache.vb = vbv.BufferLocation; }
    }
    if (!batch || cache.ib != ibv.BufferLocation) {
        cmdList->IASetIndexBuffer(&ibv);
        if (batch) { cache.ib = ibv.BufferLocation; }
    }
    if (!batch || cache.topology != D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (batch) { cache.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; }
    }
}

void Mesh::Draw(ID3D12GraphicsCommandList* cmdList, UINT lod) const {
    const D3D12_VERTEX_BUFFER_VIEW* vbv; const D3D12_INDEX_BUFFER_VIEW* ibv; UINT indexCount;
    SelectLod(lod, vbv, ibv, indexCount);
    BindIA(cmdList, *vbv, *ibv);
    cmdList->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
    render::g_renderStats.AddDraw(indexCount, 1);
}

void Mesh::DrawSubmesh(ID3D12GraphicsCommandList* cmdList, UINT submeshOrdinal, UINT lod) const {
    const D3D12_VERTEX_BUFFER_VIEW* vbv; const D3D12_INDEX_BUFFER_VIEW* ibv; UINT indexCount;
    SelectLod(lod, vbv, ibv, indexCount);
    const std::vector<Submesh>& subs = SubmeshesForLod(lod); // same lod resolution as SelectLod
    if (submeshOrdinal >= subs.size()) { return; }
    const Submesh& s = subs[submeshOrdinal];
    BindIA(cmdList, *vbv, *ibv); // repeated binds across submeshes are elided by the bind cache
    cmdList->DrawIndexedInstanced(s.indexCount, 1, s.indexOffset, 0, 0);
    render::g_renderStats.AddDraw(s.indexCount, 1);
}

void Mesh::DrawSubmeshInstanced(ID3D12GraphicsCommandList* cmdList, UINT submeshOrdinal,
    UINT instanceCount, UINT lod) const {
    const D3D12_VERTEX_BUFFER_VIEW* vbv; const D3D12_INDEX_BUFFER_VIEW* ibv; UINT indexCount;
    SelectLod(lod, vbv, ibv, indexCount);
    const std::vector<Submesh>& subs = SubmeshesForLod(lod); // same lod resolution as SelectLod
    if (submeshOrdinal >= subs.size()) { return; }
    const Submesh& s = subs[submeshOrdinal];
    BindIA(cmdList, *vbv, *ibv); // repeated binds across submeshes are elided by the bind cache
    cmdList->DrawIndexedInstanced(s.indexCount, instanceCount, s.indexOffset, 0, 0);
    render::g_renderStats.AddDraw(s.indexCount, instanceCount);
}

void Mesh::DrawInstanced(ID3D12GraphicsCommandList* cmdList, UINT instanceCount, UINT lod) const {
    const D3D12_VERTEX_BUFFER_VIEW* vbv; const D3D12_INDEX_BUFFER_VIEW* ibv; UINT indexCount;
    SelectLod(lod, vbv, ibv, indexCount);
    BindIA(cmdList, *vbv, *ibv);
    cmdList->DrawIndexedInstanced(indexCount, instanceCount, 0, 0, 0);
    render::g_renderStats.AddDraw(indexCount, instanceCount);
}

// ====== Normal and tangent generation ======
static inline XMVECTOR SafeNormalize(XMVECTOR v) {
    const float eps = 1e-6f;
    XMFLOAT3 f; XMStoreFloat3(&f, v);
    float len2 = f.x * f.x + f.y * f.y + f.z * f.z;
    if (len2 < eps) {
        return XMVectorSet(0, 1, 0, 0);
    }
    return XMVector3Normalize(v);
}

void Mesh::GenerateNormalsTangents(std::vector<VertexPNTUV>& verts,
    const uint32_t* indices, UINT indexCount)
{
    const UINT vcount = (UINT)verts.size();
    std::vector<XMFLOAT3> accN(vcount, XMFLOAT3{ 0,0,0 });
    std::vector<XMFLOAT3> accT(vcount, XMFLOAT3{ 0,0,0 });
    std::vector<XMFLOAT3> accB(vcount, XMFLOAT3{ 0,0,0 });

    // 1) Accumulate per triangle
    for (UINT i = 0; i < indexCount; i += 3) {
        uint32_t i0 = indices[i + 0];
        uint32_t i1 = indices[i + 1];
        uint32_t i2 = indices[i + 2];

        const XMFLOAT3& p0 = verts[i0].position;
        const XMFLOAT3& p1 = verts[i1].position;
        const XMFLOAT3& p2 = verts[i2].position;

        const XMFLOAT2& w0 = verts[i0].uv;
        const XMFLOAT2& w1 = verts[i1].uv;
        const XMFLOAT2& w2 = verts[i2].uv;

        XMVECTOR P0 = XMLoadFloat3(&p0);
        XMVECTOR P1 = XMLoadFloat3(&p1);
        XMVECTOR P2 = XMLoadFloat3(&p2);
        XMVECTOR e1 = XMVectorSubtract(P1, P0);
        XMVECTOR e2 = XMVectorSubtract(P2, P0);

        float du1 = w1.x - w0.x;
        float dv1 = w1.y - w0.y;
        float du2 = w2.x - w0.x;
        float dv2 = w2.y - w0.y;
        float r = (du1 * dv2 - du2 * dv1);
        if (fabsf(r) < 1e-8f) {
            r = 1.0f;
        }
        else {
            r = 1.0f / r;
        }

        XMVECTOR T = XMVectorScale(XMVectorSubtract(XMVectorScale(e1, dv2), XMVectorScale(e2, dv1)), r);
        XMVECTOR B = XMVectorScale(XMVectorSubtract(XMVectorScale(e2, du1), XMVectorScale(e1, du2)), r);
        XMVECTOR N = XMVector3Normalize(XMVector3Cross(e1, e2));

        auto add3 = [](XMFLOAT3& a, XMVECTOR v) {
            XMFLOAT3 t; XMStoreFloat3(&t, v);
            a.x += t.x; a.y += t.y; a.z += t.z;
            };
        add3(accN[i0], N); add3(accN[i1], N); add3(accN[i2], N);
        add3(accT[i0], T); add3(accT[i1], T); add3(accT[i2], T);
        add3(accB[i0], B); add3(accB[i1], B); add3(accB[i2], B);
    }

    // 2) Normalize and orthogonalize; tangent.w = sign(bitangent)
    for (UINT i = 0; i < vcount; ++i) {
        XMVECTOR n = SafeNormalize(XMLoadFloat3(&accN[i]));
        XMVECTOR t = SafeNormalize(XMLoadFloat3(&accT[i]));
        XMVECTOR b = SafeNormalize(XMLoadFloat3(&accB[i]));

        // If the source data already provided a normal, prefer it
        if (verts[i].normal.x != 0 || verts[i].normal.y != 0 || verts[i].normal.z != 0) {
            n = SafeNormalize(XMLoadFloat3(&verts[i].normal));
        }

        // Gram-Schmidt: t = normalize(t - n * dot(n,t))
        XMVECTOR tGS = SafeNormalize(XMVectorSubtract(t, XMVectorScale(n, XMVectorGetX(XMVector3Dot(n, t)))));
        // handedness = sign( dot( cross(n,tGS), b ) )
        float sign = XMVectorGetX(XMVector3Dot(XMVector3Cross(n, tGS), b)) < 0.0f ? -1.0f : +1.0f;

        XMStoreFloat3(&verts[i].normal, n);
        XMFLOAT4 tan;
        XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(&tan), tGS);
        tan.w = sign;
        verts[i].tangent = tan;
    }
}
