#include "FontGenerator.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <fstream>
#include <limits>
#include <numeric>
#include <unordered_set>
#include <vector>

#pragma warning(push)
#pragma warning(disable: 26819)
#include "third_party/json/json.hpp"
#pragma warning(pop)

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
    uint16_t glyphIndex = 0;
    int advance = 0;
    int bearingX = 0;
    int bearingY = 0;
    int boxW = 0;
    int boxH = 0;
    std::vector<uint8_t> coverage;
};

struct PackedGlyph {
    GlyphBitmap glyph;
    int atlasX = 0;
    int atlasY = 0;
    int width = 0;
    int height = 0;
};

constexpr float kInf = 1e12f;
constexpr float kSdfHaloScale = 1.25f;

void EDT1D(const float* f, int n, float* d) {
    std::vector<int> v(n);
    std::vector<float> z(n + 1);
    int k = 0;
    v[0] = 0;
    z[0] = -std::numeric_limits<float>::infinity();
    z[1] = std::numeric_limits<float>::infinity();
    for (int q = 1; q < n; ++q) {
        float s = 0.0f;
        while (true) {
            const int vk = v[k];
            const float num = (f[q] + float(q * q)) - (f[vk] + float(vk * vk));
            const float den = 2.0f * float(q - vk);
            s = (den != 0.0f) ? (num / den) : std::numeric_limits<float>::infinity();
            if (s <= z[k]) {
                if (k == 0) {
                    break;
                }
                --k;
            } else {
                break;
            }
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = std::numeric_limits<float>::infinity();
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < q) {
            ++k;
        }
        const int vk = v[k];
        const float diff = float(q - vk);
        d[q] = diff * diff + f[vk];
    }
}

void EDT2D(const std::vector<float>& f, int w, int h, std::vector<float>& d) {
    std::vector<float> tmp((size_t)w * (size_t)h);
    std::vector<float> column(h);
    std::vector<float> columnOut(h);

    for (int y = 0; y < h; ++y) {
        EDT1D(f.data() + (size_t)y * (size_t)w, w, tmp.data() + (size_t)y * (size_t)w);
    }
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) {
            column[y] = tmp[(size_t)y * (size_t)w + (size_t)x];
        }
        EDT1D(column.data(), h, columnOut.data());
        for (int y = 0; y < h; ++y) {
            d[(size_t)y * (size_t)w + (size_t)x] = columnOut[y];
        }
    }
}

void ComputeSDF(const std::vector<uint8_t>& coverage, int w, int h, float spread, std::vector<uint8_t>& out) {
    const size_t pixelCount = (size_t)w * (size_t)h;
    std::vector<float> inside(pixelCount, kInf);
    std::vector<float> outside(pixelCount, kInf);

    for (size_t i = 0; i < pixelCount; ++i) {
        const float alpha = float(coverage[i]) / 255.0f;
        if (alpha >= 0.5f) {
            inside[i] = 0.0f;
        } else {
            outside[i] = 0.0f;
        }
    }

    std::vector<float> distInside(pixelCount, kInf);
    std::vector<float> distOutside(pixelCount, kInf);
    EDT2D(inside, w, h, distInside);
    EDT2D(outside, w, h, distOutside);

    out.resize(pixelCount);
    const float spreadF = std::max(1.0f, spread);
    for (size_t i = 0; i < pixelCount; ++i) {
        const float di = std::sqrt(distInside[i]);
        const float do_ = std::sqrt(distOutside[i]);
        const float signedDist = do_ - di;
        float value = 0.5f + signedDist / (2.0f * spreadF);
        value = std::clamp(value, 0.0f, 1.0f);
        out[i] = static_cast<uint8_t>(std::round(value * 255.0f));
    }
}

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
    if (params.fontFamily.empty() || params.pixelHeight <= 0) {
        return false;
    }

    const bool isSdf = (params.type == OutputType::Sdf);
    const std::wstring baseName = params.fontFamily + L"_" + std::to_wstring(params.pixelHeight);
    const std::wstring jsonPath = JoinPath(params.fontsFolder, isSdf ? baseName + L".json" : baseName + L"_coverage.json");
    const std::wstring texturePath = JoinPath(params.fontsFolder, isSdf ? baseName + L".tga" : baseName + L"_coverage.tga");

    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) {
        return false;
    }

    const int fontHeight = -params.pixelHeight;
    HFONT font = CreateFontW(fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, FF_DONTCARE | FIXED_PITCH,
                              params.fontFamily.c_str());
    if (!font) {
        DeleteDC(hdc);
        return false;
    }

    HFONT oldFont = (HFONT)SelectObject(hdc, font);
    SetMapMode(hdc, MM_TEXT);

    TEXTMETRICW tm{};
    if (!GetTextMetricsW(hdc, &tm)) {
        SelectObject(hdc, oldFont);
        DeleteObject(font);
        DeleteDC(hdc);
        return false;
    }

    const std::vector<uint32_t> codepoints = BuildCodepointList();
    std::vector<WCHAR> wcharBuffer;
    wcharBuffer.reserve(codepoints.size());
    for (uint32_t cp : codepoints) {
        if (cp <= 0xFFFFu) {
            wcharBuffer.push_back(static_cast<WCHAR>(cp));
        }
    }

    std::vector<WORD> glyphIndices(wcharBuffer.size(), 0);
    if (!wcharBuffer.empty()) {
        if (GetGlyphIndicesW(hdc, wcharBuffer.data(), static_cast<int>(wcharBuffer.size()),
                             reinterpret_cast<LPWORD>(glyphIndices.data()), GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR) {
            SelectObject(hdc, oldFont);
            DeleteObject(font);
            DeleteDC(hdc);
            return false;
        }
    }

    MAT2 mat2{ {0,1}, {0,0}, {0,0}, {0,1} };

    std::vector<GlyphBitmap> glyphs;
    glyphs.reserve(codepoints.size());
    size_t glyphCursor = 0;
    for (size_t i = 0; i < codepoints.size(); ++i) {
        const uint32_t cp = codepoints[i];
        if (cp > 0xFFFFu) {
            continue;
        }
        if (glyphCursor >= glyphIndices.size()) {
            break;
        }
        const WORD glyphIdx = glyphIndices[glyphCursor++];
        if (glyphIdx == 0xFFFFu) {
            continue;
        }
        GLYPHMETRICS gm{};
        const DWORD bufferSize = GetGlyphOutlineW(hdc, glyphIdx, GGO_GRAY8_BITMAP | GGO_GLYPH_INDEX,
                                                  &gm, 0, nullptr, &mat2);
        if (bufferSize == GDI_ERROR) {
            continue;
        }

        GlyphBitmap bmp{};
        bmp.codepoint = cp;
        bmp.glyphIndex = static_cast<uint16_t>(glyphIdx);
        bmp.advance = gm.gmCellIncX;
        bmp.bearingX = gm.gmptGlyphOrigin.x;
        bmp.bearingY = gm.gmptGlyphOrigin.y;
        bmp.boxW = gm.gmBlackBoxX;
        bmp.boxH = gm.gmBlackBoxY;

        if (gm.gmBlackBoxX > 0 && gm.gmBlackBoxY > 0) {
            bmp.coverage.assign((size_t)gm.gmBlackBoxX * (size_t)gm.gmBlackBoxY, 0);
            if (bufferSize > 0) {
                std::vector<uint8_t> raw(bufferSize);
                if (GetGlyphOutlineW(hdc, glyphIdx, GGO_GRAY8_BITMAP | GGO_GLYPH_INDEX,
                    &gm, bufferSize, raw.data(), &mat2) != GDI_ERROR) {
                    const size_t stride = ((size_t)gm.gmBlackBoxX + 3u) & ~3u;
                    for (UINT y = 0; y < gm.gmBlackBoxY; ++y) {
                        for (UINT x = 0; x < gm.gmBlackBoxX; ++x) {
                            const uint8_t v = raw[(size_t)y * stride + (size_t)x];
                            const int scaled = (int(v) * 255 + 32) / 64;
                            bmp.coverage[(size_t)y * (size_t)gm.gmBlackBoxX + (size_t)x] = static_cast<uint8_t>(std::clamp(scaled, 0, 255));
                        }
                    }
                }
            }
        }

        glyphs.push_back(std::move(bmp));
    }

    SelectObject(hdc, oldFont);
    DeleteObject(font);
    DeleteDC(hdc);

    if (glyphs.empty()) {
        return false;
    }

    std::sort(glyphs.begin(), glyphs.end(), [](const GlyphBitmap& a, const GlyphBitmap& b) {
        return a.boxH > b.boxH;
    });

    float sdfSpread = 0.0f;
    if (params.type == OutputType::Sdf) {
        const int baseSpread = std::max(8, params.pixelHeight / 2 + 2);
        sdfSpread = float(baseSpread) * kSdfHaloScale;
    }
    const int spread = (params.type == OutputType::Sdf) ? static_cast<int>(std::round(std::max(1.0f, sdfSpread))) : 0;
    const int padding = (params.type == OutputType::Sdf) ? static_cast<int>(std::ceil(std::max(1.0f, sdfSpread))) + 1 : 1;

    std::vector<PackedGlyph> packed;
    packed.reserve(glyphs.size());
    for (const GlyphBitmap& g : glyphs) {
        PackedGlyph pg;
        pg.glyph = g;
        if (g.boxW > 0 && g.boxH > 0) {
            pg.width = g.boxW + padding * 2;
            pg.height = g.boxH + padding * 2;
        } else {
            pg.width = 0;
            pg.height = 0;
        }
        packed.push_back(std::move(pg));
    }

    const int borderSize = (params.type == OutputType::Sdf) ? padding : std::max(1, padding);

    auto tryPack = [&](int atlasW, int atlasH) -> bool {
        if (atlasW <= 0 || atlasH <= 0) {
            return false;
        }

        struct Rect { int x = 0, y = 0, w = 0, h = 0; };

        Rect initial{ borderSize, borderSize, atlasW - borderSize * 2, atlasH - borderSize * 2 };
        if (initial.w <= 0 || initial.h <= 0) {
            return false;
        }

        auto intersects = [](const Rect& a, const Rect& b) -> bool {
            return !(b.x >= a.x + a.w || b.x + b.w <= a.x || b.y >= a.y + a.h || b.y + b.h <= a.y);
        };

        auto isContained = [](const Rect& a, const Rect& b) -> bool {
            return a.x >= b.x && a.y >= b.y && (a.x + a.w) <= (b.x + b.w) && (a.y + a.h) <= (b.y + b.h);
        };

        std::vector<Rect> freeRects;
        freeRects.reserve(packed.size() * 2 + 1);
        freeRects.push_back(initial);

        for (auto& pg : packed) {
            if (pg.width > 0 && pg.height > 0) {
                pg.atlasX = -1;
                pg.atlasY = -1;
            } else {
                pg.atlasX = 0;
                pg.atlasY = 0;
            }
        }

        for (auto& pg : packed) {
            if (pg.width == 0 || pg.height == 0) {
                continue;
            }

            int bestIndex = -1;
            Rect bestPlacement{};
            int bestScoreY = std::numeric_limits<int>::max();
            int bestScoreX = std::numeric_limits<int>::max();

            for (int i = 0; i < (int)freeRects.size(); ++i) {
                const Rect& fr = freeRects[i];
                if (pg.width > fr.w || pg.height > fr.h) {
                    continue;
                }
                const int scoreY = fr.y;
                const int scoreX = fr.x;
                if (scoreY < bestScoreY || (scoreY == bestScoreY && scoreX < bestScoreX)) {
                    bestIndex = i;
                    bestPlacement = { fr.x, fr.y, pg.width, pg.height };
                    bestScoreY = scoreY;
                    bestScoreX = scoreX;
                }
            }

            if (bestIndex < 0) {
                return false;
            }

            pg.atlasX = bestPlacement.x;
            pg.atlasY = bestPlacement.y;

            std::vector<Rect> newFreeRects;
            newFreeRects.reserve(freeRects.size() + 4);

            for (const Rect& fr : freeRects) {
                if (!intersects(fr, bestPlacement)) {
                    newFreeRects.push_back(fr);
                    continue;
                }

                // split above
                if (bestPlacement.y > fr.y) {
                    Rect top = fr;
                    top.h = bestPlacement.y - fr.y;
                    if (top.w > 0 && top.h > 0) {
                        newFreeRects.push_back(top);
                    }
                }
                // split below
                if (bestPlacement.y + bestPlacement.h < fr.y + fr.h) {
                    Rect bottom = fr;
                    bottom.y = bestPlacement.y + bestPlacement.h;
                    bottom.h = fr.y + fr.h - bottom.y;
                    if (bottom.w > 0 && bottom.h > 0) {
                        newFreeRects.push_back(bottom);
                    }
                }
                // split left
                if (bestPlacement.x > fr.x) {
                    Rect left = fr;
                    left.w = bestPlacement.x - fr.x;
                    if (left.w > 0 && left.h > 0) {
                        newFreeRects.push_back(left);
                    }
                }
                // split right
                if (bestPlacement.x + bestPlacement.w < fr.x + fr.w) {
                    Rect right = fr;
                    right.x = bestPlacement.x + bestPlacement.w;
                    right.w = fr.x + fr.w - right.x;
                    if (right.w > 0 && right.h > 0) {
                        newFreeRects.push_back(right);
                    }
                }
            }

            // prune rectangles contained inside others
            for (size_t i = 0; i < newFreeRects.size(); ++i) {
                for (size_t j = i + 1; j < newFreeRects.size(); ) {
                    if (isContained(newFreeRects[i], newFreeRects[j])) {
                        newFreeRects.erase(newFreeRects.begin() + (std::ptrdiff_t)i);
                        --i;
                        break;
                    }
                    if (isContained(newFreeRects[j], newFreeRects[i])) {
                        newFreeRects.erase(newFreeRects.begin() + (std::ptrdiff_t)j);
                    } else {
                        ++j;
                    }
                }
            }

            freeRects.swap(newFreeRects);
            if (freeRects.empty()) {
                return std::all_of(packed.begin(), packed.end(), [](const PackedGlyph& g) {
                    return g.width == 0 || g.atlasX >= 0;
                });
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
    while (startSize < target) {
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

    std::vector<uint8_t> atlas((size_t)atlasW * (size_t)atlasH, 0);
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
        if (pg.width > 0 && pg.height > 0) {
            std::vector<uint8_t> padded((size_t)pg.width * (size_t)pg.height, 0);
            for (int y = 0; y < pg.glyph.boxH; ++y) {
                uint8_t* dst = padded.data() + (size_t)(y + padding) * (size_t)pg.width + padding;
                const uint8_t* src = pg.glyph.coverage.data() + (size_t)y * (size_t)pg.glyph.boxW;
                std::copy(src, src + pg.glyph.boxW, dst);
            }
            if (params.type == OutputType::Sdf) {
                std::vector<uint8_t> sdf;
                ComputeSDF(padded, pg.width, pg.height, sdfSpread, sdf);
                padded.swap(sdf);
            }
            for (int y = 0; y < pg.height; ++y) {
                uint8_t* dst = atlas.data() + (size_t)(pg.atlasY + y) * (size_t)atlasW + (size_t)pg.atlasX;
                const uint8_t* src = padded.data() + (size_t)y * (size_t)pg.width;
                std::copy(src, src + pg.width, dst);
            }
            entry.x = pg.atlasX;
            entry.y = pg.atlasY;
            entry.w = pg.width;
            entry.h = pg.height;
            entry.xoff = pg.glyph.bearingX - padding;
            entry.yoff = -pg.glyph.bearingY - padding;
        }
        glyphEntries.push_back(entry);
    }

    std::sort(glyphEntries.begin(), glyphEntries.end(), [](const GlyphJsonEntry& a, const GlyphJsonEntry& b) {
        return a.cp < b.cp;
    });

    std::unordered_set<uint32_t> glyphSet;
    glyphSet.reserve(glyphEntries.size());
    for (const auto& ge : glyphEntries) {
        glyphSet.insert(ge.cp);
    }

    // kerning pairs
    std::vector<json> kernEntries;
    {
        HDC kernDC = CreateCompatibleDC(nullptr);
        if (kernDC) {
            HFONT kernFont = CreateFontW(fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         ANTIALIASED_QUALITY, FF_DONTCARE | FIXED_PITCH,
                                         params.fontFamily.c_str());
            if (kernFont) {
                HFONT old = (HFONT)SelectObject(kernDC, kernFont);
                DWORD pairCount = GetKerningPairsW(kernDC, 0, nullptr);
                if (pairCount > 0) {
                    std::vector<KERNINGPAIR> pairs(pairCount);
                    pairCount = GetKerningPairsW(kernDC, pairCount, pairs.data());
                    for (DWORD i = 0; i < pairCount; ++i) {
                        const auto& kp = pairs[i];
                        const uint32_t a = kp.wFirst;
                        const uint32_t b = kp.wSecond;
                        if (glyphSet.find(a) == glyphSet.end() || glyphSet.find(b) == glyphSet.end()) {
                            continue;
                        }
                        if (kp.iKernAmount == 0) {
                            continue;
                        }
                        json jk;
                        jk["a"] = static_cast<int>(a);
                        jk["b"] = static_cast<int>(b);
                        jk["k"] = kp.iKernAmount;
                        kernEntries.push_back(std::move(jk));
                    }
                }
                SelectObject(kernDC, old);
                DeleteObject(kernFont);
            }
            DeleteDC(kernDC);
        }
    }

    json root;
    root["type"] = (params.type == OutputType::Sdf) ? "sdf" : "coverage";
    root["pxSize"] = params.pixelHeight;
    root["spread"] = (params.type == OutputType::Sdf) ? spread : 0;
    root["atlasW"] = atlasW;
    root["atlasH"] = atlasH;
    root["ascent"] = tm.tmAscent;
    root["descent"] = -tm.tmDescent;
    root["lineAdvance"] = tm.tmHeight + tm.tmExternalLeading;

    json glyphArray = json::array();
    //glyphArray.reserve(glyphEntries.size());
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
