#include "FontGenerator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <fstream>
#include <filesystem>
#include <unordered_map>
#include <vector>

#pragma warning(push)
#pragma warning(disable: 26819)
#include "third_party/json/json.hpp"
#pragma warning(pop)

#define STB_RECT_PACK_IMPLEMENTATION
#include "third_party/stb_rect_pack.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"

using nlohmann::json;

namespace {
std::wstring JoinPath(const std::wstring& folder, const std::wstring& name) {
    if (folder.empty()) {
        return name;
    }
    if (folder.back() == L'/' || folder.back() == L'\\') {
        return folder + name;
    }
    return folder + L"\\" + name;
}

struct GlyphBitmap {
    uint32_t codepoint = 0;
    int glyphIndex = 0;
    int advance = 0;
    int boxW = 0;
    int boxH = 0;
    int bitmapX0 = 0;
    int bitmapY0 = 0;
    int sdfW = 0;
    int sdfH = 0;
    int sdfXOffset = 0;
    int sdfYOffset = 0;
    std::vector<uint8_t> coverage;
    std::vector<uint8_t> sdf;
};

struct PackedGlyph {
    GlyphBitmap glyph;
    int atlasX = 0;
    int atlasY = 0;
    int width = 0;
    int height = 0;
};

bool WriteTGA8(const std::wstring& path, int w, int h, const std::vector<uint8_t>& data) {
    if ((int)data.size() != w * h) {
        return false;
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    unsigned char header[18]{};
    header[2] = 3; // grayscale
    header[12] = static_cast<unsigned char>(w & 0xFF);
    header[13] = static_cast<unsigned char>((w >> 8) & 0xFF);
    header[14] = static_cast<unsigned char>(h & 0xFF);
    header[15] = static_cast<unsigned char>((h >> 8) & 0xFF);
    header[16] = 8;
    header[17] = 0; // bottom-left origin
    f.write(reinterpret_cast<char*>(header), sizeof(header));
    for (int y = 0; y < h; ++y) {
        const int srcY = h - 1 - y;
        const uint8_t* row = data.data() + (size_t)srcY * (size_t)w;
        f.write(reinterpret_cast<const char*>(row), (std::streamsize)w);
    }
    return static_cast<bool>(f);
}

std::vector<uint32_t> BuildCodepointList() {
    std::vector<uint32_t> cps;
    cps.reserve(512);
    for (uint32_t cp = 32; cp <= 126; ++cp) {
        cps.push_back(cp);
    }
    // basic Cyrillic range
    for (uint32_t cp = 0x400; cp <= 0x45F; ++cp) {
        cps.push_back(cp);
    }
    cps.push_back(0x451); // ё
    cps.push_back(0x401); // Ё
    std::sort(cps.begin(), cps.end());
    cps.erase(std::unique(cps.begin(), cps.end()), cps.end());
    return cps;
}

bool WriteJson(const std::wstring& path,
               const json& j) {
    std::ofstream out(path);
    if (!out) {
        return false;
    }
    out << j.dump(2);
    return static_cast<bool>(out);
}

} // namespace

bool FontGenerator::Generate(const Params& params) {
    if (params.fontFile.empty() || params.pixelHeight <= 0) {
        return false;
    }

    namespace fs = std::filesystem;

    const bool isSdf = (params.type == OutputType::Sdf);
    const float spreadValue = isSdf ? std::max(0.0f, params.spread) : 0.0f;
    const int spreadPixels = isSdf ? std::max(0, static_cast<int>(std::ceil(spreadValue))) : 0;
    const float sdfPixelDistScale = (spreadValue > 0.0f) ? (127.0f / spreadValue) : 0.0f;

    const fs::path fontPath(params.fontFile);
    const fs::path fileStem = fontPath.stem();
    std::wstring baseName = fileStem.empty() ? L"font" : fileStem.wstring();
    baseName += L"_" + std::to_wstring(params.pixelHeight);

    const std::wstring jsonPath = JoinPath(params.fontsFolder, isSdf ? baseName + L".json" : baseName + L"_coverage.json");
    const std::wstring texturePath = JoinPath(params.fontsFolder, isSdf ? baseName + L".tga" : baseName + L"_coverage.tga");

    std::ifstream fontStream(fontPath, std::ios::binary | std::ios::ate);
    if (!fontStream) {
        return false;
    }
    const std::streamsize fontSize = fontStream.tellg();
    if (fontSize <= 0) {
        return false;
    }
    fontStream.seekg(0, std::ios::beg);
    std::vector<unsigned char> fontBuffer((size_t)fontSize);
    if (!fontStream.read(reinterpret_cast<char*>(fontBuffer.data()), fontSize)) {
        return false;
    }

    const int fontOffset = stbtt_GetFontOffsetForIndex(fontBuffer.data(), 0);
    if (fontOffset < 0) {
        return false;
    }

    stbtt_fontinfo fontInfo{};
    if (!stbtt_InitFont(&fontInfo, fontBuffer.data(), fontOffset)) {
        return false;
    }

    const float scale = stbtt_ScaleForPixelHeight(&fontInfo, static_cast<float>(params.pixelHeight));
    int ascent = 0;
    int descent = 0;
    int lineGap = 0;
    stbtt_GetFontVMetrics(&fontInfo, &ascent, &descent, &lineGap);
    const int ascentPx = static_cast<int>(std::round(scale * static_cast<float>(ascent)));
    const int descentPx = static_cast<int>(std::round(scale * static_cast<float>(descent)));
    const int lineAdvancePx = static_cast<int>(std::round(scale * static_cast<float>(ascent - descent + lineGap)));

    const std::vector<uint32_t> codepoints = BuildCodepointList();
    std::vector<GlyphBitmap> glyphs;
    glyphs.reserve(codepoints.size());
    std::unordered_map<uint32_t, int> codepointToGlyph;
    codepointToGlyph.reserve(codepoints.size());

    for (uint32_t cp : codepoints) {
        const int glyphIdx = stbtt_FindGlyphIndex(&fontInfo, static_cast<int>(cp));
        if (glyphIdx == 0) {
            continue;
        }

        int advanceWidth = 0;
        int leftSideBearing = 0;
        stbtt_GetGlyphHMetrics(&fontInfo, glyphIdx, &advanceWidth, &leftSideBearing);

        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBox(&fontInfo, glyphIdx, scale, scale, &x0, &y0, &x1, &y1);

        GlyphBitmap bmp{};
        bmp.codepoint = cp;
        bmp.glyphIndex = static_cast<int>(glyphIdx);
        bmp.advance = static_cast<int>(std::round(scale * static_cast<float>(advanceWidth)));
        bmp.boxW = x1 - x0;
        bmp.boxH = y1 - y0;
        bmp.bitmapX0 = x0;
        bmp.bitmapY0 = y0;

        if (bmp.boxW > 0 && bmp.boxH > 0) {
            bmp.coverage.assign((size_t)bmp.boxW * (size_t)bmp.boxH, 0);
            stbtt_MakeGlyphBitmap(&fontInfo, bmp.coverage.data(), bmp.boxW, bmp.boxH, bmp.boxW, scale, scale, glyphIdx);
            if (isSdf) {
                int sdfW = 0;
                int sdfH = 0;
                int sdfXOff = 0;
                int sdfYOff = 0;
                unsigned char* sdf = stbtt_GetGlyphSDF(&fontInfo,
                                                       scale,
                                                       glyphIdx,
                                                       spreadPixels,
                                                       128,
                                                       sdfPixelDistScale,
                                                       &sdfW,
                                                       &sdfH,
                                                       &sdfXOff,
                                                       &sdfYOff);
                if (sdf != nullptr && sdfW > 0 && sdfH > 0) {
                    bmp.sdf.assign(sdf, sdf + (size_t)sdfW * (size_t)sdfH);
                    bmp.sdfW = sdfW;
                    bmp.sdfH = sdfH;
                    bmp.sdfXOffset = sdfXOff;
                    bmp.sdfYOffset = sdfYOff;
                    STBTT_free(sdf, fontInfo.userdata);
                } else if (sdf != nullptr) {
                    STBTT_free(sdf, fontInfo.userdata);
                }
            }
        }

        codepointToGlyph.emplace(cp, glyphIdx);
        glyphs.push_back(std::move(bmp));
    }

    if (glyphs.empty()) {
        return false;
    }

    std::sort(glyphs.begin(), glyphs.end(), [](const GlyphBitmap& a, const GlyphBitmap& b) {
        return a.boxH > b.boxH;
    });

    std::vector<PackedGlyph> packed;
    packed.reserve(glyphs.size());
    for (const GlyphBitmap& g : glyphs) {
        PackedGlyph pg;
        pg.glyph = g;
        if (isSdf && g.sdfW > 0 && g.sdfH > 0) {
            pg.width = g.sdfW;
            pg.height = g.sdfH;
        } else if (!isSdf && g.boxW > 0 && g.boxH > 0) {
            pg.width = g.boxW;
            pg.height = g.boxH;
        } else {
            pg.width = 0;
            pg.height = 0;
        }
        packed.push_back(std::move(pg));
    }

    const int borderSize = spreadPixels;

    auto tryPack = [&](int atlasW, int atlasH) -> bool {
        if (atlasW <= borderSize * 2 || atlasH <= borderSize * 2) {
            return false;
        }

        const int packW = atlasW - borderSize * 2;
        const int packH = atlasH - borderSize * 2;
        if (packW <= 0 || packH <= 0) {
            return false;
        }

        for (auto& pg : packed) {
            if (pg.width > 0 && pg.height > 0) {
                pg.atlasX = -1;
                pg.atlasY = -1;
            } else {
                pg.atlasX = borderSize;
                pg.atlasY = borderSize;
            }
        }

        std::vector<stbrp_rect> rects;
        rects.reserve(packed.size());
        for (size_t i = 0; i < packed.size(); ++i) {
            PackedGlyph& pg = packed[i];
            if (pg.width <= 0 || pg.height <= 0) {
                continue;
            }
            stbrp_rect rect{};
            rect.id = static_cast<int>(i);
            rect.w = pg.width;
            rect.h = pg.height;
            rects.push_back(rect);
        }

        std::vector<stbrp_node> nodes(static_cast<size_t>(packW));
        stbrp_context ctx{};
        stbrp_init_target(&ctx, packW, packH, nodes.data(), packW);

        if (!rects.empty()) {
            if (!stbrp_pack_rects(&ctx, rects.data(), static_cast<int>(rects.size()))) {
                return false;
            }
            for (const stbrp_rect& rect : rects) {
                if (!rect.was_packed) {
                    return false;
                }
                PackedGlyph& pg = packed[static_cast<size_t>(rect.id)];
                pg.atlasX = rect.x + borderSize;
                pg.atlasY = rect.y + borderSize;
            }
        }

        return true;
    };

    int maxDim = 0;
    for (const auto& pg : packed) {
        maxDim = std::max(maxDim, std::max(pg.width, pg.height));
    }

    int startSize = 64;
    const int target = std::max(borderSize * 2 + 1, maxDim + borderSize * 2);
    while (startSize < target && startSize < 4096) {
        startSize *= 2;
    }
    startSize = std::min(startSize, 4096);

    int atlasW = startSize;
    int atlasH = startSize;
    bool packedOk = false;
    for (int size = startSize; size <= 4096; size *= 2) {
        atlasW = size;
        atlasH = size;
        if (tryPack(atlasW, atlasH)) {
            packedOk = true;
            break;
        }
    }
    if (!packedOk) {
        return false;
    }

    std::vector<uint8_t> atlas(static_cast<size_t>(atlasW) * static_cast<size_t>(atlasH), 0);
    struct GlyphJsonEntry {
        uint32_t cp = 0;
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        int xoff = 0;
        int yoff = 0;
        int xadv = 0;
    };
    std::vector<GlyphJsonEntry> glyphEntries;
    glyphEntries.reserve(packed.size());

    for (const PackedGlyph& pg : packed) {
        GlyphJsonEntry entry{};
        entry.cp = pg.glyph.codepoint;
        entry.xadv = pg.glyph.advance;
        if (pg.width > 0 && pg.height > 0 && pg.atlasX >= 0 && pg.atlasY >= 0) {
            const size_t expectedSize = static_cast<size_t>(pg.width) * static_cast<size_t>(pg.height);
            const uint8_t* srcData = nullptr;
            if (isSdf) {
                if (pg.glyph.sdf.size() == expectedSize) {
                    srcData = pg.glyph.sdf.data();
                }
            } else {
                if (pg.glyph.coverage.size() == expectedSize) {
                    srcData = pg.glyph.coverage.data();
                }
            }

            if (srcData != nullptr) {
                for (int y = 0; y < pg.height; ++y) {
                    uint8_t* dst = atlas.data() + (static_cast<size_t>(pg.atlasY + y) * static_cast<size_t>(atlasW) + static_cast<size_t>(pg.atlasX));
                    const uint8_t* src = srcData + static_cast<size_t>(y) * static_cast<size_t>(pg.width);
                    std::copy(src, src + pg.width, dst);
                }
            }

            entry.x = pg.atlasX;
            entry.y = pg.atlasY;
            entry.w = pg.width;
            entry.h = pg.height;
            if (isSdf) {
                entry.xoff = pg.glyph.sdfXOffset;
                entry.yoff = pg.glyph.sdfYOffset;
            } else {
                entry.xoff = pg.glyph.bitmapX0;
                entry.yoff = pg.glyph.bitmapY0;
            }
        }
        glyphEntries.push_back(entry);
    }

    std::sort(glyphEntries.begin(), glyphEntries.end(), [](const GlyphJsonEntry& a, const GlyphJsonEntry& b) {
        return a.cp < b.cp;
    });

    std::vector<json> kernEntries;
    if (!glyphEntries.empty()) {
        for (const auto& a : glyphEntries) {
            auto itA = codepointToGlyph.find(a.cp);
            if (itA == codepointToGlyph.end()) {
                continue;
            }
            for (const auto& b : glyphEntries) {
                auto itB = codepointToGlyph.find(b.cp);
                if (itB == codepointToGlyph.end()) {
                    continue;
                }
                const int kern = stbtt_GetGlyphKernAdvance(&fontInfo, itA->second, itB->second);
                if (kern == 0) {
                    continue;
                }
                const int kernPx = static_cast<int>(std::round(scale * static_cast<float>(kern)));
                if (kernPx == 0) {
                    continue;
                }
                json jk;
                jk["a"] = static_cast<int>(a.cp);
                jk["b"] = static_cast<int>(b.cp);
                jk["k"] = kernPx;
                kernEntries.push_back(std::move(jk));
            }
        }
    }

    json root;
    root["type"] = isSdf ? "sdf" : "coverage";
    root["pxSize"] = params.pixelHeight;
    root["spread"] = isSdf ? static_cast<int>(std::round(spreadValue)) : 0;
    root["atlasW"] = atlasW;
    root["atlasH"] = atlasH;
    root["ascent"] = ascentPx;
    root["descent"] = descentPx;
    root["lineAdvance"] = lineAdvancePx;

    json glyphArray = json::array();
    for (const auto& ge : glyphEntries) {
        json jg;
        jg["cp"] = static_cast<int>(ge.cp);
        jg["x"] = ge.x;
        jg["y"] = ge.y;
        jg["w"] = ge.w;
        jg["h"] = ge.h;
        jg["xoff"] = ge.xoff;
        jg["yoff"] = ge.yoff;
        jg["xadv"] = ge.xadv;
        glyphArray.push_back(std::move(jg));
    }
    root["glyphs"] = std::move(glyphArray);
    if (!kernEntries.empty()) {
        root["kern"] = std::move(kernEntries);
    }

    const bool okTga = WriteTGA8(texturePath, atlasW, atlasH, atlas);
    const bool okJson = WriteJson(jsonPath, root);
    return okTga && okJson;
}
