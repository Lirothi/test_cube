#include "text/FontAtlas.h"
#include <fstream>
#include <sstream>
#include <cwchar>
#include <algorithm>
#include <limits>
#include <cctype>

#pragma warning(push)
#pragma warning(disable: 26819)
#include "third_party/json/json.hpp"
#pragma warning(pop)
using nlohmann::json;

namespace {
constexpr uint32_t kInvalidGlyphIndex = std::numeric_limits<uint32_t>::max();

uint32_t PackUvUnorm16(float u, float v) {
    const auto toU16 = [](float value) -> uint32_t {
        const float clamped = std::clamp(value, 0.0f, 1.0f);
        return static_cast<uint32_t>(clamped * 65535.0f + 0.5f);
    };
    return toU16(u) | (toU16(v) << 16);
}

void UpdatePackedUv(FontGlyph& glyph) {
    glyph.uv00 = PackUvUnorm16(glyph.u0, glyph.v0);
    glyph.uv10 = PackUvUnorm16(glyph.u1, glyph.v0);
    glyph.uv11 = PackUvUnorm16(glyph.u1, glyph.v1);
    glyph.uv01 = PackUvUnorm16(glyph.u0, glyph.v1);
}
}

// VERY small TGA reader (8-bit)
static bool LoadTGA8(const std::wstring& path, int& w, int& h, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { return false; }
    unsigned char hdr[18];
    f.read((char*)hdr, 18);
    if (!f || hdr[2] != 3 || hdr[16] != 8) { return false; }
    w = hdr[12] | (hdr[13] << 8);
    h = hdr[14] | (hdr[15] << 8);
    out.resize((size_t)w * (size_t)h);
    // TGA stores rows bottom-to-top
    for (int y = h - 1; y >= 0; --y) {
        f.read((char*)&out[(size_t)y * (size_t)w], w);
    }
    return (bool)f;
}

static std::string ReadAllUtf8(const std::wstring& path) {
    std::ifstream f(path);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

void FontAtlas::InitAsciiBenchmarkFont() {
    pxSize_ = 32;
    spread_ = 0;
    atlasW_ = 256;
    atlasH_ = 256;
    ascent_ = 26;
    descent_ = -6;
    lineAdvance_ = 34;
    type_ = Type::Coverage;

    glyphs_.clear();
    glyphRemap_.clear();
    glyphRemapBase_ = 0;
    kerning_.clear();

    glyphs_.reserve(95);
    for (uint32_t cp = 32; cp <= 126; ++cp) {
        FontGlyph g{};
        g.cp = cp;
        g.x = static_cast<int>((cp - 32u) % 16u) * 8;
        g.y = static_cast<int>((cp - 32u) / 16u) * 16;
        g.w = (cp == 32u) ? 0 : 8;
        g.h = (cp == 32u) ? 0 : 16;
        g.xoff = 0;
        g.yoff = -14;
        g.xadv = (cp == 32u) ? 8 : 9;
        const float invW = 1.0f / float(atlasW_);
        const float invH = 1.0f / float(atlasH_);
        g.u0 = float(g.x) * invW;
        g.v0 = float(g.y) * invH;
        g.u1 = float(g.x + g.w) * invW;
        g.v1 = float(g.y + g.h) * invH;
        UpdatePackedUv(g);
        glyphs_.push_back(g);
    }

    glyphRemapBase_ = glyphs_.front().cp;
    glyphRemap_.assign(glyphs_.back().cp - glyphRemapBase_ + 1u, kInvalidGlyphIndex);
    for (size_t i = 0; i < glyphs_.size(); ++i) {
        glyphRemap_[glyphs_[i].cp - glyphRemapBase_] = static_cast<uint32_t>(i);
    }

    BuildLookupCaches();
}

bool FontAtlas::Load(Renderer* r, ID3D12GraphicsCommandList* uploadCl, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
                     const std::wstring& jsonPath, const std::wstring& tgaPath) {
    glyphs_.clear();
    glyphRemap_.clear();
    glyphRemapBase_ = 0;
    kerning_.clear();
    asciiGlyphs_.fill(nullptr);
    asciiKerning_.fill(0);

    // JSON metadata
    std::string j = ReadAllUtf8(jsonPath);
    json parsed = json::parse(j, nullptr, false);
    if (parsed.is_discarded()) {
        return false;
    }

    auto getInt = [](const json& node, const char* key) -> int {
        auto it = node.find(key);
        if (it == node.end()) { return 0; }
        if (it->is_number_integer()) { return it->get<int>(); }
        if (it->is_number_unsigned()) { return static_cast<int>(it->get<uint32_t>()); }
        if (it->is_number_float()) { return static_cast<int>(it->get<float>()); }
        return 0;
    };

    type_ = Type::SDF;
    auto typeIt = parsed.find("type");
    if (typeIt != parsed.end() && typeIt->is_string()) {
        std::string typeStr = typeIt->get<std::string>();
        std::string lowered;
        lowered.reserve(typeStr.size());
        for (unsigned char c : typeStr) {
            lowered.push_back(static_cast<char>(std::tolower(static_cast<int>(c))));
        }
        if (lowered == "coverage") {
            type_ = Type::Coverage;
        }
    }

    pxSize_      = getInt(parsed, "pxSize");
    spread_      = getInt(parsed, "spread");
    atlasW_      = getInt(parsed, "atlasW");
    atlasH_      = getInt(parsed, "atlasH");
    ascent_      = getInt(parsed, "ascent");
    descent_     = getInt(parsed, "descent");
    lineAdvance_ = getInt(parsed, "lineAdvance");

    // glyphs
    if (parsed.contains("glyphs") && parsed["glyphs"].is_array()) {
        for (const auto& entry : parsed["glyphs"]) {
            if (!entry.is_object()) { continue; }
            FontGlyph g{};
            const int cp = getInt(entry, "cp");
            if (cp < 0) { continue; }
            g.cp = static_cast<uint32_t>(cp);
            g.x = getInt(entry, "x");
            g.y = getInt(entry, "y");
            g.w = getInt(entry, "w");
            g.h = getInt(entry, "h");
            g.xoff = getInt(entry, "xoff");
            g.yoff = getInt(entry, "yoff");
            g.xadv = getInt(entry, "xadv");
            if (g.w > 0 && g.h > 0 && atlasW_ > 0 && atlasH_ > 0) {
                const float invW = 1.0f / float(atlasW_);
                const float invH = 1.0f / float(atlasH_);
                const float padU = 0.0f;// 0.5f;
                const float padV = 0.0f;// 0.5f;

                g.u0 = (g.x + padU) * invW;
                g.v0 = (g.y + padV) * invH;
                g.u1 = (g.x + g.w - padU) * invW;
                g.v1 = (g.y + g.h - padV) * invH;
            }
            else {
                g.u0 = g.v0 = g.u1 = g.v1 = 0.0f;
            }
            UpdatePackedUv(g);
            glyphs_.push_back(g);
        }
    }

    // kern
    if (parsed.contains("kern") && parsed["kern"].is_array()) {
        for (const auto& entry : parsed["kern"]) {
            if (!entry.is_object()) { continue; }
            const int a = getInt(entry, "a");
            const int b = getInt(entry, "b");
            KerningPair pair{};
            if (a < 0 || b < 0) { continue; }
            pair.key = (static_cast<uint64_t>(static_cast<uint32_t>(a)) << 32) |
                       static_cast<uint64_t>(static_cast<uint32_t>(b));
            pair.value = getInt(entry, "k");
            kerning_.push_back(pair);
        }
    }

    // Load the .tga atlas, convert it to RGBA8, and upload it via Texture2D::CreateFromRGBA8
    int w=0, h=0; std::vector<uint8_t> g;
    if (!LoadTGA8(tgaPath, w, h, g)) {
        return false;
    }
    std::vector<uint8_t> rgba((size_t)w * (size_t)h * 4u);
    for (int i = 0; i < w*h; ++i) {
        uint8_t v = g[(size_t)i];
        rgba[(size_t)i*4+0] = v;
        rgba[(size_t)i*4+1] = v;
        rgba[(size_t)i*4+2] = v;
        rgba[(size_t)i*4+3] = v;
    }
    tex_.CreateFromRGBA8(r, uploadCl, rgba.data(), (UINT)w, (UINT)h, uploadKeepAlive);

    // Sort and deduplicate glyphs (the last entry in JSON wins)
    if (!glyphs_.empty()) {
        std::stable_sort(glyphs_.begin(), glyphs_.end(), [](const FontGlyph& a, const FontGlyph& b) {
            return a.cp < b.cp;
        });
        auto write = glyphs_.begin();
        for (auto it = glyphs_.begin(); it != glyphs_.end();) {
            uint32_t cp = it->cp;
            auto last = it;
            do {
                last = it;
                ++it;
            } while (it != glyphs_.end() && it->cp == cp);
            *write++ = *last;
        }
        glyphs_.erase(write, glyphs_.end());
    }

    // If JSON did not contain a space glyph, add a synthetic one with the correct advance
    auto lowerBoundCp = [](const FontGlyph& glyph, uint32_t cp) {
        return glyph.cp < cp;
    };
    auto itSpace = std::lower_bound(glyphs_.begin(), glyphs_.end(), uint32_t(32), lowerBoundCp);
    if (itSpace == glyphs_.end() || itSpace->cp != 32u) {
        FontGlyph sp{};
        sp.cp = 32u;
        sp.w = 0;
        sp.h = 0;
        sp.xoff = 0;
        sp.yoff = 0;
        sp.u0 = sp.v0 = sp.u1 = sp.v1 = 0.0f;

        auto itN = std::lower_bound(glyphs_.begin(), glyphs_.end(), static_cast<uint32_t>('n'), lowerBoundCp);
        const bool hasN = (itN != glyphs_.end() && itN->cp == (uint32_t)'n');
        sp.xadv = (hasN ? itN->xadv : (pxSize_ / 2));
        UpdatePackedUv(sp);

        itSpace = glyphs_.insert(itSpace, sp);
    }

    // Rebuild the remap (for compact ranges use a direct index)
    glyphRemap_.clear();
    glyphRemapBase_ = 0;
    if (!glyphs_.empty()) {
        glyphRemapBase_ = glyphs_.front().cp;
        const uint32_t maxCp = glyphs_.back().cp;
        const uint64_t span = uint64_t(maxCp) - uint64_t(glyphRemapBase_) + 1ull;
        constexpr uint64_t kMaxDenseSpan = 65536ull;
        if (span <= kMaxDenseSpan && span <= glyphs_.size() * 8ull + 64ull) {
            glyphRemap_.assign(static_cast<size_t>(span), kInvalidGlyphIndex);
            for (size_t i = 0; i < glyphs_.size(); ++i) {
                glyphRemap_[glyphs_[i].cp - glyphRemapBase_] = static_cast<uint32_t>(i);
            }
        }
    }

    // Sort and deduplicate kerning pairs
    if (!kerning_.empty()) {
        std::stable_sort(kerning_.begin(), kerning_.end(), [](const KerningPair& a, const KerningPair& b) {
            return a.key < b.key;
        });
        auto write = kerning_.begin();
        for (auto it = kerning_.begin(); it != kerning_.end();) {
            uint64_t key = it->key;
            auto last = it;
            do {
                last = it;
                ++it;
            } while (it != kerning_.end() && it->key == key);
            *write++ = *last;
        }
        kerning_.erase(write, kerning_.end());
    }

    BuildLookupCaches();
    return true;
}

const FontGlyph* FontAtlas::Find(uint32_t cp) const {
    if (cp < asciiGlyphs_.size()) {
        return asciiGlyphs_[static_cast<size_t>(cp)];
    }

    if (!glyphRemap_.empty()) {
        if (cp < glyphRemapBase_) { return nullptr; }
        const uint64_t offset = uint64_t(cp) - uint64_t(glyphRemapBase_);
        if (offset >= glyphRemap_.size()) { return nullptr; }
        const uint32_t glyphIndex = glyphRemap_[static_cast<size_t>(offset)];
        if (glyphIndex == kInvalidGlyphIndex) { return nullptr; }
        return &glyphs_[glyphIndex];
    }

    auto it = std::lower_bound(glyphs_.begin(), glyphs_.end(), cp, [](const FontGlyph& glyph, uint32_t value) {
        return glyph.cp < value;
    });
    if (it == glyphs_.end() || it->cp != cp) {
        return nullptr;
    }
    return &(*it);
}

int FontAtlas::Kerning(uint32_t a, uint32_t b) const {
    if (kerning_.empty()) { return 0; }
    if (a < kAsciiCacheSize && b < kAsciiCacheSize) {
        return static_cast<int>(asciiKerning_[static_cast<size_t>(a) * kAsciiCacheSize + static_cast<size_t>(b)]);
    }

    const uint64_t key = ((uint64_t)a << 32) | (uint64_t)b;
    auto it = std::lower_bound(kerning_.begin(), kerning_.end(), key, [](const KerningPair& pair, uint64_t value) {
        return pair.key < value;
    });
    if (it == kerning_.end() || it->key != key) {
        return 0;
    }
    return it->value;
}

void FontAtlas::BuildLookupCaches() {
    asciiGlyphs_.fill(nullptr);
    asciiKerning_.fill(0);

    for (size_t cp = 0; cp < asciiGlyphs_.size(); ++cp) {
        if (!glyphRemap_.empty()) {
            if (cp >= glyphRemapBase_) {
                const uint64_t offset = uint64_t(cp) - uint64_t(glyphRemapBase_);
                if (offset < glyphRemap_.size()) {
                    const uint32_t glyphIndex = glyphRemap_[static_cast<size_t>(offset)];
                    if (glyphIndex != kInvalidGlyphIndex && glyphIndex < glyphs_.size()) {
                        asciiGlyphs_[cp] = &glyphs_[glyphIndex];
                    }
                }
            }
        }
        else {
            auto it = std::lower_bound(glyphs_.begin(), glyphs_.end(), static_cast<uint32_t>(cp), [](const FontGlyph& glyph, uint32_t value) {
                return glyph.cp < value;
            });
            if (it != glyphs_.end() && it->cp == cp) {
                asciiGlyphs_[cp] = &(*it);
            }
        }
    }

    for (const KerningPair& pair : kerning_) {
        const uint32_t a = static_cast<uint32_t>(pair.key >> 32);
        const uint32_t b = static_cast<uint32_t>(pair.key & 0xffffffffull);
        if (a < kAsciiCacheSize && b < kAsciiCacheSize) {
            const int clamped = std::clamp(pair.value,
                static_cast<int>(std::numeric_limits<int16_t>::min()),
                static_cast<int>(std::numeric_limits<int16_t>::max()));
            asciiKerning_[static_cast<size_t>(a) * kAsciiCacheSize + static_cast<size_t>(b)] = static_cast<int16_t>(clamped);
        }
    }
}
