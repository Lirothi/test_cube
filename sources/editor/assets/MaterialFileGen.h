#pragma once
#if WITH_EDITOR

#include <string>
#include <vector>

// Part I3 — write a schema-v2 material file (data/materials/<name>.json) from a glTF material,
// shared by the importer (auto-materials -> named files at import) and the inspector's
// "Save slot as material" button. Uses MeshManager::DescribeGltfMaterial so the ordinal->material
// mapping matches the runtime exactly; textures come out as the glTF's resolved paths (H2 resolves
// the DDS sibling); glTF factors are already baked into the imported DDS by H6, so a param is only
// written when the corresponding texture is absent.
namespace materialgen
{
    // How OPEN each submesh's surface is, parsed once per geometry and shared by its slots.
    //
    // Every edge that is not used exactly once in each direction -- open, shared with a flipped
    // neighbour, or shared by three or more triangles -- is a place a single-sided surface could
    // show a gap. Their total length L bounds the area of those gaps (a loop of length L encloses
    // at most L^2 / 4PI), so `L^2 / (4PI * surface area)` is the fraction of the surface that
    // could be a hole: 0 for a sealed solid, ~1.3 for a flat sheet, 2r/h for a tube open at both
    // ends. Positions are welded first, so UV and normal seams do not count.
    struct SurfaceMeasure
    {
        bool measured = false;
        std::vector<float> seamRatios;  // per submesh; empty when the geometry could not be read
    };
    // Below this a surface counts as CLOSED: its backfaces can never be seen, so a glTF
    // `doubleSided` on it only doubles the rasterised triangles. Measured on import_staging/
    // (2026-09-23): photogrammetry sticks and rocks 0 to 0.0036, a tent's open-ended sticks and
    // ropes ~0.045, palm trunks 0.013 to 0.08, tent fabric 0.83.
    constexpr float kClosedSeamRatio = 0.01f;
    std::vector<float> MeasureSeamRatios(const std::string& geometry);

    // The importer's twoSided call for one slot, and why. Shared by WriteFromGltf and
    // `--seam-ratio`, so the probe cannot report a verdict the importer would not reach. The rule
    // only ever DROPS a glTF doubleSided: a slot whose glTF has none stays single-sided however
    // open its surface is (a palm trunk open at the base, say).
    struct TwoSidedDecision
    {
        bool twoSided = false;
        const char* reason = "";
    };
    TwoSidedDecision DecideTwoSided(bool gltfDoubleSided, bool alphaMask, bool measured,
        float seamRatio);

    // Write data/materials/<name>.json for material `ordinal` of `geometry` (a glTF/GLB path, with
    // an optional #node: selector). If `overwrite` is false an existing file is preserved (keeps
    // material-editor edits across a re-import). Returns `name` when a material was found/kept,
    // "auto" for a null-material slot, or "" on parse failure.
    //
    // `twoSided` follows the glTF's `doubleSided` only where the surface needs it: an opaque
    // material on a closed surface (see SurfaceMeasure) is written single-sided, because exporters
    // such as Sketchfab's mark EVERY material double-sided. Alpha-masked materials keep the flag
    // unconditionally -- a card is open by construction. `measure` lets a caller writing several
    // slots of one geometry parse it once; null = measure here if needed.
    std::string WriteFromGltf(const std::string& geometry, int ordinal,
        const std::string& name, bool overwrite = false, SurfaceMeasure* measure = nullptr);
}

#endif // WITH_EDITOR
