// The content cook -- docs/distribution_build_plan.md §8.0, §8.1. See Cook.h for the command line.
//
// What goes into base.tcpak, in the order it is found:
//   1. every shipped level and what it names, recursively through every JSON it reaches: a string with a slash and a
//      content extension is a file from the content root, a bare name with data/materials/<name>.json is a material;
//      the derived files the engine finds by name (x.png -> x.dds, x.mesh.bin -> x.mesh.json / x.watermask.bin, a sky's
//      _spec / _diffuse and brdf_lut, a font's .tga); the level's bot memory;
//   2. the profile's `include`, as it is (not walked: a material listed there brings no textures);
//   3. the record runs' list (tools/dist/vfs_record.txt): every content file a shipped build opened -- what is named only
//      in code;
//   4. string literals of sources/ outside the editor that name an existing content file -- what the record may have
//      missed; one no record run opened is a warning;
//   5. config/graphics_defaults.json, the profile's curated settings.
// Water mask bakes are re-keyed for the geometry's content hash (its vfs::Stat::version in a package). shaders.tcpak is
// Material::CookShaders over the profile's shader list. A missing file, a mask that does not serve its geometry, and --
// in a profile with "strictLicences" -- an asset without a known licence are errors: nothing is built then.

#include "app/dist/Cook.h"

#include "core/logging/Log.h"
#include "core/vfs/Pak.h"
#include "core/vfs/PakCrypto.h"
#include "core/vfs/Vfs.h"
#include "materials/Material.h"
#include "ocean/WaterMask.h"
#include "third_party/json/json.hpp"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace dist
{
namespace
{
    using Json = nlohmann::json;
    using Clock = std::chrono::steady_clock;

    std::string ArgValue(const char* cmd, const char* key) // `key` ends with '='
    {
        const char* p = std::strstr(cmd, key);
        if (!p) { return {}; }
        p += std::strlen(key);
        std::string value;
        if (*p == '"')
        {
            for (++p; *p && *p != '"'; ++p) { value.push_back(*p); }
        }
        else
        {
            for (; *p && !std::isspace(static_cast<unsigned char>(*p)); ++p) { value.push_back(*p); }
        }
        return value;
    }

    bool HasFlag(const char* cmd, const char* flag)
    {
        const std::size_t length = std::strlen(flag);
        for (const char* p = std::strstr(cmd, flag); p; p = std::strstr(p + 1, flag))
        {
            const char next = p[length];
            if ((p == cmd || std::isspace(static_cast<unsigned char>(p[-1]))) && (next == 0 || std::isspace(static_cast<unsigned char>(next))))
            {
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> SplitComma(const std::string& text)
    {
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (start <= text.size())
        {
            std::size_t comma = text.find(',', start);
            if (comma == std::string::npos) { comma = text.size(); }
            if (comma > start) { parts.push_back(text.substr(start, comma - start)); }
            start = comma + 1;
        }
        return parts;
    }

    std::string Lower(std::string_view text)
    {
        std::string out(text);
        for (char& c : out) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
        return out;
    }

    bool EndsWith(std::string_view text, std::string_view suffix)
    {
        return text.size() >= suffix.size() && Lower(text.substr(text.size() - suffix.size())) == suffix;
    }

    bool StartsWith(std::string_view text, std::string_view prefix) { return text.rfind(prefix, 0) == 0; }

    // A file name against a pattern of * and ? (both already lower case).
    bool WildMatch(std::string_view name, std::string_view pattern)
    {
        std::size_t n = 0, p = 0, star = std::string_view::npos, mark = 0;
        while (n < name.size())
        {
            if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n])) { ++n; ++p; }
            else if (p < pattern.size() && pattern[p] == '*') { star = p++; mark = n; }
            else if (star != std::string_view::npos) { p = star + 1; n = ++mark; }
            else { return false; }
        }
        while (p < pattern.size() && pattern[p] == '*') { ++p; }
        return p == pattern.size();
    }

    std::string Extension(std::string_view path)
    {
        const std::size_t slash = path.find_last_of('/');
        const std::size_t dot = path.find_last_of('.');
        if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) { return {}; }
        return Lower(path.substr(dot));
    }

    std::string StemOf(std::string_view path) // the path without its last extension
    {
        const std::size_t slash = path.find_last_of('/');
        const std::size_t dot = path.find_last_of('.');
        if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) { return std::string(path); }
        return std::string(path.substr(0, dot));
    }

    std::string FileStem(std::string_view path)
    {
        const std::size_t slash = path.find_last_of('/');
        return StemOf(slash == std::string_view::npos ? path : path.substr(slash + 1));
    }

    constexpr std::string_view kContentExtensions[] = {
        ".json", ".bin", ".dds", ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".ttf", ".otf", ".hdr", ".exr", ".hlsl", ".hlsli",
        ".wav", ".ogg",
    };

    bool HasContentExtension(std::string_view path)
    {
        const std::string ext = Extension(path);
        return std::find(std::begin(kContentExtensions), std::end(kContentExtensions), ext) != std::end(kContentExtensions);
    }

    bool IsImage(std::string_view path)
    {
        const std::string ext = Extension(path);
        return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp";
    }

    // A content path as a package holds it: forward slashes from the content root. Nothing absolute, nothing above it.
    std::optional<std::string> Normalise(std::string_view raw)
    {
        std::string s(raw);
        std::replace(s.begin(), s.end(), '\\', '/');
        while (StartsWith(s, "./")) { s.erase(0, 2); }
        for (std::size_t at = s.find("//"); at != std::string::npos; at = s.find("//")) { s.erase(at, 1); }
        if (s.empty() || s[0] == '/' || s.find(':') != std::string::npos || s.find("..") != std::string::npos) { return std::nullopt; }
        return s;
    }

    // What is never content: the writable folders of a dev tree, and the shaders (their own package).
    bool NeverInBase(std::string_view path)
    {
        constexpr std::string_view kPrefixes[] = { "shaders/", "logs/", "shader_cache/", "editor_cache/", "traces/", "dist/",
                                                   "dist_symbols/", "x64/" };
        for (const std::string_view prefix : kPrefixes)
        {
            if (StartsWith(path, prefix)) { return true; }
        }
        return path == "graphics_settings.json" || path == "imgui.ini" || path == "editor_state.json";
    }

    struct Profile
    {
        std::string file;
        std::string product = "test_cube";
        std::string version = "0.0.0";
        std::vector<std::string> levels;
        std::string bootLevel;
        std::string startMode = "play";
        std::vector<std::string> include;
        std::string graphicsDefaults;
        std::string shaderList = "tools/dist/shader_list.txt";
        std::string vfsRecord = "tools/dist/vfs_record.txt";
        std::string shaderModel = "6.6";
        int level = 19;
        bool encrypt = false;
        bool shaderSources = true;
        bool strictLicences = false;
    };

    struct FileRecord
    {
        std::set<std::string> referencedBy;
        std::set<std::string> levels;     // the shipped levels whose walk reaches it
        std::set<std::string> roads;      // "level", "include", "record", "literal", "profile"
        std::uint64_t size = 0;
        std::uint64_t stored = 0;
        std::string source;               // where its bytes are, when not at its own path (the graphics defaults)
        std::shared_ptr<const std::vector<std::uint8_t>> bytes; // content the cook changed (a re-keyed water mask)
    };

    struct Licence
    {
        bool section = false;
        std::string text;                 // empty: the section names no licence
    };

    class Cooker
    {
    public:
        Cooker(Profile profile, std::filesystem::path out, bool check, std::vector<std::string> overrides)
            : profile_(std::move(profile)), out_(std::move(out)), check_(check), overrides_(std::move(overrides))
        {
        }

        int Run();

    private:
        void Error(std::string message)
        {
            LOG_ERROR(logging::LogCategory::Cook, "{}", message);
            errors_.push_back(std::move(message));
        }
        void Warning(std::string message)
        {
            if (!warned_.insert(message).second) { return; }
            LOG_WARNING(logging::LogCategory::Cook, "{}", message);
            warnings_.push_back(std::move(message));
        }

        bool IsFileCached(const std::string& path)
        {
            const auto found = isFile_.find(path);
            if (found != isFile_.end()) { return found->second; }
            const bool file = vfs::IsFile(path);
            isFile_.emplace(path, file);
            return file;
        }

        // `follow`: walk what it names. `level`: the shipped level whose walk this is ("" for the other roads).
        void Add(std::string_view raw, const std::string& referrer, bool follow, const std::string& level, const char* road)
        {
            const std::optional<std::string> normalised = Normalise(raw);
            if (!normalised)
            {
                Warning("not a content path: \"" + std::string(raw) + "\" (named by " + referrer + ")");
                return;
            }
            std::string path = *normalised;
            if (StartsWith(path, "shaders/"))
            {
                if (!IsFileCached(path)) { Error("missing shader source " + path + " (named by " + referrer + ")"); }
                return; // the shaders package carries the sources
            }
            if (follow && IsImage(path))
            {
                // The engine loads x.dds for x.png when it is there (Texture2D, H2); the image itself is then never read.
                if (const std::string dds = StemOf(path) + ".dds"; IsFileCached(dds)) { path = dds; }
            }
            if (NeverInBase(path)) { return; }
            if (!IsFileCached(path))
            {
                Error("missing file " + path + " (named by " + referrer + ")");
                return;
            }
            if (!vfs::IsLooseFile(path))
            {
                Error(path + " comes from a mounted package: cook from the files (--vfs=loose)");
                return;
            }
            auto [it, fresh] = files_.try_emplace(path);
            FileRecord& record = it->second;
            if (fresh)
            {
                const std::optional<vfs::Stat> stat = vfs::GetStat(path);
                record.size = stat ? stat->size : 0;
            }
            record.referencedBy.insert(referrer);
            record.roads.insert(road);
            if (!level.empty()) { record.levels.insert(level); }
            if (follow && walked_.insert(path).second) { Follow(path, level); }
        }

        void Follow(const std::string& path, const std::string& level)
        {
            if (EndsWith(path, ".json"))
            {
                WalkJson(path, level);
                if (StartsWith(path, "fonts/"))
                {
                    if (const std::string tga = StemOf(path) + ".tga"; IsFileCached(tga)) { Add(tga, path, false, level, "level"); }
                }
            }
            else if (EndsWith(path, ".mesh.bin"))
            {
                const std::string stem = path.substr(0, path.size() - std::strlen(".mesh.bin"));
                if (IsFileCached(stem + ".mesh.json")) { Add(stem + ".mesh.json", path, true, level, "level"); }
                if (IsFileCached(stem + ".watermask.bin")) { Add(stem + ".watermask.bin", path, false, level, "level"); }
            }
            else if (EndsWith(path, ".dds") && !EndsWith(path, "_spec.dds") && !EndsWith(path, "_diffuse.dds"))
            {
                // A sky imported with its IBL siblings (Skybox, F8): all three load or none.
                const std::string stem = StemOf(path);
                if (IsFileCached(stem + "_spec.dds") && IsFileCached(stem + "_diffuse.dds"))
                {
                    Add(stem + "_spec.dds", path, false, level, "level");
                    Add(stem + "_diffuse.dds", path, false, level, "level");
                    const std::size_t slash = path.find_last_of('/');
                    const std::string beside = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + "brdf_lut.dds";
                    Add(IsFileCached(beside) ? beside : std::string("textures/brdf_lut.dds"), path, false, level, "level");
                }
            }
        }

        void WalkJson(const std::string& path, const std::string& level)
        {
            std::string text;
            if (!vfs::ReadText(path, text))
            {
                Error("cannot read " + path);
                return;
            }
            const Json json = Json::parse(text, nullptr, false, true);
            if (json.is_discarded())
            {
                Error("not JSON: " + path);
                return;
            }
            if (EndsWith(path, ".mesh.json") && json.is_object() && json.value("waterMask", false))
            {
                // A hull that keeps the water out ships its bake: without it Shipping builds the field on a worker at load.
                const std::optional<std::string> geometry = Normalise(json.value("geometry", std::string()));
                const std::string bake = geometry ? ocean::watermask::BakePathOf(*geometry) : std::string();
                if (bake.empty() || !IsFileCached(bake))
                {
                    Error("water mask: " + path + " keeps the water out but has no bake" + (bake.empty() ? std::string() : " " + bake) +
                          " -- open a level with it in the editor once (it bakes) and cook again");
                }
            }
            Visit(json, std::string_view(), path, level);
        }

        void Visit(const Json& value, std::string_view key, const std::string& path, const std::string& level)
        {
            if (value.is_object())
            {
                for (auto it = value.begin(); it != value.end(); ++it)
                {
                    // `source`: the import's glTF, never read at runtime; `_comment`: prose.
                    if (it.key() == "source" || it.key() == "_comment") { continue; }
                    Visit(it.value(), it.key(), path, level);
                }
            }
            else if (value.is_array())
            {
                for (const Json& item : value) { Visit(item, key, path, level); }
            }
            else if (value.is_string())
            {
                const std::string& s = value.get_ref<const std::string&>();
                if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos)
                {
                    if (HasContentExtension(s)) { Add(s, path, true, level, "level"); }
                }
                else if (!s.empty() && s.size() < 128 && s.find_first_of(":*?\"<>|") == std::string::npos)
                {
                    const std::string material = "data/materials/" + s + ".json";
                    if (IsFileCached(material)) { Add(material, path, true, level, "level"); }
                }
            }
        }

        void ListRecursive(const std::string& dir, const std::function<void(const std::string&)>& file)
        {
            std::vector<std::string> dirs;
            vfs::List(dir, "*", [&](const vfs::ListEntry& e)
            {
                if (e.directory) { dirs.push_back(e.path); }
                else { file(e.path); }
            });
            std::sort(dirs.begin(), dirs.end());
            for (const std::string& d : dirs) { ListRecursive(d, file); }
        }

        void AddPattern(const std::string& pattern)
        {
            const std::optional<std::string> normalised = Normalise(pattern);
            if (!normalised)
            {
                Error("include: not a content pattern \"" + pattern + "\"");
                return;
            }
            const std::string p = *normalised;
            const std::size_t slash = p.find_last_of('/');
            std::string dir = slash == std::string::npos ? std::string() : p.substr(0, slash);
            const std::string name = slash == std::string::npos ? p : p.substr(slash + 1);
            if (name.find_first_of("*?") == std::string::npos && dir.find("**") == std::string::npos)
            {
                Add(p, "profile include", false, "", "include");
                return;
            }
            bool recursive = false;
            if (EndsWith(dir, "/**")) { dir.resize(dir.size() - 3); recursive = true; }
            else if (dir == "**") { dir.clear(); recursive = true; }
            std::vector<std::string> found;
            const auto take = [&](const std::string& path)
            {
                const std::size_t s = path.find_last_of('/');
                if (WildMatch(Lower(s == std::string::npos ? path : path.substr(s + 1)), Lower(name))) { found.push_back(path); }
            };
            if (recursive)
            {
                ListRecursive(dir, take);
            }
            else
            {
                vfs::List(dir, "*", [&](const vfs::ListEntry& e)
                {
                    if (!e.directory) { take(e.path); }
                });
            }
            if (found.empty()) { Warning("include \"" + pattern + "\" matches nothing"); }
            std::sort(found.begin(), found.end());
            for (const std::string& path : found) { Add(path, "profile include", false, "", "include"); }
        }

        void AddRecord()
        {
            std::string text;
            if (profile_.vfsRecord.empty()) { return; }
            if (!vfs::ReadText(profile_.vfsRecord, text))
            {
                Warning("no record list " + profile_.vfsRecord + " -- files named only in code may be missing (tools/dist/record_shaders.py)");
                return;
            }
            const std::set<std::string> shipped(profile_.levels.begin(), profile_.levels.end());
            std::size_t start = 0;
            while (start < text.size())
            {
                std::size_t end = text.find('\n', start);
                if (end == std::string::npos) { end = text.size(); }
                std::string line = text.substr(start, end - start);
                start = end + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) { line.pop_back(); }
                if (line.empty() || line[0] == '#') { continue; }
                recorded_.insert(line);
                if (StartsWith(line, "shaders/") || NeverInBase(line)) { continue; }
                // Only the levels the profile ships, and their bot memory.
                if (StartsWith(line, "data/levels/") && !shipped.count(line)) { continue; }
                if (StartsWith(line, "data/bot_memory/"))
                {
                    bool ours = false;
                    for (const std::string& level : profile_.levels)
                    {
                        ours = ours || StartsWith(line, "data/bot_memory/" + FileStem(level) + "/");
                    }
                    if (!ours) { continue; }
                }
                if (!IsFileCached(line))
                {
                    Warning("the record names " + line + ", which is not there now -- run the record again (tools/dist/record_shaders.py)");
                    continue;
                }
                Add(line, "record run", false, "", "record");
            }
        }

        // String literals of sources/ outside the editor that name a content file (§8.1 item 2).
        void AddLiterals()
        {
            std::vector<std::string> sources;
            ListRecursive("sources", [&](const std::string& path)
            {
                if (StartsWith(path, "sources/editor/")) { return; }
                const std::string ext = Extension(path);
                if (ext == ".cpp" || ext == ".h" || ext == ".hpp" || ext == ".inl") { sources.push_back(path); }
            });
            std::sort(sources.begin(), sources.end());
            for (const std::string& file : sources)
            {
                std::string text;
                if (!vfs::ReadText(file, text)) { continue; }
                ScanLiterals(file, text);
            }
        }

        void ScanLiterals(const std::string& file, const std::string& text)
        {
            int line = 1;
            const std::size_t n = text.size();
            for (std::size_t i = 0; i < n; ++i)
            {
                const char c = text[i];
                if (c == '\n') { ++line; continue; }
                if (c == '/' && i + 1 < n && text[i + 1] == '/')
                {
                    while (i < n && text[i] != '\n') { ++i; }
                    --i;
                    continue;
                }
                if (c == '/' && i + 1 < n && text[i + 1] == '*')
                {
                    for (i += 2; i + 1 < n && !(text[i] == '*' && text[i + 1] == '/'); ++i)
                    {
                        if (text[i] == '\n') { ++line; }
                    }
                    ++i;
                    continue;
                }
                if (c == '\'')
                {
                    // A character literal (or a digit separator: 1'000 -- then the next char is a digit and this is harmless).
                    for (++i; i < n && text[i] != '\'' && text[i] != '\n'; ++i)
                    {
                        if (text[i] == '\\') { ++i; }
                    }
                    continue;
                }
                if (c == 'R' && i + 1 < n && text[i + 1] == '"')
                {
                    // A raw string: R"delim( ... )delim"
                    const std::size_t open = text.find('(', i + 2);
                    if (open == std::string::npos) { break; }
                    const std::string close = ")" + text.substr(i + 2, open - (i + 2)) + "\"";
                    const std::size_t end = text.find(close, open);
                    if (end == std::string::npos) { break; }
                    for (std::size_t k = i; k < end; ++k)
                    {
                        if (text[k] == '\n') { ++line; }
                    }
                    i = end + close.size() - 1;
                    continue;
                }
                if (c == '"')
                {
                    std::string literal;
                    for (++i; i < n && text[i] != '"' && text[i] != '\n'; ++i)
                    {
                        if (text[i] == '\\' && i + 1 < n) { literal.push_back(text[i + 1]); ++i; continue; }
                        literal.push_back(text[i]);
                    }
                    Literal(file, line, literal);
                }
            }
        }

        void Literal(const std::string& file, int line, const std::string& literal)
        {
            if (literal.find('/') == std::string::npos || !HasContentExtension(literal)) { return; }
            if (literal.find_first_of("%{}*?<>| ") != std::string::npos) { return; }
            const std::optional<std::string> normalised = Normalise(literal);
            if (!normalised) { return; }
            const std::string& path = *normalised;
            constexpr std::string_view kRoots[] = { "data/", "models/", "textures/", "fonts/", "input/", "import_staging/" };
            bool content = false;
            for (const std::string_view root : kRoots) { content = content || StartsWith(path, root); }
            // Levels ship by the profile alone (code names demo.json as a default); bot memory with its level.
            if (!content || StartsWith(path, "data/levels/") || StartsWith(path, "data/bot_memory/")) { return; }
            const std::string where = file + ":" + std::to_string(line);
            std::string target = path;
            if (IsImage(path) && IsFileCached(StemOf(path) + ".dds")) { target = StemOf(path) + ".dds"; }
            if (!IsFileCached(target))
            {
                literalMissing_[path].insert(where);
                return;
            }
            Add(path, where, true, "", "literal");
            if (!recorded_.count(target) && !recorded_.count(path)) { literalOnly_[target].insert(where); }
        }

        void ReadCredits()
        {
            std::string text;
            if (!vfs::ReadText("CREDITS.md", text))
            {
                Warning("no CREDITS.md: every asset's licence is unknown");
                return;
            }
            std::string section;
            std::size_t start = 0;
            while (start < text.size())
            {
                std::size_t end = text.find('\n', start);
                if (end == std::string::npos) { end = text.size(); }
                std::string line = text.substr(start, end - start);
                start = end + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) { line.pop_back(); }
                if (StartsWith(line, "## "))
                {
                    section = line.substr(3);
                    if (const std::size_t cut = section.find(" ("); cut != std::string::npos) { section.resize(cut); }
                    licences_[section].section = true;
                    continue;
                }
                if (section.empty()) { continue; }
                std::string licence;
                if (const std::size_t at = line.find("license type:"); at != std::string::npos) { licence = line.substr(at + 13); }
                else if (StartsWith(line, "License:")) { licence = line.substr(8); }
                else { continue; }
                const std::size_t first = licence.find_first_not_of(" \t");
                licence = first == std::string::npos ? std::string() : licence.substr(first);
                if (const std::size_t cut = licence.find(" ("); cut != std::string::npos) { licence.resize(cut); }
                if (licences_[section].text.empty()) { licences_[section].text = licence; }
            }
        }

        static std::string AssetOf(const std::string& path)
        {
            for (const std::string_view root : { std::string_view("models/"), std::string_view("import_staging/") })
            {
                if (StartsWith(path, root))
                {
                    const std::size_t slash = path.find('/', root.size());
                    return slash == std::string::npos ? std::string() : path.substr(0, slash);
                }
            }
            return {};
        }

        void CheckAssets()
        {
            struct Asset
            {
                std::uint64_t files = 0;
                std::uint64_t bytes = 0;
            };
            std::map<std::string, Asset> assets;
            for (const auto& [path, record] : files_)
            {
                const std::string asset = AssetOf(path);
                if (asset.empty()) { continue; }
                assets[asset].files += 1;
                assets[asset].bytes += record.size;
            }
            for (const auto& [asset, info] : assets)
            {
                const std::string name = asset.substr(asset.find('/') + 1);
                const bool staging = StartsWith(asset, "import_staging/");
                Json entry = { { "asset", asset }, { "files", info.files }, { "bytes", info.bytes } };
                const auto found = licences_.find(name);
                if (found != licences_.end() && !found->second.text.empty())
                {
                    entry["licence"] = found->second.text;
                    if (found->second.text.find("NC") != std::string::npos)
                    {
                        Warning("non-commercial licence: " + asset + " (" + found->second.text + ") -- no sale with it");
                    }
                }
                else
                {
                    const std::string why = found == licences_.end() ? "no section in CREDITS.md" : "CREDITS.md names no licence";
                    entry["licence"] = nullptr;
                    entry["licenceProblem"] = why;
                    const std::string message = "no known licence: " + asset + " (" + why + ")";
                    if (profile_.strictLicences) { Error(message); }
                    else { Warning(message); }
                }
                if (staging)
                {
                    entry["staging"] = true;
                    Warning("a shipped asset lives in import_staging/: " + asset + " (" + std::to_string(info.files) +
                            " files) -- move it under models/ or textures/");
                }
                assetsReport_.push_back(std::move(entry));
            }
        }

        void RekeyWaterMasks()
        {
            for (auto& [path, record] : files_)
            {
                if (!EndsWith(path, ".watermask.bin")) { continue; }
                const std::string geometry = path.substr(0, path.size() - std::strlen(".watermask.bin")) + ".mesh.bin";
                Json entry = { { "bake", path }, { "geometry", geometry } };
                std::vector<std::uint8_t> bake, mesh;
                const std::optional<vfs::Stat> stat = vfs::GetStat(geometry);
                std::string why;
                if (!files_.count(geometry)) { why = "its geometry is not shipped"; }
                else if (!stat || !vfs::ReadAll(geometry, mesh)) { why = "cannot read the geometry"; }
                else if (!vfs::ReadAll(path, bake)) { why = "cannot read the bake"; }
                else
                {
                    const std::uint64_t hash = vfs::pak::ContentHash(mesh.data(), mesh.size());
                    std::vector<std::uint8_t> original = bake;
                    if (ocean::watermask::RekeyBakeForPackage(bake, stat->size, stat->version, hash, why))
                    {
                        record.bytes = std::make_shared<const std::vector<std::uint8_t>>(std::move(bake));
                        entry["status"] = "re-keyed";
                        waterMasksReport_.push_back(std::move(entry));
                        continue;
                    }
                    // Made for another write of the geometry (a copied tree, a checkout: other file times, the same
                    // bytes) -- the field is built again here with the draft and glass slots the frame keyed it with, as
                    // the editor's Collect builds it, and keyed for the package. Deterministic: the same geometry gives
                    // the same bytes as the editor's bake.
                    ocean::WaterMaskBakeKey stored;
                    std::string readWhy;
                    if (why == "made from another bake of the geometry" &&
                        ocean::watermask::ReadBakeKey(original.data(), original.size(), stored, readWhy))
                    {
                        const std::shared_ptr<const ocean::WaterMaskField> field =
                            ocean::watermask::BuildField(geometry, stored.glassSlots, stored.waterline, stored.draftOverLowest);
                        if (field)
                        {
                            ocean::WaterMaskBakeKey packaged = stored;
                            packaged.geometryBytes = stat->size;
                            packaged.geometryTime = static_cast<std::int64_t>(hash);
                            record.bytes = std::make_shared<const std::vector<std::uint8_t>>(ocean::watermask::SerializeField(*field, packaged));
                            entry["status"] = "rebuilt (the bake was made for another write of the geometry)";
                            Warning("water mask " + path + " was made for another write of its geometry: rebuilt by the cook "
                                    "(open a level with it in the editor once to bake it again)");
                            waterMasksReport_.push_back(std::move(entry));
                            continue;
                        }
                        why = "made for another write of the geometry, and building it again failed";
                    }
                }
                entry["status"] = why;
                waterMasksReport_.push_back(std::move(entry));
                Error("water mask " + path + " does not serve its geometry: " + why +
                      " -- open a level with it in the editor once (it rebakes) and cook again");
            }
        }

        std::optional<vfs::pak::Key> LoadKey()
        {
            std::filesystem::path keyFile = keyFile_;
            if (keyFile.empty())
            {
                wchar_t profileDir[MAX_PATH] = {};
                if (GetEnvironmentVariableW(L"USERPROFILE", profileDir, MAX_PATH) == 0)
                {
                    Error("encrypt: no --pak-key and no USERPROFILE to find the default key in");
                    return std::nullopt;
                }
                keyFile = std::filesystem::path(profileDir) / L".test_cube" / L"pak_key.json";
            }
            std::string why;
            std::optional<vfs::pak::Key> key = vfs::pak::LoadKeyFile(keyFile, why);
            if (!key)
            {
                Error("encrypt: no key " + vfs::Narrow(keyFile.wstring()) + " (" + why + ") -- make one with --pak-keygen=<key.json>"
                      " and keep a copy: a lost key leaves its packages unreadable");
            }
            keyPath_ = vfs::Narrow(keyFile.wstring());
            return key;
        }

        // Opens what was built, checks every file, and fills each file's stored size. False on any damage.
        bool VerifyPackage(const std::filesystem::path& file, const vfs::pak::Key* key, Json& entry, bool fillStored)
        {
            const auto started = Clock::now();
            vfs::pak::KeyRing ring;
            if (key) { ring.Add(*key); }
            std::string error;
            const std::unique_ptr<vfs::pak::Package> package = vfs::pak::Package::Open(file, error, key ? &ring : nullptr);
            if (!package)
            {
                Error("cannot open the package just built " + vfs::Narrow(file.wstring()) + ": " + error);
                return false;
            }
            std::size_t damaged = 0;
            for (std::size_t i = 0; i < package->EntryCount(); ++i)
            {
                const int index = static_cast<int>(i);
                if (!package->Verify(index, error))
                {
                    ++damaged;
                    Error("package " + package->Name() + ": " + error);
                }
                if (fillStored)
                {
                    const vfs::pak::Package::EntryInfo info = package->Entry(index);
                    std::uint64_t stored = 0;
                    for (std::uint32_t b = 0; b < info.blocks; ++b)
                    {
                        std::uint32_t bytes = 0;
                        (void)package->StoredBytes(index, b, bytes);
                        stored += bytes;
                    }
                    if (const auto found = files_.find(std::string(info.path)); found != files_.end()) { found->second.stored = stored; }
                }
            }
            char contentId[24];
            std::snprintf(contentId, sizeof(contentId), "%016llx", static_cast<unsigned long long>(package->ContentId()));
            entry["contentId"] = contentId;
            entry["encrypted"] = package->Encrypted();
            if (package->Encrypted()) { entry["keyGuid"] = package->KeyGuid(); }
            entry["verified"] = damaged == 0;
            entry["verifySeconds"] = std::chrono::duration<double>(Clock::now() - started).count();
            return damaged == 0;
        }

        static double EstimateRatio(const std::string& path)
        {
            // Measured on the tree (docs/distribution_build_plan.md §0): DDS 62 %, .mesh.bin 45 %, JSON ~16 %; PNG/JPEG stay.
            const std::string ext = Extension(path);
            if (ext == ".dds") { return 0.62; }
            if (ext == ".bin") { return 0.45; }
            if (ext == ".json") { return 0.16; }
            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg") { return 1.0; }
            return 0.6;
        }

        void WriteReport(double seconds)
        {
            Json report;
            report["profile"] = profile_.file;
            report["product"] = profile_.product;
            report["version"] = profile_.version;
            report["mode"] = check_ ? "check" : "content";
            report["overrides"] = overrides_;
            report["levels"] = profile_.levels;
            report["bootLevel"] = profile_.bootLevel;
            report["encrypt"] = profile_.encrypt;
            if (profile_.encrypt) { report["key"] = keyPath_; }
            report["errors"] = errors_;
            report["warnings"] = warnings_;

            std::uint64_t rawBytes = 0, storedBytes = 0, estimate = 0;
            Json files = Json::array();
            std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> byLevel; // files, bytes
            std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> sharedByLevel;
            for (const auto& [path, record] : files_)
            {
                rawBytes += record.size;
                storedBytes += record.stored;
                estimate += static_cast<std::uint64_t>(static_cast<double>(record.size) * EstimateRatio(path));
                Json f = { { "path", path }, { "size", record.size }, { "referencedBy", record.referencedBy },
                           { "roads", record.roads } };
                if (!check_) { f["stored"] = record.stored; }
                if (!record.levels.empty()) { f["levels"] = record.levels; }
                if (!record.source.empty()) { f["source"] = record.source; }
                if (const std::string asset = AssetOf(path); !asset.empty()) { f["asset"] = asset; }
                files.push_back(std::move(f));
                for (const std::string& level : record.levels)
                {
                    byLevel[level].first += 1;
                    byLevel[level].second += record.size;
                    if (record.levels.size() > 1)
                    {
                        sharedByLevel[level].first += 1;
                        sharedByLevel[level].second += record.size;
                    }
                }
            }
            Json levels = Json::array();
            for (const std::string& level : profile_.levels)
            {
                levels.push_back({ { "level", level }, { "files", byLevel[level].first }, { "bytes", byLevel[level].second },
                                   { "sharedFiles", sharedByLevel[level].first }, { "sharedBytes", sharedByLevel[level].second } });
            }
            Json literalOnly = Json::object();
            for (const auto& [path, where] : literalOnly_) { literalOnly[path] = where; }
            Json literalMissing = Json::object();
            for (const auto& [path, where] : literalMissing_) { literalMissing[path] = where; }
            std::uint64_t recordOnly = 0;
            for (const auto& [path, record] : files_)
            {
                if (record.roads.size() == 1 && record.roads.count("record")) { ++recordOnly; }
            }

            report["totals"] = { { "files", files_.size() }, { "rawBytes", rawBytes }, { "storedBytes", storedBytes },
                                 { "estimatedBytes", estimate }, { "filesOnlyInTheRecord", recordOnly } };
            report["levelStats"] = levels;
            report["assets"] = assetsReport_;
            report["waterMasks"] = waterMasksReport_;
            report["literalOnly"] = literalOnly;      // named in code, opened by no record run (a warning each)
            report["literalMissing"] = literalMissing; // named in code, not on disk (optional files, fallbacks)
            report["packages"] = packagesReport_;
            report["shaders"] = shadersReport_;
            report["files"] = files;
            report["seconds"] = seconds;

            std::error_code ec;
            std::filesystem::create_directories(out_, ec);
            const std::filesystem::path file = out_ / L"cook_report.json";
            const std::string text = report.dump(2) + "\n";
            std::FILE* f = nullptr;
            if (_wfopen_s(&f, file.c_str(), L"wbN") != 0 || !f) // vfs-ok: the cook writes its report, an artifact of the build
            {
                LOG_ERROR(logging::LogCategory::Cook, "cook: cannot write {}", vfs::Narrow(file.wstring()));
                return;
            }
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
            LOG_INFO(logging::LogCategory::Cook, "cook: report {} -- {} files, {:.1f} MB before compression{}, {} errors, {} warnings",
                     vfs::Narrow(file.wstring()), files_.size(), rawBytes / 1048576.0,
                     check_ ? " (about " + std::to_string(estimate / 1048576) + " MB after)" :
                              ", " + std::to_string(storedBytes / 1048576) + " MB stored",
                     errors_.size(), warnings_.size());
        }

    public:
        std::string keyFile_;

    private:
        Profile profile_;
        std::filesystem::path out_;
        bool check_ = false;
        std::vector<std::string> overrides_;
        std::map<std::string, FileRecord> files_;
        std::set<std::string> walked_;
        std::map<std::string, bool> isFile_;
        std::set<std::string> recorded_;
        std::map<std::string, std::set<std::string>> literalOnly_;
        std::map<std::string, std::set<std::string>> literalMissing_;
        std::map<std::string, Licence> licences_;
        std::vector<std::string> errors_;
        std::vector<std::string> warnings_;
        std::set<std::string> warned_;
        Json assetsReport_ = Json::array();
        Json waterMasksReport_ = Json::array();
        Json packagesReport_ = Json::array();
        Json shadersReport_ = Json::object();
        std::string keyPath_;
    };

    int Cooker::Run()
    {
        const auto started = Clock::now();
        // The walk asks for thousands of paths that are not there (every bare name as a material, every sky's siblings):
        // those are answers, not misses -- the VFS's Debug line for each would bury the cook's own.
        logging::SetCategoryThreshold(logging::LogCategory::Vfs, logging::LogLevel::Info);
        LOG_INFO(logging::LogCategory::Cook, "cook: profile {}, {} level(s), {} -> {}", profile_.file, profile_.levels.size(),
                 check_ ? "check" : "content", vfs::Narrow(out_.wstring()));

        // The profile's own rules (§8.0).
        if (profile_.levels.empty()) { Error("the profile ships no level"); }
        if (!profile_.bootLevel.empty() &&
            std::find(profile_.levels.begin(), profile_.levels.end(), profile_.bootLevel) == profile_.levels.end())
        {
            Error("bootLevel " + profile_.bootLevel + " is not one of the shipped levels");
        }

        // 1. The levels: one walk each, so every file knows which levels reach it.
        for (const std::string& level : profile_.levels)
        {
            walked_.clear();
            Add(level, "profile levels", true, level, "level");
            const std::string memory = "data/bot_memory/" + FileStem(level);
            if (vfs::IsDirectory(memory))
            {
                vfs::List(memory, "*.json", [&](const vfs::ListEntry& e)
                {
                    if (!e.directory) { Add(e.path, "bot memory of " + level, false, level, "level"); }
                });
            }
        }
        walked_.clear();
        // 2. The profile's include, as it is.
        for (const std::string& pattern : profile_.include) { AddPattern(pattern); }
        // 3. The record runs' list; 4. the literals in code (the record first: a literal it opened is no warning).
        AddRecord();
        AddLiterals();
        for (const auto& [path, where] : literalOnly_)
        {
            Warning("named only in code, opened by no record run: " + path + " (" + *where.begin() + ")");
        }
        // 5. What a shipped build starts (Shipping reads it: main.cpp), and the curated settings.
        {
            const Json boot = { { "product", profile_.product }, { "version", profile_.version },
                                { "bootLevel", profile_.bootLevel }, { "startMode", profile_.startMode } };
            const std::string text = boot.dump(2) + "\n";
            FileRecord& record = files_["config/boot.json"];
            record.bytes = std::make_shared<const std::vector<std::uint8_t>>(text.begin(), text.end());
            record.size = text.size();
            record.referencedBy.insert("profile bootLevel / startMode");
            record.roads.insert("profile");
        }
        if (!profile_.graphicsDefaults.empty())
        {
            const std::optional<vfs::Stat> stat = vfs::GetStat(profile_.graphicsDefaults);
            if (!stat || stat->directory) { Error("graphicsDefaults: no file " + profile_.graphicsDefaults); }
            else
            {
                FileRecord& record = files_["config/graphics_defaults.json"];
                record.size = stat->size;
                record.source = profile_.graphicsDefaults;
                record.referencedBy.insert("profile graphicsDefaults");
                record.roads.insert("profile");
            }
        }

        ReadCredits();
        CheckAssets();
        RekeyWaterMasks();

        std::optional<vfs::pak::Key> key;
        if (profile_.encrypt && !check_) { key = LoadKey(); }
        if (!check_ && errors_.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(out_, ec);
            vfs::pak::BuildOptions options;
            options.name = "base";
            options.level = profile_.level;
            options.key = key ? &*key : nullptr;
            std::vector<vfs::pak::BuildFile> files;
            files.reserve(files_.size());
            for (const auto& [path, record] : files_)
            {
                vfs::pak::BuildFile file;
                file.path = path;
                file.source = vfs::OsPath(record.source.empty() ? path : record.source);
                file.bytes = record.bytes;
                files.push_back(std::move(file));
            }
            const std::filesystem::path basePath = out_ / L"base.tcpak";
            const vfs::pak::BuildResult built = vfs::pak::Build(basePath, std::move(files), options);
            Json entry = { { "name", "base" }, { "file", "base.tcpak" } };
            if (!built.ok) { Error("base.tcpak: " + built.error); }
            else
            {
                entry["files"] = built.files;
                entry["rawBytes"] = built.rawBytes;
                entry["packageBytes"] = built.packageBytes;
                entry["blocks"] = built.blocks;
                entry["storedBlocks"] = built.storedBlocks;
                entry["buildSeconds"] = built.seconds;
                VerifyPackage(basePath, options.key, entry, true);
                LOG_INFO(logging::LogCategory::Cook, "cook: base.tcpak -- {} files, {:.1f} -> {:.1f} MB in {:.1f} s", built.files,
                         built.rawBytes / 1048576.0, built.packageBytes / 1048576.0, built.seconds);
            }
            packagesReport_.push_back(std::move(entry));

            // The shaders: the recorded list at the fixed target, Release flags (§5).
            if (!profile_.shaderModel.empty())
            {
                const std::size_t dot = profile_.shaderModel.find_first_of("._");
                const unsigned major = static_cast<unsigned>(std::atoi(profile_.shaderModel.c_str()));
                const unsigned minor = dot == std::string::npos ? 0u : static_cast<unsigned>(std::atoi(profile_.shaderModel.c_str() + dot + 1));
                Material::SetShaderModelCap((major << 4) | (minor & 0xFu));
            }
            const std::filesystem::path shadersPath = out_ / L"shaders.tcpak";
            Material::ShaderCookResult shaders;
            Material::CookShaders(vfs::Widen(profile_.shaderList), shadersPath.wstring(), options.key, profile_.level,
                                  profile_.shaderSources, &shaders);
            shadersReport_ = { { "list", profile_.shaderList }, { "target", profile_.shaderModel }, { "records", shaders.records },
                               { "permutations", shaders.permutations }, { "failed", shaders.failed },
                               { "compileSeconds", shaders.compileSeconds }, { "sources", profile_.shaderSources } };
            if (shaders.failed != 0) { Error(std::to_string(shaders.failed) + " shader permutation(s) did not compile (the session log names them)"); }
            Json shaderEntry = { { "name", "shaders" }, { "file", "shaders.tcpak" } };
            if (!shaders.built) { Error("shaders.tcpak: " + shaders.error); }
            else
            {
                shaderEntry["files"] = shaders.files;
                shaderEntry["packageBytes"] = shaders.packageBytes;
                VerifyPackage(shadersPath, options.key, shaderEntry, false);
            }
            packagesReport_.push_back(std::move(shaderEntry));
        }
        else if (!check_)
        {
            LOG_ERROR(logging::LogCategory::Cook, "cook: {} error(s) -- nothing built", errors_.size());
        }

        WriteReport(std::chrono::duration<double>(Clock::now() - started).count());
        return static_cast<int>(std::min<std::size_t>(errors_.size(), 255));
    }

    std::optional<Profile> ReadProfile(const std::string& file, std::string& error)
    {
        std::string text;
        if (!vfs::ReadText(file, text))
        {
            error = "cannot read the profile " + file;
            return std::nullopt;
        }
        const Json json = Json::parse(text, nullptr, false, true);
        if (json.is_discarded() || !json.is_object())
        {
            error = "the profile " + file + " is not a JSON object";
            return std::nullopt;
        }
        Profile p;
        p.file = file;
        p.product = json.value("product", p.product);
        p.version = json.value("version", p.version);
        p.levels = json.value("levels", std::vector<std::string>{});
        p.bootLevel = json.value("bootLevel", std::string());
        p.startMode = json.value("startMode", p.startMode);
        if (p.startMode != "play" && p.startMode != "free")
        {
            error = "startMode \"" + p.startMode + "\": play or free";
            return std::nullopt;
        }
        p.include = json.value("include", std::vector<std::string>{});
        p.graphicsDefaults = json.value("graphicsDefaults", std::string());
        p.shaderList = json.value("shaderList", p.shaderList);
        p.vfsRecord = json.value("vfsRecord", p.vfsRecord);
        p.shaderModel = json.value("shaderModel", p.shaderModel);
        p.encrypt = json.value("encrypt", false);
        p.shaderSources = json.value("shaderSources", true);
        p.strictLicences = json.value("strictLicences", false);
        const std::string compression = json.value("compression", std::string("zstd:19"));
        if (StartsWith(compression, "zstd:")) { p.level = std::clamp(std::atoi(compression.c_str() + 5), 1, 22); }
        else if (compression != "zstd")
        {
            error = "compression \"" + compression + "\": only zstd[:level]";
            return std::nullopt;
        }
        for (std::string& level : p.levels)
        {
            if (const std::optional<std::string> n = Normalise(level)) { level = *n; }
        }
        if (const std::optional<std::string> n = Normalise(p.bootLevel)) { p.bootLevel = *n; }
        return p;
    }
} // namespace

int RunCook(const char* cmd)
{
    if (!cmd) { return -1; }
    const std::string profileFile = ArgValue(cmd, "--cook=");
    if (profileFile.empty()) { return -1; }
    const std::string out = ArgValue(cmd, "--cook-out=");
    const bool check = HasFlag(cmd, "--cook-check");
    if (out.empty())
    {
        LOG_ERROR(logging::LogCategory::Cook, "--cook=<profile.json> needs --cook-out=<folder>");
        return 1;
    }
    std::string error;
    std::optional<Profile> profile = ReadProfile(profileFile, error);
    if (!profile)
    {
        LOG_ERROR(logging::LogCategory::Cook, "cook: {}", error);
        return 1;
    }
    // One build's overrides of the profile, named in the report (§8.0).
    std::vector<std::string> overrides;
    if (const std::string levels = ArgValue(cmd, "--levels="); !levels.empty())
    {
        profile->levels.clear();
        for (const std::string& level : SplitComma(levels))
        {
            if (const std::optional<std::string> n = Normalise(level)) { profile->levels.push_back(*n); }
        }
        if (std::find(profile->levels.begin(), profile->levels.end(), profile->bootLevel) == profile->levels.end() &&
            !profile->levels.empty())
        {
            profile->bootLevel = profile->levels.front();
        }
        overrides.push_back("--levels=" + levels);
    }
    if (const std::string boot = ArgValue(cmd, "--boot-level="); !boot.empty())
    {
        if (const std::optional<std::string> n = Normalise(boot)) { profile->bootLevel = *n; }
        overrides.push_back("--boot-level=" + boot);
    }
    if (const std::string version = ArgValue(cmd, "--version="); !version.empty())
    {
        profile->version = version;
        overrides.push_back("--version=" + version);
    }
    if (HasFlag(cmd, "--no-encrypt"))
    {
        profile->encrypt = false;
        overrides.push_back("--no-encrypt");
    }
    Cooker cooker(std::move(*profile), std::filesystem::path(vfs::Widen(out)), check, std::move(overrides));
    cooker.keyFile_ = ArgValue(cmd, "--pak-key=");
    return cooker.Run();
}

} // namespace dist
