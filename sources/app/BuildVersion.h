#pragma once

// The build's version (docs/distribution_build_plan.md §7). tools/build_dist.py writes
// sources/app/generated/BuildVersion.g.h (version, build number, git hash with "-dirty", date); a build without one gets
// the dev values the project writes when the file is missing ("0.0.0-dev"). The .rc's VERSIONINFO reads the same file.
// Only BuildVersion.cpp and the .rc include it, so a new version recompiles one file.

namespace build
{
    [[nodiscard]] const char* ProductName();    // "test_cube"
    [[nodiscard]] const char* VersionString();  // "0.1.0+12 (abc1234)" or "0.0.0-dev"
    [[nodiscard]] const char* GitHash();        // empty in a dev build
    [[nodiscard]] const char* BuildDate();      // empty in a dev build
    [[nodiscard]] constexpr bool Shipping()
    {
#if TC_SHIPPING
        return true;
#else
        return false;
#endif
    }
}
