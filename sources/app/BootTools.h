#pragma once

#include <filesystem>
#include <string>
#include <vector>

// WinMain's tools, in the order it calls them (app/BootTools.cpp, architecture plan R3.2d): each answers the exit code
// of the tool the command line names, or -1 to go on.
namespace boot
{
int RunEarlyTools(const char* lpCmdLine);       // --help, --crash-selftest
#if TC_PACKAGED
int PreparePackagedContent(const char* lpCmdLine, const std::vector<std::string>& droppedSwitches,
                           const std::filesystem::path& shippingUserFolder); // the packages, the D3D12 runtime, the card
int ReadPackagedBootLevel();                    // config/boot.json's level and start mode
#else
int RunContentTools(const char* lpCmdLine);     // --vfs / --pak-*, --shader-*, --cook
#endif
int RunHarnessTools(const char* lpCmdLine);     // self-tests, stress, benchmarks, --import
int RunAssetTools(const char* lpCmdLine);       // --uv-density, --seam-ratio, --tessy-upgrade, --water-mask-lid, --lod-report, --reimport
}
