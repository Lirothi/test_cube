#include "rendering/meshes/MeshManager.h"
#include "rendering/core/Renderer.h"
#include <fstream>
#include <sstream>
#include <cctype>
#include <algorithm>
#include "third_party/robin_hood.h"
#include <cstring> // strchr, atoi
#include <cstdint>
#include <cstdio>
#include <filesystem> // W7.1b binary mesh cache
#include <DirectXMath.h>
#include <queue>
#include "meshoptimizer.h"
#include "third_party/cgltf/cgltf.h"
#include <Windows.h> // OutputDebugStringA for load diagnostics

using namespace DirectX;

namespace
{
// One LOD as CPU arrays: indices over the SAME base vertices + its own submesh table. Shared by
// the runtime GenerateLods (uploads) and the W7.1b bake (serializes).
struct MeshLodCpu {
    std::vector<uint32_t> indices;
    std::vector<Mesh::Submesh> submeshes;
};

// Step 6 / Part B: build coarser LODs as reduced index buffers (meshopt_simplify, over the same
// vertices). Each submesh range is simplified INDEPENDENTLY and the LOD carries its own rebuilt
// submesh table — simplifying the whole buffer as one blob would dissolve the per-material
// boundaries. Single-submesh meshes reduce to the original behavior. CPU-only (no GPU).
std::vector<MeshLodCpu> BuildLodsCpu(const std::vector<VertexPNTUV>& verts,
    const std::vector<uint32_t>& indices, const std::vector<Mesh::Submesh>& baseSubs)
{
    std::vector<MeshLodCpu> out;
    const size_t baseIdx = indices.size();
    constexpr size_t kMinIndicesForLod = 384;  // ~128 tris; skip tiny meshes (box = 6 tris)
    constexpr size_t kMinRangeIndices  = 96;    // per-range floor; smaller ranges copy through
    if (verts.empty() || baseIdx < kMinIndicesForLod || baseSubs.empty()) { return out; }

    const float ratios[] = { 0.5f, 0.25f, 0.12f };
    const float errors[] = { 0.02f, 0.05f, 0.12f };
    std::vector<uint32_t> simplified;
    size_t prevCount = baseIdx;

    for (int i = 0; i < 3; ++i)
    {
        MeshLodCpu lod;
        for (const Mesh::Submesh& s : baseSubs)
        {
            const uint32_t* src = indices.data() + s.indexOffset;
            const size_t srcCount = s.indexCount;
            const uint32_t outOffset = static_cast<uint32_t>(lod.indices.size());

            size_t n = srcCount;
            bool didSimplify = false;
            size_t target = static_cast<size_t>(srcCount * ratios[i]);
            target -= target % 3;
            if (srcCount >= kMinRangeIndices && target >= 12)
            {
                simplified.resize(srcCount);
                float resultError = 0.0f;
                n = meshopt_simplify(simplified.data(), src, srcCount,
                    &verts[0].position.x, verts.size(), sizeof(VertexPNTUV),
                    target, errors[i], 0, &resultError);
                if (n == 0) { n = srcCount; }  // simplify gave up -> keep the range as-is
                else { didSimplify = true; }
                (void)resultError;
            }

            const uint32_t* from = didSimplify ? simplified.data() : src;
            lod.indices.insert(lod.indices.end(), from, from + n);
            lod.submeshes.push_back(Mesh::Submesh{ outOffset, static_cast<uint32_t>(n), s.materialSlot });
        }

        // Overall shrink gate (same spirit as before): stop once a level is < ~10% smaller.
        if (lod.indices.empty() || lod.indices.size() + (lod.indices.size() / 10) >= prevCount) { break; }
        prevCount = lod.indices.size();
        out.push_back(std::move(lod));
    }
    return out;
}

// Called once at load on the upload command list (runtime fallback path).
void GenerateLods(Mesh* mesh, ID3D12Device* device, ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* keepAlive,
    const std::vector<VertexPNTUV>& verts, const std::vector<uint32_t>& indices)
{
    if (!mesh) { return; }
    for (const MeshLodCpu& lod : BuildLodsCpu(verts, indices, mesh->GetSubmeshes()))
    {
        mesh->AddLod(device, uploadCmdList, keepAlive,
            lod.indices.data(), static_cast<UINT>(lod.indices.size()), lod.submeshes);
    }
}

// ---------- W7.1b: baked binary mesh cache (cache/meshes/<hash>.mesh.bin) ----------
std::vector<uint32_t> CanonicalNormalSlots(const std::vector<uint32_t>& slots); // defined below
constexpr uint32_t kMeshBinMagic   = 0x4253484Du; // 'MSHB'
constexpr uint32_t kMeshBinVersion = 1u;          // bump on any bake-algo / format change -> stale

struct MeshBinHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t sourceHash;   // FNV-1a of the geometry file bytes (freshness vs the source glTF/glb)
    uint64_t optionsHash;  // FNV-1a of the load options that change geometry (recomputeNormalSlots, tangents)
    uint32_t vertexCount;
    uint32_t lodCount;     // includes LOD 0
};

uint64_t Fnv1a(const void* data, size_t n, uint64_t h = 1469598103934665603ull)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

std::string GeometryFilePart(const std::string& path)
{
    const size_t frag = path.find('#');
    return frag == std::string::npos ? path : path.substr(0, frag);
}

uint64_t HashSourceFile(const std::string& pathWithFragment)
{
    std::ifstream f(GeometryFilePart(pathWithFragment), std::ios::binary);
    if (!f) { return 0; }
    std::vector<char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return buf.empty() ? 0 : Fnv1a(buf.data(), buf.size());
}

uint64_t HashOptions(const MeshLoadOptions& opt)
{
    const std::vector<uint32_t> slots = CanonicalNormalSlots(opt.recomputeNormalSlots);
    const uint8_t tangents = opt.generateTangentSpace ? 1u : 0u;
    uint64_t h = Fnv1a(&tangents, 1);
    if (!slots.empty()) { h = Fnv1a(slots.data(), slots.size() * sizeof(uint32_t), h); }
    return h;
}

bool WriteMeshBinary(const std::string& binPath, uint64_t sourceHash, uint64_t optionsHash,
    const std::vector<VertexPNTUV>& verts,
    const std::vector<uint32_t>& lod0Indices, const std::vector<Mesh::Submesh>& lod0Subs,
    const std::vector<MeshLodCpu>& extraLods)
{
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(binPath).parent_path(), ec);
    std::ofstream f(binPath, std::ios::binary | std::ios::trunc);
    if (!f) { return false; }

    MeshBinHeader h{ kMeshBinMagic, kMeshBinVersion, sourceHash, optionsHash,
        static_cast<uint32_t>(verts.size()), 1u + static_cast<uint32_t>(extraLods.size()) };
    f.write(reinterpret_cast<const char*>(&h), sizeof(h));
    f.write(reinterpret_cast<const char*>(verts.data()),
        static_cast<std::streamsize>(verts.size() * sizeof(VertexPNTUV)));

    const auto writeLod = [&](const std::vector<uint32_t>& idx, const std::vector<Mesh::Submesh>& subs)
    {
        const uint32_t ic = static_cast<uint32_t>(idx.size());
        const uint32_t sc = static_cast<uint32_t>(subs.size());
        f.write(reinterpret_cast<const char*>(&ic), sizeof(ic));
        f.write(reinterpret_cast<const char*>(&sc), sizeof(sc));
        f.write(reinterpret_cast<const char*>(idx.data()), static_cast<std::streamsize>(ic * sizeof(uint32_t)));
        f.write(reinterpret_cast<const char*>(subs.data()), static_cast<std::streamsize>(sc * sizeof(Mesh::Submesh)));
    };
    writeLod(lod0Indices, lod0Subs);
    for (const MeshLodCpu& l : extraLods) { writeLod(l.indices, l.submeshes); }
    return f.good();
}

// Reads + validates a .bin. `expectSourceHash`/`expectOptionsHash` null => skip that freshness
// check (a directly-referenced committed .bin has no runtime source to compare against; only the
// magic + format version matter). `lods[0]` = LOD 0.
bool ReadMeshBinary(const std::string& binPath, const uint64_t* expectSourceHash,
    const uint64_t* expectOptionsHash, std::vector<VertexPNTUV>& verts, std::vector<MeshLodCpu>& lods)
{
    std::ifstream f(binPath, std::ios::binary);
    if (!f) { return false; }
    MeshBinHeader h{};
    f.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (!f || h.magic != kMeshBinMagic || h.version != kMeshBinVersion ||
        (expectSourceHash && h.sourceHash != *expectSourceHash) ||
        (expectOptionsHash && h.optionsHash != *expectOptionsHash) ||
        h.vertexCount == 0 || h.lodCount == 0)
    {
        return false; // missing/stale -> caller falls back to the runtime parse
    }
    verts.resize(h.vertexCount);
    f.read(reinterpret_cast<char*>(verts.data()),
        static_cast<std::streamsize>(h.vertexCount * sizeof(VertexPNTUV)));
    lods.resize(h.lodCount);
    for (uint32_t i = 0; i < h.lodCount && f; ++i)
    {
        uint32_t ic = 0, sc = 0;
        f.read(reinterpret_cast<char*>(&ic), sizeof(ic));
        f.read(reinterpret_cast<char*>(&sc), sizeof(sc));
        if (!f) { return false; }
        lods[i].indices.resize(ic);
        lods[i].submeshes.resize(sc);
        f.read(reinterpret_cast<char*>(lods[i].indices.data()), static_cast<std::streamsize>(ic * sizeof(uint32_t)));
        f.read(reinterpret_cast<char*>(lods[i].submeshes.data()), static_cast<std::streamsize>(sc * sizeof(Mesh::Submesh)));
    }
    return static_cast<bool>(f);
}

std::vector<uint32_t> CanonicalNormalSlots(const std::vector<uint32_t>& slots)
{
    std::vector<uint32_t> result = slots;
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::string MeshCacheKey(const std::string& path, const MeshLoadOptions& opt)
{
    const std::vector<uint32_t> slots = CanonicalNormalSlots(opt.recomputeNormalSlots);
    if (slots.empty()) { return path; }

    std::string key = path + "|recomputeNormalSlots=";
    for (const uint32_t slot : slots)
    {
        key += std::to_string(slot);
        key.push_back(',');
    }
    return key;
}

void DiscardNormalsForSlots(std::vector<VertexPNTUV>& vertices,
    const std::vector<uint32_t>& indices,
    const std::vector<Mesh::Submesh>& submeshes,
    const std::vector<uint32_t>& requestedSlots)
{
    const std::vector<uint32_t> slots = CanonicalNormalSlots(requestedSlots);
    if (vertices.empty() || indices.empty() || slots.empty()) { return; }

    const auto clearRange = [&](size_t offset, size_t count)
    {
        if (offset >= indices.size()) { return; }
        const size_t end = offset + std::min(count, indices.size() - offset);
        for (size_t i = offset; i < end; ++i)
        {
            const uint32_t vertexIndex = indices[i];
            if (vertexIndex < vertices.size())
            {
                vertices[vertexIndex].normal = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
            }
        }
    };

    if (submeshes.empty())
    {
        if (std::binary_search(slots.begin(), slots.end(), 0u))
        {
            clearRange(0, indices.size());
        }
        return;
    }

    for (const Mesh::Submesh& submesh : submeshes)
    {
        if (std::binary_search(slots.begin(), slots.end(), submesh.materialSlot))
        {
            clearRange(submesh.indexOffset, submesh.indexCount);
        }
    }
}
} // namespace

using Microsoft::WRL::ComPtr;

static inline void trim(std::string& s) {
    struct {
        static bool ns(int ch) { return !std::isspace(static_cast<unsigned char>(ch)); }
    } L;
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), L.ns));
    s.erase(std::find_if(s.rbegin(), s.rend(), L.ns).base(), s.end());
}

static inline bool ieq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) { return false; }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

static inline std::string tolower_str(std::string s) {
    for (size_t i = 0; i < s.size(); ++i) {
        s[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    }
    return s;
}

bool MeshManager::BakeToBinary(const std::string& srcPath, const std::string& outBinPath,
    const MeshLoadOptions& opt)
{
    MeshCpuData cpu;
    if (!ParseFileCpu(srcPath, cpu, opt)) { return false; } // parse glTF + regen normals/tangents (CPU)

    // TODO W7.2: bake the geodesic wind weight into cpu.vertices[i].color here (currently 0).

    std::vector<Mesh::Submesh> lod0Subs = cpu.submeshes;
    if (lod0Subs.empty())
    {
        lod0Subs.push_back(Mesh::Submesh{ 0u, static_cast<uint32_t>(cpu.indices.size()), 0u });
    }
    const std::vector<MeshLodCpu> extra = BuildLodsCpu(cpu.vertices, cpu.indices, lod0Subs);
    const bool ok = WriteMeshBinary(outBinPath, HashSourceFile(srcPath), HashOptions(opt),
        cpu.vertices, cpu.indices, lod0Subs, extra);
    char msg[512];
    std::snprintf(msg, sizeof(msg), "[meshbake] %s '%s' -> '%s' (%zu verts, %zu LODs)\n",
        ok ? "ok" : "FAILED", srcPath.c_str(), outBinPath.c_str(), cpu.vertices.size(), extra.size() + 1);
    OutputDebugStringA(msg);
    return ok;
}

std::shared_ptr<Mesh> MeshManager::LoadBinaryDirect(const std::string& binPath,
    Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    if (!renderer || !uploadCmdList) { return nullptr; }
    std::vector<VertexPNTUV> verts;
    std::vector<MeshLodCpu> lods;
    if (!ReadMeshBinary(binPath, nullptr, nullptr, verts, lods) || verts.empty() || lods.empty())
    {
        OutputDebugStringA(("[meshbin] FAILED to read '" + binPath + "'\n").c_str());
        return nullptr;
    }
    std::shared_ptr<Mesh> mesh = std::make_shared<Mesh>();
    mesh->CreateGPU_PNTUV(renderer->GetDevice(), uploadCmdList, uploadKeepAlive,
        verts, lods[0].indices.data(), static_cast<UINT>(lods[0].indices.size()),
        /*generateTangentSpace=*/false, &lods[0].submeshes);
    for (size_t i = 1; i < lods.size(); ++i)
    {
        mesh->AddLod(renderer->GetDevice(), uploadCmdList, uploadKeepAlive,
            lods[i].indices.data(), static_cast<UINT>(lods[i].indices.size()), lods[i].submeshes);
    }
    return mesh;
}

std::shared_ptr<Mesh> MeshManager::Load(const std::string& path,
    Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive,
    const MeshLoadOptions& opt)
{
    // New pipeline: geometry referenced directly as our committed .mesh.bin (glTF stays in
    // import_staging/, never loaded at runtime). No hash cache + no glTF fallback — the .bin IS the
    // shipped geometry.
    {
        const std::string geom = GeometryFilePart(path); // .mesh.bin never carries a #fragment
        if (geom.size() >= 9 && geom.compare(geom.size() - 9, 9, ".mesh.bin") == 0)
        {
            const std::string memKeyBin = MeshCacheKey(path, opt);
            if (auto cit = cache_.find(memKeyBin); cit != cache_.end()) { return cit->second; }
            std::shared_ptr<Mesh> m = LoadBinaryDirect(geom, renderer, uploadCmdList, uploadKeepAlive);
            if (m) { cache_[memKeyBin] = m; }
            return m;
        }
    }

    // In-memory cache (the sub-loaders also cache; this short-circuits repeat loads).
    const std::string memKey = MeshCacheKey(path, opt);
    if (auto cit = cache_.find(memKey); cit != cache_.end()) { return cit->second; }

    // Match the extension on the file part only (a "#fragment" selector may follow, e.g. .glb#2).
    std::string low = tolower_str(path);
    const size_t frag = low.find('#');
    const std::string ext = (frag == std::string::npos) ? low : low.substr(0, frag);
    auto endsWith = [&](const char* s, size_t n) {
        return ext.size() >= n && ext.compare(ext.size() - n, n, s) == 0;
    };
    if (endsWith(".obj", 4)) {
        return LoadOBJ(path, renderer, uploadCmdList, uploadKeepAlive, opt);
    }
    else if (endsWith(".gltf", 5) || endsWith(".glb", 4)) {
        return LoadGltf(path, renderer, uploadCmdList, uploadKeepAlive, opt);
    }
    else {
        return LoadText(path, renderer, uploadCmdList, uploadKeepAlive, opt);
    }
}

bool MeshManager::ParseFileCpu(const std::string& path, MeshCpuData& out,
    const MeshLoadOptions& opt)
{
    out.vertices.clear();
    out.indices.clear();
    out.submeshes.clear();

    std::string low = tolower_str(path);
    const size_t fragment = low.find('#');
    const std::string filePart = fragment == std::string::npos ? low : low.substr(0, fragment);
    const auto endsWith = [&filePart](const char* suffix)
    {
        const size_t length = std::strlen(suffix);
        return filePart.size() >= length &&
            filePart.compare(filePart.size() - length, length, suffix) == 0;
    };

    // W7.1b: our baked binary geometry (glTF lives in import_staging/). Read LOD0's verts/indices/
    // submeshes directly — they already carry regenerated normals/tangents (+ the wind color), so
    // return WITHOUT re-parsing or regenerating. Editor CPU consumers (thumbnails, mesh preview) go
    // through here, so they get the baked mesh instead of failing on the binary as "text".
    if (endsWith(".mesh.bin"))
    {
        std::vector<MeshLodCpu> lods;
        if (!ReadMeshBinary(GeometryFilePart(path), nullptr, nullptr, out.vertices, lods) ||
            out.vertices.empty() || lods.empty())
        {
            out = {};
            return false;
        }
        out.indices = std::move(lods[0].indices);
        out.submeshes = std::move(lods[0].submeshes);
        return true;
    }

    bool parsed = false;
    if (endsWith(".obj"))
    {
        parsed = ParseOBJFile(path, out.vertices, out.indices, opt);
    }
    else if (endsWith(".gltf") || endsWith(".glb"))
    {
        parsed = ParseGltfFile(path, out.vertices, out.indices, out.submeshes, opt);
    }
    else
    {
        parsed = ParseTextFile(path, out.vertices, out.indices, opt);
    }

    if (!parsed || out.vertices.empty() || out.indices.empty())
    {
        out = {};
        return false;
    }
    if (opt.generateTangentSpace || !opt.recomputeNormalSlots.empty())
    {
        DiscardNormalsForSlots(out.vertices, out.indices, out.submeshes,
            opt.recomputeNormalSlots);
        Mesh::GenerateNormalsTangents(out.vertices, out.indices.data(),
            static_cast<UINT>(out.indices.size()));
    }
    return true;
}

std::shared_ptr<Mesh> MeshManager::CreateFromCpuData(const std::string& key,
    Renderer* renderer,
    const MeshCpuData& data,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    const auto cached = cache_.find(key);
    if (cached != cache_.end()) { return cached->second; }
    if (!renderer || !uploadCmdList || data.vertices.empty() || data.indices.empty())
    {
        return nullptr;
    }

    std::vector<VertexPNTUV> vertices = data.vertices;
    std::shared_ptr<Mesh> mesh = std::make_shared<Mesh>();
    mesh->CreateGPU_PNTUV(renderer->GetDevice(), uploadCmdList, uploadKeepAlive,
        vertices, data.indices.data(), static_cast<UINT>(data.indices.size()),
        false, data.submeshes.empty() ? nullptr : &data.submeshes);
    cache_[key] = mesh;
    return mesh;
}

std::shared_ptr<Mesh> MeshManager::LoadText(const std::string& path,
    Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive,
    const MeshLoadOptions& opt)
{
    const std::string cacheKey = MeshCacheKey(path, opt);
    robin_hood::unordered_map<std::string, std::shared_ptr<Mesh>>::iterator it = cache_.find(cacheKey);
    if (it != cache_.end()) {
        return it->second;
    }

    std::vector<VertexPNTUV> verts;
    std::vector<uint32_t>    inds;
    if (!ParseTextFile(path, verts, inds, opt)) {
        return std::shared_ptr<Mesh>();
    }

    DiscardNormalsForSlots(verts, inds, {}, opt.recomputeNormalSlots);
    std::shared_ptr<Mesh> m = std::make_shared<Mesh>();
    m->CreateGPU_PNTUV(renderer->GetDevice(), uploadCmdList, uploadKeepAlive,
        verts, inds.data(), (UINT)inds.size(),
        opt.generateTangentSpace || !opt.recomputeNormalSlots.empty());
    GenerateLods(m.get(), renderer->GetDevice(), uploadCmdList, uploadKeepAlive, verts, inds);
    cache_[cacheKey] = m;
    return m;
}

std::shared_ptr<Mesh> MeshManager::LoadOBJ(const std::string& path,
    Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive,
    const MeshLoadOptions& opt)
{
    const std::string cacheKey = MeshCacheKey(path, opt);
    robin_hood::unordered_map<std::string, std::shared_ptr<Mesh>>::iterator it = cache_.find(cacheKey);
    if (it != cache_.end()) {
        return it->second;
    }

    std::vector<VertexPNTUV> verts;
    std::vector<uint32_t>    inds;
    if (!ParseOBJFile(path, verts, inds, opt)) {
        return std::shared_ptr<Mesh>();
    }

    DiscardNormalsForSlots(verts, inds, {}, opt.recomputeNormalSlots);
    std::shared_ptr<Mesh> m = std::make_shared<Mesh>();
    m->CreateGPU_PNTUV(renderer->GetDevice(), uploadCmdList, uploadKeepAlive,
        verts, inds.data(), (UINT)inds.size(),
        opt.generateTangentSpace || !opt.recomputeNormalSlots.empty());
    GenerateLods(m.get(), renderer->GetDevice(), uploadCmdList, uploadKeepAlive, verts, inds);
    cache_[cacheKey] = m;
    return m;
}

std::shared_ptr<Mesh> MeshManager::LoadGltf(const std::string& path,
    Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive,
    const MeshLoadOptions& opt)
{
    // Cache by the full path INCLUDING the fragment and normal-recompute policy.
    const std::string cacheKey = MeshCacheKey(path, opt);
    robin_hood::unordered_map<std::string, std::shared_ptr<Mesh>>::iterator it = cache_.find(cacheKey);
    if (it != cache_.end()) {
        return it->second;
    }

    std::vector<VertexPNTUV> verts;
    std::vector<uint32_t>    inds;
    std::vector<Mesh::Submesh> submeshes;
    if (!ParseGltfFile(path, verts, inds, submeshes, opt)) {
        return std::shared_ptr<Mesh>();
    }

    DiscardNormalsForSlots(verts, inds, submeshes, opt.recomputeNormalSlots);
    std::shared_ptr<Mesh> m = std::make_shared<Mesh>();
    m->CreateGPU_PNTUV(renderer->GetDevice(), uploadCmdList, uploadKeepAlive,
        verts, inds.data(), (UINT)inds.size(),
        opt.generateTangentSpace || !opt.recomputeNormalSlots.empty(),
        submeshes.size() > 1 ? &submeshes : nullptr);
    GenerateLods(m.get(), renderer->GetDevice(), uploadCmdList, uploadKeepAlive, verts, inds);
    cache_[cacheKey] = m;
    return m;
}

std::shared_ptr<Mesh> MeshManager::CreateFromMemory(const std::string& key,
    Renderer* renderer,
    const std::vector<VertexPNTUV>& vertsIn,
    const std::vector<uint32_t>& indices,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive,
    bool generateTangentSpace)
{
    robin_hood::unordered_map<std::string, std::shared_ptr<Mesh>>::iterator it = cache_.find(key);
    if (it != cache_.end()) {
        return it->second;
    }

    std::shared_ptr<Mesh> m = std::make_shared<Mesh>();
    std::vector<VertexPNTUV> verts = vertsIn; // CreateGPU_PNTUV may modify the data
    m->CreateGPU_PNTUV(renderer->GetDevice(), uploadCmdList, uploadKeepAlive,
        verts, indices.data(), (UINT)indices.size(), generateTangentSpace);
    GenerateLods(m.get(), renderer->GetDevice(), uploadCmdList, uploadKeepAlive, verts, indices);
    cache_[key] = m;
    return m;
}

std::shared_ptr<Mesh> MeshManager::Get(const std::string& key) const {
    robin_hood::unordered_map<std::string, std::shared_ptr<Mesh>>::const_iterator it = cache_.find(key);
    if (it != cache_.end()) {
        return it->second;
    }
    else {
        return std::shared_ptr<Mesh>();
    }
}

void MeshManager::Clear() {
    cache_.clear();
}

// ---------- Parsers ----------

static void addTri(std::vector<uint32_t>& I, uint32_t a, uint32_t b, uint32_t c, bool wantCW)
{
    if (wantCW)
    {
        I.push_back(a); I.push_back(c); I.push_back(b);
    }
    else
    {
        I.push_back(a); I.push_back(b); I.push_back(c);
    }
}

bool MeshManager::ParseTextFile(const std::string& path,
    std::vector<VertexPNTUV>& outVerts,
    std::vector<uint32_t>& outIndices,
    const MeshLoadOptions& opt)
{
    std::ifstream in(path.c_str());
    if (!in) {
        return false;
    }
    outVerts.clear();
    outIndices.clear();

    std::string line;
    int iBase = opt.iBase;
    bool wantCW = opt.wantCW;

    while (std::getline(in, line)) {
        size_t p1 = line.find('#');
        if (p1 != std::string::npos) {
            line.resize(p1);
        }
        size_t p2 = line.find("//");
        if (p2 != std::string::npos) {
            line.resize(p2);
        }
        trim(line);
        if (line.empty()) {
            continue;
        }

        std::istringstream ss(line);
        std::string op;
        ss >> op;

        if (ieq(op, "winding")) {
            std::string w;
            ss >> w;
            for (size_t i = 0; i < w.size(); ++i) {
                w[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(w[i])));
            }
            wantCW = (w != "ccw");
            continue;
        }
        if (ieq(op, "ibase")) {
            ss >> iBase;
            continue;
        }

        if (ieq(op, "v")) {
            VertexPNTUV v;
            v.position = DirectX::XMFLOAT3(0, 0, 0);
            v.normal = DirectX::XMFLOAT3(0, 0, 0);
            v.tangent = DirectX::XMFLOAT4(0, 0, 0, 0);
            v.uv = DirectX::XMFLOAT2(0, 0);

            ss >> v.position.x >> v.position.y >> v.position.z;
            if (!(ss >> v.uv.x >> v.uv.y)) {
                v.uv = DirectX::XMFLOAT2(0, 0);
                ss.clear();
            }
            if (!(ss >> v.normal.x >> v.normal.y >> v.normal.z)) {
                v.normal = DirectX::XMFLOAT3(0, 0, 0);
                ss.clear();
            }
            if (!(ss >> v.tangent.x >> v.tangent.y >> v.tangent.z >> v.tangent.w)) {
                v.tangent = DirectX::XMFLOAT4(0, 0, 0, 0);
            }
            outVerts.push_back(v);
            continue;
        }

        if (ieq(op, "i") || ieq(op, "tri")) {
            int a, b, c;
            ss >> a >> b >> c;
            addTri(outIndices, (uint32_t)(a - iBase), (uint32_t)(b - iBase), (uint32_t)(c - iBase), wantCW);
            continue;
        }
    }

    return !outVerts.empty() && !outIndices.empty();
}

struct OBJKey { int v, vt, vn; };
struct OBJKeyHash {
    size_t operator()(const OBJKey& k) const noexcept {
        return (size_t)k.v * 73856093u ^ (size_t)k.vt * 19349663u ^ (size_t)k.vn * 83492791u;
    }
};
static bool operator==(const OBJKey& a, const OBJKey& b) {
    return a.v == b.v && a.vt == b.vt && a.vn == b.vn;
}

bool MeshManager::ParseOBJFile(const std::string& path,
    std::vector<VertexPNTUV>& outVerts,
    std::vector<uint32_t>& outIndices,
    const MeshLoadOptions& opt)
{
    std::ifstream in(path.c_str());
    if (!in) {
        return false;
    }

    std::vector<DirectX::XMFLOAT3> pos;
    std::vector<DirectX::XMFLOAT2> uv;
    std::vector<DirectX::XMFLOAT3> nrm;

    robin_hood::unordered_map<OBJKey, uint32_t, OBJKeyHash> vmap;
    outVerts.clear();
    outIndices.clear();

    std::string line;
    while (std::getline(in, line)) {
        size_t p1 = line.find('#');
        if (p1 != std::string::npos) {
            line.resize(p1);
        }
        size_t p2 = line.find("//");
        if (p2 != std::string::npos) {
            line.resize(p2);
        }
        trim(line);
        if (line.empty()) {
            continue;
        }

        std::istringstream ss(line);
        std::string op;
        ss >> op;

        if (op == "v") {
            DirectX::XMFLOAT3 p(0, 0, 0);
            ss >> p.x >> p.y >> p.z;
            pos.push_back(p);
            continue;
        }
        if (op == "vt") {
            DirectX::XMFLOAT2 t(0, 0);
            ss >> t.x >> t.y;
            uv.push_back(t);
            continue;
        }
        if (op == "vn") {
            DirectX::XMFLOAT3 n(0, 0, 0);
            ss >> n.x >> n.y >> n.z;
            nrm.push_back(n);
            continue;
        }
        if (op == "f") {
            std::vector<OBJKey> face;
            std::string tok;
            while (ss >> tok) {
                int v = 0, vt = 0, vn = 0;
                const char* c = tok.c_str();
                v = std::atoi(c);

                const char* s = std::strchr(c, '/');
                if (s) {
                    if (*(s + 1) != '/' && *(s + 1) != '\0') {
                        vt = std::atoi(s + 1);
                    }
                    const char* s2 = std::strchr(s + 1, '/');
                    if (s2 && *(s2 + 1) != '\0') {
                        vn = std::atoi(s2 + 1);
                    }
                }
                OBJKey k; k.v = v; k.vt = vt; k.vn = vn;
                face.push_back(k);
            }

            if (face.size() < 3) {
                continue;
            }

            // Produce an ID for (v/vt/vn), creating a unique vertex when needed
            std::vector<uint32_t> id(face.size());
            for (size_t i = 0; i < face.size(); ++i) {
                robin_hood::unordered_map<OBJKey, uint32_t, OBJKeyHash>::iterator it = vmap.find(face[i]);
                if (it != vmap.end()) {
                    id[i] = it->second;
                }
                else {
                    VertexPNTUV vx;
                    vx.position = DirectX::XMFLOAT3(0, 0, 0);
                    vx.uv = DirectX::XMFLOAT2(0, 0);
                    vx.normal = DirectX::XMFLOAT3(0, 0, 0);
                    vx.tangent = DirectX::XMFLOAT4(0, 0, 0, 0);

                    if (face[i].v > 0 && (size_t)(face[i].v - 1) < pos.size()) {
                        vx.position = pos[face[i].v - 1];
                    }
                    if (face[i].vt > 0 && (size_t)(face[i].vt - 1) < uv.size()) {
                        vx.uv = uv[face[i].vt - 1];
                    }
                    if (face[i].vn > 0 && (size_t)(face[i].vn - 1) < nrm.size()) {
                        vx.normal = nrm[face[i].vn - 1];
                    }
                    uint32_t newId = (uint32_t)outVerts.size();
                    outVerts.push_back(vx);
                    vmap.emplace(std::make_pair(face[i], newId));
                    id[i] = newId;
                }
            }

            // Triangulate as a fan: (0,1,2), (0,2,3), ...
            for (size_t t = 1; t + 1 < id.size(); ++t) {
                addTri(outIndices, id[0], id[t], id[t + 1], opt.wantCW);
            }
        }
    }

    return !outVerts.empty() && !outIndices.empty();
}

// ---------- glTF / GLB (cgltf) ----------

namespace {

// A fragment selector parsed off the model path (see MeshManager::LoadGltf docs).
struct GltfSelector {
    std::string file;            // path with the "#..." fragment stripped
    bool        wholeFile = true;
    int         groupIndex = 0;  // "#N"
    std::string nodeName;        // "#node:Name" (empty = not a node selector)
};

GltfSelector ParseGltfSelector(const std::string& path) {
    GltfSelector s;
    const size_t hash = path.find('#');
    if (hash == std::string::npos) { s.file = path; return s; }
    s.file = path.substr(0, hash);
    s.wholeFile = false;
    const std::string frag = path.substr(hash + 1);
    const std::string nodePrefix = "node:";
    if (frag.rfind(nodePrefix, 0) == 0) {
        s.nodeName = frag.substr(nodePrefix.size());
    } else {
        s.groupIndex = std::atoi(frag.c_str());
    }
    return s;
}

void GltfLog(const std::string& msg) {
    OutputDebugStringA(("[gltf] " + msg + "\n").c_str());
}

constexpr cgltf_size kNoMat = static_cast<cgltf_size>(-1);

struct PrimRef { const cgltf_primitive* prim; float world[16]; };
struct GltfGroup { cgltf_size materialIndex = kNoMat; std::vector<PrimRef> prims; };

// Shared selector -> group resolution used by BOTH geometry load and material describe, so "#N"
// addresses the same group in both. Traverses the selected node subtree (or every root), buckets
// primitives by material, and orders groups by ascending glTF material index. No geometry read.
bool ResolveGltfGroups(cgltf_data* data, const GltfSelector& sel,
    std::vector<GltfGroup>& outGroups, std::string& err)
{
    outGroups.clear();

    std::vector<const cgltf_node*> stack;
    if (!sel.nodeName.empty()) {
        const cgltf_node* found = nullptr;
        for (cgltf_size i = 0; i < data->nodes_count; ++i) {
            if (data->nodes[i].name && sel.nodeName == data->nodes[i].name) { found = &data->nodes[i]; break; }
        }
        if (!found) { err = "node not found: '" + sel.nodeName + "'"; return false; }
        stack.push_back(found);
    } else {
        for (cgltf_size i = 0; i < data->nodes_count; ++i) {
            if (data->nodes[i].parent == nullptr) { stack.push_back(&data->nodes[i]); }
        }
    }

    auto groupOf = [&](const cgltf_material* mat) -> GltfGroup& {
        const cgltf_size mi = mat ? static_cast<cgltf_size>(mat - data->materials) : kNoMat;
        for (GltfGroup& g : outGroups) { if (g.materialIndex == mi) { return g; } }
        outGroups.push_back(GltfGroup{ mi, {} });
        return outGroups.back();
    };

    while (!stack.empty()) {
        const cgltf_node* n = stack.back();
        stack.pop_back();
        if (n->mesh) {
            float w[16];
            cgltf_node_transform_world(n, w);
            for (cgltf_size p = 0; p < n->mesh->primitives_count; ++p) {
                const cgltf_primitive* prim = &n->mesh->primitives[p];
                if (prim->type != cgltf_primitive_type_triangles) { continue; }
                PrimRef pr; pr.prim = prim; std::memcpy(pr.world, w, sizeof(w));
                groupOf(prim->material).prims.push_back(pr);
            }
        }
        for (cgltf_size c = 0; c < n->children_count; ++c) { stack.push_back(n->children[c]); }
    }

    if (outGroups.empty()) { err = "no triangle geometry in selection"; return false; }

    // Stable, intuitive "#N": order by ascending material index (null-material sorts last).
    std::sort(outGroups.begin(), outGroups.end(),
        [](const GltfGroup& a, const GltfGroup& b) { return a.materialIndex < b.materialIndex; });
    return true;
}

// Which group index does the selector address (clamped, with a diagnostic note)? Only meaningful
// for "#N" selectors; whole-file/#node paths load ALL groups since B2 (multi-submesh).
size_t SelectGltfGroup(const GltfSelector& sel, const std::string& fullPath, size_t groupCount) {
    if (sel.wholeFile || !sel.nodeName.empty()) {
        return 0;
    }
    if (sel.groupIndex < 0 || static_cast<size_t>(sel.groupIndex) >= groupCount) {
        GltfLog("group index " + std::to_string(sel.groupIndex) + " out of range (" +
            std::to_string(groupCount) + " groups); using 0: " + fullPath);
        return 0;
    }
    return static_cast<size_t>(sel.groupIndex);
}

// Directory prefix (with trailing separator) of a file path, or "" if none.
std::string DirOf(const std::string& path) {
    const size_t s = path.find_last_of("/\\");
    return (s == std::string::npos) ? std::string{} : path.substr(0, s + 1);
}

// Resolve a glTF texture's image URI to a path relative to the glTF file (URI-decoded). Returns
// "" for absent textures and for embedded data:/GLB-buffer images (not handled in A3).
std::string ResolveTexUri(const cgltf_texture* tex, const std::string& dir) {
    if (!tex || !tex->image || !tex->image->uri) { return {}; }
    std::string uri = tex->image->uri;
    if (uri.rfind("data:", 0) == 0) { return {}; }
    std::vector<char> buf(uri.begin(), uri.end());
    buf.push_back('\0');
    cgltf_decode_uri(buf.data());
    return dir + std::string(buf.data());
}

} // namespace

bool MeshManager::ParseGltfFile(const std::string& fullPath,
    std::vector<VertexPNTUV>& outVerts,
    std::vector<uint32_t>& outIndices,
    std::vector<Mesh::Submesh>& outSubmeshes,
    const MeshLoadOptions& opt)
{
    outVerts.clear();
    outIndices.clear();
    outSubmeshes.clear();

    const GltfSelector sel = ParseGltfSelector(fullPath);

    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, sel.file.c_str(), &data) != cgltf_result_success) {
        GltfLog("parse failed: " + sel.file);
        return false;
    }
    if (cgltf_load_buffers(&options, data, sel.file.c_str()) != cgltf_result_success) {
        GltfLog("load_buffers failed (missing .bin / external URI?): " + sel.file);
        cgltf_free(data);
        return false;
    }

    std::vector<GltfGroup> groups;
    std::string err;
    if (!ResolveGltfGroups(data, sel, groups, err)) {
        GltfLog(err + ": " + fullPath);
        cgltf_free(data);
        return false;
    }

    // B2: "#N" loads that single group; whole-file/#node selectors load EVERY group,
    // concatenated in group order, with one submesh range per group (materialSlot = ordinal).
    const bool allGroups = sel.wholeFile || !sel.nodeName.empty();
    const size_t firstGroup = allGroups ? 0 : SelectGltfGroup(sel, fullPath, groups.size());
    const size_t lastGroup = allGroups ? groups.size() : firstGroup + 1;

    for (size_t g = firstGroup; g < lastGroup; ++g) {
    const uint32_t submeshFirstIndex = static_cast<uint32_t>(outIndices.size());

    // Read geometry for this group (bake node transforms; flip winding for mirrored
    // nodes). glTF normals are kept; tangents are regenerated downstream from UVs.
    for (const PrimRef& pr : groups[g].prims) {
        const cgltf_primitive* prim = pr.prim;

        // glTF stores column-major matrices; copying those bytes into an XMFLOAT4X4 (row-major)
        // yields the transpose == the row-vector matrix DirectXMath's transforms expect, with
        // translation landing in the last row. So XMVector3Transform(v, W) == M * v.
        DirectX::XMFLOAT4X4 f;
        std::memcpy(&f, pr.world, sizeof(f));
        const DirectX::XMMATRIX W = DirectX::XMLoadFloat4x4(&f);
        const DirectX::XMMATRIX Wn = DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, W));
        const float det = DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(W));
        bool wantCW = opt.wantCW;
        if (det < 0.0f) { wantCW = !wantCW; } // mirrored (negative-scale) node flips winding

        const cgltf_accessor* accPos = nullptr;
        const cgltf_accessor* accNrm = nullptr;
        const cgltf_accessor* accUV = nullptr;
        for (cgltf_size a = 0; a < prim->attributes_count; ++a) {
            const cgltf_attribute* at = &prim->attributes[a];
            if (at->type == cgltf_attribute_type_position) { accPos = at->data; }
            else if (at->type == cgltf_attribute_type_normal) { accNrm = at->data; }
            else if (at->type == cgltf_attribute_type_texcoord && at->index == 0) { accUV = at->data; }
        }
        if (!accPos) { continue; }

        const uint32_t base = static_cast<uint32_t>(outVerts.size());
        const size_t vc = accPos->count;
        for (size_t i = 0; i < vc; ++i) {
            VertexPNTUV v;
            v.position = DirectX::XMFLOAT3(0, 0, 0);
            v.normal = DirectX::XMFLOAT3(0, 0, 0);
            v.tangent = DirectX::XMFLOAT4(0, 0, 0, 0); // regenerated by GenerateNormalsTangents
            v.uv = DirectX::XMFLOAT2(0, 0);

            float p[3] = { 0, 0, 0 };
            cgltf_accessor_read_float(accPos, i, p, 3);
            DirectX::XMStoreFloat3(&v.position,
                DirectX::XMVector3Transform(DirectX::XMVectorSet(p[0], p[1], p[2], 1.0f), W));

            if (accNrm) {
                float nn[3] = { 0, 0, 0 };
                cgltf_accessor_read_float(accNrm, i, nn, 3);
                DirectX::XMStoreFloat3(&v.normal, DirectX::XMVector3Normalize(
                    DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(nn[0], nn[1], nn[2], 0.0f), Wn)));
            }
            if (accUV) {
                float uv[2] = { 0, 0 };
                cgltf_accessor_read_float(accUV, i, uv, 2);
                v.uv = DirectX::XMFLOAT2(uv[0], uv[1]);
            }
            outVerts.push_back(v);
        }

        if (prim->indices) {
            const size_t ic = prim->indices->count;
            for (size_t i = 0; i + 2 < ic; i += 3) {
                const uint32_t a = static_cast<uint32_t>(cgltf_accessor_read_index(prim->indices, i + 0));
                const uint32_t b = static_cast<uint32_t>(cgltf_accessor_read_index(prim->indices, i + 1));
                const uint32_t c = static_cast<uint32_t>(cgltf_accessor_read_index(prim->indices, i + 2));
                addTri(outIndices, base + a, base + b, base + c, wantCW);
            }
        } else {
            for (size_t i = 0; i + 2 < vc; i += 3) {
                addTri(outIndices, base + static_cast<uint32_t>(i), base + static_cast<uint32_t>(i + 1),
                    base + static_cast<uint32_t>(i + 2), wantCW);
            }
        }
    }

    outSubmeshes.push_back(Mesh::Submesh{ submeshFirstIndex,
        static_cast<uint32_t>(outIndices.size()) - submeshFirstIndex,
        static_cast<uint32_t>(g - firstGroup) });
    } // per-group loop

    GltfLog("loaded '" + fullPath + "': " + std::to_string(outSubmeshes.size()) + "/" +
        std::to_string(groups.size()) + " group(s), " + std::to_string(outVerts.size()) +
        " verts, " + std::to_string(outIndices.size() / 3) + " tris");

    cgltf_free(data);
    return !outVerts.empty() && !outIndices.empty();
}

GltfMaterialDesc MeshManager::DescribeGltfMaterial(const std::string& pathWithFragment,
    int groupOrdinal)
{
    GltfMaterialDesc out;
    const GltfSelector sel = ParseGltfSelector(pathWithFragment);

    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, sel.file.c_str(), &data) != cgltf_result_success) {
        GltfLog("material parse failed: " + sel.file);
        return out; // valid=false
    }
    // No cgltf_load_buffers: material + image URIs don't need buffer data.

    std::vector<GltfGroup> groups;
    std::string err;
    if (!ResolveGltfGroups(data, sel, groups, err)) {
        GltfLog("material " + err + ": " + pathWithFragment);
        cgltf_free(data);
        return out;
    }
    size_t want = SelectGltfGroup(sel, pathWithFragment, groups.size());
    if (groupOrdinal >= 0) {
        // B2: explicit ordinal = submesh index of a multi-submesh load (same ordered group list).
        want = std::min(static_cast<size_t>(groupOrdinal), groups.size() - 1);
    }
    const cgltf_size mi = groups[want].materialIndex;
    if (mi == kNoMat || mi >= data->materials_count) {
        cgltf_free(data);
        return out; // null-material group -> valid stays false
    }

    const cgltf_material& m = data->materials[mi];
    const std::string dir = DirOf(sel.file);

    if (m.has_pbr_metallic_roughness) {
        const cgltf_pbr_metallic_roughness& pbr = m.pbr_metallic_roughness;
        for (int i = 0; i < 4; ++i) { out.baseColor[i] = pbr.base_color_factor[i]; }
        out.metallic = pbr.metallic_factor;
        out.roughness = pbr.roughness_factor;
        out.albedoPath = ResolveTexUri(pbr.base_color_texture.texture, dir);
        out.mrPath = ResolveTexUri(pbr.metallic_roughness_texture.texture, dir);
    }
    out.normalPath = ResolveTexUri(m.normal_texture.texture, dir);
    out.normalScale = (m.normal_texture.texture ? m.normal_texture.scale : 1.0f);
    for (int i = 0; i < 3; ++i) { out.emissive[i] = m.emissive_factor[i]; }
    out.emissivePath = ResolveTexUri(m.emissive_texture.texture, dir);
    out.alphaMask = (m.alpha_mode == cgltf_alpha_mode_mask);
    out.alphaCutoff = m.alpha_cutoff;
    out.doubleSided = (m.double_sided != 0);
    out.valid = true;

    GltfLog("material '" + pathWithFragment + "': group " + std::to_string(want) +
        ", metal=" + std::to_string(out.metallic) + " rough=" + std::to_string(out.roughness) +
        (out.alphaMask ? " MASK" : "") + (out.doubleSided ? " 2sided" : "") +
        " albedo=" + (out.albedoPath.empty() ? "-" : "y") +
        " mr=" + (out.mrPath.empty() ? "-" : "y") +
        " nrm=" + (out.normalPath.empty() ? "-" : "y"));

    cgltf_free(data);
    return out;
}

size_t MeshManager::CountSubmeshes(const std::string& pathWithFragment)
{
    const GltfSelector sel = ParseGltfSelector(pathWithFragment);

    const auto endsWithNoCase = [](const std::string& s, const char* suf)
    {
        const size_t n = std::strlen(suf);
        if (s.size() < n) { return false; }
        for (size_t i = 0; i < n; ++i)
        {
            if (std::tolower((unsigned char)s[s.size() - n + i]) != std::tolower((unsigned char)suf[i])) { return false; }
        }
        return true;
    };
    if (endsWithNoCase(sel.file, ".mesh.bin"))
    {
        // W7.1b: our baked binary — the submesh count is stored (LOD0's table). The Mesh Editor uses
        // this to show one material picker per slot, so a multi-slot palm must report all its slots.
        std::vector<VertexPNTUV> verts;
        std::vector<MeshLodCpu> lods;
        if (ReadMeshBinary(sel.file, nullptr, nullptr, verts, lods) && !lods.empty())
        {
            return std::max<size_t>(1, lods[0].submeshes.size());
        }
        return 1;
    }
    if (!endsWithNoCase(sel.file, ".gltf") && !endsWithNoCase(sel.file, ".glb"))
    {
        return 1; // .obj / .mesh.txt / .txt = a single material slot
    }

    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, sel.file.c_str(), &data) != cgltf_result_success)
    {
        return 1;
    }
    std::vector<GltfGroup> groups;
    std::string err;
    size_t count = 1;
    if (ResolveGltfGroups(data, sel, groups, err) && !groups.empty())
    {
        // Mirror LoadGltf: whole-file/#node selectors load EVERY resolved group (one submesh each);
        // a "#N" selector loads that single group.
        const bool allGroups = sel.wholeFile || !sel.nodeName.empty();
        count = allGroups ? groups.size() : 1u;
    }
    cgltf_free(data);
    return count;
}
