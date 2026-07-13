#pragma once

#include <cstddef>
#include <string_view>

// Allocation-free, ASCII case-insensitive string matching shared by the editor
// panels and the asset registry. This matches the default-"C"-locale behavior of
// the std::tolower-based helpers it replaced (only A-Z fold to a-z), but without
// building a lowercased temporary std::string per call/field/comparison.
namespace textmatch
{
    inline char ToLowerAscii(char c)
    {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    // Case-insensitive substring test. An empty needle matches everything. The
    // needle may be any case; passing an already-lowercased needle also works,
    // since folding is idempotent.
    inline bool ContainsCaseInsensitive(std::string_view haystack, std::string_view needle)
    {
        if (needle.empty())
        {
            return true;
        }
        if (needle.size() > haystack.size())
        {
            return false;
        }

        const std::size_t last = haystack.size() - needle.size();
        for (std::size_t start = 0; start <= last; ++start)
        {
            std::size_t i = 0;
            for (; i < needle.size(); ++i)
            {
                if (ToLowerAscii(haystack[start + i]) != ToLowerAscii(needle[i]))
                {
                    break;
                }
            }
            if (i == needle.size())
            {
                return true;
            }
        }
        return false;
    }

    // Three-way case-insensitive comparison: <0 if a<b, 0 if equal, >0 if a>b.
    inline int CompareCaseInsensitive(std::string_view a, std::string_view b)
    {
        const std::size_t count = a.size() < b.size() ? a.size() : b.size();
        for (std::size_t i = 0; i < count; ++i)
        {
            const unsigned char ca = static_cast<unsigned char>(ToLowerAscii(a[i]));
            const unsigned char cb = static_cast<unsigned char>(ToLowerAscii(b[i]));
            if (ca != cb)
            {
                return ca < cb ? -1 : 1;
            }
        }
        if (a.size() == b.size())
        {
            return 0;
        }
        return a.size() < b.size() ? -1 : 1;
    }
}
