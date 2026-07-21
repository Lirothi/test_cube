#include "editor/ui/ImportPanel.h"
#if WITH_EDITOR

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "assets/AssetImporter.h"
#include "editor/assets/AssetRegistry.h"
#include "editor/assets/MaterialFileGen.h" // I3: write named material files from glTF at import
#include "rendering/meshes/MeshManager.h" // CountSubmeshes for the slot count
#include "third_party/cgltf/cgltf.h"
#include "imgui.h"

#pragma warning(push)
#pragma warning(disable: 26819)
#include "third_party/json/json.hpp"
#pragma warning(pop)

namespace fs = std::filesystem;

namespace
{
    constexpr const char* kStagingRoot = "import_staging";
    constexpr const char* kModelsRoot = "models";
    constexpr const char* kTexturesRoot = "textures";

    const char* ImportStatusBadge(EditorAssetImportStatus status)
    {
        switch (status)
        {
        case EditorAssetImportStatus::Staged:      return "STAGED";
        case EditorAssetImportStatus::UpToDate:    return "CURRENT";
        case EditorAssetImportStatus::SourceNewer: return "CHANGED";
        case EditorAssetImportStatus::Incomplete:  return "MISSING";
        case EditorAssetImportStatus::Untracked:   return "--";
        }
        return "--";
    }

    ImVec4 ImportStatusColor(EditorAssetImportStatus status)
    {
        switch (status)
        {
        case EditorAssetImportStatus::Staged:      return ImVec4(0.55f, 0.80f, 1.00f, 1.0f);
        case EditorAssetImportStatus::UpToDate:    return ImVec4(0.45f, 0.85f, 0.50f, 1.0f);
        case EditorAssetImportStatus::SourceNewer: return ImVec4(1.00f, 0.72f, 0.24f, 1.0f);
        case EditorAssetImportStatus::Incomplete:  return ImVec4(1.00f, 0.38f, 0.28f, 1.0f);
        case EditorAssetImportStatus::Untracked:   return ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
        }
        return ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
    }

    const char* ImportStatusHint(EditorAssetImportStatus status)
    {
        switch (status)
        {
        case EditorAssetImportStatus::Staged:
            return "Source is staged but no project output exists yet.";
        case EditorAssetImportStatus::UpToDate:
            return "Project output is at least as new as every staged source.";
        case EditorAssetImportStatus::SourceNewer:
            return "The staged source contents or file inventory changed. Re-import recommended.";
        case EditorAssetImportStatus::Incomplete:
            return "The project output is missing a DDS or copied mesh file. Re-import recommended.";
        case EditorAssetImportStatus::Untracked:
            return "No tracked import source.";
        }
        return "No tracked import source.";
    }

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

    bool IsManifestSource(const std::string& ext)
    {
        return IsConvertibleTexture(ext) || ext == ".gltf" || ext == ".glb" || ext == ".bin";
    }

    uint64_t FileWriteTime(const fs::path& path)
    {
        std::error_code ec;
        const fs::file_time_type time = fs::last_write_time(path, ec);
        return ec ? 0 : static_cast<uint64_t>(time.time_since_epoch().count());
    }

    uint64_t FileSize(const fs::path& path)
    {
        std::error_code ec;
        const uintmax_t size = fs::file_size(path, ec);
        return ec ? 0 : static_cast<uint64_t>(size);
    }

    uint64_t HashFile(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) { return 0; }

        uint64_t hash = 14695981039346656037ull;
        std::vector<char> buffer(1024 * 1024);
        while (file)
        {
            file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = file.gcount();
            for (std::streamsize i = 0; i < count; ++i)
            {
                hash ^= static_cast<unsigned char>(buffer[static_cast<size_t>(i)]);
                hash *= 1099511628211ull;
            }
        }
        return hash;
    }

    std::string NormalizeRelativePath(const fs::path& path)
    {
        std::string normalized = path.lexically_normal().generic_string();
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return normalized;
    }

    std::string BuildSourceManifestJson(const fs::path& source,
        const std::vector<std::string>& includeRel)
    {
        nlohmann::json manifest;
        manifest["version"] = 1;
        manifest["source"] = source.lexically_normal().generic_string();
        manifest["sourceRootWriteTime"] = FileWriteTime(source);
        manifest["sources"] = nlohmann::json::array();

        std::error_code ec;
        if (fs::is_regular_file(source, ec))
        {
            nlohmann::json entry;
            entry["path"] = source.filename().generic_string();
            entry["size"] = FileSize(source);
            entry["writeTime"] = FileWriteTime(source);
            entry["hash"] = HashFile(source);
            manifest["sources"].push_back(std::move(entry));
            manifest["trackAllSources"] = true;
            return manifest.dump();
        }

        std::set<std::string> includeSet;
        for (const std::string& rel : includeRel)
        {
            includeSet.insert(NormalizeRelativePath(rel));
        }

        std::vector<std::pair<fs::path, fs::path>> selected;
        size_t sourceCount = 0;
        size_t selectedSourceCount = 0;
        for (fs::recursive_directory_iterator it(source,
                 fs::directory_options::skip_permission_denied, ec), end;
             it != end;
             it.increment(ec))
        {
            if (ec) { break; }
            std::error_code fileEc;
            if (!it->is_regular_file(fileEc)) { continue; }
            const std::string ext = LowerExt(it->path());
            if (!IsManifestSource(ext)) { continue; }

            std::error_code relativeEc;
            const fs::path relative = fs::relative(it->path(), source, relativeEc);
            if (relativeEc) { continue; }

            ++sourceCount;
            const bool included = includeSet.empty() ||
                includeSet.count(NormalizeRelativePath(relative)) != 0;
            if (!included) { continue; }
            ++selectedSourceCount;
            selected.emplace_back(it->path(), relative);
        }

        std::sort(selected.begin(), selected.end(),
            [](const auto& a, const auto& b)
            {
                return a.second.generic_string() < b.second.generic_string();
            });
        for (const auto& [path, relative] : selected)
        {
            nlohmann::json entry;
            entry["path"] = relative.generic_string();
            entry["size"] = FileSize(path);
            entry["writeTime"] = FileWriteTime(path);
            entry["hash"] = HashFile(path);
            manifest["sources"].push_back(std::move(entry));
        }
        manifest["trackAllSources"] = includeSet.empty() ||
            selectedSourceCount == sourceCount;
        return manifest.dump();
    }

    fs::path ImportManifestPath(const fs::path& projectOutput, bool outputIsFile)
    {
        if (!outputIsFile)
        {
            return projectOutput / ".assetimport.json";
        }
        return projectOutput.parent_path() /
            (projectOutput.stem().string() + ".assetimport.json");
    }

    float ReadManifestSpawnScale(const fs::path& manifestPath)
    {
        std::ifstream file(manifestPath);
        if (!file) { return 0.0f; }
        const nlohmann::json manifest = nlohmann::json::parse(
            file, nullptr, false, true);
        if (manifest.is_discarded() || !manifest.is_object()) { return 0.0f; }
        const float scale = manifest.value("spawnScale", 0.0f);
        return scale > 0.0f ? scale : 0.0f;
    }

    std::vector<std::string> ReadManifestSplitNodes(const fs::path& manifestPath,
        std::string* gltfRelativeOut = nullptr)
    {
        std::vector<std::string> nodes;
        std::ifstream file(manifestPath);
        if (!file) { return nodes; }
        const nlohmann::json manifest = nlohmann::json::parse(
            file, nullptr, false, true);
        if (manifest.is_discarded() || !manifest.is_object()) { return nodes; }
        const auto splitIt = manifest.find("splitTopLevelNodes");
        if (splitIt == manifest.end() || !splitIt->is_object()) { return nodes; }
        const auto gltfIt = splitIt->find("gltf");
        const auto nodesIt = splitIt->find("nodes");
        if (gltfIt == splitIt->end() || !gltfIt->is_string() ||
            nodesIt == splitIt->end() || !nodesIt->is_array())
        {
            return nodes;
        }
        if (gltfRelativeOut) { *gltfRelativeOut = gltfIt->get<std::string>(); }
        std::set<std::string> unique;
        for (const nlohmann::json& node : *nodesIt)
        {
            if (node.is_string() && !node.get<std::string>().empty() &&
                unique.insert(node.get<std::string>()).second)
            {
                nodes.push_back(node.get<std::string>());
            }
        }
        return nodes;
    }

    bool IsSafeRelativePath(const fs::path& path)
    {
        if (path.empty() || path.is_absolute()) { return false; }
        return std::none_of(path.begin(), path.end(),
            [](const fs::path& component) { return component == ".."; });
    }

    void RemoveStaleProjectOutputs(const fs::path& projectRoot,
        const std::set<std::string>& newOutputs)
    {
        std::vector<fs::path> stale;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(projectRoot,
                 fs::directory_options::skip_permission_denied, ec), end;
             it != end;
             it.increment(ec))
        {
            if (ec) { break; }
            std::error_code fileEc;
            if (!it->is_regular_file(fileEc) || !IsEngineReady(LowerExt(it->path())))
            {
                continue;
            }
            std::error_code relativeEc;
            const fs::path relative = fs::relative(it->path(), projectRoot, relativeEc);
            if (!relativeEc && IsSafeRelativePath(relative) &&
                newOutputs.count(NormalizeRelativePath(relative)) == 0)
            {
                stale.push_back(it->path());
            }
        }
        for (const fs::path& path : stale)
        {
            std::error_code removeEc;
            fs::remove(path, removeEc);
        }
    }

    std::set<std::string> EnumerateProjectOutputs(const fs::path& projectRoot)
    {
        std::set<std::string> outputs;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(projectRoot,
                 fs::directory_options::skip_permission_denied, ec), end;
             it != end;
             it.increment(ec))
        {
            if (ec) { break; }
            std::error_code fileEc;
            if (!it->is_regular_file(fileEc) || !IsEngineReady(LowerExt(it->path())))
            {
                continue;
            }
            std::error_code relativeEc;
            const fs::path relative = fs::relative(it->path(), projectRoot, relativeEc);
            if (!relativeEc && IsSafeRelativePath(relative))
            {
                outputs.insert(NormalizeRelativePath(relative));
            }
        }
        return outputs;
    }

    bool WriteImportManifest(const std::string& sourceManifestJson,
        const std::set<std::string>& outputs,
        const fs::path& source,
        const fs::path& manifestPath,
        bool partial,
        const std::vector<std::string>& removedSources,
        float spawnScale,
        const std::string& splitGltf,
        const std::vector<std::string>& splitNodes)
    {
        nlohmann::json manifest = nlohmann::json::parse(
            sourceManifestJson, nullptr, false, true);
        if (manifest.is_discarded() || !manifest.is_object()) { return false; }

        // Spawn-scale normalizer verdict: written on a full import; a partial/merge import
        // preserves whatever the owning full import decided.
        if (!partial && spawnScale > 0.0f)
        {
            manifest["spawnScale"] = spawnScale;
        }
        if (!partial && !splitGltf.empty() && !splitNodes.empty())
        {
            manifest["splitTopLevelNodes"] = {
                { "gltf", splitGltf },
                { "nodes", splitNodes }
            };
        }

        if (partial)
        {
            std::ifstream existingFile(manifestPath);
            if (existingFile)
            {
                nlohmann::json existing = nlohmann::json::parse(
                    existingFile, nullptr, false, true);
                if (!existing.is_discarded() && existing.is_object() &&
                    existing.value("version", 0) == 1 &&
                    existing.contains("sources") && existing["sources"].is_array())
                {
                    std::set<std::string> replaced;
                    for (const nlohmann::json& entry : manifest["sources"])
                    {
                        if (entry.is_object() && entry.contains("path") &&
                            entry["path"].is_string())
                        {
                            replaced.insert(NormalizeRelativePath(
                                entry["path"].get<std::string>()));
                        }
                    }
                    for (const std::string& path : removedSources)
                    {
                        replaced.insert(NormalizeRelativePath(path));
                    }

                    nlohmann::json merged = nlohmann::json::array();
                    for (const nlohmann::json& entry : existing["sources"])
                    {
                        if (!entry.is_object() || !entry.contains("path") ||
                            !entry["path"].is_string() ||
                            replaced.count(NormalizeRelativePath(
                                entry["path"].get<std::string>())) != 0)
                        {
                            continue;
                        }
                        merged.push_back(entry);
                    }
                    for (const nlohmann::json& entry : manifest["sources"])
                    {
                        merged.push_back(entry);
                    }
                    manifest["sources"] = std::move(merged);
                    manifest["trackAllSources"] = existing.value(
                        "trackAllSources", false);
                    if (existing.contains("spawnScale"))
                    {
                        manifest["spawnScale"] = existing["spawnScale"];
                    }
                    if (existing.contains("splitTopLevelNodes"))
                    {
                        manifest["splitTopLevelNodes"] = existing["splitTopLevelNodes"];
                    }
                }
            }
        }

        // RunImport temporarily creates DDS siblings in staging. Snapshot the
        // source-root timestamp only after those generated files were removed.
        manifest["sourceRootWriteTime"] = FileWriteTime(source);
        manifest["outputs"] = nlohmann::json::array();
        for (const std::string& output : outputs)
        {
            manifest["outputs"].push_back(output);
        }
        std::error_code ec;
        fs::create_directories(manifestPath.parent_path(), ec);
        std::ofstream out(manifestPath, std::ios::trunc);
        if (!out) { return false; }
        out << manifest.dump(2) << '\n';
        return static_cast<bool>(out);
    }

    bool RemoveOutputFromManifest(const fs::path& manifestPath,
        const std::string& outputRelative,
        const std::vector<std::string>& removedSources)
    {
        std::ifstream file(manifestPath);
        if (!file) { return true; } // Legacy imports have no manifest to update.
        nlohmann::json manifest = nlohmann::json::parse(file, nullptr, false, true);
        if (manifest.is_discarded() || !manifest.is_object() ||
            !manifest.contains("outputs") || !manifest["outputs"].is_array() ||
            !manifest.contains("sources") || !manifest["sources"].is_array())
        {
            return false;
        }

        const std::string normalizedOutput = NormalizeRelativePath(outputRelative);
        nlohmann::json outputs = nlohmann::json::array();
        for (const nlohmann::json& entry : manifest["outputs"])
        {
            if (!entry.is_string() ||
                NormalizeRelativePath(entry.get<std::string>()) == normalizedOutput)
            {
                continue;
            }
            outputs.push_back(entry);
        }
        manifest["outputs"] = std::move(outputs);

        std::set<std::string> removed;
        for (const std::string& path : removedSources)
        {
            removed.insert(NormalizeRelativePath(path));
        }
        nlohmann::json sources = nlohmann::json::array();
        for (const nlohmann::json& entry : manifest["sources"])
        {
            if (!entry.is_object() || !entry.contains("path") ||
                !entry["path"].is_string() ||
                removed.count(NormalizeRelativePath(
                    entry["path"].get<std::string>())) != 0)
            {
                continue;
            }
            sources.push_back(entry);
        }
        manifest["sources"] = std::move(sources);

        std::ofstream out(manifestPath, std::ios::trunc);
        if (!out) { return false; }
        out << manifest.dump(2) << '\n';
        return static_cast<bool>(out);
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

    // Light cgltf parse for the row summary + copyright fallback. No buffers loaded — the
    // world-space size comes from POSITION accessor min/max (stored in the JSON) transformed
    // by each referencing node's world matrix. glTF has no unit guarantee: Sketchfab assets
    // are frequently cm-authored (a "rock" ends up ~115 m at scale 1.0), so the row surfaces
    // the baked size and the table warns when it is implausible for a prop.
    void DescribeGltf(const fs::path& gltf, std::string& metaOut, std::string& copyrightOut,
        float& maxDimOut)
    {
        maxDimOut = 0.0f;
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

        // World-space bounding box: union of every node-instanced primitive's
        // POSITION min/max, transformed through the node's world matrix.
        float bbMin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
        float bbMax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        bool anyBounds = false;
        for (cgltf_size n = 0; n < data->nodes_count; ++n)
        {
            const cgltf_node& node = data->nodes[n];
            if (!node.mesh) { continue; }
            float world[16];
            cgltf_node_transform_world(&node, world);
            for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p)
            {
                const cgltf_primitive& prim = node.mesh->primitives[p];
                for (cgltf_size a = 0; a < prim.attributes_count; ++a)
                {
                    const cgltf_attribute& attr = prim.attributes[a];
                    if (attr.type != cgltf_attribute_type_position || !attr.data ||
                        !attr.data->has_min || !attr.data->has_max)
                    {
                        continue;
                    }
                    for (int corner = 0; corner < 8; ++corner)
                    {
                        const float local[3] = {
                            (corner & 1) ? attr.data->max[0] : attr.data->min[0],
                            (corner & 2) ? attr.data->max[1] : attr.data->min[1],
                            (corner & 4) ? attr.data->max[2] : attr.data->min[2]
                        };
                        for (int axis = 0; axis < 3; ++axis)
                        {
                            const float v = world[0 + axis] * local[0] +
                                            world[4 + axis] * local[1] +
                                            world[8 + axis] * local[2] +
                                            world[12 + axis];
                            bbMin[axis] = std::min(bbMin[axis], v);
                            bbMax[axis] = std::max(bbMax[axis], v);
                            anyBounds = true;
                        }
                    }
                }
            }
        }

        std::ostringstream os;
        os << data->materials_count << " material" << (data->materials_count == 1 ? "" : "s")
           << ", " << tris << " tris";
        if (anyBounds)
        {
            const float dims[3] = { bbMax[0] - bbMin[0], bbMax[1] - bbMin[1], bbMax[2] - bbMin[2] };
            maxDimOut = std::max(dims[0], std::max(dims[1], dims[2]));
            char size[32];
            if (maxDimOut >= 10.0f) { snprintf(size, sizeof(size), ", ~%.0f m", maxDimOut); }
            else { snprintf(size, sizeof(size), ", ~%.1f m", maxDimOut); }
            os << size;
        }
        metaOut = os.str();
        if (data->asset.copyright) { copyrightOut = data->asset.copyright; }
        cgltf_free(data);
    }

    bool GltfNodeSubtreeHasMesh(const cgltf_node* root)
    {
        if (!root) { return false; }
        std::vector<const cgltf_node*> stack{ root };
        while (!stack.empty())
        {
            const cgltf_node* node = stack.back();
            stack.pop_back();
            if (node->mesh) { return true; }
            for (cgltf_size i = 0; i < node->children_count; ++i)
            {
                stack.push_back(node->children[i]);
            }
        }
        return false;
    }

    std::vector<std::string> ListSplittableTopLevelNodes(const fs::path& gltf)
    {
        std::vector<std::string> result;
        cgltf_options options{};
        cgltf_data* data = nullptr;
        if (cgltf_parse_file(&options, gltf.string().c_str(), &data) != cgltf_result_success)
        {
            return result;
        }

        std::vector<const cgltf_node*> roots;
        const cgltf_scene* scene = data->scene ? data->scene :
            (data->scenes_count > 0 ? &data->scenes[0] : nullptr);
        if (scene)
        {
            for (cgltf_size i = 0; i < scene->nodes_count; ++i)
            {
                if (GltfNodeSubtreeHasMesh(scene->nodes[i])) { roots.push_back(scene->nodes[i]); }
            }
        }
        else
        {
            for (cgltf_size i = 0; i < data->nodes_count; ++i)
            {
                const cgltf_node* node = &data->nodes[i];
                if (!node->parent && GltfNodeSubtreeHasMesh(node)) { roots.push_back(node); }
            }
        }

        // Blender/exporter scene wrappers are often a single meshless root.
        // Peel those containers so the actual prop nodes become the split units.
        while (roots.size() == 1 && !roots[0]->mesh && roots[0]->children_count > 0)
        {
            std::vector<const cgltf_node*> children;
            for (cgltf_size i = 0; i < roots[0]->children_count; ++i)
            {
                if (GltfNodeSubtreeHasMesh(roots[0]->children[i]))
                {
                    children.push_back(roots[0]->children[i]);
                }
            }
            if (children.empty()) { break; }
            roots = std::move(children);
        }

        std::set<std::string> names;
        std::set<std::string> duplicates;
        for (const cgltf_node* node : roots)
        {
            if (!node->name || !*node->name) { continue; }
            const std::string name(node->name);
            if (!names.insert(name).second) { duplicates.insert(name); }
        }
        for (const cgltf_node* node : roots)
        {
            if (!node->name || !*node->name || duplicates.count(node->name) != 0) { continue; }
            result.emplace_back(node->name);
        }
        cgltf_free(data);
        return result;
    }

    // Mesh sidecar names are filesystem-safe while retaining readable glTF node names.
    std::string MeshAssetFileComponent(const std::string& text)
    {
        std::string result;
        result.reserve(text.size());
        for (const unsigned char ch : text)
        {
            if (std::isalnum(ch) || ch == '_' || ch == '-')
            {
                result.push_back(static_cast<char>(ch));
            }
            else if (result.empty() || result.back() != '_')
            {
                result.push_back('_');
            }
        }
        while (!result.empty() && result.back() == '_') { result.pop_back(); }
        return result.empty() ? std::string("node") : result;
    }

    // I3: promote a glTF's runtime "auto" materials to real named material files, one per submesh
    // slot, so imported meshes are fully authorable (editable in the material editor, reusable).
    // Uses the SAME ordinal->material mapping the runtime uses (MeshManager::DescribeGltfMaterial),
    // so materials[i] lines up with submesh i. Textures come out as the imported models/<name>/
    // DDS paths (H2 resolves them); glTF factors are already baked into those DDS by H6, so a param
    // is only written when the corresponding texture is ABSENT. Existing files are preserved (a
    // re-import keeps material-editor edits; the DDS behind them is overwritten in place). Returns
    // the per-slot names ("auto" for null-material slots); empty on parse failure.
    std::vector<std::string> GenerateMaterialFilesForGltf(const std::string& geometry,
        const std::string& baseName)
    {
        std::vector<std::string> names;
        const size_t slotCount = std::max<size_t>(1, MeshManager::CountSubmeshes(geometry));
        for (size_t i = 0; i < slotCount; ++i)
        {
            // Preserve existing files (overwrite=false) so a re-import keeps material-editor edits.
            const std::string result = materialgen::WriteFromGltf(
                geometry, static_cast<int>(i), baseName + "_" + std::to_string(i), /*overwrite=*/false);
            names.push_back(result.empty() ? std::string("auto") : result);
        }
        return names;
    }

    bool WriteImportedMeshAsset(const fs::path& path,
        const std::string& geometry,
        float spawnScale,
        const std::string& materialBaseName)
    {
        nlohmann::json asset = nlohmann::json::object();
        std::ifstream existingFile(path);
        if (existingFile)
        {
            nlohmann::json existing = nlohmann::json::parse(
                existingFile, nullptr, false, true);
            if (!existing.is_discarded() && existing.is_object())
            {
                asset = std::move(existing); // preserve Mesh Editor overrides
            }
        }

        asset["geometry"] = geometry;
        // First import (no material binding yet): promote the glTF's auto-materials to named
        // files. A re-import keeps whatever material/materials the asset already carries.
        if (!asset.contains("material") && !asset.contains("materials"))
        {
            const std::vector<std::string> names =
                GenerateMaterialFilesForGltf(geometry, materialBaseName);
            bool anyNamed = false;
            for (const std::string& n : names) { if (n != "auto") { anyNamed = true; break; } }
            if (!anyNamed || names.empty()) { asset["material"] = "auto"; }
            else if (names.size() == 1) { asset["material"] = names[0]; }
            else { asset["materials"] = names; }
        }
        if (spawnScale > 0.0f) { asset["spawnScale"] = spawnScale; }
        else { asset.erase("spawnScale"); }

        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (ec) { return false; }
        std::ofstream out(path, std::ios::trunc);
        if (!out) { return false; }
        out << asset.dump(2) << '\n';
        return static_cast<bool>(out);
    }

    // After moving a texture set out of staging, repoint its material file(s) because the backend
    // wrote them with import_staging/ paths, which now dangle.
    void RepointPresetPaths(const std::string& stagingPrefix, const std::string& destPrefix)
    {
        std::error_code ec;
        for (const fs::directory_entry& entry :
            fs::directory_iterator("data/materials", fs::directory_options::skip_permission_denied, ec))
        {
            if (ec) { break; }
            std::error_code fileEc;
            if (!entry.is_regular_file(fileEc) || LowerExt(entry.path()) != ".json") { continue; }

            std::string content;
            {
                std::ifstream in(entry.path());
                if (!in) { continue; }
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
            if (!changed) { continue; }
            std::ofstream out(entry.path(), std::ios::trunc);
            if (out) { out << content; }
        }
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

bool ImportPanel::RecreateMeshAssets(const Item& item, float spawnScale,
    const std::vector<std::string>& splitNodes)
{
    if (item.kind != Kind::Mesh || item.gltfFile.empty()) { return false; }

    std::error_code relEc;
    const fs::path rel = fs::relative(
        fs::path(item.gltfFile), fs::path(item.path), relEc);
    if (relEc || !IsSafeRelativePath(rel)) { return false; }

    const fs::path dst = ProjectDest(item);
    const std::string geometry = (dst / rel).generic_string();
    const fs::path meshAssetRoot = dst.parent_path();
    if (splitNodes.empty())
    {
        return WriteImportedMeshAsset(
            meshAssetRoot / (item.name + ".mesh.json"), geometry, spawnScale, item.name);
    }

    for (const std::string& node : splitNodes)
    {
        const std::string component = MeshAssetFileComponent(node);
        const fs::path meshAssetPath = meshAssetRoot /
            (item.name + "_node_" + component + ".mesh.json");
        if (!WriteImportedMeshAsset(meshAssetPath,
                geometry + "#node:" + node, spawnScale, item.name + "_" + component))
        {
            return false;
        }
    }
    return true;
}

bool ImportPanel::BeginReimport(const EditorAssetRecord& asset, AssetRegistry& registry)
{
    if (running_.load())
    {
        status_ = "Another asset import is already running.";
        statusIsError_ = true;
        return false;
    }
    if (asset.importSourcePath.empty())
    {
        status_ = "This asset has no source under import_staging/.";
        statusIsError_ = true;
        return false;
    }

    Rescan();
    const fs::path requested(asset.importSourcePath);
    for (const Item& item : items_)
    {
        std::error_code ec;
        bool matches = fs::equivalent(fs::path(item.path), requested, ec);
        if (ec)
        {
            ec.clear();
            const fs::path itemAbsolute = fs::absolute(item.path, ec).lexically_normal();
            ec.clear();
            const fs::path requestedAbsolute = fs::absolute(requested, ec).lexically_normal();
            matches = !ec && itemAbsolute == requestedAbsolute;
        }
        if (!matches) { continue; }

        const fs::path destination(ProjectDest(item));
        fs::path outputRelative;
        bool isProjectResource = false;
        ec.clear();
        if (item.kind == Kind::Skybox)
        {
            isProjectResource = fs::equivalent(fs::path(asset.path), destination, ec);
            if (isProjectResource) { outputRelative = destination.filename(); }
        }
        else
        {
            ec.clear();
            outputRelative = fs::absolute(asset.path, ec).lexically_normal().lexically_relative(
                fs::absolute(destination, ec).lexically_normal());
            isProjectResource = !ec && IsSafeRelativePath(outputRelative);
        }

        // Material presets and staging records represent the whole import. The
        // texture/mesh records below are true per-resource reimports.
        if (!isProjectResource)
        {
            BeginImport(item, {}, true);
            return true;
        }

        std::vector<std::string> existingSources;
        std::vector<std::string> missingSources;
        for (const std::string& relative : asset.importSourceFiles)
        {
            std::error_code sourceEc;
            const fs::path sourceFile = item.kind == Kind::Skybox ?
                fs::path(item.path) : (fs::path(item.path) / relative);
            if (fs::is_regular_file(sourceFile, sourceEc))
            {
                existingSources.push_back(relative);
            }
            else { missingSources.push_back(relative); }
        }

        // No producer remains (or this is a legacy orphan): remove this one
        // mapped output, leaving every sibling in the folder untouched.
        if (existingSources.empty())
        {
            std::error_code removeEc;
            if (!fs::remove(fs::path(asset.path), removeEc) || removeEc)
            {
                status_ = "Could not remove stale output: " + asset.path;
                statusIsError_ = true;
                return false;
            }
            if (!RemoveOutputFromManifest(
                    ImportManifestPath(destination, item.kind == Kind::Skybox),
                    outputRelative.generic_string(), asset.importSourceFiles))
            {
                status_ = "Removed output, but failed to update its import manifest.";
                statusIsError_ = true;
                registry.Refresh();
                Rescan();
                return false;
            }
            status_ = "Removed stale output: " + asset.path;
            statusIsError_ = false;
            registry.Refresh();
            Rescan();
            return true;
        }

        BeginImport(item, existingSources, true,
            { outputRelative.generic_string() }, missingSources);
        return true;
    }

    status_ = "Staged source is no longer available: " + asset.importSourcePath;
    statusIsError_ = true;
    return false;
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
            it.importStatus = InspectAssetImportStatus(it.path, ProjectDest(it));
            items_.push_back(std::move(it));
            continue;
        }
        if (!entry.is_directory(ec)) { continue; }

        // A directory: mesh asset if it holds a glTF/GLB, else skybox(es) for folder-wrapped
        // .hdr downloads (Poly Haven HDRIs unzip into their own folder), else a texture set
        // if it holds images. An .hdr can coexist with a texture set — both items are listed.
        fs::path gltf;
        std::vector<fs::path> hdrs;
        bool hasTex = false;
        for (auto it = fs::recursive_directory_iterator(p, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec) { break; }
            if (!it->is_regular_file(ec)) { continue; }
            const std::string ext = LowerExt(it->path());
            if (gltf.empty() && (ext == ".gltf" || ext == ".glb")) { gltf = it->path(); }
            else if (ext == ".hdr") { hdrs.push_back(it->path()); }
            else if (IsConvertibleTexture(ext)) { hasTex = true; }
        }

        const std::string folderLicense = ReadLicense(p);

        if (gltf.empty())
        {
            for (const fs::path& hdr : hdrs)
            {
                Item sky;
                sky.path = hdr.string();
                sky.name = hdr.stem().string();
                sky.kind = Kind::Skybox;
                sky.meta = "equirect HDRI -> BC6H cubemap";
                sky.license = folderLicense;
                sky.alreadyInProject = fs::exists(ProjectDest(sky), ec);
                sky.importStatus = InspectAssetImportStatus(sky.path, ProjectDest(sky));
                items_.push_back(std::move(sky));
            }
        }

        Item it;
        it.path = p.string();
        it.name = p.filename().string();
        it.license = folderLicense;

        if (!gltf.empty())
        {
            it.kind = Kind::Mesh;
            it.gltfFile = gltf.string();
            std::string copyright;
            DescribeGltf(gltf, it.meta, copyright, it.worldSizeM);
            if (it.license.empty() && !copyright.empty()) { it.license = copyright + "\n"; }
        }
        else if (hasTex)
        {
            it.kind = Kind::TextureSet;
            it.meta = "texture set -> DDS + material preset";
        }
        else
        {
            continue; // nothing importable beyond any skybox rows above
        }
        it.alreadyInProject = fs::exists(ProjectDest(it), ec);
        it.importStatus = InspectAssetImportStatus(it.path, ProjectDest(it));
        items_.push_back(std::move(it));
    }

    std::sort(items_.begin(), items_.end(),
        [](const Item& a, const Item& b) { return a.name < b.name; });
}

void ImportPanel::BeginImport(const Item& item,
    const std::vector<std::string>& includeRel,
    bool registerPreset,
    const std::vector<std::string>& targetOutputs,
    const std::vector<std::string>& removedSources,
    float meshSpawnScale,
    const std::vector<std::string>& meshSplitNodes,
    bool meshSplitChoiceProvided)
{
    if (running_.load()) { return; }
    activeItem_ = item;
    activeTargetOutputs_.clear();
    for (const std::string& output : targetOutputs)
    {
        activeTargetOutputs_.push_back(NormalizeRelativePath(output));
    }
    activeRemovedSources_ = removedSources;
    // Any subset (dialog file selection or per-resource reimport) merges into the existing
    // manifest and must not delete sibling outputs; only a full import owns the whole folder.
    activeMergeManifest_ = !includeRel.empty() || !activeTargetOutputs_.empty();
    activeMeshSpawnScale_ = 0.0f;
    activeMeshSplitGltf_.clear();
    activeMeshSplitNodes_.clear();
    if (item.kind == Kind::Mesh)
    {
        const fs::path manifestPath = ImportManifestPath(ProjectDest(item), false);
        activeMeshSpawnScale_ = meshSpawnScale >= 0.0f ? meshSpawnScale :
            ReadManifestSpawnScale(manifestPath);
        if (meshSplitChoiceProvided)
        {
            activeMeshSplitNodes_ = meshSplitNodes;
            std::error_code relativeEc;
            const fs::path relative = fs::relative(
                fs::path(item.gltfFile), fs::path(item.path), relativeEc);
            if (!relativeEc && IsSafeRelativePath(relative))
            {
                activeMeshSplitGltf_ = relative.generic_string();
            }
        }
        else
        {
            activeMeshSplitNodes_ = ReadManifestSplitNodes(
                manifestPath, &activeMeshSplitGltf_);
        }
    }
    running_.store(true);
    workerFailures_.store(0);
    progressDone_.store(0);
    progressTotal_.store(0);
    workerManifestJson_.clear();
    joinPending_ = true;
    status_ = "Importing " + item.name + " ...";
    statusIsError_ = false;

    // Snapshot options for the worker (avoid racing the UI).
    assets::ImportOptions opt;
    opt.maxTextureSize = maxTextureSize_;
    opt.highQuality = highQuality_;
    opt.flipGreen = flipGreen_;
    opt.useGpu = useGpu_;
    opt.skyboxFaceSize = skyboxFaceSize_;
    opt.logPath = "asset_import.log";
    opt.registerPreset = registerPreset;
    opt.includeRel = includeRel;
    opt.progressDone = &progressDone_;   // atomics live in this panel, which outlives the worker
    opt.progressTotal = &progressTotal_;
    if (item.kind == Kind::Skybox) { opt.skyboxHdr = item.path; }
    else { opt.stagingDir = item.path; }

    std::vector<std::string> manifestIncludeRel = includeRel;
    if (activeMergeManifest_)
    {
        const fs::path destination(ProjectDest(item));
        const fs::path manifestPath = ImportManifestPath(
            destination, item.kind == Kind::Skybox);
        std::error_code manifestEc;
        if (!fs::is_regular_file(manifestPath, manifestEc))
        {
            // Establish a complete baseline for a legacy import so untouched
            // sibling resources do not become dirty when the manifest appears.
            manifestIncludeRel.clear();
        }
    }

    const std::string sourcePath = item.path;
    worker_ = std::thread([this, opt, sourcePath, manifestIncludeRel]()
    {
        const int failures = assets::RunImport(opt); // CPU-heavy BC7 — off the UI thread
        if (failures == 0)
        {
            workerManifestJson_ = BuildSourceManifestJson(sourcePath, manifestIncludeRel);
        }
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
        std::error_code sizeEc;
        const uintmax_t bytes = fs::file_size(it->path(), sizeEc);
        if (!sizeEc)
        {
            char sizeText[32];
            snprintf(sizeText, sizeof(sizeText), "%.1f MB", double(bytes) / (1024.0 * 1024.0));
            df.sizeText = sizeText;
        }
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
    ImGui::SetNextWindowSize(ImVec2(560.0f, 430.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 280.0f), ImVec2(FLT_MAX, FLT_MAX));

    if (!ImGui::BeginPopupModal("Import Texture Set", nullptr))
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

    // File list fills the dialog; the footer (preset checkbox + buttons) stays pinned below.
    const float dialogFooterH = ImGui::GetFrameHeightWithSpacing() * 2.0f +
        ImGui::GetTextLineHeightWithSpacing();
    ImGui::BeginChild("##dlgFiles", ImVec2(0.0f, -dialogFooterH), true);
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
        if (!f.sizeText.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", f.sizeText.c_str());
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::Checkbox("Create material preset (synthesize MR)", &dialogCreatePreset_);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "On: albedo + rough(+metal) + normal -> one material file in data/materials/.\n"
            "Off: just convert the selected images to DDS (no material).");
    }

    ImGui::Separator();
    int selCount = 0;
    for (const auto& f : dialogFiles_) { if (f.selected) { ++selCount; } }

    ImGui::BeginDisabled(selCount == 0);
    if (ImGui::Button("Import", ImVec2(120.0f, 0.0f)))
    {
        // Everything selected = a FULL import (empty whitelist): the destination folder is
        // synced to the run, stale outputs of deleted sources get cleaned. A subset = a
        // merge import: only the selected maps are converted/copied, existing outputs of
        // unselected maps are left untouched (unchecking never deletes).
        std::vector<std::string> includeRel;
        for (const auto& f : dialogFiles_) { if (f.selected) { includeRel.push_back(f.rel); } }
        if (includeRel.size() == dialogFiles_.size()) { includeRel.clear(); }
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

void ImportPanel::OpenMeshImportDialog(const Item& item)
{
    meshDialogItem_ = item;
    meshDialogNormalizeSpawn_ = false;
    meshDialogTargetM_ = 6.0f;
    meshDialogTopLevelNodes_ = ListSplittableTopLevelNodes(item.gltfFile);
    meshDialogSplitTopLevelNodes_ = false;

    const fs::path manifestPath = ImportManifestPath(ProjectDest(item), false);
    const float existingScale = ReadManifestSpawnScale(manifestPath);
    const std::vector<std::string> existingSplitNodes =
        ReadManifestSplitNodes(manifestPath);
    std::error_code ec;
    if (fs::is_regular_file(manifestPath, ec))
    {
        meshDialogNormalizeSpawn_ = existingScale > 0.0f;
    }
    if (existingScale > 0.0f && item.worldSizeM > 0.0f)
    {
        meshDialogTargetM_ = item.worldSizeM * existingScale;
    }
    if (!existingSplitNodes.empty() && meshDialogTopLevelNodes_.size() > 1)
    {
        meshDialogSplitTopLevelNodes_ = true;
    }
    showMeshImportDialog_ = true;
}

void ImportPanel::DrawMeshImportDialog(AssetRegistry& registry)
{
    if (showMeshImportDialog_)
    {
        ImGui::OpenPopup("Import Mesh");
        showMeshImportDialog_ = false;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 center(
        viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
        viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(470.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Import Mesh", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }

    ImGui::Text("Import '%s'", meshDialogItem_.name.c_str());
    ImGui::Separator();
    if (meshDialogItem_.worldSizeM > 0.0f)
    {
        ImGui::Text("Detected longest side: %.3f m", meshDialogItem_.worldSizeM);
    }
    else
    {
        ImGui::TextColored(ImVec4(0.96f, 0.62f, 0.16f, 1.0f),
            "Mesh size could not be determined from the glTF bounds.");
        meshDialogNormalizeSpawn_ = false;
    }

    ImGui::BeginDisabled(meshDialogItem_.worldSizeM <= 0.0f);
    ImGui::Checkbox("Normalize spawn size", &meshDialogNormalizeSpawn_);
    ImGui::BeginDisabled(!meshDialogNormalizeSpawn_);
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputFloat("Target longest side (m)", &meshDialogTargetM_,
        0.0f, 0.0f, "%.2f");
    meshDialogTargetM_ = std::clamp(meshDialogTargetM_, 0.1f, 1000.0f);
    if (meshDialogNormalizeSpawn_ && meshDialogItem_.worldSizeM > 0.0f)
    {
        ImGui::TextDisabled("Default spawn scale: %.6f",
            meshDialogTargetM_ / meshDialogItem_.worldSizeM);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    if (meshDialogTopLevelNodes_.size() > 1)
    {
        ImGui::Spacing();
        ImGui::Checkbox("Split by top-level nodes", &meshDialogSplitTopLevelNodes_);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Create one spawnable content-browser asset per top-level prop.\n"
                "The glTF stays as one file; each asset uses a #node selector.");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%d props detected",
            static_cast<int>(meshDialogTopLevelNodes_.size()));
        if (meshDialogSplitTopLevelNodes_)
        {
            ImGui::Indent();
            const size_t previewCount = std::min<size_t>(
                meshDialogTopLevelNodes_.size(), 6);
            for (size_t i = 0; i < previewCount; ++i)
            {
                ImGui::BulletText("%s", meshDialogTopLevelNodes_[i].c_str());
            }
            if (previewCount < meshDialogTopLevelNodes_.size())
            {
                ImGui::TextDisabled("... and %d more",
                    static_cast<int>(meshDialogTopLevelNodes_.size() - previewCount));
            }
            ImGui::Unindent();
        }
    }

    ImGui::Spacing();
    ImGui::TextWrapped(
        "Normalization records a default spawn scale in the import manifest. "
        "It does not modify the glTF or its vertex data.");
    ImGui::Separator();

    const float selectedSpawnScale =
        meshDialogNormalizeSpawn_ && meshDialogItem_.worldSizeM > 0.0f ?
        meshDialogTargetM_ / meshDialogItem_.worldSizeM : 0.0f;
    const std::vector<std::string> selectedSplitNodes =
        meshDialogSplitTopLevelNodes_ ? meshDialogTopLevelNodes_ :
        std::vector<std::string>{};

    if (ImGui::Button(meshDialogItem_.alreadyInProject ? "Re-import" : "Import",
            ImVec2(0.0f, 0.0f)))
    {
        BeginImport(meshDialogItem_, {}, true, {}, {}, selectedSpawnScale,
            selectedSplitNodes, true);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!meshDialogItem_.alreadyInProject);
    const bool recreateMeshJson = ImGui::Button(
        "Recreate mesh JSON only", ImVec2(0.0f, 0.0f));
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip(
            "Regenerate only the spawnable .mesh.json asset(s).\n"
            "No glTF, binary, or texture files are re-imported.\n"
            "Uses the current normalization and split options.");
    }
    if (recreateMeshJson)
    {
        const bool recreated = RecreateMeshAssets(meshDialogItem_,
            selectedSpawnScale, selectedSplitNodes);
        status_ = recreated ?
            (selectedSplitNodes.empty() ?
                "Recreated mesh JSON for " + meshDialogItem_.name :
                "Recreated " + std::to_string(selectedSplitNodes.size()) +
                " split mesh JSON files for " + meshDialogItem_.name) :
            "Failed to recreate mesh JSON for " + meshDialogItem_.name;
        statusIsError_ = !recreated;
        if (recreated) { registry.Refresh(); }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(0.0f, 0.0f)))
    {
        ImGui::CloseCurrentPopup();
    }

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
        bool finalizeFailed = false;
        std::set<std::string> projectOutputs;
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
                    if (!ec)
                    {
                        std::error_code removeEc;
                        fs::remove(produced, removeEc);
                    }
                }
                finalizeFailed = static_cast<bool>(ec);
                if (!finalizeFailed)
                {
                    projectOutputs.insert(NormalizeRelativePath(dst.filename()));
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
                    if (rec || !IsSafeRelativePath(rel))
                    {
                        finalizeFailed = true;
                        continue;
                    }
                    const std::string normalizedRel = NormalizeRelativePath(rel);
                    // Per-resource reimports copy only their mapped output; a dialog
                    // subset import copies everything this run produced.
                    if (!activeTargetOutputs_.empty() &&
                        std::find(activeTargetOutputs_.begin(), activeTargetOutputs_.end(),
                            normalizedRel) == activeTargetOutputs_.end())
                    {
                        continue;
                    }
                    const fs::path target = dst / rel;
                    fs::create_directories(target.parent_path(), rec);
                    if (!rec)
                    {
                        fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, rec);
                    }
                    if (rec)
                    {
                        finalizeFailed = true;
                    }
                    else
                    {
                        projectOutputs.insert(normalizedRel);
                        if (ext == ".dds") { generatedDds.push_back(it->path()); }
                    }
                }
                for (const fs::path& d : generatedDds)
                {
                    std::error_code rrec;
                    fs::remove(d, rrec);
                }

                if (!finalizeFailed && !activeMergeManifest_)
                {
                    // A FULL import owns the destination folder: synchronize it to
                    // this run so deleted sources cannot leave stale DDS or mesh
                    // files behind. Subset/per-resource imports never delete —
                    // outputs of earlier imports survive.
                    RemoveStaleProjectOutputs(dst, projectOutputs);
                }
                else if (!finalizeFailed)
                {
                    projectOutputs = EnumerateProjectOutputs(dst);
                }

                // A texture set's material preset was written by the backend pointing into
                // import_staging/ — repoint it to the moved copy so it doesn't dangle.
                if (!finalizeFailed && activeItem_.kind == Kind::TextureSet)
                {
                    RepointPresetPaths("import_staging/" + activeItem_.name + "/",
                        "textures/" + activeItem_.name + "/");
                }
            }
            if (!finalizeFailed)
            {
                const bool outputIsFile = activeItem_.kind == Kind::Skybox;
                finalizeFailed = !WriteImportManifest(workerManifestJson_, projectOutputs,
                    fs::path(activeItem_.path), ImportManifestPath(dst, outputIsFile),
                    activeMergeManifest_, activeRemovedSources_, activeMeshSpawnScale_,
                    activeMeshSplitGltf_, activeMeshSplitNodes_);
            }

            // Emit first-class mesh assets next to the imported pack directory. A whole-pack
            // import owns models/<name>.mesh.json; a split import owns one sidecar per selected
            // top-level node. Existing sidecars retain Mesh Editor overrides when re-imported.
            if (!finalizeFailed && activeItem_.kind == Kind::Mesh &&
                !activeItem_.gltfFile.empty())
            {
                finalizeFailed = !RecreateMeshAssets(activeItem_,
                    activeMeshSpawnScale_, activeMeshSplitNodes_);
            }
            destLabel = finalizeFailed ? ("FINALIZE FAILED -> " + dst.string()) :
                ("-> " + dst.string());
        }
        if (!finalizeFailed)
        {
            WriteCreditsEntry(activeItem_.name, activeItem_.license);
        }
        registry.Refresh();
        Rescan(); // pick up the new alreadyInProject state
        status_ = finalizeFailed ? ("Import FINALIZE FAILED for " + activeItem_.name) :
            ("Imported " + activeItem_.name + "  " + destLabel);
        statusIsError_ = finalizeFailed;
        finishedOut = !finalizeFailed;
    }
    else
    {
        status_ = "Import FAILED (" + std::to_string(failures) + ") — see asset_import.log";
        statusIsError_ = true;
    }
}

bool ImportPanel::Draw(AssetRegistry& registry, bool* open)
{
    bool finished = false;
    PollImport(registry, finished);

    ImGui::SetNextWindowSize(ImVec2(860.0f, 440.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 260.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin("Import Assets", open))
    {
        ImGui::End();
        return finished;
    }

    if (!scanned_ || (!running_.load() && lastRegistryRevision_ != registry.Revision()))
    {
        Rescan();
        lastRegistryRevision_ = registry.Revision();
    }

    // "Re-import all changed" queue pump — imports run one at a time, so start the
    // next queued item as soon as the worker is idle and fully joined.
    if (!running_.load() && !joinPending_ && !reimportQueue_.empty())
    {
        const std::string next = reimportQueue_.front();
        reimportQueue_.erase(reimportQueue_.begin());
        for (const Item& queued : items_)
        {
            if (queued.path == next)
            {
                BeginImport(queued, {}, true);
                break;
            }
        }
    }

    // --- Header -------------------------------------------------------------
    ImGui::TextWrapped(
        "Drop raw asset folders (glTF meshes, texture sets) or .hdr skyboxes into "
        "import_staging/, then import them into the project here.");
    if (ImGui::Button("Rescan")) { Rescan(); }
    ImGui::SameLine();
    ImGui::TextDisabled("%d item%s staged", static_cast<int>(items_.size()),
        items_.size() == 1 ? "" : "s");
    int changedCount = 0;
    for (const Item& it : items_)
    {
        if (it.importStatus == EditorAssetImportStatus::SourceNewer ||
            it.importStatus == EditorAssetImportStatus::Incomplete)
        {
            ++changedCount;
        }
    }
    if (changedCount > 0)
    {
        ImGui::SameLine();
        ImGui::BeginDisabled(running_.load() || !reimportQueue_.empty());
        char reimportLabel[48];
        snprintf(reimportLabel, sizeof(reimportLabel), "Re-import changed (%d)", changedCount);
        if (ImGui::Button(reimportLabel))
        {
            for (const Item& it : items_)
            {
                if (it.importStatus == EditorAssetImportStatus::SourceNewer ||
                    it.importStatus == EditorAssetImportStatus::Incomplete)
                {
                    reimportQueue_.push_back(it.path);
                }
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("Queue a full re-import of every CHANGED/MISSING item, one at a time.");
        }
    }

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
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::SetNextItemWidth(120.0f);
        const char* faces[] = { "512", "1024", "2048" };
        int faceIdx = skyboxFaceSize_ >= 2048 ? 2 : (skyboxFaceSize_ >= 1024 ? 1 : 0);
        if (ImGui::Combo("Skybox face size", &faceIdx, faces, 3))
        {
            skyboxFaceSize_ = faceIdx == 2 ? 2048 : (faceIdx == 1 ? 1024 : 512);
        }
        ImGui::Checkbox("High-quality BC7 (slower)", &highQuality_);
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::Checkbox("Move into project after import", &moveIntoProject_);
        ImGui::Checkbox("GPU texture encode", &useGpu_);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Encode BC6H/BC7 on the GPU (D3D11 compute) — seconds -> sub-second per 2K.\n"
                "Falls back to the CPU encoder automatically if no device is available.");
        }
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::Checkbox("Flip normal-map green", &flipGreen_);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Leave OFF for glTF meshes and plain textures.\n"
                "Only enable if a normal-mapped surface ends up lit from the wrong side.");
        }
    }

    // --- Status: blue while running, then green on success / red on failure --
    if (running_.load())
    {
        ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.0f, 1.0f), "%s", status_.c_str());
    }
    else if (!status_.empty())
    {
        const ImVec4 statusColor = statusIsError_ ?
            ImVec4(1.00f, 0.42f, 0.34f, 1.0f) : ImVec4(0.50f, 0.85f, 0.50f, 1.0f);
        ImGui::TextColored(statusColor, "%s", status_.c_str());
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
    else if (ImGui::BeginTable("##importItems", 5, tableFlags))
    {
        ImGui::TableSetupColumn("Asset", ImGuiTableColumnFlags_WidthStretch, 0.38f);
        ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, 0.34f);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 72.0f);
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

            // Details. A "prop" measured in tens/hundreds of meters is almost always a
            // cm-authored glTF with no unit conversion — warn so the user expects to scale.
            ImGui::TableSetColumnIndex(1);
            const bool implausiblyLarge = it.kind == Kind::Mesh && it.worldSizeM > 50.0f;
            if (implausiblyLarge)
            {
                ImGui::TextColored(ImVec4(0.96f, 0.62f, 0.16f, 1.0f), "%s", it.meta.c_str());
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "Baked world size is ~%.0f m — likely centimeter-authored.\n"
                        "Use the mesh import dialog to choose a normalized spawn size.",
                        it.worldSizeM);
                }
            }
            else
            {
                ImGui::TextDisabled("%s", it.meta.c_str());
            }

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

            // Import freshness: source timestamps are compared against DDS/copies.
            ImGui::TableSetColumnIndex(3);
            ImGui::TextColored(ImportStatusColor(it.importStatus), "%s",
                ImportStatusBadge(it.importStatus));
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s\nSource: %s\nOutput: %s",
                    ImportStatusHint(it.importStatus),
                    it.path.c_str(),
                    ProjectDest(it).c_str());
            }

            // Action: Import / Re-import; the row being imported shows live state instead.
            ImGui::TableSetColumnIndex(4);
            if (busy && it.path == activeItem_.path)
            {
                ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.0f, 1.0f), "importing...");
            }
            else
            {
                ImGui::BeginDisabled(busy);
                if (ImGui::SmallButton(it.alreadyInProject ? "Re-import" : "Import"))
                {
                    // Each asset type asks only for the decisions relevant to it.
                    if (it.kind == Kind::TextureSet) { OpenImportDialog(it); }
                    else if (it.kind == Kind::Mesh) { OpenMeshImportDialog(it); }
                    else { BeginImport(it, {}, true); }
                }
                ImGui::EndDisabled();
                if (it.alreadyInProject && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::SetTooltip("%s\nRe-import all source files to: %s",
                        ImportStatusHint(it.importStatus), ProjectDest(it).c_str());
                }
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

    DrawImportDialog();     // texture-set pick-files + preset modal
    DrawMeshImportDialog(registry); // per-mesh spawn-size normalization modal

    ImGui::End();
    return finished;
}

#endif // WITH_EDITOR
