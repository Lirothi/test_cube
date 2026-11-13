#pragma once

// The VFS's mount table and the lookups against one (internal: Vfs.cpp, VfsTools.cpp and the self-test, which builds a
// table of its own instead of touching the process's).

#include "core/vfs/Pak.h"
#include "core/vfs/Vfs.h"

#include <memory>
#include <optional>
#include <vector>

namespace vfs::detail
{
    struct Mounts
    {
        bool loose = true;          // the loose files under ContentRoot()
        bool looseFirst = true;     // --vfs=both: loose files over the packages
        std::vector<std::unique_ptr<pak::Package>> packages; // highest priority first
    };

    // The process's table: loose only until Configure says otherwise.
    Mounts& Global();
    // The keys packages open with: the one the build embedded (Shipping), and --pak-key's.
    pak::KeyRing& Keys();

    std::optional<Stat> StatIn(const Mounts& mounts, const Path& path);
    File OpenIn(const Mounts& mounts, const Path& path);
    void ListIn(const Mounts& mounts, const Path& directory, std::string_view pattern,
                const std::function<void(const ListEntry&)>& visit);
    // Which mount serves the path: true when a loose file (or an absolute path) does.
    bool LooseServesIn(const Mounts& mounts, const Path& path);

    // Highest priority first, then by name: the order Mounts::packages keeps.
    void SortPackages(std::vector<std::unique_ptr<pak::Package>>& packages);
}
