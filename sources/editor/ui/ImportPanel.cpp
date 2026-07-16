#include "editor/ui/ImportPanel.h"
#if WITH_EDITOR

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "assets/AssetImporter.h"
#include "editor/assets/AssetRegistry.h"
#include "third_party/cgltf/cgltf.h"
#include "imgui.h"

namespace fs = std::filesystem;

namespace
{
    constexpr const char* kStagingRoot = "import_staging";
    constexpr const char* kModelsRoot = "models";
    constexpr const char* kTexturesRoot = "textures";

    std::string LowerExt(const fs::path& p)
    {
        std::string e = p.extension().string();
        std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return e;
    }

    bool IsConvertibleTexture(const std::string& ext)
    {
        return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp";
    }

    // Files that belong in the engine tree: converted textures (.dds) + mesh geometry (glTF/bin).
    // Raw source images, source.txt/license.txt, readmes etc. are deliberately excluded so the
    // project stays clean (they remain in import_staging/ for re-import).
    bool IsEngineReady(const std::string& ext)
    {
        return ext == ".dds" || ext == ".gltf" || ext == ".glb" || ext == ".bin";
    }

    // Best-effort role guess from the filename, for the import dialog's display only (the backend
    // does its own classification). Mirrors the backend's substring heuristics.
    std::string GuessTextureRole(const fs::path& p)
    {
        std::string s = p.stem().string();
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto has = [&](const char* k) { return s.find(k) != std::string::npos; };
        if (has("albedo") || has("basecolor") || has("base_color") || has("diffuse") || has("_diff") || has("_col")) { return "albedo"; }
        if (has("normal") || has("_nor") || has("_nrm")) { return "normal"; }
        if (has("rough") || has("_rgh")) { return "roughness"; }
        if (has("metal") || has("_met")) { return "metallic"; }
        if (has("metallicroughness") || has("_mr")) { return "metal/rough"; }
        if (has("_ao") || has("ambientocclusion") || has("occlusion")) { return "AO"; }
        if (has("height") || has("_disp") || has("displac") || has("bump")) { return "height"; }
        if (has("emiss") || has("_emit")) { return "emissive"; }
        if (has("opacity") || has("alpha")) { return "opacity"; }
        return "";
    }

    // First ~3 non-empty lines of a source/license text file (trimmed), for the row tooltip.
    std::string ReadLicense(const fs::path& dir)
    {
        for (const char* name : { "source.txt", "license.txt", "License.txt", "credits.txt" })
        {
            std::ifstream f(dir / name);
            if (!f) { continue; }
            std::string out, line;
            int lines = 0;
            while (std::getline(f, line) && lines < 4)
            {
                const size_t a = line.find_first_not_of(" \t\r\n");
                if (a == std::string::npos) { continue; }
                const size_t b = line.find_last_not_of(" \t\r\n");
                out += line.substr(a, b - a + 1);
                out += '\n';
                ++lines;
            }
            if (!out.empty()) { return out; }
        }
        return {};
    }

    // Light cgltf parse for the row summary + copyright fallback. No buffers loaded.
    void DescribeGltf(const fs::path& gltf, std::string& metaOut, std::string& copyrightOut)
    {
        cgltf_options opt{};
        cgltf_data* data = nullptr;
        if (cgltf_parse_file(&opt, gltf.string().c_str(), &data) != cgltf_result_success)
        {
            metaOut = "(unreadable glTF)";
            return;
        }
        size_t tris = 0;
        for (cgltf_size m = 0; m < data->meshes_count; ++m)
        {
            for (cgltf_size p = 0; p < data->meshes[m].primitives_count; ++p)
            {
                const cgltf_primitive& prim = data->meshes[m].primitives[p];
                if (prim.indices) { tris += prim.indices->count / 3; }
            }
        }
        std::ostringstream os;
        os << data->materials_count << " material" << (data->materials_count == 1 ? "" : "s")
           << ", " << tris << " tris";
        metaOut = os.str();
        if (data->asset.copyright) { copyrightOut = data->asset.copyright; }
        cgltf_free(data);
    }

    // After moving a texture set out of staging, repoint its material preset — the backend wrote it
    // (data/materials.json) with import_staging/ paths, which now dangle. Plain text replace keeps
    // the file's exact formatting/key order (cleaner round-trip than a JSON re-dump).
    void RepointPresetPaths(const std::string& stagingPrefix, const std::string& destPrefix)
    {
        const fs::path mat = "data/materials.json";
        std::string content;
        {
            std::ifstream in(mat);
            if (!in) { return; }
            std::ostringstream ss;
            ss << in.rdbuf();
            content = ss.str();
        }
        bool changed = false;
        for (size_t pos = 0; (pos = content.find(stagingPrefix, pos)) != std::string::npos; )
        {
            content.replace(pos, stagingPrefix.size(), destPrefix);
            pos += destPrefix.size();
            changed = true;
        }
        if (!changed) { return; }
        std::ofstream out(mat, std::ios::trunc);
        if (out) { out << content; }
    }

    void WriteCreditsEntry(const std::string& name, const std::string& license)
    {
        if (license.empty()) { return; }
        const fs::path path = "CREDITS.md";
        // Skip if an entry for this asset already exists (dedupe by the "## <name>" header).
        std::string existing;
        {
            std::ifstream in(path);
            if (in) { std::ostringstream ss; ss << in.rdbuf(); existing = ss.str(); }
        }
        const std::string header = "## " + name;
        if (existing.find(header) != std::string::npos) { return; }

        std::ofstream out(path, std::ios::app);
        if (!out) { return; }
        if (existing.empty()) { out << "# Asset credits\n\n"; }
        out << header << "\n" << license;
        if (license.back() != '\n') { out << '\n'; }
        out << "\n";
    }
}

ImportPanel::ImportPanel() = default;

ImportPanel::~ImportPanel()
{
    if (worker_.joinable()) { worker_.join(); }
}

std::string ImportPanel::ProjectDest(const Item& item) const
{
    switch (item.kind)
    {
    case Kind::Mesh:       return (fs::path(kModelsRoot) / item.name).string();
    case Kind::TextureSet: return (fs::path(kTexturesRoot) / item.name).string();
    case Kind::Skybox:     return (fs::path(kTexturesRoot) / (item.name + ".dds")).string();
    }
    return (fs::path(kModelsRoot) / item.name).string();
}

void ImportPanel::Rescan()
{
    items_.clear();
    scanned_ = true;

    std::error_code ec;
    if (!fs::exists(kStagingRoot, ec) || !fs::is_directory(kStagingRoot, ec)) { return; }

    for (const auto& entry : fs::directory_iterator(kStagingRoot, ec))
    {
        if (ec) { break; }
        const fs::path& p = entry.path();

        if (entry.is_regular_file(ec) && LowerExt(p) == ".hdr")
        {
            Item it;
            it.path = p.string();
            it.name = p.stem().string();
            it.kind = Kind::Skybox;
            it.meta = "equirect HDRI -> BC6H cubemap";
            it.alreadyInProject = fs::exists(ProjectDest(it), ec);
            items_.push_back(std::move(it));
            continue;
        }
        if (!entry.is_directory(ec)) { continue; }

        // A directory: mesh asset if it holds a glTF/GLB, else a texture set if it holds images.
        fs::path gltf;
        bool hasTex = false;
        for (auto it = fs::recursive_directory_iterator(p, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec) { break; }
            if (!it->is_regular_file(ec)) { continue; }
            const std::string ext = LowerExt(it->path());
            if (gltf.empty() && (ext == ".gltf" || ext == ".glb")) { gltf = it->path(); }
            else if (IsConvertibleTexture(ext)) { hasTex = true; }
        }

        Item it;
        it.path = p.string();
        it.name = p.filename().string();
        it.license = ReadLicense(p);

        if (!gltf.empty())
        {
            it.kind = Kind::Mesh;
            it.gltfFile = gltf.string();
            std::string copyright;
            DescribeGltf(gltf, it.meta, copyright);
            if (it.license.empty() && !copyright.empty()) { it.license = copyright + "\n"; }
        }
        else if (hasTex)
        {
            it.kind = Kind::TextureSet;
            it.meta = "texture set -> DDS + material preset";
        }
        else
        {
            continue; // nothing importable
        }
        it.alreadyInProject = fs::exists(ProjectDest(it), ec);
        items_.push_back(std::move(it));
    }

    std::sort(items_.begin(), items_.end(),
        [](const Item& a, const Item& b) { return a.name < b.name; });
}

void ImportPanel::BeginImport(const Item& item, const std::vector<std::string>& includeRel, bool registerPreset)
{
    if (running_.load()) { return; }
    activeItem_ = item;
    running_.store(true);
    workerFailures_.store(0);
    progressDone_.store(0);
    progressTotal_.store(0);
    joinPending_ = true;
    status_ = "Importing " + item.name + " ...";

    // Snapshot options for the worker (avoid racing the UI).
    assets::ImportOptions opt;
    opt.maxTextureSize = maxTextureSize_;
    opt.highQuality = highQuality_;
    opt.flipGreen = flipGreen_;
    opt.skyboxFaceSize = skyboxFaceSize_;
    opt.logPath = "asset_import.log";
    opt.registerPreset = registerPreset;
    opt.includeRel = includeRel;
    opt.progressDone = &progressDone_;   // atomics live in this panel, which outlives the worker
    opt.progressTotal = &progressTotal_;
    if (item.kind == Kind::Skybox) { opt.skyboxHdr = item.path; }
    else { opt.stagingDir = item.path; }

    worker_ = std::thread([this, opt]()
    {
        const int failures = assets::RunImport(opt); // CPU-heavy BC7 — off the UI thread
        workerFailures_.store(failures);
        running_.store(false);
    });
}

void ImportPanel::OpenImportDialog(const Item& item)
{
    dialogItem_ = item;
    dialogFiles_.clear();
    dialogCreatePreset_ = true;

    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(item.path, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec) { break; }
        if (!it->is_regular_file(ec)) { continue; }
        if (!IsConvertibleTexture(LowerExt(it->path()))) { continue; }
        DialogFile df;
        df.rel = fs::relative(it->path(), item.path, ec).generic_string();
        df.role = GuessTextureRole(it->path());
        df.selected = true;
        dialogFiles_.push_back(std::move(df));
    }
    std::sort(dialogFiles_.begin(), dialogFiles_.end(),
        [](const DialogFile& a, const DialogFile& b) { return a.rel < b.rel; });
    showImportDialog_ = true;
}

void ImportPanel::DrawImportDialog()
{
    if (showImportDialog_)
    {
        ImGui::OpenPopup("Import Texture Set");
        showImportDialog_ = false; // OpenPopup latches the popup open until it is closed
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 center(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (!ImGui::BeginPopupModal("Import Texture Set", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }

    ImGui::Text("Import '%s'", dialogItem_.name.c_str());
    ImGui::Separator();

    ImGui::TextDisabled("Textures to convert:");
    ImGui::SameLine();
    if (ImGui::SmallButton("All")) { for (auto& f : dialogFiles_) { f.selected = true; } }
    ImGui::SameLine();
    if (ImGui::SmallButton("None")) { for (auto& f : dialogFiles_) { f.selected = false; } }

    ImGui::BeginChild("##dlgFiles", ImVec2(440.0f, 220.0f), true);
    for (size_t i = 0; i < dialogFiles_.size(); ++i)
    {
        DialogFile& f = dialogFiles_[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Checkbox(f.rel.c_str(), &f.selected);
        if (!f.role.empty())
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "[%s]", f.role.c_str());
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::Checkbox("Create material preset (synthesize MR)", &dialogCreatePreset_);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "On: albedo + rough(+metal) + normal -> one material preset in data/materials.json.\n"
            "Off: just convert the selected images to DDS (no preset).");
    }

    ImGui::Separator();
    int selCount = 0;
    for (const auto& f : dialogFiles_) { if (f.selected) { ++selCount; } }

    ImGui::BeginDisabled(selCount == 0);
    if (ImGui::Button("Import", ImVec2(120.0f, 0.0f)))
    {
        std::vector<std::string> includeRel;
        for (const auto& f : dialogFiles_) { if (f.selected) { includeRel.push_back(f.rel); } }
        BeginImport(dialogItem_, includeRel, dialogCreatePreset_);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) { ImGui::CloseCurrentPopup(); }
    ImGui::SameLine();
    ImGui::TextDisabled("%d selected", selCount);

    ImGui::EndPopup();
}

void ImportPanel::PollImport(AssetRegistry& registry, bool& finishedOut)
{
    if (!joinPending_ || running_.load()) { return; }

    // Worker finished: join, then do the main-thread finalize (file copy + credits + refresh).
    if (worker_.joinable()) { worker_.join(); }
    joinPending_ = false;
    const int failures = workerFailures_.load();

    std::error_code ec;
    if (failures == 0)
    {
        // Move the CONVERTED asset into the engine tree, by type. We copy only engine-ready files
        // (.dds + glTF/bin) — never the raw PNG/JPG/source.txt — so the project tree stays clean.
        // Generated .dds are then deleted from staging (they're "converted", shouldn't linger); the
        // raw sources + glTF stay in staging so the asset can be re-imported.
        //   Mesh       -> models/<name>/    (glTF + bin + DDS siblings; textures resolve relatively)
        //   TextureSet -> textures/<name>/  (DDS only — the AssetRegistry root that indexes .dds)
        //   Skybox     -> textures/<name>.dds
        const fs::path dst = ProjectDest(activeItem_);
        std::string destLabel = "converted (kept in staging)";
        if (moveIntoProject_)
        {
            if (activeItem_.kind == Kind::Skybox)
            {
                // RunImport wrote <staging>/<name>.dds next to the source .hdr; move that one file.
                fs::path produced = fs::path(activeItem_.path);
                produced.replace_extension(".dds");
                fs::create_directories(fs::path(dst).parent_path(), ec);
                fs::rename(produced, dst, ec);
                if (ec) // cross-volume rename can fail — fall back to copy + delete
                {
                    ec.clear();
                    fs::copy_file(produced, dst, fs::copy_options::overwrite_existing, ec);
                    fs::remove(produced);
                }
            }
            else
            {
                // Walk the staging folder once: copy engine-ready files (preserving subfolders),
                // and remember which .dds to delete from staging afterward.
                std::vector<fs::path> generatedDds;
                for (auto it = fs::recursive_directory_iterator(activeItem_.path, ec);
                     it != fs::recursive_directory_iterator(); it.increment(ec))
                {
                    if (ec) { break; }
                    if (!it->is_regular_file(ec)) { continue; }
                    const std::string ext = LowerExt(it->path());
                    if (!IsEngineReady(ext)) { continue; }

                    std::error_code rec;
                    const fs::path rel = fs::relative(it->path(), activeItem_.path, rec);
                    const fs::path target = dst / rel;
                    fs::create_directories(target.parent_path(), rec);
                    fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, rec);
                    if (ext == ".dds") { generatedDds.push_back(it->path()); }
                }
                for (const fs::path& d : generatedDds)
                {
                    std::error_code rrec;
                    fs::remove(d, rrec);
                }

                // A texture set's material preset was written by the backend pointing into
                // import_staging/ — repoint it to the moved copy so it doesn't dangle.
                if (activeItem_.kind == Kind::TextureSet)
                {
                    RepointPresetPaths("import_staging/" + activeItem_.name + "/",
                        "textures/" + activeItem_.name + "/");
                }
            }
            destLabel = ec ? ("COPY FAILED -> " + dst.string()) : ("-> " + dst.string());
        }
        WriteCreditsEntry(activeItem_.name, activeItem_.license);
        registry.Refresh();
        Rescan(); // pick up the new alreadyInProject state
        status_ = "Imported " + activeItem_.name + "  " + destLabel;
        finishedOut = true;
    }
    else
    {
        status_ = "Import FAILED (" + std::to_string(failures) + ") — see asset_import.log";
    }
}

bool ImportPanel::Draw(AssetRegistry& registry, bool* open)
{
    bool finished = false;
    PollImport(registry, finished);

    if (!ImGui::Begin("Import Assets", open))
    {
        ImGui::End();
        return finished;
    }

    if (!scanned_) { Rescan(); }

    // --- Header -------------------------------------------------------------
    ImGui::TextWrapped(
        "Drop raw asset folders (glTF meshes, texture sets) or .hdr skyboxes into "
        "import_staging/, then import them into the project here.");
    if (ImGui::Button("Rescan")) { Rescan(); }
    ImGui::SameLine();
    ImGui::TextDisabled("%d item%s staged", static_cast<int>(items_.size()),
        items_.size() == 1 ? "" : "s");

    // --- Options (collapsible; the item list is the focus) ------------------
    if (ImGui::CollapsingHeader("Options", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SetNextItemWidth(120.0f);
        const char* sizes[] = { "1024", "2048", "4096" };
        int sizeIdx = maxTextureSize_ >= 4096 ? 2 : (maxTextureSize_ >= 2048 ? 1 : 0);
        if (ImGui::Combo("Max texture size", &sizeIdx, sizes, 3))
        {
            maxTextureSize_ = sizeIdx == 2 ? 4096 : (sizeIdx == 1 ? 2048 : 1024);
        }
        ImGui::Checkbox("High-quality BC7 (slower)", &highQuality_);
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::Checkbox("Move into project after import", &moveIntoProject_);
        ImGui::Checkbox("Flip normal-map green", &flipGreen_);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Leave OFF for glTF meshes and plain textures.\n"
                "Only enable if a normal-mapped surface ends up lit from the wrong side.");
        }
    }

    // --- Status -------------------------------------------------------------
    if (running_.load())
    {
        ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.0f, 1.0f), "%s", status_.c_str());
    }
    else if (!status_.empty())
    {
        ImGui::TextUnformatted(status_.c_str());
    }

    ImGui::Separator();

    // --- Item table (scrolls in its own region; progress bar pinned below) --
    const bool busy = running_.load();
    const float bottomBarH = busy ? ImGui::GetFrameHeightWithSpacing() : 0.0f;
    ImGui::BeginChild("##itemsRegion", ImVec2(0.0f, -bottomBarH), false);

    const ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
    if (items_.empty())
    {
        ImGui::TextDisabled("Nothing importable found in import_staging/.");
    }
    else if (ImGui::BeginTable("##importItems", 4, tableFlags))
    {
        ImGui::TableSetupColumn("Asset", ImGuiTableColumnFlags_WidthStretch, 0.44f);
        ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, 0.40f);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 96.0f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < items_.size(); ++i)
        {
            const Item& it = items_[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            // Asset: colored kind tag + name.
            ImGui::TableSetColumnIndex(0);
            const ImVec4 kindColor =
                it.kind == Kind::Mesh       ? ImVec4(0.55f, 0.80f, 1.00f, 1.0f) :
                it.kind == Kind::TextureSet ? ImVec4(0.70f, 0.90f, 0.50f, 1.0f) :
                                              ImVec4(0.95f, 0.78f, 0.45f, 1.0f);
            const char* kindStr = it.kind == Kind::Mesh ? "mesh" :
                (it.kind == Kind::TextureSet ? "tex" : "sky");
            ImGui::TextColored(kindColor, "%-4s", kindStr);
            ImGui::SameLine();
            ImGui::TextUnformatted(it.name.c_str());

            // Details.
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", it.meta.c_str());

            // Source / license status (hover for detail).
            ImGui::TableSetColumnIndex(2);
            if (it.license.empty())
            {
                ImGui::TextColored(ImVec4(0.90f, 0.70f, 0.20f, 1.0f), "no license");
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "Add source.txt or license.txt in the asset folder\n"
                        "so it is recorded in CREDITS.md on import.");
                }
            }
            else
            {
                ImGui::TextColored(ImVec4(0.50f, 0.85f, 0.50f, 1.0f), "credited");
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", it.license.c_str());
                }
            }

            // Action: Import, or Re-import if it is already in the project tree.
            ImGui::TableSetColumnIndex(3);
            ImGui::BeginDisabled(busy);
            if (ImGui::SmallButton(it.alreadyInProject ? "Re-import" : "Import"))
            {
                // Texture sets get a pick-files + preset dialog; meshes/skyboxes import directly.
                if (it.kind == Kind::TextureSet) { OpenImportDialog(it); }
                else { BeginImport(it, {}, true); }
            }
            ImGui::EndDisabled();
            if (it.alreadyInProject && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip("Already in project: %s", ProjectDest(it).c_str());
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    // --- Conversion progress bar (bottom of the window) ---------------------
    if (busy)
    {
        const int done = progressDone_.load();
        const int total = progressTotal_.load();
        const float width = ImGui::GetContentRegionAvail().x;
        if (total > 0)
        {
            const std::string overlay =
                "converting  " + std::to_string(done) + " / " + std::to_string(total);
            ImGui::ProgressBar(static_cast<float>(done) / static_cast<float>(total),
                ImVec2(width, 0.0f), overlay.c_str());
        }
        else
        {
            // Total not known yet (scanning, or a skybox with no staged textures): sweep.
            ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()),
                ImVec2(width, 0.0f), "scanning...");
        }
    }

    DrawImportDialog(); // texture-set pick-files + preset modal (no-op unless open)

    ImGui::End();
    return finished;
}

#endif // WITH_EDITOR
