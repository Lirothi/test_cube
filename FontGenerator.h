#pragma once

#include <string>

class FontGenerator {
public:
    enum class OutputType { Sdf, Coverage };

    struct Params {
        std::wstring fontFile;
        int pixelHeight = 32;
        OutputType type = OutputType::Coverage;
        std::wstring fontsFolder;
        int spread = 0;
    };

    bool Generate(const Params& params);
};
