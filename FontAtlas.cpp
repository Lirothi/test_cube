#include "FontAtlas.h"
#include <fstream>
#include <sstream>
#include <cwchar>
#include <algorithm>
#include <limits>

namespace {
constexpr uint32_t kInvalidGlyphIndex = std::numeric_limits<uint32_t>::max();
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
    // данные в TGA снизу-вверх
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

bool FontAtlas::Load(Renderer* r, ID3D12GraphicsCommandList* uploadCl, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
                     const std::wstring& jsonPath, const std::wstring& tgaPath) {
    // JSON (без сторонних либ — парсим по-минимуму под нашу схему)
    std::string j = ReadAllUtf8(jsonPath);
    auto findNum = [&](const char* key)->int {
        std::string k = std::string("\"") + key + "\":";
        size_t p = j.find(k);
        if (p == std::string::npos) { return 0; }
        p += k.size();
        return std::atoi(j.c_str() + p);
    };
    pxSize_     = findNum("pxSize");
    spread_     = findNum("spread");
    atlasW_     = findNum("atlasW");
    atlasH_     = findNum("atlasH");
    ascent_     = findNum("ascent");
    descent_    = findNum("descent");
    lineAdvance_= findNum("lineAdvance");

    // glyphs
    glyphs_.clear();
    glyphRemap_.clear();
    glyphRemapBase_ = 0;
    {
        size_t p = j.find("\"glyphs\":[");
        if (p != std::string::npos) {
            p = j.find('[', p);
            size_t e = j.find(']', p);
            std::string arr = j.substr(p + 1, e - p - 1);
            size_t i = 0;
            while (i < arr.size()) {
                size_t b = arr.find('{', i);
                if (b == std::string::npos) { break; }
                size_t c = arr.find('}', b);
                std::string obj = arr.substr(b + 1, c - b - 1);
                auto get = [&](const char* k) -> int {
                    std::string kk = std::string("\"") + k + "\":";
                    size_t pp = obj.find(kk);
                    if (pp == std::string::npos) { return 0; }
                    pp += kk.size();
                    return std::atoi(obj.c_str() + pp);
                };
                FontGlyph g{};
                g.cp = (uint32_t)get("cp");
                g.x = get("x");
                g.y = get("y");
                g.w = get("w");
                g.h = get("h");
                g.xoff = get("xoff");
                g.yoff = get("yoff");
                g.xadv = get("xadv");
                if (g.w > 0 && g.h > 0) {
                    const float invW = 1.0f / float(atlasW_);
                    const float invH = 1.0f / float(atlasH_);
                    const float padU = 0.5f;
                    const float padV = 0.5f;

                    g.u0 = (g.x + padU) * invW;
                    g.v0 = (g.y + padV) * invH;
                    g.u1 = (g.x + g.w - padU) * invW;
                    g.v1 = (g.y + g.h - padV) * invH;
                }
                else {
                    g.u0 = g.v0 = g.u1 = g.v1 = 0.0f;
                }
                glyphs_.push_back(g);
                i = c + 1;
            }
        }
    }

    // kern
    kerning_.clear();
    {
        size_t p = j.find("\"kern\":[");
        if (p != std::string::npos) {
            p = j.find('[', p);
            size_t e = j.find(']', p);
            std::string arr = j.substr(p + 1, e - p - 1);
            size_t i = 0;
            while (i < arr.size()) {
                size_t b = arr.find('{', i);
                if (b == std::string::npos) { break; }
                size_t c = arr.find('}', b);
                std::string obj = arr.substr(b + 1, c - b - 1);
                auto get = [&](const char* k) -> int {
                    std::string kk = std::string("\"") + k + "\":";
                    size_t pp = obj.find(kk);
                    if (pp == std::string::npos) { return 0; }
                    pp += kk.size();
                    return std::atoi(obj.c_str() + pp);
                };
                uint32_t a = (uint32_t)get("a");
                uint32_t b2 = (uint32_t)get("b");
                int k = get("k");
                KerningPair pair{};
                pair.key = ((uint64_t)a << 32) | (uint64_t)b2;
                pair.value = k;
                kerning_.push_back(pair);
                i = c + 1;
            }
        }
    }

    // загрузим атлас .tga → в RGBA8 и на GPU через твою Texture2D::CreateFromRGBA8
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

    // Отсортируем и дедуплицируем глифы (последнее в JSON имеет приоритет)
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

    // Если в JSON не было пробела — добавим синтетический с корректным advance
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

        itSpace = glyphs_.insert(itSpace, sp);
    }

    // Пересоберём remap (для компактных диапазонов используем прямой индекс)
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

    // Отсортируем и дедуплицируем кернинги
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

    return true;
}

const FontGlyph* FontAtlas::Find(uint32_t cp) const {
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
    const uint64_t key = ((uint64_t)a << 32) | (uint64_t)b;
    auto it = std::lower_bound(kerning_.begin(), kerning_.end(), key, [](const KerningPair& pair, uint64_t value) {
        return pair.key < value;
    });
    if (it == kerning_.end() || it->key != key) {
        return 0;
    }
    return it->value;
}
