#pragma once

#include <cstddef>
#include <string>
#include <vector>

// The set of files ONE import just (re)wrote, published so every cache keyed on a path can drop
// exactly those entries instead of waiting for a process restart.
//
// WHY A PUBLISHED SET RATHER THAN A WRITE TIME IN THE KEY: a write time makes every cache LOOKUP
// stat the file -- a syscall on the texture-load path -- and widens a key that is hashed on every
// material build. Eviction happens once per import, so paying the whole cost there is free, and the
// hot path keeps the key it already has. The trade is that the import side has to be honest about
// what it wrote, which is why the outputs are collected where they are produced (ImportPanel) and
// handed to the caches, rather than rediscovered by scanning afterwards.
namespace assets {

// Lowercase, forward slashes, no leading "./". Two paths naming the same file always normalize to
// the same string, so a preset's "Textures\\Sand\\x.DDS" and the importer's "textures/sand/x.dds"
// compare equal.
std::string NormalizeAssetPath(const std::string& path);
std::string NormalizeAssetPath(const std::wstring& path);

class InvalidationSet
{
public:
    // `path` is an engine-relative output path, e.g. "textures/sand/sand_albedo.dds".
    void Add(const std::string& path);
    void Add(const std::wstring& path);

    bool Empty() const { return paths_.empty(); }
    std::size_t Size() const { return paths_.size(); }

    // True when `candidate` names one of the rewritten files. Two allowances, both of which came
    // from real content:
    //  * SUFFIX matching on a separator boundary, so an absolute path ("D:/proj/textures/...") and
    //    the engine-relative one the importer publishes agree.
    //  * the ".dds" sibling, because a material may name the SOURCE image (x.png) while the loader
    //    resolves to the imported x.dds -- the file the import actually rewrote.
    bool Contains(const std::string& candidate) const;
    bool Contains(const std::wstring& candidate) const;

private:
    bool MatchesAny_(const std::string& normalized) const;

    // A vector, not a set: an import publishes tens of files and matching is by suffix, which a
    // hash lookup cannot answer anyway.
    std::vector<std::string> paths_;
};

} // namespace assets
