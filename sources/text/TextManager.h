#pragma once
#include <vector>
#include <array>
#include <cassert>
#include <string>
#include <string_view>
#include <cstdint>
#include <optional>
#include <memory>
#include <utility>
#include <algorithm>
#include <wrl/client.h>
#include "materials/Material.h"
#include "rendering/core/RenderContext.h"
#include "rendering/core/RenderConstants.h"
#include "core/math/Math.h"

class FontAtlas;
class Renderer;

class TextManager {
public:
    enum class Align : uint8_t { Left = 0, Center = 1, Right = 2 };
    using RegionId = uint32_t;

#ifndef TEXT_MANAGER_PERF_STATS
#if defined(_DEBUG)
#define TEXT_MANAGER_PERF_STATS 0
#else
#define TEXT_MANAGER_PERF_STATS 0
#endif
#endif

    static constexpr bool kPerfStatsEnabled = TEXT_MANAGER_PERF_STATS != 0;

    struct PerfStats {
        uint32_t regions = 0;
        uint32_t backgrounds = 0;
        uint32_t addTextCalls = 0;
        uint32_t addCachedTextCalls = 0;
        uint32_t addTextfCalls = 0;
        uint32_t positionalTextCalls = 0;
        uint32_t directLines = 0;
        uint32_t deferredLines = 0;
        uint32_t glyphRunBuilds = 0;
        uint32_t directEmitCalls = 0;
        uint32_t runEmitCalls = 0;
        uint32_t inputChars = 0;
        uint32_t directGlyphs = 0;
        uint32_t runGlyphs = 0;
        uint32_t retargetedVertices = 0;

        double beginUs = 0.0;
        double addTextUs = 0.0;
        double addCachedTextUs = 0.0;
        double formatUs = 0.0;
        double buildGlyphRunUs = 0.0;
        double directEmitUs = 0.0;
        double directEmitReserveUs = 0.0;
        double directEmitSetupUs = 0.0;
        double directEmitLoopUs = 0.0;
        double runEmitUs = 0.0;
        double lineRetargetUs = 0.0;
        double buildUs = 0.0;
        double buildReserveUs = 0.0;
        double buildRegionsUs = 0.0;
        double uploadRectUs = 0.0;
        double uploadTextUs = 0.0;
        double drawUs = 0.0;
    };

    struct ShadowDesc {
        float offsetX = 1.0f;
        float offsetY = 1.0f;
        float4 color = float4(0.0f, 0.0f, 0.0f, 0.65f);
        float alphaMultiplier = 1.0f;
        bool  scaleWithDpi = true;
        bool  scaleWithTextSize = false;
    };

    void Init(Renderer* r);
    void Begin(UINT vpW, UINT vpH, float dpiScale = 1.0f);

    void SetShadow(const ShadowDesc& desc);
    void DisableShadow();

    // Positional text API
    void AddText(int x, int y, float px, const float4& color, std::wstring_view text);
    void AddText(int x, int y, float px, const float4& color, std::string_view utf8);
    void AddText(int x, int y, float px, const float4& color, std::wstring_view text, bool enableShadow);
    void AddText(int x, int y, float px, const float4& color, std::string_view utf8, bool enableShadow);
    void AddTextf(int x, int y, float px, const float4& color, const wchar_t* fmt, ...);
    void AddTextfShadow(int x, int y, float px, const float4& color, bool enableShadow, const wchar_t* fmt, ...);

    // -------------------- Regions --------------------
    RegionId CreateRegion(int x, int y, Align align = Align::Left);

    void RegionSetBackground(RegionId id, std::optional<float4> color);
    void RegionSetPadding(RegionId id, int padX, int padY);
    void RegionSetAlign(RegionId id, Align a);

    // Fixed region width (if set, the background uses it)
    void RegionSetFixedWidth(RegionId id, float wPx);

    // Disable per-line width measurement inside the region (faster for Align::Left)
    void RegionSetAutoMeasure(RegionId id, bool enabled);

    // Disable kerning for debug/profiler-style aligned text.
    void RegionSetKerning(RegionId id, bool enabled);

    void AddText(RegionId id, float px, const float4& color, std::wstring_view text);
    void AddText(RegionId id, float px, const float4& color, std::string_view utf8);
    void AddText(RegionId id, float px, const float4& color, std::wstring_view text, bool enableShadow);
    void AddText(RegionId id, float px, const float4& color, std::string_view utf8, bool enableShadow);
    void AddCachedText(RegionId id, float px, const float4& color, std::wstring_view text, bool enableShadow);
    void AddTextf(RegionId id, float px, const float4& color, const wchar_t* fmt, ...);
    void AddTextfShadow(RegionId id, float px, const float4& color, bool enableShadow, const wchar_t* fmt, ...);

    void Build(Renderer* r, ID3D12GraphicsCommandList* cl);
    void Draw(Renderer* r, ID3D12GraphicsCommandList* cl);

    void SetFont(FontAtlas* f);
    void Clear();
    const PerfStats& GetPerfStats() const noexcept { return lastPerf_; }
    bool GetPerfStatsEnabled() const noexcept { return perfStatsEnabled_; }
    void SetPerfStatsEnabled(bool enabled) noexcept {
        if (perfStatsEnabled_ == enabled) { return; }
        perfStatsEnabled_ = enabled;
        framePerf_ = {};
        lastPerf_ = {};
    }

private:
    struct Vertex {
        float2 pos;
        uint32_t col;
        uint32_t uv; // R16G16_UNORM
        uint32_t shadowParams; // half2: x = offset scale factor, y = final shadow alpha
    };
    static_assert(sizeof(Vertex) == 20, "Text vertex packing expects a 20-byte stride");

    // Precomputed glyph run for a single line
    struct GlyphRun {
        static constexpr size_t kDefaultCapacity = 256;

        std::array<const struct FontGlyph*, kDefaultCapacity> glyphs{}; // pointers to glyphs in the atlas
        std::array<float, kDefaultCapacity> xOffsets{};                  // cumulative X offset before each glyph (with kerning)
        size_t glyphCount = 0;
        float scale = 1.0f;   // px / fontPx
        bool  ready = false;

        void Reset() {
            glyphCount = 0;
            scale = 1.0f;
            ready = false;
        }

        void Append(const struct FontGlyph* glyph, float xOffset) {
            if (glyphCount >= kDefaultCapacity) {
                assert(glyphCount < kDefaultCapacity);
                return;
            }
            glyphs[glyphCount] = glyph;
            xOffsets[glyphCount] = xOffset;
            ++glyphCount;
        }

        const struct FontGlyph* GlyphAt(size_t index) const noexcept {
            assert(index < glyphCount);
            return glyphs[index];
        }
        float XOffsetAt(size_t index) const noexcept {
            assert(index < glyphCount);
            return xOffsets[index];
        }
    };

    struct RegionLine {
        float4   color;
        float    px = 16.0f;
        float    widthPx = 0;   // line width (used for Center/Right)
        GlyphRun run;           // cached glyphs/offsets
        size_t   directVertexFirst = 0;
        size_t   directVertexCount = 0;
        uint32_t glyphCount = 0; // glyph reserve count
        int      lineIndex = 0;
        int      emittedY = 0;
        bool     inUse = false;
        bool     shadowEnabled = false;
        bool     emittedDirect = false;
    };

    struct Region {
        int   x = 0, y = 0;
        Align align = Align::Left;
        int   padX = 8, padY = 6;
        std::optional<float4> bg;

        std::optional<float> fixedWidthPx; // use for background/alignment when provided
        bool  autoMeasure = true;          // when false and Align::Left, skip line width measurement

        bool  kerning = true;

        std::vector<RegionLine*> lines;
        float maxLineWidth = 0;
        int   totalLines = 0;
        int   lineStepPx = 18;
        size_t glyphCount = 0;
    };

    struct CachedGlyphRun {
        std::wstring text;
        float px = 0.0f;
        GlyphRun run;
        float widthPx = 0.0f;
    };

private:
    static constexpr size_t kRegionLinePoolCapacity = 128;

    template<typename T>
    class GrowOnlyArray {
    public:
        void clear() noexcept { size_ = 0; }
        [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
        [[nodiscard]] size_t size() const noexcept { return size_; }
        [[nodiscard]] size_t capacity() const noexcept { return capacity_; }
        [[nodiscard]] T* data() noexcept { return storage_.get(); }
        [[nodiscard]] const T* data() const noexcept { return storage_.get(); }

        void ensureAdditional(size_t additional) {
            if (additional == 0) { return; }
            ensureCapacity(size_ + additional);
        }

        size_t appendUninitialized(size_t count) {
            if (count == 0) { return size_; }
            ensureCapacity(size_ + count);
            const size_t base = size_;
            size_ += count;
            return base;
        }

        size_t appendUninitializedReserved(size_t count) {
            if (count == 0) { return size_; }
            assert(size_ + count <= capacity_);
            const size_t base = size_;
            size_ += count;
            return base;
        }

        void resizeReserved(size_t size) {
            assert(size <= capacity_);
            size_ = size;
        }

    private:
        void ensureCapacity(size_t required) {
            if (required <= capacity_) { return; }

            size_t newCapacity = (capacity_ != 0) ? capacity_ : kInitialCapacity;
            if (newCapacity == 0) { newCapacity = 1; }
            while (newCapacity < required) {
                const size_t next = newCapacity * 2;
                if (next <= newCapacity) { // overflow guard
                    newCapacity = required;
                    break;
                }
                newCapacity = next;
            }

            std::unique_ptr<T[]> newStorage = std::make_unique<T[]>(newCapacity);
            if (storage_) {
                std::copy_n(storage_.get(), size_, newStorage.get());
            }
            storage_.swap(newStorage);
            capacity_ = newCapacity;
        }

        static constexpr size_t kInitialCapacity = 256;

        std::unique_ptr<T[]> storage_;
        size_t size_ = 0;
        size_t capacity_ = 0;
    };

    static std::wstring UTF8toW(std::string_view s);

    // === Shared helper to build a glyph run and compute width ===
    void  BuildGlyphRun(std::wstring_view text, float px, GlyphRun& outRun, float& outWidthPx) const;
    const CachedGlyphRun& GetCachedGlyphRun(std::wstring_view text, float px);
    static bool CanEmitRegionLineImmediately(const Region& rg) noexcept;
    void  SetRegionLineStep(Region& rg, int lineStepPx);
    size_t EmitTextDirect(int x, int y, float px, const float4& color, std::wstring_view text, bool enableShadow, bool useKerning, float* outWidthPx);

    // Fast rendering of a prepared glyph run
    void  EmitGlyphRun(int x, int y, float xOffset, const float4& color, const GlyphRun& run, bool enableShadow);
    void  EmitGlyphRunReserved(int x, int y, float xOffset, const float4& color, const GlyphRun& run, bool enableShadow);
    void  EmitGlyphRunImpl(int x, int y, float xOffset, const float4& color, const GlyphRun& run, bool enableShadow, bool reservedAppend);

    // Legacy positional rendering now routed through BuildGlyphRun
    void  EmitTextImmediate(int x, int y, float px, const float4& color, std::wstring_view text, bool enableShadow);

    // Render a rectangle (background)
    void  EmitRect(int x, int y, float w, float h, const float4& color);
    void  EmitRectReserved(int x, int y, float w, float h, const float4& color);
    void  EmitRectImpl(int x, int y, float w, float h, const float4& color, bool reservedAppend);

    // CPU half of Build(): reserve + emit region backgrounds/lines into the
    // vertex arrays. No Renderer dependency, so it is callable headless (the
    // benchmark uses it to measure AddText+emit without a GPU device).
    void  BuildVerticesCPU();

    RegionLine* AcquireRegionLine(size_t glyphReserveHint);
    void       RecycleRegionLines();
    void       EnsureTextIndexCapacity(Renderer* r, size_t quadCount);
    void       CollectRetiredTextIndexBuffers(Renderer* r);
    void       RetireTextIndexBuffer(Renderer* r);

private:
    FontAtlas* font_ = nullptr;

    std::shared_ptr<Material> matTextSdf_;
    std::shared_ptr<Material> matTextCoverage_;
    std::shared_ptr<Material> matRect_;

    GrowOnlyArray<Vertex>    verts_;
    D3D12_VERTEX_BUFFER_VIEW vbv_{};
    D3D12_INDEX_BUFFER_VIEW  textIBV_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> textIndexBuffer_;
    std::array<std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>, render::kFrameCount> retiredTextIndexBuffers_;
    uint32_t retiredTextIndexFrameMask_ = 0;
    size_t textIndexQuadCapacity_ = 0;

    GrowOnlyArray<Vertex>    rectVerts_;
    GrowOnlyArray<uint32_t>  rectIdx_;
    D3D12_VERTEX_BUFFER_VIEW rectVBV_{};
    D3D12_INDEX_BUFFER_VIEW  rectIBV_{};

    std::vector<Region> regions_;
    std::vector<Region> regionPool_;
    std::array<RegionLine, kRegionLinePoolCapacity> regionLinePool_{};
    size_t nextUnusedRegionLine_ = 0;
    std::vector<RegionLine*> freeRegionLines_;
    std::vector<CachedGlyphRun> cachedGlyphRuns_;
    const FontAtlas* cachedGlyphRunsFont_ = nullptr;
    size_t frameRegionGlyphCount_ = 0;
    size_t frameBackgroundRectCount_ = 0;
    mutable PerfStats framePerf_{};
    PerfStats lastPerf_{};
    bool perfStatsEnabled_ = false;

    UINT  vpW_ = 1, vpH_ = 1;
    float dpi_ = 1.0f;
    std::optional<ShadowDesc> shadow_;

    // Headless benchmark needs the private CPU-only emit path (BuildVerticesCPU).
    friend int RunTextManagerBenchmark(const char* outputPath);
};

int RunTextManagerBenchmark(const char* outputPath);
