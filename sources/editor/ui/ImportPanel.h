#pragma once
#if WITH_EDITOR

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "editor/assets/AssetRegistry.h"

struct MeshLoadOptions; // rendering/meshes/MeshManager.h — LOD/bake knobs a mesh import carries

// Part H3 — content-browser "Import Assets" window. Scans import_staging/ for raw downloads
// (glTF/GLB folders, texture-set folders, .hdr skyboxes), shows what was detected + license, and
// runs the H1 backend (assets::RunImport) on a background thread to produce engine-ready DDS
// siblings — so the user never hand-runs the --import CLI. On completion it copies the asset into
// the right engine tree BY TYPE (meshes -> models/, texture sets + skyboxes -> textures/, which is
// the only root the AssetRegistry indexes .dds under), appends a CREDITS.md entry, and refreshes
// the AssetRegistry.
class ImportPanel
{
public:
    ImportPanel();
    ~ImportPanel();

    // Draw the window body (inside a lambda panel). Returns true if an import just finished this
    // frame (the caller refreshes the AssetRegistry).
    bool Draw(AssetRegistry& registry, bool* open);

    // Name of the asset the LAST completed import wrote (the folder/file stem, so
    // models/<name>.mesh.json and the split-node models/<name>_<part>.mesh.json both start with
    // it). Valid when Draw() returned true; the caller uses it to refresh exactly the placed
    // instances that changed rather than every static mesh in the level.
    const std::string& LastImportedName() const { return lastImportedName_; }

    // Engine-relative paths of every file the LAST completed import (re)wrote: the converted .dds,
    // the skybox's IBL siblings, and the data/materials/*.json definitions it created or repointed.
    //
    // PUBLISHED RATHER THAN REDISCOVERED. Caches keyed on a path (the shared Texture2D index, the
    // decode cache, built MaterialData) go on serving the previous import until something tells
    // them the file underneath changed, and a running session has no other way to learn it -- the
    // symptom is a re-imported asset that is verifiably correct on disk and still wrong on screen
    // until a restart. Only this code knows exactly which files it wrote, so it says so, and the
    // caches drop those entries by path instead of paying a stat on every lookup. Valid when
    // Draw() returned true.
    const std::vector<std::string>& LastImportedFiles() const { return lastImportedFiles_; }

    // Reimports only the content-browser resource and its actual source
    // dependencies. Deleted sources remove only their mapped output.
    bool BeginReimport(const EditorAssetRecord& asset, AssetRegistry& registry);

private:
    enum class Kind { Mesh, TextureSet, Skybox };
    struct Item
    {
        std::string path;      // absolute/relative dir (Mesh/TextureSet) or .hdr file (Skybox)
        std::string name;      // folder / file stem
        Kind kind = Kind::Mesh;
        std::string gltfFile;  // relative gltf/glb inside the folder (Mesh)
        std::string meta;      // "5 materials, 6630 tris, ~5.8 m" etc.
        std::string license;   // first lines of source/license.txt or glTF copyright
        float worldSizeM = 0.0f; // Mesh: longest world-space bbox axis (0 = unknown)
        bool alreadyInProject = false;
        EditorAssetImportStatus importStatus = EditorAssetImportStatus::Untracked;
    };

    // One selectable image row in the texture-import dialog.
    struct DialogFile
    {
        std::string rel;      // path relative to the item folder (what the backend whitelists on)
        std::string role;     // guessed role for display: albedo / normal / rough / metal / ao / ...
        std::string sizeText; // "12.4 MB" — helps judge the max-texture-size option
        bool selected = true;
    };

    void Rescan();
    // includeRel empty = convert everything; registerPreset=false skips MR synth + material preset.
    void BeginImport(const Item& item, const std::vector<std::string>& includeRel,
        bool registerPreset,
        const std::vector<std::string>& targetOutputs = {},
        const std::vector<std::string>& removedSources = {},
        float meshSpawnScale = -1.0f,
        const std::vector<std::string>& meshSplitNodes = {},
        bool meshSplitChoiceProvided = false,
        int meshChunkGrid = -1,
        // Non-null ONLY from the import dialog's buttons: a SNAPSHOT of its LOD/bake-scale
        // values, taken at click time. The worker finalizes seconds later, and reading the
        // live widget fields then means a dialog reopened mid-import re-bakes with the wrong
        // values. Null (every bulk / per-resource caller) = reproduce the asset's OWN
        // mesh.json settings, never the dialog's leftovers.
        const MeshLoadOptions* meshLodFromDialog = nullptr);
    void PollImport(AssetRegistry& registry, bool& finishedOut);
    void OpenImportDialog(const Item& item); // texture sets: choose files + preset before importing
    void DrawImportDialog();
    // Skybox import asks its own two questions -- cube face size and the calibration target -- in a
    // modal, the same way meshes and texture sets do. They used to sit in the shared Options block,
    // where they were noise for every other asset kind and easy to miss for the one kind that needs
    // them.
    void OpenSkyboxImportDialog(const Item& item);
    void DrawSkyboxImportDialog();
    void OpenMeshImportDialog(const Item& item);
    void DrawMeshImportDialog(AssetRegistry& registry);
    // `chunkGrid`: -1 = no explicit choice (bulk / per-resource re-import), keep whatever the
    // existing mesh.json carries; >= 0 is the import dialog's answer, and 0 must be able to REMOVE
    // an existing grid or the dialog's checkbox could be cleared and silently do nothing.
    bool RecreateMeshAssets(const Item& item, float spawnScale,
        const std::vector<std::string>& splitNodes, int chunkGrid,
        const MeshLoadOptions* dialogLod); // null = preserve the asset's own mesh.json knobs

    // The import dialog's LOD + bake-scale widget values as MeshLoadOptions. The ONLY reader
    // of those widgets on a bake path — snapshot the result at click time (see BeginImport).
    MeshLoadOptions DialogLodOptions() const;

    // Engine-tree destination for an item, by kind: models/<name> (Mesh), textures/<name>
    // (TextureSet), textures/<name>.dds (Skybox). Meshes go to models/ (their sibling DDS ride
    // along, referenced by the glTF); standalone textures + skyboxes go to textures/ because that
    // is the only AssetRegistry root that indexes .dds.
    std::string ProjectDest(const Item& item) const;

    std::vector<Item> items_;
    bool scanned_ = false;
    std::uint64_t lastRegistryRevision_ = 0;
    std::vector<std::string> reimportQueue_; // item paths pending "Re-import all changed" (one job at a time)

    // Options (mirror assets::ImportOptions).
    int  maxTextureSize_ = 2048;
    bool highQuality_ = false;
    // Sky calibration target (median luminance). HDRI libraries are not calibrated to the engine's
    // linear scale, so this is the dial that decides how bright an imported sky is. 0 = keep the
    // source's own radiance. See ImportOptions::skyTargetMedianLuma.
    float skyTargetMedianLuma_ = 0.18f;
    // P16.3: take the sun disc out of the LIGHTING derivatives. See ImportOptions.
    bool  skyRemoveSunFromIbl_ = true;
    bool flipGreen_ = false;
    bool centerNormals_ = true; // re-center normal maps with a DC "purple cast" lean (threshold-gated)
    bool moveIntoProject_ = true;
    bool useGpu_ = true; // H5: BC6H/BC7 on the GPU (auto CPU fallback)
    int  skyboxFaceSize_ = 1024;

    // Background import job (one at a time).
    std::thread worker_;
    std::atomic<bool> running_{ false };
    std::atomic<int> workerFailures_{ 0 };
    std::atomic<int> progressDone_{ 0 };   // textures converted so far (worker writes, UI reads)
    std::atomic<int> progressTotal_{ 0 };  // total convertible textures (0 until the worker scans)
    std::string workerManifestJson_;       // source snapshot built by worker after successful import
    Item activeItem_;
    std::string lastImportedName_;
    std::vector<std::string> lastImportedFiles_;
    // data/materials/*.json write times captured when the import STARTED. Diffed on completion to
    // find the definitions this run created or rewrote, whoever wrote them.
    std::map<std::string, std::uint64_t> materialsBeforeImport_;
    std::vector<std::string> activeTargetOutputs_;
    std::vector<std::string> activeRemovedSources_;
    // True for any non-full import (dialog subset or per-resource reimport): the manifest is
    // MERGED instead of replaced, and the destination folder is NOT synced — files produced by
    // earlier imports survive. Only a full import treats the folder as importer-owned.
    bool activeMergeManifest_ = false;
    // >= 0 is the explicit choice from the mesh import dialog. -1 preserves
    // an existing manifest value for non-interactive/bulk reimports.
    float activeMeshSpawnScale_ = -1.0f;
    // Same contract for mesh chunking: -1 preserves the asset's existing chunkGrid for
    // non-interactive/bulk reimports, >= 0 is the dialog's explicit answer.
    int activeMeshChunkGrid_ = -1;
    // Snapshot of the dialog's LOD values for the running async import (see BeginImport's
    // meshLodFromDialog). Null for non-dialog imports = preserve per-asset mesh.json knobs.
    std::unique_ptr<MeshLoadOptions> activeMeshLodOpt_;
    std::string activeMeshSplitGltf_;
    std::vector<std::string> activeMeshSplitNodes_;
    bool joinPending_ = false;
    // Set by the synchronous "Recreate JSON + re-bake bin" path; makes the NEXT Draw() return
    // report "finished" so the caller runs the same cache-invalidation + respawn it does for
    // async imports. Without it a re-bake was invisible to the running session until restart.
    bool pendingInvalidate_ = false;
    std::string status_;
    bool statusIsError_ = false; // colors the status line red on failure, green on success

    // Import dialog (texture sets only): pick which images to convert + whether to make a preset.
    bool showImportDialog_ = false;
    bool showSkyboxImportDialog_ = false;
    Item skyboxDialogItem_{};
    Item dialogItem_;
    std::vector<DialogFile> dialogFiles_;
    bool dialogCreatePreset_ = true;

    // Mesh import confirmation. Size normalization is intentionally decided
    // per mesh here rather than being a persistent global importer option.
    bool showMeshImportDialog_ = false;
    Item meshDialogItem_;
    bool meshDialogNormalizeSpawn_ = false;
    float meshDialogTargetM_ = 6.0f;
    // How the unit correction is expressed. Target-side is the readable one when you know the real
    // object ("this crate is 1.2 m"); the plain multiplier is the one you want when the asset is
    // simply in the wrong unit and 0.01 is the whole answer.
    enum class MeshScaleMode { TargetSide, Multiplier };
    MeshScaleMode meshDialogScaleMode_ = MeshScaleMode::TargetSide;
    float meshDialogMultiplier_ = 0.01f;
    // true = fold the factor into the VERTICES at bake time (the mesh becomes metres and instances
    // spawn at scale 1); false = the old behaviour, a spawnScale on the asset that leaves model
    // space in the source unit.
    bool meshDialogBakeIntoVertices_ = true;
    float meshDialogBakeScale_ = 1.0f;
    float meshDialogPendingSpawnScale_ = 0.0f;
    bool meshDialogSplitTopLevelNodes_ = false;
    // LOD generation knobs for this import (defaults = the shipped chain; see MeshLoadOptions).
    // Read straight by RecreateMeshAssets, which is where every mesh bake funnels through.
    float meshDialogLodRatio_ = 1.0f;
    float meshDialogLodError_ = 1.0f;
    bool  meshDialogLodPermissive_ = false; // meshopt_SimplifyPermissive — shreds masked foliage
    bool  meshDialogLodPrune_ = false;      // meshopt_SimplifyPrune — drops small loose components
    bool  meshDialogLod3Aggressive_ = true; // harsh last level (solid ~5%; OFF for terrain)
    float meshDialogFoliageKeep_ = 0.35f;   // LOD3 leaf-prune keep fraction (0/1 = no prune)
    float meshDialogLod3Ratio_ = 1.0f;      // LOD3-only multiplier over the 5% target
    float meshDialogLod3Error_ = 1.0f;      // LOD3-only multiplier over the 0.25 error
    float meshDialogInnerRatio_ = 0.5f;     // LOD3 kept-leaf interior decimation target
    float meshDialogInnerError_ = 0.15f;    // and its error budget
    float meshDialogFoliageGrow_ = 1.0f;    // survivor-inflation dial (1 = full area comp)
    float meshDialogUvWeight_ = 0.0f;       // foliage UV weight for attribute-aware simplify
    // Mesh chunking (mesh.json "chunkGrid"). Splits LOD0 into an N x N grid of submeshes so each
    // tile becomes its own shadow caster and a shadow page rasterizes only the tiles it overlaps.
    // Seeded from the asset on dialog open, so re-opening shows what the asset actually carries.
    bool meshDialogChunk_ = false;
    int  meshDialogChunkGrid_ = 6;
    // Submesh count of the source, read once when the dialog opens: chunking is single-submesh only
    // (v1), and the dialog says so up front rather than letting the bake reject it silently.
    int  meshDialogSubmeshCount_ = 1;
    // Triangle count parsed out of Item::meta (both describe paths write "<N> tris"). 0 = unknown.
    // Chunking needs it because the thing that actually limits the grid is TRIANGLES PER TILE, not
    // the tile count — see the readout in DrawMeshImportDialog.
    int  meshDialogTriCount_ = 0;
    std::vector<std::string> meshDialogTopLevelNodes_;
};

#endif // WITH_EDITOR
