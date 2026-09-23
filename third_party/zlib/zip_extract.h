#pragma once
// Unpack a .zip into a folder: zlib 1.3.1 + its contrib/minizip reader, wrapped so the engine
// never needs zlib's include paths. Used by the editor's Import window, which unpacks archives
// dropped into import_staging/ (a download goes in exactly as it came).
#include <cstddef>
#include <filesystem>
#include <string>

namespace thirdparty
{
    // Extracts every entry under `destDir` (created if missing). An entry that is absolute or
    // climbs out with ".." is refused, and so is the whole archive with it -- a zip that tries
    // that is not an asset download. False and a reason on any failure; `filesOut` = files written.
    bool ExtractZip(const std::filesystem::path& zipPath, const std::filesystem::path& destDir,
        std::string& error, std::size_t* filesOut = nullptr);
}
