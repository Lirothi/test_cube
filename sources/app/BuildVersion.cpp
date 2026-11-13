#include "app/BuildVersion.h"

#include "app/generated/BuildVersion.g.h"

namespace build
{
    const char* ProductName() { return TC_PRODUCT_NAME; }
    const char* VersionString() { return TC_VERSION_STRING; }
    const char* GitHash() { return TC_GIT_HASH; }
    const char* BuildDate() { return TC_BUILD_DATE; }
}
