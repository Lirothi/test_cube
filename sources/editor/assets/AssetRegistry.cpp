#include "editor/assets/AssetRegistry.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <system_error>

// nlohmann/json — single header
#pragma warning(push)
#pragma warning(disable: 26819)
#include "third_party/json/json.hpp"
#pragma warning(pop)

namespace fs = std::filesystem;

namespace
{
    std::string ToLower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    uint64_t WriteTimeOf(const fs::path& p)
    {
        std::error_code ec;
        const fs::file_time_type t = fs::last_write_time(p, ec);
        if (ec)
        {
            return 0;
        }
        return static_cast<uint64_t>(t.time_since_epoch().count());
    }

    // Returns the matching allowed extension (lowercased, with leading dot),
    // preferring the longest match so ".mesh.txt" wins over ".txt". Empty if no
    // allowed extension matches.
    std::string MatchExtension(const fs::path& p,
        const std::vector<std::string>& allowed)
    {
        const std::string name = ToLower(p.filename().string());
        std::string best;
        for (const std::string& ext : allowed)
        {
            if (name.size() >= ext.size() &&
                name.compare(name.size() - ext.size(), ext.size(), ext) == 0 &&
                ext.size() > best.size())
            {
                best = ext;
            }
        }
        return best;
    }

    struct DirRoot
    {
        const char* dir;
        EditorAssetType type;
        std::vector<std::string> extensions;
    };
}

void AssetRegistry::Refresh()
{
    assets_.clear();

    const std::array<DirRoot, 4> roots = { {
        { "models",      EditorAssetType::Mesh,    { ".obj", ".mesh.txt", ".txt" } },
        { "textures",    EditorAssetType::Texture, { ".dds", ".png" } },
        { "data/levels", EditorAssetType::Level,   { ".json" } },
        { "shaders",     EditorAssetType::Shader,  { ".hlsl" } },
    } };

    for (const DirRoot& root : roots)
    {
        std::error_code ec;
        if (!fs::is_directory(root.dir, ec))
        {
            continue;
        }

        for (fs::recursive_directory_iterator it(root.dir,
                 fs::directory_options::skip_permission_denied, ec), end;
             it != end;
             it.increment(ec))
        {
            if (ec)
            {
                break;
            }

            std::error_code fileEc;
            if (!it->is_regular_file(fileEc))
            {
                continue;
            }

            const fs::path& p = it->path();
            const std::string ext = MatchExtension(p, root.extensions);
            if (ext.empty())
            {
                continue;
            }

            EditorAssetRecord record;
            record.id.type = root.type;
            record.path = p.generic_string();
            record.id.key = record.path;
            record.displayName = p.filename().string();
            record.extension = ext;
            record.fileWriteTime = WriteTimeOf(p);
            assets_.push_back(std::move(record));
        }
    }

    // Material presets: names under "presets" in data/materials.json. Parsed
    // directly here (no MaterialDataManager / GPU dependency).
    {
        const char* materialsPath = "data/materials.json";
        std::error_code ec;
        if (fs::exists(materialsPath, ec))
        {
            std::ifstream file(materialsPath);
            nlohmann::json doc;
            bool parsed = false;
            if (file)
            {
                try
                {
                    file >> doc;
                    parsed = true;
                }
                catch (const std::exception&)
                {
                    parsed = false;
                }
            }

            if (parsed)
            {
                const auto presetsIt = doc.find("presets");
                if (presetsIt != doc.end() && presetsIt->is_object())
                {
                    const uint64_t writeTime = WriteTimeOf(materialsPath);
                    for (auto it = presetsIt->begin(); it != presetsIt->end(); ++it)
                    {
                        EditorAssetRecord record;
                        record.id.type = EditorAssetType::MaterialPreset;
                        record.id.key = it.key();
                        record.path = materialsPath;
                        record.displayName = it.key();
                        record.fileWriteTime = writeTime;
                        assets_.push_back(std::move(record));
                    }
                }
            }
        }
    }

    // Stable order (type, then display name) so counts and the later content
    // browser are deterministic across refreshes.
    std::sort(assets_.begin(), assets_.end(),
        [](const EditorAssetRecord& a, const EditorAssetRecord& b)
        {
            if (a.id.type != b.id.type)
            {
                return static_cast<int>(a.id.type) < static_cast<int>(b.id.type);
            }
            return a.displayName < b.displayName;
        });
}

std::vector<const EditorAssetRecord*> AssetRegistry::Search(std::string_view text,
    EditorAssetType typeFilter) const
{
    std::vector<const EditorAssetRecord*> results;
    const std::string needle = ToLower(std::string(text));

    for (const EditorAssetRecord& record : assets_)
    {
        if (typeFilter != EditorAssetType::Unknown && record.id.type != typeFilter)
        {
            continue;
        }
        if (!needle.empty())
        {
            if (ToLower(record.displayName).find(needle) == std::string::npos &&
                ToLower(record.path).find(needle) == std::string::npos)
            {
                continue;
            }
        }
        results.push_back(&record);
    }
    return results;
}

const EditorAssetRecord* AssetRegistry::FindByPath(std::string_view path) const
{
    for (const EditorAssetRecord& record : assets_)
    {
        if (record.path == path)
        {
            return &record;
        }
    }
    return nullptr;
}

size_t AssetRegistry::CountByType(EditorAssetType type) const
{
    size_t count = 0;
    for (const EditorAssetRecord& record : assets_)
    {
        if (record.id.type == type)
        {
            ++count;
        }
    }
    return count;
}

const char* ToString(EditorAssetType type)
{
    switch (type)
    {
    case EditorAssetType::Mesh:           return "Mesh";
    case EditorAssetType::MaterialPreset: return "Material";
    case EditorAssetType::Texture:        return "Texture";
    case EditorAssetType::Level:          return "Level";
    case EditorAssetType::Shader:         return "Shader";
    case EditorAssetType::Unknown:        return "Unknown";
    }
    return "Unknown";
}
