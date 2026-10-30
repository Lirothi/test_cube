#include "rendering/meshes/TessyDag.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>

#include "third_party/meshoptimizer/src/meshoptimizer.h"

namespace tessy
{
namespace
{
// One cluster while the DAG grows: its triangles (mesh-VB indices) and the LOD record the cut reads.
struct Node
{
    std::vector<uint32_t> indices;
    Mesh::ClusterLod lod;
    bool hasParent = false;
};

// UE's limits (Mesh::kClusterMaxTris / kClusterMaxVerts) and the leaf builder's floor (BuildClustersCpu).
constexpr size_t kMinTris = 32;
// A group whose simplification keeps more than this share of its triangles did not halve: UE asks for half
// (ClusterDAG.cpp:730-748) and retries smaller; meshoptimizer's clusterlod gives up past 85 %. Its clusters go up a
// level as they are and are grouped again with the next level's.
constexpr float kStuckShare = 0.85f;
constexpr uint32_t kMaxLevels = 32;

// The meshopt spatial clusterizer over `idx` (BuildClustersCpu's call): each cluster's triangles, in vertex-cache order.
void Recut(const std::vector<VertexPNTUV>& verts, const uint32_t* idx, size_t count, std::vector<std::vector<uint32_t>>& out)
{
    out.clear();
    if (count < 3) { return; }
    std::vector<meshopt_Meshlet> meshlets(meshopt_buildMeshletsBound(count, Mesh::kClusterMaxVerts, kMinTris));
    std::vector<unsigned int> mlVerts(count);
    std::vector<unsigned char> mlTris(count);
    const size_t n = meshopt_buildMeshletsSpatial(meshlets.data(), mlVerts.data(), mlTris.data(), idx, count,
        &verts[0].position.x, verts.size(), sizeof(VertexPNTUV), Mesh::kClusterMaxVerts, kMinTris, Mesh::kClusterMaxTris, 0.5f);
    out.reserve(n);
    for (size_t m = 0; m < n; ++m)
    {
        const meshopt_Meshlet& ml = meshlets[m];
        meshopt_optimizeMeshlet(&mlVerts[ml.vertex_offset], &mlTris[ml.triangle_offset], ml.triangle_count, ml.vertex_count);
        std::vector<uint32_t> tri(ml.triangle_count * 3u);
        for (unsigned int t = 0; t < ml.triangle_count * 3u; ++t) { tri[t] = mlVerts[ml.vertex_offset + mlTris[ml.triangle_offset + t]]; }
        out.push_back(std::move(tri));
    }
}

// A cluster record (Mesh::Cluster) of `idx` at `firstIndex`: its box, sphere and wind maxima, as BuildClustersCpu fills.
Mesh::Cluster MakeCluster(const std::vector<VertexPNTUV>& verts, const std::vector<uint32_t>& idx, uint32_t firstIndex)
{
    Mesh::Cluster c;
    c.firstIndex = firstIndex;
    c.triCount = static_cast<uint32_t>(idx.size() / 3u);
    float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (const uint32_t i : idx)
    {
        const VertexPNTUV& vx = verts[i];
        const float p[3] = { vx.position.x, vx.position.y, vx.position.z };
        for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); }
        c.windAlongMax = std::max(c.windAlongMax, static_cast<float>(vx.color & 0xFFu) / 255.0f);
        c.windLeafMax = std::max(c.windLeafMax, static_cast<float>((vx.color >> 16) & 0xFFu) / 255.0f);
    }
    c.boxCenter = DirectX::XMFLOAT3(0.5f * (lo[0] + hi[0]), 0.5f * (lo[1] + hi[1]), 0.5f * (lo[2] + hi[2]));
    c.boxExtent = DirectX::XMFLOAT3(0.5f * (hi[0] - lo[0]), 0.5f * (hi[1] - lo[1]), 0.5f * (hi[2] - lo[2]));
    const meshopt_Bounds b = meshopt_computeClusterBounds(idx.data(), idx.size(), &verts[0].position.x, verts.size(), sizeof(VertexPNTUV));
    c.sphere = DirectX::XMFLOAT4(b.center[0], b.center[1], b.center[2], b.radius);
    return c;
}
} // namespace

bool BuildDag(const std::vector<VertexPNTUV>& verts, const std::vector<uint32_t>& lod0Indices,
              const std::vector<Mesh::Cluster>& lod0Clusters, const std::vector<Mesh::Submesh>& lod0Submeshes,
              const DagBuildParams& params, Mesh::Dag& out, DagStats* stats)
{
    const auto t0 = std::chrono::steady_clock::now();
    out = {};
    DagStats st;
    if (verts.empty() || lod0Indices.empty() || lod0Clusters.empty()) { return false; }
    std::vector<Mesh::Submesh> subs = lod0Submeshes;
    if (subs.empty()) { subs.push_back(Mesh::Submesh{ 0u, static_cast<uint32_t>(lod0Indices.size()), 0u }); }

    // A border vertex is locked by POSITION: two groups meeting at a UV seam use two vertices at one point, and moving
    // either opens a crack.
    std::vector<unsigned int> posOf(verts.size());
    meshopt_generatePositionRemap(posOf.data(), &verts[0].position.x, verts.size(), sizeof(VertexPNTUV));
    // The discrete LODs' metric: [normal.xyz, uv.xy] per vertex, the normal unit length (BuildLodsCpu's).
    constexpr size_t kAttr = 5;
    std::vector<float> attribs(verts.size() * kAttr);
    for (size_t v = 0; v < verts.size(); ++v)
    {
        float nx = verts[v].normal.x, ny = verts[v].normal.y, nz = verts[v].normal.z;
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-8f) { nx /= len; ny /= len; nz /= len; }
        float* a = attribs.data() + v * kAttr;
        a[0] = nx; a[1] = ny; a[2] = nz; a[3] = verts[v].uv.x; a[4] = verts[v].uv.y;
    }
    constexpr uint32_t kNone = ~0u, kMany = ~0u - 1u;
    std::vector<uint32_t> groupOfPos(verts.size(), kNone); // per position: the one group using it, or kMany
    std::vector<unsigned char> lock(verts.size(), 0u);
    std::vector<uint32_t> touched;
    // Each submesh grows its own DAG (its clusters keep to one material, as LOD0's do), so a position two submeshes
    // share is a border of every group that holds it: always locked, or a material seam would open at the coarse cuts.
    std::vector<unsigned char> sharedPos(verts.size(), 0u);
    {
        std::vector<uint32_t> subOfPos(verts.size(), kNone);
        for (uint32_t k = 0; k < subs.size(); ++k)
        {
            const size_t first = subs[k].indexOffset, last = std::min<size_t>(first + subs[k].indexCount, lod0Indices.size());
            for (size_t i = first; i < last; ++i)
            {
                const uint32_t p = posOf[lod0Indices[i]];
                if (subOfPos[p] == kNone) { subOfPos[p] = k; }
                else if (subOfPos[p] != k) { sharedPos[p] = 1u; }
            }
        }
    }

    for (const Mesh::Submesh& s : subs)
    {
        Mesh::DagSubmesh ds;
        ds.firstCluster = static_cast<uint32_t>(out.clusters.size());
        ds.firstLevel = static_cast<uint32_t>(out.levels.size());
        // The leaves: LOD0's clusters of this submesh, their triangles as they are.
        std::vector<Node> nodes;
        for (const Mesh::Cluster& c : lod0Clusters)
        {
            if (c.firstIndex < s.indexOffset || c.firstIndex >= s.indexOffset + s.indexCount) { continue; }
            if (static_cast<size_t>(c.firstIndex) + c.triCount * 3ull > lod0Indices.size()) { return false; }
            Node n;
            n.indices.assign(lod0Indices.begin() + c.firstIndex, lod0Indices.begin() + c.firstIndex + c.triCount * 3u);
            n.lod.lodSphere = c.sphere;
            n.lod.lodError = 0.0f;
            n.lod.level = 0;
            nodes.push_back(std::move(n));
        }
        if (nodes.empty()) { out.submeshes.push_back(ds); continue; }
        const float uvWeight = s.materialSlot < params.slotUvWeight.size() ? params.slotUvWeight[s.materialSlot] : 0.0f;
        const float weights[kAttr] = { params.normalWeight, params.normalWeight, params.normalWeight, uvWeight, uvWeight };
        // Leaf cards: whole cards may go (meshopt prunes a disconnected piece once its radius fits the error, and the error
        // it reports includes them, so the DAG's errors stay what the cut reads).
        const bool prune = s.materialSlot < params.slotPrune.size() && params.slotPrune[s.materialSlot] != 0u;
        const unsigned int simplifyOptions = meshopt_SimplifyErrorAbsolute | meshopt_SimplifySparse |
                                             (prune ? static_cast<unsigned int>(meshopt_SimplifyPrune) : 0u);

        std::vector<uint32_t> work(nodes.size()); // the current level: indices into `nodes`
        for (uint32_t i = 0; i < work.size(); ++i) { work[i] = i; }
        for (uint32_t level = 1; level < kMaxLevels && work.size() > 1u; ++level)
        {
            // Groups of the level's clusters by shared boundary and proximity.
            std::vector<unsigned int> concat, counts;
            for (const uint32_t w : work)
            {
                concat.insert(concat.end(), nodes[w].indices.begin(), nodes[w].indices.end());
                counts.push_back(static_cast<unsigned int>(nodes[w].indices.size()));
            }
            std::vector<unsigned int> part(work.size());
            const size_t groups = meshopt_partitionClusters(part.data(), concat.data(), concat.size(), counts.data(), work.size(),
                &verts[0].position.x, verts.size(), sizeof(VertexPNTUV), params.groupSize);
            std::vector<std::vector<uint32_t>> members(groups);
            for (size_t k = 0; k < work.size(); ++k) { members[part[k]].push_back(work[k]); }
            // Lock what two groups share.
            for (const uint32_t p : touched) { groupOfPos[p] = kNone; }
            touched.clear();
            for (uint32_t g = 0; g < groups; ++g)
            {
                for (const uint32_t m : members[g])
                {
                    for (const uint32_t i : nodes[m].indices)
                    {
                        const uint32_t p = posOf[i];
                        if (groupOfPos[p] == kNone) { groupOfPos[p] = g; touched.push_back(p); }
                        else if (groupOfPos[p] != g) { groupOfPos[p] = kMany; }
                    }
                }
            }
            std::vector<uint32_t> next;
            bool progress = false;
            for (uint32_t g = 0; g < groups; ++g)
            {
                std::vector<uint32_t> merged;
                float childError = 0.0f;
                for (const uint32_t m : members[g])
                {
                    merged.insert(merged.end(), nodes[m].indices.begin(), nodes[m].indices.end());
                    childError = std::max(childError, nodes[m].lod.lodError);
                }
                for (const uint32_t i : merged) { lock[i] = (groupOfPos[posOf[i]] == kMany || sharedPos[posOf[i]]) ? 1u : 0u; }
                const size_t target = std::max<size_t>((merged.size() / 6u) * 3u, 3u); // half the triangles
                std::vector<uint32_t> simplified(merged.size());
                float error = 0.0f;
                const size_t n = meshopt_simplifyWithAttributes(simplified.data(), merged.data(), merged.size(),
                    &verts[0].position.x, verts.size(), sizeof(VertexPNTUV), attribs.data(), kAttr * sizeof(float),
                    weights, kAttr, lock.data(), target, FLT_MAX, simplifyOptions, &error);
                if (n == 0 || static_cast<float>(n) > kStuckShare * static_cast<float>(merged.size()))
                {
                    next.insert(next.end(), members[g].begin(), members[g].end()); // up a level as they are
                    ++st.stuckGroups;
                    continue;
                }
                progress = true;
                // The group's error: the simplification's, never below what its inputs already carry (monotone).
                const float groupError = std::max(error, childError);
                // Its sphere: around its inputs' spheres (UE ClusterDAG.cpp:1438-1446).
                std::vector<float> centres, radii;
                for (const uint32_t m : members[g])
                {
                    const DirectX::XMFLOAT4& sp = nodes[m].lod.lodSphere;
                    centres.insert(centres.end(), { sp.x, sp.y, sp.z });
                    radii.push_back(sp.w);
                }
                const meshopt_Bounds gb = meshopt_computeSphereBounds(centres.data(), members[g].size(), 3 * sizeof(float),
                                                                      radii.data(), sizeof(float));
                const DirectX::XMFLOAT4 groupSphere(gb.center[0], gb.center[1], gb.center[2], gb.radius);
                for (const uint32_t m : members[g])
                {
                    nodes[m].lod.parentError = groupError;
                    nodes[m].lod.parentSphere = groupSphere;
                    nodes[m].hasParent = true;
                }
                std::vector<std::vector<uint32_t>> cut;
                Recut(verts, simplified.data(), n, cut);
                for (std::vector<uint32_t>& tri : cut)
                {
                    Node child;
                    child.indices = std::move(tri);
                    child.lod.lodSphere = groupSphere;
                    child.lod.lodError = groupError;
                    child.lod.level = level;
                    next.push_back(static_cast<uint32_t>(nodes.size()));
                    nodes.push_back(std::move(child));
                }
            }
            work = std::move(next);
            if (!progress) { break; } // nothing halves any more: what is left are the roots
        }
        // The roots: no simplification consumes them.
        for (Node& n : nodes)
        {
            if (!n.hasParent)
            {
                n.lod.parentError = FLT_MAX;
                n.lod.parentSphere = n.lod.lodSphere;
                ++st.roots;
                st.rootTriangles += n.indices.size() / 3u;
                st.rootError = std::max(st.rootError, n.lod.lodError);
            }
        }
        // Emit by level (the nodes are in production order, which is level order), each level's range and bounds.
        std::stable_sort(nodes.begin(), nodes.end(), [](const Node& a, const Node& b) { return a.lod.level < b.lod.level; });
        uint32_t levelCount = 0, lastLevel = ~0u;
        for (const Node& n : nodes)
        {
            if (n.lod.level != lastLevel)
            {
                Mesh::DagLevel l;
                l.firstCluster = static_cast<uint32_t>(out.clusters.size());
                l.minLodError = FLT_MAX;
                l.maxParentError = 0.0f;
                out.levels.push_back(l);
                ++levelCount;
                lastLevel = n.lod.level;
            }
            Mesh::DagLevel& l = out.levels.back();
            ++l.clusterCount;
            l.minLodError = std::min(l.minLodError, n.lod.lodError);
            l.maxParentError = std::max(l.maxParentError, n.lod.parentError);
            out.clusters.push_back(MakeCluster(verts, n.indices, static_cast<uint32_t>(out.indices.size())));
            out.lod.push_back(n.lod);
            out.indices.insert(out.indices.end(), n.indices.begin(), n.indices.end());
            st.triangles += n.indices.size() / 3u;
            if (n.lod.level == 0) { ++st.leaves; st.leafTriangles += n.indices.size() / 3u; }
        }
        ds.clusterCount = static_cast<uint32_t>(out.clusters.size()) - ds.firstCluster;
        ds.levelCount = levelCount;
        st.levels = std::max(st.levels, levelCount);
        out.submeshes.push_back(ds);
    }
    st.clusters = static_cast<uint32_t>(out.clusters.size());
    st.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (stats) { *stats = st; }
    return !out.clusters.empty();
}

bool ValidateDag(const Mesh::Dag& dag, std::string* why)
{
    const auto fail = [why](const std::string& s) { if (why) { *why = s; } return false; };
    if (dag.lod.size() != dag.clusters.size()) { return fail("lod records do not match the clusters"); }
    for (size_t i = 0; i < dag.lod.size(); ++i)
    {
        const Mesh::ClusterLod& l = dag.lod[i];
        if (!(l.parentError >= l.lodError)) { return fail("cluster " + std::to_string(i) + ": parentError < lodError"); }
        const float dx = l.parentSphere.x - l.lodSphere.x, dy = l.parentSphere.y - l.lodSphere.y, dz = l.parentSphere.z - l.lodSphere.z;
        const float reach = std::sqrt(dx * dx + dy * dy + dz * dz) + l.lodSphere.w;
        if (reach > l.parentSphere.w * 1.0001f + 1e-5f) { return fail("cluster " + std::to_string(i) + ": its parent's sphere does not hold its own"); }
        if (i > 0 && l.level < dag.lod[i - 1].level)
        {
            // a new submesh starts at level 0 again; anything else is out of order
            bool submeshStart = false;
            for (const Mesh::DagSubmesh& s : dag.submeshes) { submeshStart = submeshStart || s.firstCluster == i; }
            if (!submeshStart) { return fail("cluster " + std::to_string(i) + ": levels out of order"); }
        }
    }
    for (const Mesh::DagSubmesh& s : dag.submeshes)
    {
        if (s.firstCluster + s.clusterCount > dag.clusters.size() || s.firstLevel + s.levelCount > dag.levels.size())
        {
            return fail("a submesh's range runs past the tables");
        }
        uint32_t covered = 0;
        for (uint32_t k = 0; k < s.levelCount; ++k) { covered += dag.levels[s.firstLevel + k].clusterCount; }
        if (covered != s.clusterCount) { return fail("a submesh's levels do not cover its clusters"); }
    }
    return true;
}

uint64_t DagCutTriangles(const Mesh::Dag& dag, float t)
{
    uint64_t tris = 0;
    for (size_t i = 0; i < dag.clusters.size(); ++i)
    {
        if (dag.lod[i].lodError <= t && t < dag.lod[i].parentError) { tris += dag.clusters[i].triCount; }
    }
    return tris;
}
} // namespace tessy
