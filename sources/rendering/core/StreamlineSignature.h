#pragma once

#include <filesystem>
#include <string>

namespace render
{
    // NVIDIA's signature on the Streamline interposer a shipped build is about to load (docs/distribution_build_plan.md §7;
    // sl_security.h's verifyEmbeddedSignature, WinVerifyTrust). Shipping delay-loads sl.interposer.dll, so nothing of it has
    // run yet when this asks; a file that is not NVIDIA's (or is missing) is not loaded at all -- the game runs without
    // DLSS. The interposer itself checks the signatures of the plugins it loads (a production build).
    [[nodiscard]] bool VerifyStreamlineSignature(const std::filesystem::path& interposer, std::string& why);
}
