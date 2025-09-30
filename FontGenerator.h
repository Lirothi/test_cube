#pragma once

#include <string>

class FontGenerator {
public:
    enum class OutputType { Sdf, Coverage };

    struct Params {
        std::wstring fontFamily;
        int pixelHeight = 32;
        OutputType type = OutputType::Coverage;
        std::wstring fontsFolder;
    };

    bool Generate(const Params& params);
};
