// The one translation unit that includes sl_security.h: it defines functions and statics, and #defines UNICODE.
#include "rendering/core/StreamlineSignature.h"

#include "streamline/include/sl_security.h"

namespace render
{
    bool VerifyStreamlineSignature(const std::filesystem::path& interposer, std::string& why)
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(interposer, ec)) // vfs-ok: a DLL beside the exe, not content
        {
            why = "missing";
            return false;
        }
        if (!sl::security::verifyEmbeddedSignature(interposer.c_str()))
        {
            why = "not signed by NVIDIA (or the signature does not verify)";
            return false;
        }
        return true;
    }
}
