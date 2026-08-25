#include "rendering/meshes/MeshManager.h"
#include "rendering/core/Renderer.h"
#include <fstream>
#include "third_party/json/json.hpp" // MeshManager::ApplyManifestOptions (mesh.json -> bake options)
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
#include <cfloat>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <functional> // LOD3 foliage prune: union-find's recursive-free find lambda
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
    // Worst-case geometric deviation of this level from LOD0, in OBJECT-SPACE units, as reported by
    // meshopt and normalized out of its relative form. This is the number Unreal has and this engine
    // did not: it makes a LOD switch decidable WITHOUT a per-asset screen size, because a deviation
    // projects to a pixel count and a pixel count has an obviously right threshold.
    // 0 = unknown (a level that copied through without simplifying).
    float error = 0.0f;
};

// Mesh chunking (MeshLoadOptions::chunkGrid): partition a SINGLE-submesh LOD0 into an N x N grid
// of submeshes over the mesh's XZ extent, by triangle centroid. Pure reordering — every triangle
// survives exactly once, the material slot is inherited, and empty cells emit nothing. Cells are
// emitted row-major (z-major, x-minor) and triangles keep their relative order inside a cell, so
// the partition is deterministic and slot ordinal <-> chunk identity is positional (the runtime's
// per-slot caster bounds rely on that, and so does the "same submesh count in the same order at
// every LOD" invariant BuildLodsCpu preserves).
//
// Returns false (leaving the mesh untouched) for anything it cannot chunk: grid < 2, a mesh that
// already carries multiple material submeshes, or a degenerate XZ extent.
bool ChunkifyLod0(const std::vector<VertexPNTUV>& verts, std::vector<uint32_t>& indices,
    std::vector<Mesh::Submesh>& subs, uint32_t grid)
{
    if (grid < 2u || subs.size() != 1u || indices.size() < 3u || verts.empty()) { return false; }
    const uint32_t slot = subs[0].materialSlot;

    float mnx = FLT_MAX, mnz = FLT_MAX, mxx = -FLT_MAX, mxz = -FLT_MAX;
    for (const uint32_t i : indices)
    {
        if (i >= verts.size()) { return false; }
        const DirectX::XMFLOAT3& p = verts[i].position;
        mnx = std::min(mnx, p.x); mxx = std::max(mxx, p.x);
        mnz = std::min(mnz, p.z); mxz = std::max(mxz, p.z);
    }
    const float spanX = mxx - mnx, spanZ = mxz - mnz;
    if (!(spanX > 0.0f) || !(spanZ > 0.0f)) { return false; }

    const size_t triCount = indices.size() / 3;
    const uint32_t cells = grid * grid;
    std::vector<std::vector<uint32_t>> bucket(cells);
    const float invCellX = static_cast<float>(grid) / spanX;
    const float invCellZ = static_cast<float>(grid) / spanZ;
    const int last = static_cast<int>(grid) - 1;
    for (size_t t = 0; t < triCount; ++t)
    {
        const DirectX::XMFLOAT3& a = verts[indices[t * 3 + 0]].position;
        const DirectX::XMFLOAT3& b = verts[indices[t * 3 + 1]].position;
        const DirectX::XMFLOAT3& c = verts[indices[t * 3 + 2]].position;
        const float cx = (a.x + b.x + c.x) * (1.0f / 3.0f);
        const float cz = (a.z + b.z + c.z) * (1.0f / 3.0f);
        int gx = static_cast<int>((cx - mnx) * invCellX);
        int gz = static_cast<int>((cz - mnz) * invCellZ);
        gx = gx < 0 ? 0 : (gx > last ? last : gx);   // the max-coordinate triangle lands on `grid`
        gz = gz < 0 ? 0 : (gz > last ? last : gz);
        bucket[static_cast<uint32_t>(gz) * grid + static_cast<uint32_t>(gx)].push_back(
            static_cast<uint32_t>(t));
    }

    std::vector<uint32_t> reordered;
    reordered.reserve(indices.size());
    std::vector<Mesh::Submesh> chunked;
    chunked.reserve(cells);
    for (uint32_t c = 0; c < cells; ++c)
    {
        if (bucket[c].empty()) { continue; }
        const uint32_t offset = static_cast<uint32_t>(reordered.size());
        for (const uint32_t t : bucket[c])
        {
            reordered.push_back(indices[t * 3 + 0]);
            reordered.push_back(indices[t * 3 + 1]);
            reordered.push_back(indices[t * 3 + 2]);
        }
        chunked.push_back(Mesh::Submesh{ offset,
            static_cast<uint32_t>(reordered.size()) - offset, slot });
    }
    indices.swap(reordered);
    subs.swap(chunked);
    return true;
}

// LOD3 foliage prune (see MeshLoadOptions::foliagePruneKeep). Removes whole leaf components
// from one foliage submesh range and APPENDS scaled copies of the survivors' vertices —
// silhouette density is preserved by growing what stays instead of collapsing what a
// position-only error metric cannot see. Components come from INDEX connectivity: meshes are
// index-split at UV seams, so the units land at leaflet/frond granularity — exactly the
// islands meshopt refuses to collapse across. Deterministic (ordering keyed on the smallest
// vertex index), so a re-bake is byte-stable.
// Returns false when the range has too few components to prune meaningfully (caller falls
// back to meshopt).
bool PruneFoliageRange(std::vector<VertexPNTUV>& verts, const uint32_t* srcIdx, size_t srcCount,
                       float keepRatio, float innerRatio, float innerError,
                       float grow, float uvWeight,
                       std::vector<uint32_t>& outIndices)
{
    if (srcCount < 96 || keepRatio <= 0.0f || keepRatio >= 1.0f) { return false; }

    // Union-find over the range's vertex indices.
    robin_hood::unordered_map<uint32_t, uint32_t> parent;
    parent.reserve(srcCount);
    std::function<uint32_t(uint32_t)> find = [&](uint32_t x) -> uint32_t
    {
        auto it = parent.find(x);
        if (it == parent.end()) { parent[x] = x; return x; }
        uint32_t root = x;
        while (parent[root] != root) { root = parent[root]; }
        while (parent[x] != root) { uint32_t next = parent[x]; parent[x] = root; x = next; }
        return root;
    };
    for (size_t t = 0; t + 2 < srcCount; t += 3)
    {
        const uint32_t a = find(srcIdx[t]);
        const uint32_t b = find(srcIdx[t + 1]);
        const uint32_t c = find(srcIdx[t + 2]);
        parent[b] = a;
        parent[c] = a;
    }

    struct Comp
    {
        std::vector<uint32_t> tris;   // first-index of each triangle in the range
        Math::float3 centroid{};
        uint32_t minVert = UINT32_MAX;
    };
    robin_hood::unordered_map<uint32_t, uint32_t> rootToComp;
    std::vector<Comp> comps;
    for (size_t t = 0; t + 2 < srcCount; t += 3)
    {
        const uint32_t root = find(srcIdx[t]);
        auto it = rootToComp.find(root);
        if (it == rootToComp.end()) { it = rootToComp.emplace(root, (uint32_t)comps.size()).first; comps.emplace_back(); }
        Comp& c = comps[it->second];
        c.tris.push_back((uint32_t)t);
        for (int k = 0; k < 3; ++k)
        {
            const uint32_t vi = srcIdx[t + k];
            c.minVert = vi < c.minVert ? vi : c.minVert;
            const auto& p = verts[vi].position;
            c.centroid = Math::float3(c.centroid.x + p.x, c.centroid.y + p.y, c.centroid.z + p.z);
        }
    }
    // A crown that welded into a handful of blobs cannot be pruned leaf-wise — let meshopt try.
    if (comps.size() < 8) { return false; }

    for (Comp& c : comps)
    {
        const float inv = 1.0f / (float)(c.tris.size() * 3);
        c.centroid = Math::float3(c.centroid.x * inv, c.centroid.y * inv, c.centroid.z * inv);
    }

    // FARTHEST-POINT selection over the centroids: start from the largest leaf, then always
    // keep the candidate farthest from everything already kept (ties: bigger, then smallest
    // vertex index — deterministic). Blue-noise spread by construction: no grid to alias
    // against, no bald sector, and a multi-crown asset (the coconut is a DOUBLE palm) splits
    // its quota between crowns automatically. The first cut of this used a 3x3x3 centroid
    // grid and visibly skewed the coconut's kept leaves to one side — an axis-aligned grid
    // over two overlapping radial crowns is exactly the wrong stratifier.
    std::vector<char> kept(comps.size(), 0);
    size_t keptTris = 0, totalTris = srcCount / 3;
    const size_t quota = std::max<size_t>(1, (size_t)std::ceil((double)comps.size() * keepRatio));
    {
        uint32_t seed = 0;
        for (uint32_t i = 1; i < comps.size(); ++i)
        {
            if (comps[i].tris.size() > comps[seed].tris.size() ||
                (comps[i].tris.size() == comps[seed].tris.size() && comps[i].minVert < comps[seed].minVert))
            {
                seed = i;
            }
        }
        kept[seed] = 1;
        keptTris += comps[seed].tris.size();
        std::vector<float> minDistSq(comps.size(), FLT_MAX);
        auto relax = [&](uint32_t keptIdx)
        {
            const Math::float3& k = comps[keptIdx].centroid;
            for (uint32_t i = 0; i < comps.size(); ++i)
            {
                const Math::float3& c = comps[i].centroid;
                const float dx = c.x - k.x, dy = c.y - k.y, dz = c.z - k.z;
                const float d = dx * dx + dy * dy + dz * dz;
                minDistSq[i] = std::min(minDistSq[i], d);
            }
        };
        relax(seed);
        for (size_t taken = 1; taken < quota; ++taken)
        {
            uint32_t best = UINT32_MAX;
            for (uint32_t i = 0; i < comps.size(); ++i)
            {
                if (kept[i]) { continue; }
                if (best == UINT32_MAX ||
                    minDistSq[i] > minDistSq[best] ||
                    (minDistSq[i] == minDistSq[best] &&
                     (comps[i].tris.size() > comps[best].tris.size() ||
                      (comps[i].tris.size() == comps[best].tris.size() && comps[i].minVert < comps[best].minVert))))
                {
                    best = i;
                }
            }
            if (best == UINT32_MAX) { break; }
            kept[best] = 1;
            keptTris += comps[best].tris.size();
            relax(best);
        }
    }
    if (keptTris == 0 || keptTris >= totalTris) { return false; }

    // Area compensation: triangles are the area proxy, measured BEFORE the interior decimation
    // below (decimation keeps the card's area, so the pre-decimation count is the honest one).
    // Keeping big leaves first means the kept AREA fraction exceeds the component fraction, so
    // the needed growth stays modest. `grow` dials the compensation itself: full inflation
    // holds silhouette DENSITY but can read fluffier than the source crown.
    const float autoScale = std::min(2.5f, std::sqrt((float)totalTris / (float)keptTris));
    const float scale = 1.0f + (autoScale - 1.0f) * std::max(0.0f, std::min(2.0f, grow));

    // Gather the kept leaves' ORIGINAL triangles.
    std::vector<uint32_t> keptIdx;
    keptIdx.reserve(keptTris * 3);
    for (uint32_t ci = 0; ci < comps.size(); ++ci)
    {
        if (!kept[ci]) { continue; }
        for (const uint32_t t : comps[ci].tris)
        {
            keptIdx.push_back(srcIdx[t]);
            keptIdx.push_back(srcIdx[t + 1]);
            keptIdx.push_back(srcIdx[t + 2]);
        }
    }

    // INTERIOR decimation of the survivors: the pruned leaves kept their full tessellation
    // (a frond is an ~80-triangle grid whether it is 2 m or 20 m away). LockBorder pins every
    // open-edge vertex — the card's OUTLINE, which is what a leaf's shape actually lives in —
    // and with the silhouette nailed down Permissive is finally safe: it lets the interior
    // collapse across the attribute continuity that stalls safe mode, and the spikes it caused
    // when the border was free cannot happen. Components are disconnected, so collapses never
    // cross leaves and every simplified vertex keeps its component identity.
    const uint32_t* emitIdx = keptIdx.data();
    size_t emitCount = keptIdx.size();
    std::vector<uint32_t> innerSimplified;
    size_t innerTarget = (size_t)((double)keptIdx.size() * std::max(0.05f, std::min(1.0f, innerRatio)));
    innerTarget -= innerTarget % 3;
    if (innerTarget >= 12 && innerTarget < keptIdx.size())
    {
        innerSimplified.resize(keptIdx.size());
        float resultError = 0.0f;
        size_t n = 0;
        if (uvWeight > 0.0f)
        {
            // Interior collapses smear UVs exactly like the chain-wide ones do; same cure.
            const float uvw[2] = { uvWeight, uvWeight };
            n = meshopt_simplifyWithAttributes(innerSimplified.data(), keptIdx.data(),
                keptIdx.size(), &verts[0].position.x, verts.size(), sizeof(VertexPNTUV),
                &verts[0].uv.x, sizeof(VertexPNTUV), uvw, 2, nullptr,
                innerTarget, innerError, meshopt_SimplifyLockBorder | meshopt_SimplifyPermissive,
                &resultError);
        }
        else
        {
            n = meshopt_simplify(innerSimplified.data(), keptIdx.data(), keptIdx.size(),
                &verts[0].position.x, verts.size(), sizeof(VertexPNTUV),
                innerTarget, innerError, meshopt_SimplifyLockBorder | meshopt_SimplifyPermissive,
                &resultError);
        }
        if (n >= 12)
        {
            emitIdx = innerSimplified.data();
            emitCount = n;
        }
    }

    // Emit; every survivor's vertices are APPENDED scaled copies about the OWNING leaf's
    // centroid. LODs are index buffers over ONE shared vertex buffer — scaling in place would
    // corrupt LOD 0. A vertex belongs to exactly one component, so one remap serves all.
    robin_hood::unordered_map<uint32_t, uint32_t> remap;
    remap.reserve(emitCount);
    for (size_t t = 0; t + 2 < emitCount; t += 3)
    {
        const Comp& c = comps[rootToComp[find(emitIdx[t])]];
        for (int k = 0; k < 3; ++k)
        {
            const uint32_t vi = emitIdx[t + k];
            auto it = remap.find(vi);
            if (it == remap.end())
            {
                VertexPNTUV v = verts[vi]; // normals/tangents/uv/wind weights verbatim
                v.position.x = c.centroid.x + (v.position.x - c.centroid.x) * scale;
                v.position.y = c.centroid.y + (v.position.y - c.centroid.y) * scale;
                v.position.z = c.centroid.z + (v.position.z - c.centroid.z) * scale;
                it = remap.emplace(vi, (uint32_t)verts.size()).first;
                verts.push_back(v);
            }
            outIndices.push_back(it->second);
        }
    }

    char msg[224];
    std::snprintf(msg, sizeof(msg),
        "[meshbake] LOD3 foliage prune: %zu comps -> %zu kept, tris %zu -> %zu -> inner %zu, scale x%.2f\n",
        comps.size(), (size_t)std::count(kept.begin(), kept.end(), (char)1), totalTris, keptTris,
        emitCount / 3, scale);
    OutputDebugStringA(msg);
    return true;
}

// Step 6 / Part B: build coarser LODs as reduced index buffers (meshopt_simplify, over the same
// vertices). Each submesh range is simplified INDEPENDENTLY and the LOD carries its own rebuilt
// submesh table — simplifying the whole buffer as one blob would dissolve the per-material
// boundaries. Single-submesh meshes reduce to the original behavior. CPU-only (no GPU).
//
// `chunkedSubsets` (ChunkifyLod0 ran): the ranges are SPATIAL neighbours of one continuous surface,
// not disjoint material groups, so independent simplification would pull their shared borders apart
// into cracks. See the flag block below.
//
// `allowVertexAppend` (the BAKE path only): permits the LOD3 foliage prune, which appends scaled
// leaf copies to `verts`. The runtime path uploads the VB before LODs are built, so it must pass
// false and its foliage LOD3 stays plain meshopt until the asset is re-baked.
std::vector<MeshLodCpu> BuildLodsCpu(std::vector<VertexPNTUV>& verts,
    const std::vector<uint32_t>& indices, const std::vector<Mesh::Submesh>& baseSubs,
    const MeshLoadOptions& opt, bool chunkedSubsets = false, bool allowVertexAppend = false)
{
    std::vector<MeshLodCpu> out;
    const size_t baseIdx = indices.size();
    constexpr size_t kMinIndicesForLod = 384;  // ~128 tris; skip tiny meshes (box = 6 tris)
    constexpr size_t kMinRangeIndices  = 96;    // per-range floor; smaller ranges copy through
    if (verts.empty() || baseIdx < kMinIndicesForLod || baseSubs.empty()) { return out; }

    // Per-level target ratio + error budget; overridable per import (see MeshLoadOptions).
    const float ratios[] = { 0.5f * opt.lodRatioScale, 0.25f * opt.lodRatioScale, 0.12f * opt.lodRatioScale };
    const float errors[] = { 0.02f * opt.lodErrorScale, 0.05f * opt.lodErrorScale, 0.12f * opt.lodErrorScale };

    // Chunked (spatially partitioned) ranges need three extra meshopt flags, and the third one
    // forces the error budget to be restated:
    //   LockBorder     pins every vertex on an OPEN edge of the range. A chunk's seam is exactly
    //                  that, and both sides of a seam lock the same shared positions, so adjacent
    //                  chunks stay welded no matter how differently their interiors collapse.
    //   Sparse         restricts meshopt's per-call work to the vertices the range actually uses
    //                  (36 calls over a 41k-vertex array otherwise walk the whole array 36 times).
    //   ErrorAbsolute  Sparse ALSO re-bases the relative error onto the SUBSET's extent (~60 m per
    //                  chunk instead of the island's 388 m), which would silently tighten the
    //                  budget ~6x. Absolute error + an explicit multiply by the whole mesh's
    //                  simplify scale restores exactly the meaning `errors[]` has today:
    //                  a fraction of the WHOLE mesh's largest extent.
    const unsigned int simplifyOptions = chunkedSubsets
        ? (opt.lodSimplifyOptions | meshopt_SimplifyLockBorder | meshopt_SimplifySparse |
           meshopt_SimplifyErrorAbsolute)
        : opt.lodSimplifyOptions;
    // meshopt reports error RELATIVE to this scale unless SimplifyErrorAbsolute is on, so it is
    // needed unconditionally now: the chunked path uses it to restate the BUDGET in absolute
    // units, and every path uses it to turn the reported error back into object-space metres.
    const float simplifyScale =
        meshopt_simplifyScale(&verts[0].position.x, verts.size(), sizeof(VertexPNTUV));
    const float errorScale = chunkedSubsets ? simplifyScale : 1.0f;

    std::vector<uint32_t> simplified;

    // Attribute scratch for the normal-weighted metric: [normal.xyz, uv.xy] per vertex. Built
    // lazily and rebuilt when `verts` grows -- the foliage prune APPENDS vertices mid-loop, and a
    // stale array here would hand meshopt attributes for vertices that no longer line up.
    constexpr size_t kAttrCount = 5;
    std::vector<float> attribs;
    auto ensureAttribs = [&]()
    {
        if (attribs.size() == verts.size() * kAttrCount) { return; }
        attribs.resize(verts.size() * kAttrCount);
        for (size_t v = 0; v < verts.size(); ++v)
        {
            const VertexPNTUV& vx = verts[v];
            // Normalized on the way in: the weight's meaning ("one unit of normal delta costs W
            // units of position delta") only holds if the deltas are on the unit sphere.
            float nx = vx.normal.x, ny = vx.normal.y, nz = vx.normal.z;
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 1e-8f) { nx /= len; ny /= len; nz /= len; }
            float* a = attribs.data() + v * kAttrCount;
            a[0] = nx; a[1] = ny; a[2] = nz;
            a[3] = vx.uv.x; a[4] = vx.uv.y;
        }
    };

    for (int i = 0; i < 3; ++i)
    {
        MeshLodCpu lod;
        for (const Mesh::Submesh& s : baseSubs)
        {
            const uint32_t* src = indices.data() + s.indexOffset;
            const size_t srcCount = s.indexCount;
            const uint32_t outOffset = static_cast<uint32_t>(lod.indices.size());

            // LOD3 (i == 2) is the HARSH level (user decision 2026-08-21: no fifth level;
            // the last one gets aggressive instead). Opt-out per asset via mesh.json
            // "lod3Aggressive": false — the terrain manifests carry it, because their
            // far-clipmap shadow casters are tuned against low-sun banding and a 0.25 error
            // deforms dune silhouettes.
            float ratio = ratios[i];
            float errBudget = errors[i];
            if (i == 2 && opt.lod3Aggressive)
            {
                // LOD3's OWN multipliers (dialog "LOD3 triangle/error x") — deliberately NOT the
                // whole-chain scales, so the last level is pushed without dragging LODs 1-2.
                ratio = 0.05f * opt.lod3RatioScale;
                errBudget = 0.25f * opt.lod3ErrorScale;
            }

            // Per-level slot drop: the surgical knife for trims that survive every metric (a
            // date palm's husk scales are welded to the trunk — neither Prune nor the error
            // budget can take them without eating the silhouette-critical cylinder first). An
            // EMPTY submesh keeps the table aligned; the draw skips zero-count ranges. Each
            // level's list is independent — every LOD builds from the BASE indices.
            const std::vector<uint32_t>* dropLists[3] =
                { &opt.lod1DropSlots, &opt.lod2DropSlots, &opt.lod3DropSlots };
            if (std::find(dropLists[i]->begin(), dropLists[i]->end(),
                          (uint32_t)s.materialSlot) != dropLists[i]->end())
            {
                lod.submeshes.push_back(Mesh::Submesh{ outOffset, 0u, s.materialSlot });
                continue;
            }

            // LOD3 foliage: whole-leaf prune instead of meshopt (see PruneFoliageRange).
            // Falls through to meshopt when the range has too few components.
            const bool foliageSlot = s.materialSlot < opt.slotFoliage.size() &&
                                     opt.slotFoliage[s.materialSlot] > 0.0f;
            if (i == 2 && !chunkedSubsets && foliageSlot && allowVertexAppend &&
                opt.foliagePruneKeep > 0.0f && opt.foliagePruneKeep < 1.0f)
            {
                std::vector<uint32_t> pruned;
                if (PruneFoliageRange(verts, src, srcCount, opt.foliagePruneKeep,
                                      opt.foliageInnerRatio, opt.foliageInnerError,
                                      opt.foliageGrow, opt.foliageUvWeight, pruned))
                {
                    lod.indices.insert(lod.indices.end(), pruned.begin(), pruned.end());
                    lod.submeshes.push_back(Mesh::Submesh{ outOffset,
                        static_cast<uint32_t>(pruned.size()), s.materialSlot });
                    continue;
                }
            }

            size_t n = srcCount;
            bool didSimplify = false;
            size_t target = static_cast<size_t>(srcCount * ratio);  
            target -= target % 3;
            if (srcCount >= kMinRangeIndices && target >= 12)
            {
                simplified.resize(srcCount);
                float resultError = 0.0f;
                // Options are per-import (MeshLoadOptions::lodSimplifyOptions, exposed in the mesh
                // import window) and default to 0 = meshopt's safe behaviour.
                //
                // WARNING about meshopt_SimplifyPermissive on FOLIAGE (measured + eyeballed 2026-07-23):
                // it does unstick the hard topological floor that alpha-card leaves hit (date_palm's
                // foliage slot otherwise stalls at 5600 -> 4006 tris across LOD 2 AND 3), and it reports
                // a LOWER geometric error while doing it (0.0018 vs 0.0427 at LOD3) — but the result is
                // visually destroyed: the leaf blades collapse into spikes, because the position-only
                // error metric is blind to what actually carries a leaf card's shape (its silhouette
                // and UV island). Do not trust `resultError` on masked foliage; look at the wireframe.
                if (opt.lodNormalWeight > 0.0f)
                {
                    // Normal-weighted collapse (see MeshLoadOptions::lodNormalWeight). Kept as its
                    // OWN branch rather than folded into the UV one so that leaving the weight at 0
                    // reproduces the previous bake exactly, down to meshopt's attribute count --
                    // every asset already on disk was authored against that behaviour.
                    float w[kAttrCount] = { opt.lodNormalWeight, opt.lodNormalWeight,
                                            opt.lodNormalWeight, 0.0f, 0.0f };
                    if (foliageSlot && opt.foliageUvWeight > 0.0f)
                    {
                        w[3] = opt.foliageUvWeight;
                        w[4] = opt.foliageUvWeight;
                    }
                    ensureAttribs();
                    n = meshopt_simplifyWithAttributes(simplified.data(), src, srcCount,
                        &verts[0].position.x, verts.size(), sizeof(VertexPNTUV),
                        attribs.data(), kAttrCount * sizeof(float), w, kAttrCount, nullptr,
                        target, errBudget * errorScale, simplifyOptions, &resultError);
                }
                else if (foliageSlot && opt.foliageUvWeight > 0.0f)
                {
                    // Attribute-aware collapse for alpha cards: a vertex sliding along a flat
                    // frond has ZERO position error but drags its UV across the leaf texture —
                    // the smeared streaks visible from LOD1 on. Weighting UV like position
                    // (weights are relative to mesh extent, ~0.5-1 is meshopt's ballpark)
                    // makes those collapses expensive, so the simplifier spends its budget on
                    // ones that keep the mapping intact. UE's simplifier is attribute-aware
                    // for the same reason.
                    const float uvw[2] = { opt.foliageUvWeight, opt.foliageUvWeight };
                    n = meshopt_simplifyWithAttributes(simplified.data(), src, srcCount,
                        &verts[0].position.x, verts.size(), sizeof(VertexPNTUV),
                        &verts[0].uv.x, sizeof(VertexPNTUV), uvw, 2, nullptr,
                        target, errBudget * errorScale, simplifyOptions, &resultError);
                }
                else
                {
                    n = meshopt_simplify(simplified.data(), src, srcCount,
                        &verts[0].position.x, verts.size(), sizeof(VertexPNTUV),
                        target, errBudget * errorScale, simplifyOptions, &resultError);
                }
                // n == 0 is AMBIGUOUS: without SimplifyPrune it means "gave up" (keep the range
                // as-is), but WITH it it is a legitimate verdict — every part of the range was a
                // small disconnected piece within budget (a trunk's husk scales at LOD3), and the
                // range should VANISH. Resurrecting it here was why "drop small parts" left the
                // scales in place at LOD3 and bloated the level past LOD2 (user-hit 2026-08-21).
                const bool pruneEnabled = (simplifyOptions & meshopt_SimplifyPrune) != 0u;
                if (n == 0 && !pruneEnabled) { n = srcCount; }
                else { didSimplify = true; }
                // Worst case over the ranges: a LOD pops as badly as its WORST submesh does, so
                // an average would describe a level nobody sees. Absolute already when the
                // chunked path asked for absolute; relative to simplifyScale otherwise.
                if (didSimplify && resultError > 0.0f)
                {
                    const float abs = chunkedSubsets ? resultError : resultError * simplifyScale;
                    lod.error = std::max(lod.error, abs);
                }
            }

            const uint32_t* from = didSimplify ? simplified.data() : src;
            lod.indices.insert(lod.indices.end(), from, from + n);
            lod.submeshes.push_back(Mesh::Submesh{ outOffset, static_cast<uint32_t>(n), s.materialSlot });
        }

        // NO shrink gate: every level is emitted even when it failed to shrink (user decision
        // 2026-08-21). The old ~10% gate silently RENUMBERED the chain — a stalled level was
        // skipped and everything after it landed one slot up, so the harsh LOD3 showed at the
        // LOD2 distance. The UV-aware collapse made this routine (it deliberately refuses the
        // texture-smearing collapses, so foliage LOD2 often lands within 10% of LOD1), and a
        // near-identical level costs only its index-buffer bytes — a stable slot->distance
        // mapping is worth far more.
        if (lod.indices.empty()) { break; }
        out.push_back(std::move(lod));
    }
    return out;
}

// Called once at load on the upload command list (runtime fallback path).
void GenerateLods(Mesh* mesh, ID3D12Device* device, ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* keepAlive,
    std::vector<VertexPNTUV>& verts, const std::vector<uint32_t>& indices,
    const MeshLoadOptions& opt)
{
    if (!mesh) { return; }
    // allowVertexAppend = false: the VB is already on the GPU here, so the foliage prune (which
    // appends vertices) is bake-only; this path's foliage LOD3 stays plain meshopt.
    for (const MeshLodCpu& lod : BuildLodsCpu(verts, indices, mesh->GetSubmeshes(), opt,
                                              /*chunkedSubsets=*/false, /*allowVertexAppend=*/false))
    {
        mesh->AddLod(device, uploadCmdList, keepAlive,
            lod.indices.data(), static_cast<UINT>(lod.indices.size()), lod.submeshes, lod.error);
    }
}

// ---------- W7.1b: baked binary mesh cache (cache/meshes/<hash>.mesh.bin) ----------
std::vector<uint32_t> CanonicalNormalSlots(const std::vector<uint32_t>& slots); // defined below
constexpr uint32_t kMeshBinMagic   = 0x4253484Du; // 'MSHB'
// v2 (2026-08-25): each LOD block carries its geometric error, so LOD selection can be driven by
// projected pixels instead of a hand-tuned distance/radius curve. Every .bin must be re-baked; all
// 14 mesh.json manifests carry a "source", so every one of them is regenerable (audited before the
// bump -- a manifest without a source would have been bricked by it).
constexpr uint32_t kMeshBinVersion = 2u;          // bump on any bake-algo / format change -> stale
// NOTE when bumping: a directly-referenced models/*.mesh.bin has no runtime source to rebuild from,
// so a bump makes every one of them stale -> they must ALL be re-baked (--reimport-src/--reimport-out)
// or their meshes silently fail to load. LOD *settings* do not need a bump: they ride in optionsHash.

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
    // Only the wood/foliage PATTERN reaches the baked geometry, not the weights: hashing the raw
    // floats would invalidate every .bin whenever an artist nudges a slider that the runtime reads
    // anyway. Bit i = "slot i is foliage".
    // The bake scale CHANGES THE VERTICES, so it has to invalidate an existing .bin -- otherwise
    // re-importing at a new unit silently reuses geometry baked at the old one.
    h = Fnv1a(&opt.bakeScale, sizeof(opt.bakeScale), h);
    uint64_t pattern = 0;
    for (size_t i = 0; i < opt.slotFoliage.size() && i < 64; ++i)
    {
        if (opt.slotFoliage[i] > 0.0f) { pattern |= (1ull << i); }
    }
    if (pattern != 0) { h = Fnv1a(&pattern, sizeof(pattern), h); }
    // LOD generation knobs change the baked index buffers, so a cached .bin made with different
    // settings must not be reused (only affects the runtime cache; a directly-referenced .bin skips
    // the options check by design). Non-default values only, so existing caches stay valid.
    if (opt.lodRatioScale != 1.0f) { h = Fnv1a(&opt.lodRatioScale, sizeof(float), h); }
    if (opt.lodErrorScale != 1.0f) { h = Fnv1a(&opt.lodErrorScale, sizeof(float), h); }
    if (opt.lodSimplifyOptions != 0u) { h = Fnv1a(&opt.lodSimplifyOptions, sizeof(unsigned int), h); }
    // Reaches the baked geometry: it changes WHICH edges collapse, so a .bin baked without it must
    // not be reused once it is on. Non-default only, so existing caches stay valid.
    if (opt.lodNormalWeight != 0.0f) { h = Fnv1a(&opt.lodNormalWeight, sizeof(float), h); }
    // The prune rewrites LOD3's indices AND appends vertices; non-default only, same rule as above.
    if (opt.foliagePruneKeep != 0.35f) { h = Fnv1a(&opt.foliagePruneKeep, sizeof(float), h); }
    if (!opt.lod3Aggressive) { const uint8_t off = 1u; h = Fnv1a(&off, 1, h); }
    if (opt.lod3RatioScale != 1.0f) { h = Fnv1a(&opt.lod3RatioScale, sizeof(float), h); }
    if (opt.lod3ErrorScale != 1.0f) { h = Fnv1a(&opt.lod3ErrorScale, sizeof(float), h); }
    if (opt.foliageInnerRatio != 0.5f) { h = Fnv1a(&opt.foliageInnerRatio, sizeof(float), h); }
    if (opt.foliageInnerError != 0.15f) { h = Fnv1a(&opt.foliageInnerError, sizeof(float), h); }
    if (!opt.lod3DropSlots.empty())
    {
        h = Fnv1a(opt.lod3DropSlots.data(), opt.lod3DropSlots.size() * sizeof(uint32_t), h);
    }
    // The earlier levels' drop lists are level-TAGGED so {lod1:[2]} cannot hash like
    // {lod2:[2]}; lod3's stays untagged because existing baked hashes already depend on it.
    if (!opt.lod1DropSlots.empty())
    {
        const uint8_t tag = 1u; h = Fnv1a(&tag, 1, h);
        h = Fnv1a(opt.lod1DropSlots.data(), opt.lod1DropSlots.size() * sizeof(uint32_t), h);
    }
    if (!opt.lod2DropSlots.empty())
    {
        const uint8_t tag = 2u; h = Fnv1a(&tag, 1, h);
        h = Fnv1a(opt.lod2DropSlots.data(), opt.lod2DropSlots.size() * sizeof(uint32_t), h);
    }
    if (opt.foliageGrow != 1.0f) { h = Fnv1a(&opt.foliageGrow, sizeof(float), h); }
    if (opt.foliageUvWeight != 0.0f) { h = Fnv1a(&opt.foliageUvWeight, sizeof(float), h); }
    // Chunking rewrites the index buffers AND the submesh tables of every LOD, so a .bin baked at a
    // different grid must not be reused. Non-default only, so existing caches stay valid.
    if (opt.chunkGrid != 0u) { h = Fnv1a(&opt.chunkGrid, sizeof(unsigned int), h); }
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

    const auto writeLod = [&](const std::vector<uint32_t>& idx, const std::vector<Mesh::Submesh>& subs,
                              float error)
    {
        const uint32_t ic = static_cast<uint32_t>(idx.size());
        const uint32_t sc = static_cast<uint32_t>(subs.size());
        f.write(reinterpret_cast<const char*>(&ic), sizeof(ic));
        f.write(reinterpret_cast<const char*>(&sc), sizeof(sc));
        f.write(reinterpret_cast<const char*>(&error), sizeof(error)); // v2
        f.write(reinterpret_cast<const char*>(idx.data()), static_cast<std::streamsize>(ic * sizeof(uint32_t)));
        f.write(reinterpret_cast<const char*>(subs.data()), static_cast<std::streamsize>(sc * sizeof(Mesh::Submesh)));
    };
    writeLod(lod0Indices, lod0Subs, 0.0f); // LOD0 IS the reference; its deviation is zero
    for (const MeshLodCpu& l : extraLods) { writeLod(l.indices, l.submeshes, l.error); }
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
        float err = 0.0f;
        f.read(reinterpret_cast<char*>(&ic), sizeof(ic));
        f.read(reinterpret_cast<char*>(&sc), sizeof(sc));
        f.read(reinterpret_cast<char*>(&err), sizeof(err)); // v2
        if (!f) { return false; }
        lods[i].error = err;
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
    // chunkGrid changes what the loaded Mesh IS (per-submesh caster bounds + the chunked flag), so
    // two objects asking for the same file at different grids must not share one cached Mesh.
    if (slots.empty() && opt.chunkGrid == 0u) { return path; }

    std::string key = path;
    if (!slots.empty())
    {
        key += "|recomputeNormalSlots=";
        for (const uint32_t slot : slots)
        {
            key += std::to_string(slot);
            key.push_back(',');
        }
    }
    if (opt.chunkGrid != 0u) { key += "|chunkGrid=" + std::to_string(opt.chunkGrid); }
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

// ---------------------------------------------------------------------------------------------
// W7.2 — per-vertex wind weights, baked from the mesh's own topology (no tuning constants, no
// assumed crown position). Geodesic distance ALONG THE SURFACE from the plant's ground contact:
// trunk base -> 0, frond tips -> 1. Multi-trunk falls out for free (each trunk seeds from its own
// contact patch), and a frond hanging DOWN the trunk measures "up the trunk, then out along the
// frond" instead of reading as trunk, which is exactly what the old radial heuristic got wrong.
//
// Channels (R8G8B8A8_UNORM in VertexPNTUV::color):
//   R = geodesic weight 0..1
//   G = per-limb id (hashed limb anchor) -> per-frond phase decorrelation
//   B = along-limb edge weight (0 at the limb's axis, 1 at its edge) -> leaf-edge flutter
//   A = 255 marks "weights are baked". Old .bin files predate the bake and have A = 0, so the
//       runtime can tell them apart WITHOUT a format-version bump (a bump would invalidate every
//       committed .bin at once and hard-fail the load, since LoadBinaryDirect has no fallback).
namespace
{
struct WeldGrid
{
    // Uniform grid over welded vertex positions for nearest-neighbour queries. Brute force is
    // O(unreached x reached) per bridge, which is fine at a few thousand verts but not at 100k.
    float cell = 1.0f;
    DirectX::XMFLOAT3 mn{};
    int dim[3]{ 1, 1, 1 };
    std::unordered_map<uint64_t, std::vector<uint32_t>> buckets;

    uint64_t Key(int x, int y, int z) const
    {
        return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 42) ^
               (static_cast<uint64_t>(static_cast<uint32_t>(y)) << 21) ^
                static_cast<uint64_t>(static_cast<uint32_t>(z));
    }
    void CellOf(const DirectX::XMFLOAT3& p, int out[3]) const
    {
        out[0] = static_cast<int>(std::floor((p.x - mn.x) / cell));
        out[1] = static_cast<int>(std::floor((p.y - mn.y) / cell));
        out[2] = static_cast<int>(std::floor((p.z - mn.z) / cell));
    }
};

inline float Dist3(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b)
{
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Fill VertexPNTUV::color for every vertex. No-op (leaves color 0 = rigid) if the mesh is too
// degenerate to walk.
//
// `submeshes` + `slotFoliage` (mesh.json "windFoliage", one entry per material slot) tell the bake
// which geometry is WOOD (weight 0) and which is FOLIAGE. That classification is what lets the
// along-limb channel be a distance from the wood surface rather than a per-component ramp; without it
// the bake still works, it just falls back to the per-component ramp. Pass them whenever they exist:
// a mesh baked without them and then given per-slot foliage weights can show a step at every junction
// the modeller happened to cut.
void BakeWindWeightsCpu(std::vector<VertexPNTUV>& verts, const std::vector<uint32_t>& indices,
    const std::vector<Mesh::Submesh>& submeshes, const std::vector<float>& slotFoliage)
{
    const size_t vcount = verts.size();
    if (vcount == 0 || indices.size() < 3) { return; }

    // ---- weld by quantised position: UV seams would otherwise cut the graph in half ----
    DirectX::XMFLOAT3 bbMin{ FLT_MAX, FLT_MAX, FLT_MAX };
    DirectX::XMFLOAT3 bbMax{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (const VertexPNTUV& v : verts)
    {
        bbMin.x = std::min(bbMin.x, v.position.x); bbMax.x = std::max(bbMax.x, v.position.x);
        bbMin.y = std::min(bbMin.y, v.position.y); bbMax.y = std::max(bbMax.y, v.position.y);
        bbMin.z = std::min(bbMin.z, v.position.z); bbMax.z = std::max(bbMax.z, v.position.z);
    }
    const float diag = Dist3(bbMin, bbMax);
    if (!(diag > 0.0f)) { return; }
    // Scale-relative so a mesh authored in centimetres welds the same as one in metres.
    const float weldEps = std::max(1.0e-6f, diag * 1.0e-4f);
    const float invWeld = 1.0f / weldEps;

    std::unordered_map<uint64_t, uint32_t> weldMap;
    weldMap.reserve(vcount * 2);
    std::vector<uint32_t> weldOf(vcount, 0u);
    std::vector<DirectX::XMFLOAT3> wpos;
    wpos.reserve(vcount);
    for (size_t i = 0; i < vcount; ++i)
    {
        const DirectX::XMFLOAT3& p = verts[i].position;
        const int64_t qx = static_cast<int64_t>(std::llround(p.x * invWeld));
        const int64_t qy = static_cast<int64_t>(std::llround(p.y * invWeld));
        const int64_t qz = static_cast<int64_t>(std::llround(p.z * invWeld));
        const uint64_t key = (static_cast<uint64_t>(qx) * 0x9E3779B97F4A7C15ull) ^
                             (static_cast<uint64_t>(qy) * 0xC2B2AE3D27D4EB4Full) ^
                             (static_cast<uint64_t>(qz) * 0x165667B19E3779F9ull);
        auto it = weldMap.find(key);
        if (it == weldMap.end())
        {
            const uint32_t idx = static_cast<uint32_t>(wpos.size());
            weldMap.emplace(key, idx);
            wpos.push_back(p);
            weldOf[i] = idx;
        }
        else
        {
            weldOf[i] = it->second;
        }
    }
    const uint32_t wcount = static_cast<uint32_t>(wpos.size());
    if (wcount == 0) { return; }

    // ---- wood/foliage classification from the per-slot weights ----
    std::vector<uint32_t> woodSeeds;
    bool haveLeaf = false;
    if (!slotFoliage.empty() && !submeshes.empty())
    {
        std::vector<uint8_t> kind(vcount, 0); // bit0 = referenced, bit1 = referenced by a foliage slot
        for (const Mesh::Submesh& s : submeshes)
        {
            const bool leaf = (s.materialSlot < slotFoliage.size()) && (slotFoliage[s.materialSlot] > 0.0f);
            const size_t end = std::min<size_t>(indices.size(), static_cast<size_t>(s.indexOffset) + s.indexCount);
            for (size_t i = s.indexOffset; i < end; ++i)
            {
                const uint32_t v = indices[i];
                if (v < vcount) { kind[v] |= static_cast<uint8_t>(leaf ? 0x3 : 0x1); }
            }
        }
        std::vector<uint8_t> seedMark(wcount, 0);
        for (size_t i = 0; i < vcount; ++i)
        {
            if (!(kind[i] & 0x1)) { continue; }
            if (kind[i] & 0x2) { haveLeaf = true; }
            else { seedMark[weldOf[i]] = 1; }
        }
        for (uint32_t i = 0; i < wcount; ++i) { if (seedMark[i]) { woodSeeds.push_back(i); } }
    }

    // ---- edge graph from triangles ----
    std::vector<std::vector<std::pair<uint32_t, float>>> adj(wcount);
    {
        std::unordered_set<uint64_t> seen;
        seen.reserve(indices.size());
        const auto addEdge = [&](uint32_t a, uint32_t b)
        {
            if (a == b) { return; }
            const uint64_t k = a < b ? (static_cast<uint64_t>(a) << 32 | b)
                                     : (static_cast<uint64_t>(b) << 32 | a);
            if (!seen.insert(k).second) { return; }
            const float w = Dist3(wpos[a], wpos[b]);
            adj[a].emplace_back(b, w);
            adj[b].emplace_back(a, w);
        };
        for (size_t t = 0; t + 2 < indices.size(); t += 3)
        {
            const uint32_t a = weldOf[indices[t]], b = weldOf[indices[t + 1]], c = weldOf[indices[t + 2]];
            addEdge(a, b); addEdge(b, c); addEdge(c, a);
        }
    }

    // ---- connected components (islands): foliage is usually separate cards ----
    std::vector<int> comp(wcount, -1);
    int compCount = 0;
    {
        std::vector<uint32_t> stack;
        for (uint32_t s = 0; s < wcount; ++s)
        {
            if (comp[s] >= 0) { continue; }
            comp[s] = compCount;
            stack.push_back(s);
            while (!stack.empty())
            {
                const uint32_t u = stack.back(); stack.pop_back();
                for (const auto& e : adj[u])
                {
                    if (comp[e.first] < 0) { comp[e.first] = compCount; stack.push_back(e.first); }
                }
            }
            ++compCount;
        }
    }

    // ---- proximity edges: parts that rest against each other are one surface for wind ----
    // Components come from TRIANGLES, but a plant's parts are modelled as separate meshes that merely
    // rest against each other: bark scales on the trunk, a blade against its petiole, a trunk skin
    // sleeved over a low-poly core. Wiring those contacts up front does three things at once. The
    // weight fields become continuous across them (a scale inherits the r of the trunk it sits on
    // instead of accumulating its own along a chain of siblings), the fields stop depending on which
    // single point an island happened to be bridged through, and -- the reason this is up here rather
    // than folded into the attachment loop -- almost every island is then reached by the plain
    // Dijkstra below, so the O(islands * vertices) loop has nearly nothing left to do. That loop cost
    // 132 s on date_palm in a Debug build; with this pass the whole bake is 0.4 s.
    //
    // K NEAREST, not a fixed radius. A radius has to be guessed per asset and gets it wrong both ways:
    // 0.5 % of the diagonal is 1.6 cm on curly_palm, whose trunk skin sits 4 cm off its core, so the
    // skin stayed unattached and slid over the trunk. Taking each vertex's two nearest neighbours in
    // OTHER components is self-limiting instead -- where geometry actually touches the links are
    // millimetres long, and a long link only appears where there is genuinely nothing closer.
    {
        WeldGrid pg;
        pg.mn = bbMin;
        pg.cell = std::max(diag / 48.0f, weldEps * 4.0f);
        for (uint32_t i = 0; i < wcount; ++i)
        {
            int c[3]; pg.CellOf(wpos[i], c);
            pg.buckets[pg.Key(c[0], c[1], c[2])].push_back(i);
        }
        // Beyond this the attachment loop takes over (it reaches 15 % of the diagonal). Bounding the
        // ring expansion is what keeps a vertex deep inside a large isolated mesh from scanning the
        // whole grid to discover that nothing is near it.
        const float reachR = diag * 0.05f;
        const int maxProxRing = static_cast<int>(std::ceil(reachR / pg.cell)) + 1;
        // The nearest vertex in each of the K nearest DISTINCT components -- not simply the K
        // nearest vertices. date_palm's trunk carries 96 separate bark scales and curly_palm's 100:
        // a scale's two nearest vertices are both on the SAME neighbouring scale, so plain K-nearest
        // just rebuilt the sibling chain it was meant to break. Insisting on distinct components is
        // what gives every scale a direct link to the trunk core underneath it, and Dijkstra then
        // routes it that way because going up the smooth core beats crawling over its siblings.
        constexpr int kNear = 3;
        struct NearComp { float d; uint32_t v; int c; };
        NearComp nrst[kNear];
        for (uint32_t i = 0; i < wcount; ++i)
        {
            for (int k = 0; k < kNear; ++k) { nrst[k] = NearComp{ reachR, 0xFFFFFFFFu, -1 }; }
            int c[3]; pg.CellOf(wpos[i], c);
            int firstHitRing = -1;
            for (int r = 0; r <= maxProxRing; ++r)
            {
                for (int dz = -r; dz <= r; ++dz)
                    for (int dy = -r; dy <= r; ++dy)
                        for (int dx = -r; dx <= r; ++dx)
                        {
                            if (r > 0 && std::abs(dx) != r && std::abs(dy) != r && std::abs(dz) != r) { continue; }
                            auto it = pg.buckets.find(pg.Key(c[0] + dx, c[1] + dy, c[2] + dz));
                            if (it == pg.buckets.end()) { continue; }
                            for (uint32_t j : it->second)
                            {
                                const int cj = comp[j];
                                if (cj == comp[i]) { continue; } // already joined through triangles
                                const float d = Dist3(wpos[i], wpos[j]);
                                int at = -1;
                                for (int k = 0; k < kNear; ++k) { if (nrst[k].c == cj) { at = k; break; } }
                                if (at >= 0)
                                {
                                    if (d >= nrst[at].d) { continue; }
                                    nrst[at].d = d; nrst[at].v = j;
                                }
                                else
                                {
                                    if (d >= nrst[kNear - 1].d) { continue; }
                                    nrst[kNear - 1] = NearComp{ d, j, cj };
                                    at = kNear - 1;
                                }
                                for (int k = at; k > 0 && nrst[k].d < nrst[k - 1].d; --k)
                                {
                                    std::swap(nrst[k], nrst[k - 1]);
                                }
                            }
                        }
                if (firstHitRing < 0 && nrst[0].v != 0xFFFFFFFFu) { firstHitRing = r; }
                // One extra ring past the first hit: a closer component can sit in a diagonal
                // neighbour. Not waiting for all K keeps a vertex with only one neighbouring part
                // from scanning out to the cap.
                if (firstHitRing >= 0 && r > firstHitRing) { break; }
            }
            for (int k = 0; k < kNear; ++k)
            {
                if (nrst[k].v == 0xFFFFFFFFu) { continue; }
                adj[i].emplace_back(nrst[k].v, nrst[k].d);
                adj[nrst[k].v].emplace_back(i, nrst[k].d);
            }
        }
    }

    // ---- multi-source Dijkstra from the ground-contact band ----
    constexpr float kInf = std::numeric_limits<float>::infinity();
    std::vector<float> dist(wcount, kInf);
    std::vector<uint32_t> parent(wcount, 0xFFFFFFFFu);
    using Node = std::pair<float, uint32_t>;
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> pq;

    const float seedBand = std::max(1.0e-5f, diag * 0.01f);
    for (uint32_t i = 0; i < wcount; ++i)
    {
        if (wpos[i].y <= bbMin.y + seedBand) { dist[i] = 0.0f; parent[i] = i; pq.emplace(0.0f, i); }
    }
    const auto relax = [&]()
    {
        while (!pq.empty())
        {
            const auto [d, u] = pq.top(); pq.pop();
            if (d > dist[u]) { continue; }
            for (const auto& e : adj[u])
            {
                const float nd = d + e.second;
                if (nd < dist[e.first]) { dist[e.first] = nd; parent[e.first] = u; pq.emplace(nd, e.first); }
            }
        }
    };
    relax();

    // ---- attach unreachable islands to the plant ----
    // Foliage and bark detail are almost never welded to the trunk, so most of a plant arrives as
    // separate islands that have to be attached. Two properties matter, and the obvious greedy
    // version has neither:
    //
    //  * COST is `dist[target] + gap`, i.e. the geodesic distance the island would INHERIT -- not the
    //    raw gap. Minimising the raw gap lets an island attach to a SIBLING island, and siblings
    //    chain: date_palm's bark scales attached to each other instead of to the trunk 2 mm
    //    underneath, so r accumulated along the chain and a scale ended up 0.11 (28/255) away from
    //    the trunk it sits on -- it visibly slid across the bark as the tree bent. Minimising the
    //    resulting distance is just Dijkstra with the bridge as an edge, and it cannot chain: routing
    //    through a sibling can never beat routing through whatever that sibling itself hangs off.
    //  * The grid holds ONLY REACHED vertices and every island caches its best candidate. The naive
    //    version rescanned every unreached vertex against a grid full of (mostly unreached) vertices
    //    once per attachment -- O(islands * vertices * ring-search), which cost 132 s for date_palm
    //    in a Debug build.
    std::vector<std::vector<uint32_t>> compVerts(compCount);
    for (uint32_t i = 0; i < wcount; ++i) { compVerts[comp[i]].push_back(i); }
    std::vector<bool> compReached(compCount, false);
    for (uint32_t i = 0; i < wcount; ++i) { if (std::isfinite(dist[i])) { compReached[comp[i]] = true; } }

    WeldGrid grid;
    grid.mn = bbMin;
    grid.cell = std::max(diag / 48.0f, weldEps * 4.0f);
    const auto addToGrid = [&](uint32_t v)
    {
        int c[3]; grid.CellOf(wpos[v], c);
        grid.buckets[grid.Key(c[0], c[1], c[2])].push_back(v);
    };
    for (uint32_t i = 0; i < wcount; ++i) { if (std::isfinite(dist[i])) { addToGrid(i); } }

    // A far-away stray island would otherwise attach through a huge bridge and inherit a high
    // weight. Cap it; anything past the cap stays unreached and bakes as rigid.
    const float maxBridge = diag * 0.15f;
    const int maxRing = static_cast<int>(std::ceil(maxBridge / grid.cell)) + 1;

    // Returns the cheapest inherited distance, and the vertex to hang off. Rings expand by GEOMETRY
    // (so the search stays local and terminates fast) but candidates within the searched rings are
    // scored by COST -- which is what stops the sibling chaining above: the trunk and the neighbouring
    // scale are both a millimetre away and land in the same ring, so the cost is what separates them.
    const auto bestAttach = [&](const DirectX::XMFLOAT3& p, uint32_t& outIdx) -> float
    {
        int c[3]; grid.CellOf(p, c);
        float best = kInf, bestGap = kInf; outIdx = 0xFFFFFFFFu;
        for (int r = 0; r <= maxRing; ++r)
        {
            for (int dz = -r; dz <= r; ++dz)
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx)
                    {
                        // ring shell only
                        if (r > 0 && std::abs(dx) != r && std::abs(dy) != r && std::abs(dz) != r) { continue; }
                        auto it = grid.buckets.find(grid.Key(c[0] + dx, c[1] + dy, c[2] + dz));
                        if (it == grid.buckets.end()) { continue; }
                        for (uint32_t v : it->second)
                        {
                            const float g = Dist3(p, wpos[v]);
                            if (g > maxBridge) { continue; }
                            const float cost = dist[v] + g;
                            if (cost < best) { best = cost; bestGap = g; outIdx = v; }
                        }
                    }
            // One extra ring after the first hit: a better candidate can sit in a diagonal neighbour.
            if (std::isfinite(bestGap) && bestGap <= static_cast<float>(r) * grid.cell) { break; }
        }
        return best;
    };

    for (;;)
    {
        int bestComp = -1; float bestCost = kInf; uint32_t bestFrom = 0, bestTo = 0;
        for (int ci = 0; ci < compCount; ++ci)
        {
            if (compReached[ci]) { continue; }
            for (uint32_t v : compVerts[ci])
            {
                uint32_t to = 0xFFFFFFFFu;
                const float cost = bestAttach(wpos[v], to);
                if (cost < bestCost) { bestCost = cost; bestComp = ci; bestFrom = v; bestTo = to; }
            }
        }
        if (bestComp < 0 || !std::isfinite(bestCost)) { break; }

        const float w = Dist3(wpos[bestFrom], wpos[bestTo]);
        dist[bestFrom] = dist[bestTo] + w;
        parent[bestFrom] = bestTo;
        // The bridge is also a real edge of the graph from here on: the wood-distance pass below has
        // to be able to walk crown -> petiole -> blade, and those junctions are bridges, not triangles.
        adj[bestFrom].emplace_back(bestTo, w);
        adj[bestTo].emplace_back(bestFrom, w);
        pq.emplace(dist[bestFrom], bestFrom);
        relax();
        compReached[bestComp] = true;
        for (uint32_t v : compVerts[bestComp]) { addToGrid(v); }
    }

    // ---- normalise -> R ----
    float maxDist = 0.0f;
    for (uint32_t i = 0; i < wcount; ++i) { if (std::isfinite(dist[i])) { maxDist = std::max(maxDist, dist[i]); } }
    if (!(maxDist > 0.0f)) { return; }
    const float invMax = 1.0f / maxDist;

    // ---- G: per-limb id -> per-frond phase decorrelation ----
    // The limb IS the connected component: game foliage is card-based (each frond is its own island,
    // bridged to the trunk above), so hashing the component id gives every vertex of one frond the
    // SAME phase, which is what matters — a phase that varies WITHIN a leaf tears it apart.
    // Rejected alternative: walking the parent chain back a fixed distance. Measured on the staged
    // palm it produced 164 distinct anchors across 35 fronds, because "1.34 m back from ME" is a
    // per-vertex point, not a per-limb one. A welded single-island mesh degrades to one phase for
    // the whole plant, which is correct-but-boring rather than broken.
    // ---- B: position ALONG the limb, 0 at its own attachment -> 1 at its tip ----
    // NOT the global geodesic r. A frond's streaming must bend it about ITS OWN base; driving that
    // with r (which is 0.62..1.00 on a coconut frond) gives the frond BASE 62 % of the push, so the
    // whole leaf translates away from the crown and the canopy tears into streaks.
    //
    // Per-component normalisation (below) is only the FALLBACK, because "component" is a modelling
    // accident, not an anatomical limb. On the coconut palm a frond is a petiole (slot 3, 42 sticks)
    // plus a blade (slot 2, 36 cards), and only some of those pairs are welded: on the rest, b runs
    // 0..1 along the petiole and then RESTARTS at 0 on the blade. The two sides of that junction then
    // differ by the full streaming term -- a measured 1.3 m step at swayAmp 1 -- which reads as a leaf
    // snapped in half a third of the way out. Whether it shows depends on the authored windFoliage,
    // which is why raising the petiole's weight to match the blade's exposed it.
    std::vector<float> alongLimb(wcount, 0.0f);
    for (int ci = 0; ci < compCount; ++ci)
    {
        const std::vector<uint32_t>& vs = compVerts[ci];
        float dmin = kInf, dmax = -kInf;
        for (uint32_t v : vs)
        {
            if (!std::isfinite(dist[v])) { continue; }
            dmin = std::min(dmin, dist[v]);
            dmax = std::max(dmax, dist[v]);
        }
        if (!std::isfinite(dmin) || !(dmax > dmin)) { continue; }
        const float inv = 1.0f / (dmax - dmin);
        for (uint32_t v : vs)
        {
            if (!std::isfinite(dist[v])) { continue; }
            alongLimb[v] = std::clamp((dist[v] - dmin) * inv, 0.0f, 1.0f);
        }
    }

    // The real definition, when the caller told us which slots are wood: b = geodesic distance from
    // the WOOD SURFACE. That is a distance field on the same graph, so it is continuous everywhere by
    // construction -- it cannot restart mid-leaf no matter how the mesh was cut up, and it is exactly
    // 0 where a leaf meets wood, which is what keeps the runtime `foliage * b` continuous across that
    // boundary whatever per-slot weights the artist picked. A welded vertex shared by a wood and a
    // foliage submesh counts as WOOD, so a shared seam is pinned at 0 on both sides and cannot tear.
    float leafScaleMeters = 0.0f; // longest arc from the wood surface; 0 = fell back to per-component
    if (!woodSeeds.empty() && haveLeaf)
    {
        std::vector<float> wdist(wcount, kInf);
        std::priority_queue<Node, std::vector<Node>, std::greater<Node>> wq;
        for (uint32_t v : woodSeeds) { wdist[v] = 0.0f; wq.emplace(0.0f, v); }
        while (!wq.empty())
        {
            const Node n = wq.top(); wq.pop();
            if (n.first > wdist[n.second]) { continue; }
            for (const auto& e : adj[n.second])
            {
                const float nd = n.first + e.second;
                if (nd < wdist[e.first]) { wdist[e.first] = nd; wq.emplace(nd, e.first); }
            }
        }
        float wmax = 0.0f;
        for (uint32_t i = 0; i < wcount; ++i) { if (std::isfinite(wdist[i])) { wmax = std::max(wmax, wdist[i]); } }
        if (wmax > 0.0f)
        {
            leafScaleMeters = wmax;
            const float invW = 1.0f / wmax;
            // Anything the wood cannot reach at all keeps its per-component fallback above.
            for (uint32_t i = 0; i < wcount; ++i)
            {
                if (std::isfinite(wdist[i])) { alongLimb[i] = std::clamp(wdist[i] * invW, 0.0f, 1.0f); }
            }
        }
    }

    // ---- pack ----
    const auto quant = [](float v01) -> uint32_t
    {
        return static_cast<uint32_t>(std::lround(std::clamp(v01, 0.0f, 1.0f) * 255.0f)) & 0xFFu;
    };
    // A: 0 still means "not baked" (legacy .bin -> rigid). Otherwise it carries 1 + the along-limb
    // scale as a fraction of the bbox diagonal, which is what lets the shader turn the normalised B
    // back into METRES of arc from a leaf's attachment. It needs that to keep the leaf's streaming
    // bounded by the leaf's OWN length instead of by a global amplitude -- see wind.hlsli. The
    // per-component fallback has no single scale, so it reports the whole diagonal: the weakest
    // possible bound, i.e. exactly the old unbounded behaviour, rather than a wrong one.
    const float leafFrac = (leafScaleMeters > 0.0f) ? std::min(1.0f, leafScaleMeters / diag) : 1.0f;
    const uint32_t aVal = 1u + static_cast<uint32_t>(std::lround(leafFrac * 254.0f));
    for (size_t i = 0; i < vcount; ++i)
    {
        const uint32_t w = weldOf[i];
        const float r01 = std::isfinite(dist[w]) ? dist[w] * invMax : 0.0f;
        const uint32_t limbHash = (static_cast<uint32_t>(comp[w] + 1) * 2654435761u) >> 24;
        const uint32_t r = quant(r01);
        const uint32_t g = limbHash & 0xFFu;
        const uint32_t b = quant(alongLimb[w]);
        verts[i].color = r | (g << 8) | (b << 16) | (aVal << 24);
    }
}
} // namespace

bool MeshManager::BinaryNeedsRebake(const std::string& binPath, const MeshLoadOptions& opt)
{
    std::ifstream f(binPath, std::ios::binary);
    if (!f) { return true; }
    MeshBinHeader h{};
    f.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (!f || h.magic != kMeshBinMagic || h.version != kMeshBinVersion) { return true; }
    return h.optionsHash != HashOptions(opt);
}

bool MeshManager::BakeToBinary(const std::string& srcPath, const std::string& outBinPath,
    const MeshLoadOptions& opt)
{
    MeshCpuData cpu;
    if (!ParseFileCpu(srcPath, cpu, opt)) { return false; } // parse glTF + regen normals/tangents (CPU)

    // Unit correction into the GEOMETRY. Before everything below, so the wind bake's metre-based
    // sway extent and the LOD error budgets both see the final scale.
    if (opt.bakeScale > 0.0f && opt.bakeScale != 1.0f)
    {
        for (VertexPNTUV& v : cpu.vertices)
        {
            v.position.x *= opt.bakeScale;
            v.position.y *= opt.bakeScale;
            v.position.z *= opt.bakeScale;
        }
    }

    std::vector<Mesh::Submesh> lod0Subs = cpu.submeshes;
    if (lod0Subs.empty())
    {
        lod0Subs.push_back(Mesh::Submesh{ 0u, static_cast<uint32_t>(cpu.indices.size()), 0u });
    }

    // W7.2: per-vertex wind weights into .color. After the submeshes exist, so the bake can tell wood
    // from foliage; before LOD building, so every LOD inherits the same weights.
    BakeWindWeightsCpu(cpu.vertices, cpu.indices, lod0Subs, opt.slotFoliage);

    // Mesh chunking, between the wind bake (vertex colors only — the reorder below cannot disturb
    // it) and LOD building (which then simplifies every chunk independently). v1 supports
    // single-submesh input only: a multi-material mesh would need the grid crossed with the material
    // table, and nothing needs that yet.
    bool chunked = false;
    if (opt.chunkGrid > 1u) // grid 0/1 == one tile == not chunked; not a failure, so not reported as one
    {
        chunked = ChunkifyLod0(cpu.vertices, cpu.indices, lod0Subs, opt.chunkGrid);
        char cmsg[256];
        std::snprintf(cmsg, sizeof(cmsg),
            chunked ? "[meshbake] chunkGrid=%u -> %zu chunk submeshes\n"
                    : "[meshbake] chunkGrid=%u REJECTED (needs a single-submesh mesh with a "
                      "non-degenerate XZ extent); baking unchunked. submeshes=%zu\n",
            opt.chunkGrid, lod0Subs.size());
        OutputDebugStringA(cmsg);
    }

    // allowVertexAppend = true: the prune's scaled leaf copies grow cpu.vertices BEFORE
    // WriteMeshBinary serialises the VB, so the bin carries them with no format change.
    const std::vector<MeshLodCpu> extra = BuildLodsCpu(cpu.vertices, cpu.indices, lod0Subs, opt,
                                                       chunked, /*allowVertexAppend=*/true);
    const bool ok = WriteMeshBinary(outBinPath, HashSourceFile(srcPath), HashOptions(opt),
        cpu.vertices, cpu.indices, lod0Subs, extra);
    char msg[512];
    int len = std::snprintf(msg, sizeof(msg), "[meshbake] %s '%s' -> '%s' (%zu verts, %zu LODs, %zu submeshes; tris",
        ok ? "ok" : "FAILED", srcPath.c_str(), outBinPath.c_str(), cpu.vertices.size(),
        extra.size() + 1, lod0Subs.size());
    len += std::snprintf(msg + len, sizeof(msg) - len, " %zu", cpu.indices.size() / 3);
    for (const MeshLodCpu& l : extra)
    {
        if (len < (int)sizeof(msg) - 24) { len += std::snprintf(msg + len, sizeof(msg) - len, "/%zu", l.indices.size() / 3); }
    }
    std::snprintf(msg + len, sizeof(msg) - len, ")\n");
    OutputDebugStringA(msg);
    return ok;
}

std::shared_ptr<Mesh> MeshManager::LoadBinaryDirect(const std::string& binPath,
    Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive,
    const MeshLoadOptions& opt)
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
            lods[i].indices.data(), static_cast<UINT>(lods[i].indices.size()), lods[i].submeshes,
            lods[i].error);
    }
    // mesh.json "chunkGrid": this .bin's LOD0 submeshes are spatial chunks, so give each one its own
    // local AABB (the shadow path turns those into independent casters). The .bin format carries no
    // flag of its own — mesh.json is the single place that says a mesh is chunked, which is why the
    // bake flag and the mesh.json value MUST agree (see --reimport-chunk).
    if (opt.chunkGrid > 0u && lods[0].submeshes.size() > 1u)
    {
        mesh->MarkChunkedSubmeshes(verts, lods[0].indices.data(),
            static_cast<UINT>(lods[0].indices.size()));
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
            std::shared_ptr<Mesh> m = LoadBinaryDirect(geom, renderer, uploadCmdList, uploadKeepAlive, opt);
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
    GenerateLods(m.get(), renderer->GetDevice(), uploadCmdList, uploadKeepAlive, verts, inds, opt);
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
    GenerateLods(m.get(), renderer->GetDevice(), uploadCmdList, uploadKeepAlive, verts, inds, opt);
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
    GenerateLods(m.get(), renderer->GetDevice(), uploadCmdList, uploadKeepAlive, verts, inds, opt);
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
    // This overload builds from raw vertex data (no import options in scope) — default LOD settings.
    GenerateLods(m.get(), renderer->GetDevice(), uploadCmdList, uploadKeepAlive, verts, indices,
                 MeshLoadOptions{});
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

// One parse per SELECTOR, not per material slot.
//
// This used to `cgltf_parse_file` the whole .gltf/.glb on every call, and MaterialDataManager calls
// it once per slot — so a four-material mesh re-read and re-parsed the entire file four times, and
// did it again for every thumbnail. Measured in the editor's thumbnail path: single jobs at
// 519 ms, 2170 ms, 2220 ms, essentially all of it here (logs/thumbnail_profile.log).
//
// The parse fills EVERY group's descriptor, so slots 1..N are free, and the entry is keyed on the
// file's last-write time so a re-import or an edit in another tool still takes effect.
namespace {

struct GltfMaterialCacheEntry {
    std::filesystem::file_time_type stamp{};
    std::vector<GltfMaterialDesc> byGroup; // index = resolved group ordinal
};

std::mutex gGltfMaterialCacheMtx;
std::unordered_map<std::string, GltfMaterialCacheEntry> gGltfMaterialCache;

bool EndsWithNoCase(const std::string& s, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    if (s.size() < n) { return false; }
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower((unsigned char)s[s.size() - n + i]) !=
            std::tolower((unsigned char)suffix[i])) { return false; }
    }
    return true;
}

std::filesystem::file_time_type FileStamp(const std::string& file)
{
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(file, ec);
    return ec ? std::filesystem::file_time_type{} : t;
}

} // namespace

void MeshManager::InvalidateGltfMaterialCache()
{
    std::lock_guard<std::mutex> lk(gGltfMaterialCacheMtx);
    gGltfMaterialCache.clear();
}

GltfMaterialDesc MeshManager::DescribeGltfMaterial(const std::string& pathWithFragment,
    int groupOrdinal)
{
    GltfMaterialDesc out;
    const GltfSelector sel = ParseGltfSelector(pathWithFragment);

    // NOT A GLTF -> do not touch it. `cgltf_parse_file` READS THE WHOLE FILE before it can decide
    // the contents are not glTF, so calling this for a baked .mesh.bin (which the "auto" material
    // slot did, for every slot, for every thumbnail) meant reading the asset off disk purely to
    // throw the result away. The engine's own meshes have not been glTF for a long time; this path
    // exists only for previewing an unimported staging asset.
    if (!EndsWithNoCase(sel.file, ".gltf") && !EndsWithNoCase(sel.file, ".glb")) {
        return out; // valid=false: no glTF material to describe
    }

    // Cache hit: the file has not changed since it was described, so every group is already known.
    const std::filesystem::file_time_type stamp = FileStamp(sel.file);
    {
        std::lock_guard<std::mutex> lk(gGltfMaterialCacheMtx);
        const auto it = gGltfMaterialCache.find(pathWithFragment);
        if (it != gGltfMaterialCache.end() && it->second.stamp == stamp) {
            const std::vector<GltfMaterialDesc>& all = it->second.byGroup;
            if (all.empty()) { return out; }
            const size_t want = (groupOrdinal >= 0)
                ? std::min(static_cast<size_t>(groupOrdinal), all.size() - 1)
                : SelectGltfGroup(sel, pathWithFragment, all.size());
            return (want < all.size()) ? all[want] : out;
        }
    }

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
    const std::string dir = DirOf(sel.file);

    // Describe EVERY group in this one parse — the caller asks slot by slot.
    std::vector<GltfMaterialDesc> all(groups.size());
    for (size_t g = 0; g < groups.size(); ++g) {
        GltfMaterialDesc& d = all[g];
        const cgltf_size mi = groups[g].materialIndex;
        if (mi == kNoMat || mi >= data->materials_count) {
            continue; // null-material group -> valid stays false
        }
        const cgltf_material& m = data->materials[mi];
        if (m.has_pbr_metallic_roughness) {
            const cgltf_pbr_metallic_roughness& pbr = m.pbr_metallic_roughness;
            for (int i = 0; i < 4; ++i) { d.baseColor[i] = pbr.base_color_factor[i]; }
            d.metallic = pbr.metallic_factor;
            d.roughness = pbr.roughness_factor;
            d.albedoPath = ResolveTexUri(pbr.base_color_texture.texture, dir);
            d.mrPath = ResolveTexUri(pbr.metallic_roughness_texture.texture, dir);
        }
        d.normalPath = ResolveTexUri(m.normal_texture.texture, dir);
        d.normalScale = (m.normal_texture.texture ? m.normal_texture.scale : 1.0f);
        for (int i = 0; i < 3; ++i) { d.emissive[i] = m.emissive_factor[i]; }
        d.emissivePath = ResolveTexUri(m.emissive_texture.texture, dir);
        d.alphaMask = (m.alpha_mode == cgltf_alpha_mode_mask);
        d.alphaCutoff = m.alpha_cutoff;
        d.doubleSided = (m.double_sided != 0);
        d.valid = true;
    }
    cgltf_free(data);

    size_t want = SelectGltfGroup(sel, pathWithFragment, groups.size());
    if (groupOrdinal >= 0) {
        // B2: explicit ordinal = submesh index of a multi-submesh load (same ordered group list).
        want = std::min(static_cast<size_t>(groupOrdinal), groups.size() - 1);
    }
    if (want < all.size()) { out = all[want]; }

    {
        std::lock_guard<std::mutex> lk(gGltfMaterialCacheMtx);
        gGltfMaterialCache[pathWithFragment] = GltfMaterialCacheEntry{ stamp, std::move(all) };
    }

    GltfLog("material '" + pathWithFragment + "': group " + std::to_string(want) +
        ", metal=" + std::to_string(out.metallic) + " rough=" + std::to_string(out.roughness) +
        (out.alphaMask ? " MASK" : "") + (out.doubleSided ? " 2sided" : "") +
        " albedo=" + (out.albedoPath.empty() ? "-" : "y") +
        " mr=" + (out.mrPath.empty() ? "-" : "y") +
        " nrm=" + (out.normalPath.empty() ? "-" : "y"));

    return out;
}

bool MeshManager::DescribeMeshBinary(const std::string& binPath, BinaryInfo& out)
{
    out = {};
    std::vector<VertexPNTUV> verts;
    std::vector<MeshLodCpu> lods;
    // Freshness checks skipped on purpose (same reason LoadBinaryDirect skips them): a committed
    // .bin IS the asset, there is no source to compare against.
    if (!ReadMeshBinary(GeometryFilePart(binPath), nullptr, nullptr, verts, lods) || lods.empty())
    {
        return false;
    }
    out.vertexCount = static_cast<uint32_t>(verts.size());
    out.lods.reserve(lods.size());
    for (const MeshLodCpu& lod : lods)
    {
        BinaryLodInfo info;
        info.totalTris = static_cast<uint32_t>(lod.indices.size() / 3u);
        info.error = lod.error;
        info.submeshTris.reserve(lod.submeshes.size());
        for (const Mesh::Submesh& sub : lod.submeshes)
        {
            info.submeshTris.push_back(sub.indexCount / 3u);
        }
        out.lods.push_back(std::move(info));
    }
    return true;
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
        // NOTE: returns the count of unique MATERIAL SLOTS, not submeshes — a chunked terrain
        // has 36 spatial submeshes that all share slot 0, and the Mesh Editor was rendering a
        // material picker per CHUNK (user-reported wall of "auto" rows).
        // W7.1b: our baked binary — the submesh count is stored (LOD0's table). The Mesh Editor uses
        // this to show one material picker per slot, so a multi-slot palm must report all its slots.
        std::vector<VertexPNTUV> verts;
        std::vector<MeshLodCpu> lods;
        if (ReadMeshBinary(sel.file, nullptr, nullptr, verts, lods) && !lods.empty())
        {
            uint32_t maxSlot = 0;
            for (const Mesh::Submesh& s : lods[0].submeshes) { maxSlot = std::max(maxSlot, s.materialSlot); }
            return static_cast<size_t>(maxSlot) + 1;
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

// ---------- mesh.json -> MeshLoadOptions (the one authoritative reader; see the header) ----------
bool MeshManager::ApplyManifestOptions(const std::string& meshJsonPath, MeshLoadOptions& opt)
{
    std::ifstream f(meshJsonPath);
    if (!f) { return false; }
    nlohmann::json doc;
    try { f >> doc; } catch (...) { return false; }
    if (!doc.is_object()) { return false; }

    const auto num = [&doc](const char* k, float& dst)
    {
        const auto it = doc.find(k);
        if (it != doc.end() && it->is_number()) { dst = it->get<float>(); }
    };
    const auto boolean = [&doc](const char* k, bool& dst)
    {
        const auto it = doc.find(k);
        if (it != doc.end() && it->is_boolean()) { dst = it->get<bool>(); }
    };
    const auto uints = [&doc](const char* k, std::vector<uint32_t>& dst)
    {
        const auto it = doc.find(k);
        if (it == doc.end() || !it->is_array()) { return; }
        dst.clear();
        for (const nlohmann::json& v : *it)
        {
            if (v.is_number_unsigned() || v.is_number_integer()) { dst.push_back(v.get<uint32_t>()); }
        }
    };

    num("lodRatioScale", opt.lodRatioScale);
    num("lodErrorScale", opt.lodErrorScale);
    num("lod3RatioScale", opt.lod3RatioScale);
    num("lod3ErrorScale", opt.lod3ErrorScale);
    num("foliagePruneKeep", opt.foliagePruneKeep);
    num("foliageInnerRatio", opt.foliageInnerRatio);
    num("foliageInnerError", opt.foliageInnerError);
    num("foliageGrow", opt.foliageGrow);
    num("foliageUvWeight", opt.foliageUvWeight);
    num("lodNormalWeight", opt.lodNormalWeight);
    num("bakeScale", opt.bakeScale);
    boolean("lod3Aggressive", opt.lod3Aggressive);
    uints("lod1DropSlots", opt.lod1DropSlots);
    uints("lod2DropSlots", opt.lod2DropSlots);
    uints("lod3DropSlots", opt.lod3DropSlots);
    uints("recomputeNormalSlots", opt.recomputeNormalSlots);

    // Two keys do not map one-to-one and are restated here rather than left to the caller:
    //   chunkGrid  is an unsigned grid, not a float
    //   windFoliage is a per-SLOT weight array whose position IS the slot index
    {
        const auto it = doc.find("chunkGrid");
        if (it != doc.end() && it->is_number_integer())
        {
            const int g = it->get<int>();
            opt.chunkGrid = g > 0 ? static_cast<unsigned int>(g) : 0u;
        }
    }
    {
        const auto it = doc.find("windFoliage");
        if (it != doc.end() && it->is_array())
        {
            opt.slotFoliage.clear();
            for (const nlohmann::json& v : *it)
            {
                opt.slotFoliage.push_back(v.is_number() ? v.get<float>() : 0.0f);
            }
        }
    }

    // The two meshopt flag bits ride in one uint, so they are set/cleared rather than assigned.
    {
        const auto it = doc.find("lodPermissive");
        if (it != doc.end() && it->is_boolean())
        {
            if (it->get<bool>()) { opt.lodSimplifyOptions |= meshopt_SimplifyPermissive; }
            else                 { opt.lodSimplifyOptions &= ~meshopt_SimplifyPermissive; }
        }
    }
    {
        const auto it = doc.find("lodDropSmallParts");
        if (it != doc.end() && it->is_boolean())
        {
            if (it->get<bool>()) { opt.lodSimplifyOptions |= meshopt_SimplifyPrune; }
            else                 { opt.lodSimplifyOptions &= ~meshopt_SimplifyPrune; }
        }
    }
    return true;
}
