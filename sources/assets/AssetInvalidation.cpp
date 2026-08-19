#include "assets/AssetInvalidation.h"

#include <algorithm>
#include <cctype>

namespace assets {
namespace {

char ToLowerAscii(char c)
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// Replace the extension with ".dds", or return an empty string when there is nothing to replace.
// The dot has to come after the last separator, otherwise "models/v1.2/x" loses half its folder.
std::string DdsSibling(const std::string& normalized)
{
    const std::size_t dot = normalized.find_last_of('.');
    if (dot == std::string::npos) { return {}; }
    const std::size_t slash = normalized.find_last_of('/');
    if (slash != std::string::npos && dot < slash) { return {}; }
    return normalized.substr(0, dot) + ".dds";
}

} // namespace

std::string NormalizeAssetPath(const std::string& path)
{
    std::string out;
    out.reserve(path.size());
    for (const char c : path)
    {
        out.push_back(c == '\\' ? '/' : ToLowerAscii(c));
    }
    // Leading "./" is noise the suffix match would otherwise trip over.
    std::size_t start = 0;
    while (out.compare(start, 2, "./") == 0) { start += 2; }
    return start == 0 ? out : out.substr(start);
}

std::string NormalizeAssetPath(const std::wstring& path)
{
    // Asset paths are ASCII by construction (the engine stores them in JSON as narrow strings and
    // widens them char-by-char), so the narrow round-trip is lossless here.
    std::string narrow;
    narrow.reserve(path.size());
    for (const wchar_t c : path)
    {
        narrow.push_back(c < 128 ? static_cast<char>(c) : '?');
    }
    return NormalizeAssetPath(narrow);
}

void InvalidationSet::Add(const std::string& path)
{
    std::string n = NormalizeAssetPath(path);
    if (n.empty()) { return; }
    if (std::find(paths_.begin(), paths_.end(), n) == paths_.end())
    {
        paths_.push_back(std::move(n));
    }
}

void InvalidationSet::Add(const std::wstring& path)
{
    Add(NormalizeAssetPath(path));
}

bool InvalidationSet::MatchesAny_(const std::string& c) const
{
    for (const std::string& p : paths_)
    {
        if (c.size() < p.size()) { continue; }
        if (c.size() == p.size())
        {
            if (c == p) { return true; }
            continue;
        }
        // Suffix, but only on a separator boundary: "textures/sand/x.dds" must not match
        // "textures/oldsand/x.dds".
        if (c[c.size() - p.size() - 1] == '/' &&
            c.compare(c.size() - p.size(), p.size(), p) == 0)
        {
            return true;
        }
    }
    return false;
}

bool InvalidationSet::Contains(const std::string& candidate) const
{
    if (paths_.empty()) { return false; }
    const std::string c = NormalizeAssetPath(candidate);
    if (MatchesAny_(c)) { return true; }
    const std::string dds = DdsSibling(c);
    return !dds.empty() && dds != c && MatchesAny_(dds);
}

bool InvalidationSet::Contains(const std::wstring& candidate) const
{
    if (paths_.empty()) { return false; }
    return Contains(NormalizeAssetPath(candidate));
}

} // namespace assets
