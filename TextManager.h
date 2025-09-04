#pragma once
#include <vector>
#include <string>
#include <string_view>
#include <cstdint>
#include <optional>
#include <wrl/client.h>
#include "Material.h"
#include "RenderContext.h"
#include "Math.h"

class FontAtlas;
class Renderer;

class TextManager {
public:
    enum class Align : uint8_t { Left = 0, Center = 1, Right = 2 };
    using RegionId = uint32_t;

    void Init(Renderer* r);
    void Begin(UINT vpW, UINT vpH, float dpiScale = 1.0f);

    // Позиционные API
    void AddText(int x, int y, const float4& color, float px, std::wstring_view text);
    void AddText(int x, int y, const float4& color, float px, std::string_view utf8);
    void AddTextf(int x, int y, const float4& color, float px, const wchar_t* fmt, ...);

    // -------------------- Регионы --------------------
    RegionId CreateRegion(int x, int y, Align align = Align::Left);

    void RegionSetBackground(RegionId id, std::optional<float4> color);
    void RegionSetPadding(RegionId id, int padX, int padY);
    void RegionSetAlign(RegionId id, Align a);

    // фиксированная ширина региона (если задана — фон рисуем по ней)
    void RegionSetFixedWidth(RegionId id, float wPx);

    // отключить измерение ширины строк внутри региона (ускорение для Align::Left)
    void RegionSetAutoMeasure(RegionId id, bool enabled);

    void AddText(RegionId id, float px, const float4& color, std::wstring_view text);
    void AddText(RegionId id, float px, const float4& color, std::string_view utf8);
    void AddTextf(RegionId id, float px, const float4& color, const wchar_t* fmt, ...);

    void Build(Renderer* r, ID3D12GraphicsCommandList* cl);
    void Draw(Renderer* r, ID3D12GraphicsCommandList* cl);

    void SetFont(FontAtlas* f) { font_ = f; }
    void Clear();

private:
    struct Vertex { float3 pos; float4 col; float2 uv; };

    // Предподсчитанный «глиф-ран» одной строки
    struct GlyphRun {
        std::vector<const struct FontGlyph*> glyphs; // ptr на глифы в атласе
        std::vector<float> xOffsets;                 // кумулятивные X до каждого глифа (с кернингом)
        float scale = 1.0f;                          // px / fontPx
        bool  ready = false;
    };

    struct RegionLine {
        std::wstring text;        // исходная широкая строка
        float4       color;
        float        px = 16.0f;
        float        widthPx = 0; // ширина строки (для Center/Right)
        GlyphRun     run;         // кэш глифов/офсетов
    };

    struct Region {
        int   x = 0, y = 0;
        Align align = Align::Left;
        int   padX = 8, padY = 6;
        std::optional<float4> bg;

        std::optional<float> fixedWidthPx; // если есть — используем для фона/выравнивания
        bool  autoMeasure = true;          // если false и Align::Left — измерение строк не требуется

        std::vector<RegionLine> lines;
        float maxLineWidth = 0;
        int   totalLines = 0;
        int   lineStepPx = 18;
    };

private:
    static std::wstring UTF8toW(std::string_view s);

    // === Общий хелпер построения глиф-рана и ширины ===
    void  BuildGlyphRun(std::wstring_view text, float px, GlyphRun& outRun, float& outWidthPx) const;

    // Быстрый вывод подготовленного глиф-рана
    void  EmitGlyphRun(int x, int y, float xOffset, const float4& color, const GlyphRun& run);

    // Старая «неподготовленная» отрисовка (позиционная) — теперь через BuildGlyphRun
    void  EmitTextImmediate(int x, int y, const float4& color, float px, std::wstring_view text);

    // Рисунок прямоугольника (фон)
    void  EmitRect(int x, int y, float w, float h, const float4& color);

private:
    FontAtlas* font_ = nullptr;

    std::shared_ptr<Material> matText_;
    std::shared_ptr<Material> matRect_;

    std::vector<Vertex>      verts_;
    std::vector<uint32_t>    idx_;
    D3D12_VERTEX_BUFFER_VIEW vbv_{};
    D3D12_INDEX_BUFFER_VIEW  ibv_{};

    std::vector<Vertex>      rectVerts_;
    std::vector<uint32_t>    rectIdx_;
    D3D12_VERTEX_BUFFER_VIEW rectVBV_{};
    D3D12_INDEX_BUFFER_VIEW  rectIBV_{};

    std::vector<Region> regions_;

    UINT  vpW_ = 1, vpH_ = 1;
    float dpi_ = 1.0f;
};