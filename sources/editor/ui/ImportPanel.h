#pragma once
#if WITH_EDITOR

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class AssetRegistry;

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

private:
    enum class Kind { Mesh, TextureSet, Skybox };
    struct Item
    {
        std::string path;      // absolute/relative dir (Mesh/TextureSet) or .hdr file (Skybox)
        std::string name;      // folder / file stem
        Kind kind = Kind::Mesh;
        std::string gltfFile;  // relative gltf/glb inside the folder (Mesh)
        std::string meta;      // "5 materials, 6630 tris" etc.
        std::string license;   // first lines of source/license.txt or glTF copyright
        bool alreadyInProject = false;
    };

    // One selectable image row in the texture-import dialog.
    struct DialogFile
    {
        std::string rel;   // path relative to the item folder (what the backend whitelists on)
        std::string role;  // guessed role for display: albedo / normal / rough / metal / ao / ...
        bool selected = true;
    };

    void Rescan();
    // includeRel empty = convert everything; registerPreset=false skips MR synth + material preset.
    void BeginImport(const Item& item, const std::vector<std::string>& includeRel, bool registerPreset);
    void PollImport(AssetRegistry& registry, bool& finishedOut);
    void OpenImportDialog(const Item& item); // texture sets: choose files + preset before importing
    void DrawImportDialog();

    // Engine-tree destination for an item, by kind: models/<name> (Mesh), textures/<name>
    // (TextureSet), textures/<name>.dds (Skybox). Meshes go to models/ (their sibling DDS ride
    // along, referenced by the glTF); standalone textures + skyboxes go to textures/ because that
    // is the only AssetRegistry root that indexes .dds.
    std::string ProjectDest(const Item& item) const;

    std::vector<Item> items_;
    bool scanned_ = false;

    // Options (mirror assets::ImportOptions).
    int  maxTextureSize_ = 2048;
    bool highQuality_ = false;
    bool flipGreen_ = false;
    bool moveIntoProject_ = true;
    int  skyboxFaceSize_ = 1024;

    // Background import job (one at a time).
    std::thread worker_;
    std::atomic<bool> running_{ false };
    std::atomic<int> workerFailures_{ 0 };
    std::atomic<int> progressDone_{ 0 };   // textures converted so far (worker writes, UI reads)
    std::atomic<int> progressTotal_{ 0 };  // total convertible textures (0 until the worker scans)
    Item activeItem_;
    bool joinPending_ = false;
    std::string status_;

    // Import dialog (texture sets only): pick which images to convert + whether to make a preset.
    bool showImportDialog_ = false;
    Item dialogItem_;
    std::vector<DialogFile> dialogFiles_;
    bool dialogCreatePreset_ = true;
};

#endif // WITH_EDITOR
