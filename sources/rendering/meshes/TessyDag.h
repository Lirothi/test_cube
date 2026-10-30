#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rendering/meshes/Mesh.h"

// Tessy N7 (docs/tessy_plan.md): the CLUSTER DAG -- UE Nanite's continuous LOD (NaniteBuilder ClusterDAG.cpp) built
// over a mesh's LOD0 clusters. Per submesh: the clusters are partitioned into groups of ~16 (meshopt_partitionClusters;
// UE 8-32), each group's triangles simplified to half with the vertices it shares with other groups LOCKED (by
// position, so a UV seam on the border locks too; a mesh's open edges stay free, as UE leaves them), the result recut
// into clusters, and the new level partitioned again -- until one cluster is left or no group simplifies any more.
// Every cluster carries the error and bounding sphere of the simplification that MADE it (lod*) and of the one that
// CONSUMES it (parent*); errors only grow up an edge and a parent's sphere holds its children's, so a view that draws
// "my error is fine and my parent's is not" draws a crack-free cut at any distance (UE ClusterDAG.cpp:1438-1446, 1587).
// Errors are meshopt's absolute ones -- object-space metres, as Mesh::GetLodError's.
namespace tessy
{
struct DagBuildParams
{
    float normalWeight = 0.0f;       // MeshLoadOptions::lodNormalWeight (the discrete LODs' normal-weighted metric)
    std::vector<float> slotUvWeight; // per material slot: MeshLoadOptions::foliageUvWeight on foliage slots, else 0
    // Per material slot: 1 = leaf cards, simplified with meshopt_SimplifyPrune -- a whole card leaves once its size fits
    // the group's error (the discrete foliage chain drops whole leaves; edge collapses alone stall a crown at ~45 %).
    std::vector<unsigned char> slotPrune;
    uint32_t groupSize = 16;         // meshopt_partitionClusters' target (clusters a group)
};

struct DagStats
{
    uint32_t levels = 0;        // the deepest submesh's, leaves included
    uint32_t clusters = 0;      // every level
    uint32_t leaves = 0;
    uint32_t roots = 0;
    uint32_t stuckGroups = 0;   // groups that would not halve (their clusters went up a level as they were)
    uint64_t triangles = 0;     // every level
    uint64_t leafTriangles = 0;
    uint64_t rootTriangles = 0;
    float rootError = 0.0f;     // the largest error a root carries (object space)
    double ms = 0.0;
};

// Builds the DAG of every LOD0 submesh. `lod0Indices` / `lod0Clusters` are LOD0 as uploaded (in cluster order). False
// (and `out` empty) for a mesh without clusters.
bool BuildDag(const std::vector<VertexPNTUV>& verts, const std::vector<uint32_t>& lod0Indices,
              const std::vector<Mesh::Cluster>& lod0Clusters, const std::vector<Mesh::Submesh>& lod0Submeshes,
              const DagBuildParams& params, Mesh::Dag& out, DagStats* stats);

// The invariants a cut relies on: per cluster parentError >= lodError and the parent's sphere holding its own; a root's
// parentError is FLT_MAX; every submesh's levels contiguous and in order. `why` names the first failure.
bool ValidateDag(const Mesh::Dag& dag, std::string* why);

// The triangles of the cut a threshold `t` (object space, the ortho case -- no distance) selects: lodError <= t <
// parentError. t = 0 is LOD0; a t past every error is the roots.
uint64_t DagCutTriangles(const Mesh::Dag& dag, float t);
} // namespace tessy
