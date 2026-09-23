#include "zip_extract.h"

#include <fstream>
#include <string>
#include <vector>

extern "C" {
#include "contrib/minizip/unzip.h"
#include "contrib/minizip/iowin32.h"
}

namespace thirdparty
{
    bool ExtractZip(const std::filesystem::path& zipPath, const std::filesystem::path& destDir,
        std::string& error, std::size_t* filesOut)
    {
        namespace fs = std::filesystem;
        if (filesOut) { *filesOut = 0; }

        // Wide paths through iowin32: the plain ioapi opens with fopen and a char path.
        zlib_filefunc64_def io{};
        fill_win32_filefunc64W(&io);
        const std::wstring wide = zipPath.wstring();
        unzFile zip = unzOpen2_64(wide.c_str(), &io);
        if (!zip)
        {
            error = "not a readable zip";
            return false;
        }

        std::error_code ec;
        fs::create_directories(destDir, ec);
        std::vector<char> buffer(1u << 16);
        bool ok = true;
        for (int rc = unzGoToFirstFile(zip); rc == UNZ_OK; rc = unzGoToNextFile(zip))
        {
            unz_file_info64 info{};
            char name[1024] = {};
            if (unzGetCurrentFileInfo64(zip, &info, name, sizeof(name), nullptr, 0, nullptr, 0) != UNZ_OK)
            {
                error = "unreadable entry header";
                ok = false;
                break;
            }
            // Names are UTF-8 in every archive a browser download produces (flag bit 11); a
            // legacy code-page name still gets through for plain ASCII, which is what matters.
            const std::string entry(name);
            const fs::path rel = fs::path(std::u8string(entry.begin(), entry.end())).lexically_normal();
            if (rel.empty() || rel.is_absolute() || rel.has_root_name() ||
                (!rel.empty() && *rel.begin() == ".."))
            {
                error = "entry '" + entry + "' points outside the folder";
                ok = false;
                break;
            }
            const fs::path out = destDir / rel;
            if (!entry.empty() && (entry.back() == '/' || entry.back() == '\\'))
            {
                fs::create_directories(out, ec);
                continue;
            }
            fs::create_directories(out.parent_path(), ec);
            if (unzOpenCurrentFile(zip) != UNZ_OK)
            {
                error = "cannot open entry '" + entry + "' (encrypted?)";
                ok = false;
                break;
            }
            std::ofstream file(out, std::ios::binary | std::ios::trunc);
            int got = 0;
            while (file && (got = unzReadCurrentFile(zip, buffer.data(), static_cast<unsigned>(buffer.size()))) > 0)
            {
                file.write(buffer.data(), got);
            }
            const bool crcOk = unzCloseCurrentFile(zip) == UNZ_OK; // UNZ_CRCERROR on a damaged entry
            if (got < 0 || !file || !crcOk)
            {
                error = "failed reading entry '" + entry + "'" + (crcOk ? "" : " (CRC mismatch)");
                ok = false;
                break;
            }
            if (filesOut) { ++*filesOut; }
        }
        unzClose(zip);
        return ok;
    }
}
