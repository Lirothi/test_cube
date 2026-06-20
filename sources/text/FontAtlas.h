#pragma once
#include <array>
#include <vector>
#include <string>
#include <cstdint>
#include "materials/Texture2D.h"

class Renderer;

struct FontGlyph {
    uint32_t cp = 0;
    int x=0,y=0,w=0,h=0;
    int xoff=0,yoff=0,xadv=0;
    float u0=0, v0=0, u1=0, v1=0;
};

class FontAtlas {
public:
    enum class Type : uint8_t { SDF, Coverage };

    bool Load(Renderer* r, ID3D12GraphicsCommandList* uploadCl, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
              const std::wstring& jsonPath, const std::wstring& tgaPath);

    const FontGlyph* Find(uint32_t cp) const;
    int Kerning(uint32_t a, uint32_t b) const;
    int Ascent() const { return ascent_; }
    int Descent() const { return descent_; }
    int LineAdvance() const { return lineAdvance_; }
    int PxSize() const { return pxSize_; }
    int Spread() const { return spread_; }
    int AtlasWidth() const { return atlasW_; }
    int AtlasHeight() const { return atlasH_; }
    bool HasKerning() const { return kerning_.size() > 0; }
    Type GetType() const { return type_; }
    bool IsCoverage() const { return type_ == Type::Coverage; }

    D3D12_CPU_DESCRIPTOR_HANDLE GetSRVCPU() const { return tex_.GetSRVCPU(); }

private:
    void BuildLookupCaches();

private:
    static constexpr size_t kAsciiCacheSize = 128;

    Texture2D tex_;
    int pxSize_=0, spread_=0, atlasW_=0, atlasH_=0, ascent_=0, descent_=0, lineAdvance_=0;
    Type type_ = Type::SDF;
    std::vector<FontGlyph> glyphs_;
    std::vector<uint32_t> glyphRemap_;
    uint32_t glyphRemapBase_ = 0;
    std::array<const FontGlyph*, kAsciiCacheSize> asciiGlyphs_{};
    std::array<int16_t, kAsciiCacheSize * kAsciiCacheSize> asciiKerning_{};

    struct KerningPair {
        uint64_t key = 0;
        int value = 0;
    };
    std::vector<KerningPair> kerning_;
};
