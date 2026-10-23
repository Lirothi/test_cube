#pragma once
#if WITH_EDITOR

#include <optional>
#include <string>
#include <vector>

struct GltfMaterialDesc; // rendering/meshes/MeshManager.h
struct MeshCpuData;      // rendering/meshes/MeshManager.h
namespace assets { struct AlphaImage; } // assets/AssetImporter.h

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

    // Is this glTF material an alpha CUTOUT, and at what threshold? MASK is one by definition. BLEND
    // with a base-colour texture is read as one too: it is how Sketchfab and most exporters ship
    // cut-out foliage, and this renderer has no blended opaque path -- drawn opaque, every leaf card
    // is a rectangle. A BLEND material with no texture (tinted glass) stays opaque.
    bool IsAlphaCutout(const GltfMaterialDesc& d, float* cutoffOut = nullptr);

    // LEAF CARD or SURFACE. A leaf card (frond, fern, grass blade) gets the palms' foliage shading
    // -- twoSidedFoliage, light through the leaf -- and its slot is marked wind foliage at import;
    // a surface is lit as an opaque card and stands rigid.
    //
    // A leaf is a two-sided alpha cutout whose cutout REMOVES a real share of the surface. The
    // flags alone said leaf for anything Sketchfab touched: it exports every material doubleSided
    // and every material with a hint of opacity as BLEND, so the_bathroom_free's curtains, doors,
    // towels and carpets came in as leaf cards and swayed in the wind indoors (2026-10-23). The
    // share of the textured surface whose alpha is below the cutoff, measured over import_staging/:
    // the leaf materials 34-77 % (coconut, curly and date palm fronds, fern_02, ground_palm,
    // young_palm); the bathroom's Door_and_windows 4.5 %, Fabric 0.5 %, Cups 11.8 %.
    //
    // doubleSided stays a condition: curly_palm's single-sided frond boot cuts 43 % away and is
    // bark, rigid and defaultLit by hand. The price is date_palm's fronds, also single-sided and
    // leaves -- the import dialog's per-material choice is for those.
    constexpr float kLeafCutShare = 0.2f;

    struct LeafVerdict
    {
        int materialIndex = -1;  // the glTF material (GltfMaterialDesc::materialIndex); -1 = none
        std::string name;        // its glTF name
        bool candidate = false;  // a valid two-sided alpha cutout: the only kind that can be a leaf
        float cutShare = -1.0f;  // share of the surface the cutout removes; -1 = not measured
        bool leaf = false;
        std::string reason;      // one line, for the dialog, the log and the MCP answer
    };
    // One verdict per material group of `geometry`, in submesh order (a "#node:" selector narrows it
    // to that node's groups, the whole file gives every material it draws). Only candidates cost
    // anything: the geometry is parsed once and each candidate's albedo alpha decoded once.
    std::vector<LeafVerdict> ClassifyLeafCards(const std::string& geometry);
    // The measurement itself: the share of submesh `submesh`'s area whose texcoord-0 lands on alpha
    // below `cutoff` (0..1), sampled at `samples` area-weighted points, the UVs wrapped as the
    // sampler repeats them. -1 when the submesh has no area or the image is empty.
    float MeasureCutShare(const MeshCpuData& cpu, size_t submesh, const assets::AlphaImage& alpha,
        float cutoff, int samples = 16384);
    // The rule, given the measurement: candidates at or over kLeafCutShare are leaves.
    LeafVerdict DecideLeafCard(const GltfMaterialDesc& d, float cutShare);

    // GLASS (shadingModel thinTranslucent, glTF KHR_materials_transmission) or not, per material group
    // of `geometry` -- per NODE for a split import, because one glTF material is often a whole atlas:
    // the_bathroom_free's "Cups" covers ceramic mugs and glass tumblers alike, and only its
    // transmission map says which texels are glass (measured: Cup_glass_1 99 %, Cup_glass_2 18 %,
    // Cup_Ceramic_* 0 %). A group none of whose surface transmits stays as it was (defaultLit). A glass
    // group that is also a cutout KEEPS its alpha test when some of its surface is opaque (alpha at or
    // over the cutoff) -- that half takes the G-buffer, the rest is glass; with nothing opaque it drops
    // the test and is all glass (MaterialData::IsGlassOnly).
    constexpr float kGlassTransmissionMin = 0.02f; // a texel transmits from here (factor * map)
    constexpr float kGlassOpaqueShareMin = 0.001f; // a split needs at least this much opaque surface
    struct GlassVerdict
    {
        bool glass = false;
        bool split = false;         // keeps the alpha test (see above)
        float glassShare = -1.0f;   // share of the surface that transmits; -1 = not measured
        float opaqueShare = -1.0f;  // share the cutout keeps; -1 = not a cutout / not measured
        std::string reason;         // one line, for the log
    };
    GlassVerdict ClassifyGlass(const std::string& geometry, int ordinal);

    // Write data/materials/<name>.json for material `ordinal` of `geometry` (a glTF/GLB path, with
    // an optional #node: selector). If `overwrite` is false an existing file is preserved (keeps
    // material-editor edits across a re-import). Returns `name` when a material was found/kept,
    // "auto" for a null-material slot, or "" on parse failure.
    //
    // `twoSided` follows the glTF's `doubleSided` only where the surface needs it: an opaque
    // material on a closed surface (see SurfaceMeasure) is written single-sided, because exporters
    // such as Sketchfab's mark EVERY material double-sided. Alpha-masked materials keep the flag
    // unconditionally -- a card is open by construction. `measure` lets a caller writing several
    // slots of one geometry parse it once; null = measure here if needed. `leaf` is the caller's
    // decision (the import's, which may be the person's); empty = ClassifyLeafCards here.
    std::string WriteFromGltf(const std::string& geometry, int ordinal,
        const std::string& name, bool overwrite = false, SurfaceMeasure* measure = nullptr,
        std::optional<bool> leaf = std::nullopt);

    // Switch an EXISTING data/materials/<name>.json between leaf and surface shading: the four keys
    // WriteFromGltf writes differently for a leaf (shadingModel, transmissionStrength,
    // transmissionNormalWeight, indirectSpecularScale). A leaf also gets twoSided (twoSidedFoliage
    // draws both faces regardless); a surface keeps the twoSided it has, because a cut-out curtain
    // still needs its back face -- unlike the Material Editor's combo, which clears it. Every other
    // key, edits included, is kept. True when the file was rewritten; false when it already said so
    // or cannot be read.
    bool ApplyLeafShading(const std::string& name, bool leaf);
}

#endif // WITH_EDITOR
