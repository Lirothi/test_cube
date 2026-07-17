#include "rendering/meshes/MeshManager.h"
#include "rendering/core/Renderer.h"
#include <fstream>
#include <sstream>
#include <cctype>
#include <algorithm>
#include "third_party/robin_hood.h"
#include <cstring> // strchr, atoi
#include <DirectXMath.h>
#include <queue>
#include "meshoptimizer.h"
#include "third_party/cgltf/cgltf.h"
#include <Windows.h> // OutputDebugStringA for load diagnostics

using namespace DirectX;

namespace
{
// Step 6 / Part B: generate coarser LODs as reduced index buffers (meshopt_simplify, over the
// same vertices) and append them to the mesh. Each submesh range is simplified INDEPENDENTLY and
// the LOD carries its own rebuilt submesh table — simplifying the whole buffer as one blob would
// dissolve the per-material boundaries. Single-submesh meshes reduce to the original behavior.
// Called once at load on the upload command list.
void GenerateLods(Mesh* mesh, ID3D12Device* device, ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* keepAlive,
    const std::vector<VertexPNTUV>& verts, const std::vector<uint32_t>& indices)
{
    const size_t baseIdx = indices.size();
    constexpr size_t kMinIndicesForLod = 384;  // ~128 tris; skip tiny meshes (box = 6 tris)
    constexpr size_t kMinRangeIndices  = 96;    // per-range floor; smaller ranges copy through
    if (!mesh || verts.empty() || baseIdx < kMinIndicesForLod) { return; }

    const std::vector<Mesh::Submesh>& baseSubs = mesh->GetSubmeshes();
    if (baseSubs.empty()) { return; }

    const float ratios[] = { 0.5f, 0.25f, 0.12f };
    const float errors[] = { 0.02f, 0.05f, 0.12f };

    std::vector<uint32_t> lodIdx;
    std::vector<Mesh::Submesh> lodSubs;
    std::vector<uint32_t> simplified;
    size_t prevCount = baseIdx;

    for (int i = 0; i < 3; ++i)
    {
        lodIdx.clear();
        lodSubs.clear();

        for (const Mesh::Submesh& s : baseSubs)
        {
            const uint32_t* src = indices.data() + s.indexOffset;
            const size_t srcCount = s.indexCount;
            const uint32_t outOffset = static_cast<uint32_t>(lodIdx.size());

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
            lodIdx.insert(lodIdx.end(), from, from + n);
            lodSubs.push_back(Mesh::Submesh{ outOffset, static_cast<uint32_t>(n), s.materialSlot });
        }

        // Overall shrink gate (same spirit as before): stop once a level is < ~10% smaller.
        if (lodIdx.empty() || lodIdx.size() + (lodIdx.size() / 10) >= prevCount) { break; }
        mesh->AddLod(device, uploadCmdList, keepAlive,
            lodIdx.data(), static_cast<UINT>(lodIdx.size()), lodSubs);
        prevCount = lodIdx.size();
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

std::shared_ptr<Mesh> MeshManager::Load(const std::string& path,
    Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive,
    const MeshLoadOptions& opt)
{
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

std::shared_ptr<Mesh> MeshManager::LoadText(const std::string& path,
    Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive,
    const MeshLoadOptions& opt)
{
    robin_hood::unordered_map<std::string, std::shared_ptr<Mesh>>::iterator it = cache_.find(path);
    if (it != cache_.end()) {
        return it->second;
    }

    std::vector<VertexPNTUV> verts;
    std::vector<uint32_t>    inds;
    if (!ParseTextFile(path, verts, inds, opt)) {
        return std::shared_ptr<Mesh>();
    }

    std::shared_ptr<Mesh> m = std::make_shared<Mesh>();
    m->CreateGPU_PNTUV(renderer->GetDevice(), uploadCmdList, uploadKeepAlive,
        verts, inds.data(), (UINT)inds.size(), opt.generateTangentSpace);
    GenerateLods(m.get(), renderer->GetDevice(), uploadCmdList, uploadKeepAlive, verts, inds);
    cache_[path] = m;
    return m;
}

std::shared_ptr<Mesh> MeshManager::LoadOBJ(const std::string& path,
    Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive,
    const MeshLoadOptions& opt)
{
    robin_hood::unordered_map<std::string, std::shared_ptr<Mesh>>::iterator it = cache_.find(path);
    if (it != cache_.end()) {
        return it->second;
    }

    std::vector<VertexPNTUV> verts;
    std::vector<uint32_t>    inds;
    if (!ParseOBJFile(path, verts, inds, opt)) {
        return std::shared_ptr<Mesh>();
    }

    std::shared_ptr<Mesh> m = std::make_shared<Mesh>();
    m->CreateGPU_PNTUV(renderer->GetDevice(), uploadCmdList, uploadKeepAlive,
        verts, inds.data(), (UINT)inds.size(), opt.generateTangentSpace);
    GenerateLods(m.get(), renderer->GetDevice(), uploadCmdList, uploadKeepAlive, verts, inds);
    cache_[path] = m;
    return m;
}

std::shared_ptr<Mesh> MeshManager::LoadGltf(const std::string& path,
    Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive,
    const MeshLoadOptions& opt)
{
    // Cache by the full path INCLUDING the fragment: "foo.glb#0" and "foo.glb#1" are distinct meshes.
    robin_hood::unordered_map<std::string, std::shared_ptr<Mesh>>::iterator it = cache_.find(path);
    if (it != cache_.end()) {
        return it->second;
    }

    std::vector<VertexPNTUV> verts;
    std::vector<uint32_t>    inds;
    std::vector<Mesh::Submesh> submeshes;
    if (!ParseGltfFile(path, verts, inds, submeshes, opt)) {
        return std::shared_ptr<Mesh>();
    }

    std::shared_ptr<Mesh> m = std::make_shared<Mesh>();
    m->CreateGPU_PNTUV(renderer->GetDevice(), uploadCmdList, uploadKeepAlive,
        verts, inds.data(), (UINT)inds.size(), opt.generateTangentSpace,
        submeshes.size() > 1 ? &submeshes : nullptr);
    GenerateLods(m.get(), renderer->GetDevice(), uploadCmdList, uploadKeepAlive, verts, inds);
    cache_[path] = m;
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