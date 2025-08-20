#include "MeshManager.h"
#include "Renderer.h"
#include <fstream>
#include <sstream>
#include <cctype>
#include <algorithm>
#include <unordered_map>
#include <cstring> // strchr, atoi
#include <DirectXMath.h>
#include <queue>

using namespace DirectX;

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
    std::string low = tolower_str(path);
    if (low.size() >= 4 && low.substr(low.size() - 4) == ".obj") {
        return LoadOBJ(path, renderer, uploadCmdList, uploadKeepAlive, opt);
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
    std::unordered_map<std::string, std::shared_ptr<Mesh>>::iterator it = cache_.find(path);
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
    cache_[path] = m;
    return m;
}

std::shared_ptr<Mesh> MeshManager::LoadOBJ(const std::string& path,
    Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive,
    const MeshLoadOptions& opt)
{
    std::unordered_map<std::string, std::shared_ptr<Mesh>>::iterator it = cache_.find(path);
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
    std::unordered_map<std::string, std::shared_ptr<Mesh>>::iterator it = cache_.find(key);
    if (it != cache_.end()) {
        return it->second;
    }

    std::shared_ptr<Mesh> m = std::make_shared<Mesh>();
    std::vector<VertexPNTUV> verts = vertsIn; // CreateGPU_PNTUV может модифицировать
    m->CreateGPU_PNTUV(renderer->GetDevice(), uploadCmdList, uploadKeepAlive,
        verts, indices.data(), (UINT)indices.size(), generateTangentSpace);
    cache_[key] = m;
    return m;
}

std::shared_ptr<Mesh> MeshManager::Get(const std::string& key) const {
    std::unordered_map<std::string, std::shared_ptr<Mesh>>::const_iterator it = cache_.find(key);
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

    std::unordered_map<OBJKey, uint32_t, OBJKeyHash> vmap;
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

            // выдаёт ID для (v/vt/vn), создавая уникальную вершину
            std::vector<uint32_t> id(face.size());
            for (size_t i = 0; i < face.size(); ++i) {
                std::unordered_map<OBJKey, uint32_t, OBJKeyHash>::iterator it = vmap.find(face[i]);
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
                    vmap.insert(std::make_pair(face[i], newId));
                    id[i] = newId;
                }
            }

            // триангуляция фаном: (0,1,2), (0,2,3), ...
            for (size_t t = 1; t + 1 < id.size(); ++t) {
                addTri(outIndices, id[0], id[t], id[t + 1], opt.wantCW);
            }
        }
    }

    return !outVerts.empty() && !outIndices.empty();
}