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
        float spread = 0.0f;
    };

    bool Generate(const Params& params);
};
