#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <optional>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>
#include "TextManager.h"
#include "UploadManager.h"
#include "SamplerManager.h"
#include "InputLayoutManager.h"
#include "Renderer.h"
#include "FontAtlas.h"
#include "Profiler.h"

using Microsoft::WRL::ComPtr;

// utf8 → wide
std::wstring TextManager::UTF8toW(std::string_view s) {
    if (s.empty()) { return L""; }
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w; w.resize((size_t)wlen);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), wlen);
    return w;
}

void TextManager::Init(Renderer* r) {
    // текст (SDF)
    {
        Material::GraphicsDesc gd;
        gd.shaderFile = L"shaders/font_sdf.hlsl";
        gd.inputLayoutKey = "PosColorUV";
        gd.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        gd.depth.DepthEnable = FALSE;
        gd.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        gd.raster.CullMode = D3D12_CULL_MODE_NONE;
        auto& b = gd.blend.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ONE;
        b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        matText_ = r->GetMaterialManager()->GetOrCreateGraphics(r, gd);
    }
    // прямоугольник (фон)
    {
        Material::GraphicsDesc gd;
        gd.shaderFile = L"shaders/ui_rect.hlsl";
        gd.vsEntry = "VSMain";
        gd.psEntry = "PSMain";
        gd.inputLayoutKey = "PosColorUV";
        gd.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        gd.depth.DepthEnable = FALSE;
        gd.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        gd.raster.CullMode = D3D12_CULL_MODE_NONE;
        auto& b = gd.blend.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ONE;
        b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        matRect_ = r->GetMaterialManager()->GetOrCreateGraphics(r, gd);
    }
}

void TextManager::Begin(UINT vpW, UINT vpH, float dpiScale) {
    vpW_ = (vpW == 0u ? 1u : vpW);
    vpH_ = (vpH == 0u ? 1u : vpH);
    dpi_ = (dpiScale <= 0.f ? 1.f : dpiScale);

    verts_.clear();   idx_.clear();
    rectVerts_.clear(); rectIdx_.clear();
    RecycleRegionLines();
    regions_.clear();
}

// позиционные (не кэшируем — как было)
void TextManager::AddText(int x, int y, const float4& color, float px, std::wstring_view text) {
    EmitTextImmediate(x, y, color, px, text);
}
void TextManager::AddText(int x, int y, const float4& color, float px, std::string_view utf8) {
    AddText(x, y, color, px, UTF8toW(utf8));
}
void TextManager::AddTextf(int x, int y, const float4& color, float px, const wchar_t* fmt, ...) {
    if (fmt == nullptr) { return; }
    wchar_t buf[256];
    va_list args; va_start(args, fmt);
    int len = std::vswprintf(buf, sizeof(buf) / sizeof(wchar_t), fmt, args);
    va_end(args);
    if (len > 0) { AddText(x, y, color, px, std::wstring_view(buf, (size_t)len)); }
}

// регионы
TextManager::RegionId TextManager::CreateRegion(int x, int y, Align align) {
    Region r; r.x = x; r.y = y; r.align = align;
    regions_.push_back(std::move(r));
    return (RegionId)(regions_.size() - 1);
}
void TextManager::RegionSetBackground(RegionId id, std::optional<float4> color) {
    if (id >= regions_.size()) { return; }
    regions_[id].bg = color;
}
void TextManager::RegionSetPadding(RegionId id, int padX, int padY) {
    if (id >= regions_.size()) { return; }
    regions_[id].padX = padX; regions_[id].padY = padY;
}
void TextManager::RegionSetAlign(RegionId id, Align a) {
    if (id >= regions_.size()) { return; }
    regions_[id].align = a;
}
void TextManager::RegionSetFixedWidth(RegionId id, float wPx) {
    if (id >= regions_.size()) { return; }
    regions_[id].fixedWidthPx = wPx;
}
void TextManager::RegionSetAutoMeasure(RegionId id, bool enabled) {
    if (id >= regions_.size()) { return; }
    regions_[id].autoMeasure = enabled;
}

void TextManager::AddText(RegionId id, float px, const float4& color, std::wstring_view text) {
    if (id >= regions_.size() || font_ == nullptr || text.empty()) { return; }
    Region& rg = regions_[id];

    const size_t charCount = text.size();
    RegionLine ln = AcquireRegionLine(charCount);
    ln.color = color;
    ln.px = px;
    BuildGlyphRun(text, ln.px, ln.run, ln.widthPx);
    const size_t glyphReserve = (ln.run.ready ? ln.run.glyphs.size() : charCount);
    ln.glyphCount = (uint32_t)std::min<size_t>(glyphReserve, std::numeric_limits<uint32_t>::max());

    if (rg.autoMeasure || (rg.align != Align::Left)) {
        rg.maxLineWidth = std::max(rg.maxLineWidth, ln.widthPx);
    }
    else {
        // для Align::Left + fixedWidth измерение не обязательно (ширина уже в ln.widthPx)
    }

    rg.glyphCount += glyphReserve;
    rg.lines.push_back(std::move(ln));
    rg.totalLines = (int)rg.lines.size();
    rg.lineStepPx = (int)std::round(px + 2.0f);
}
void TextManager::AddText(RegionId id, float px, const float4& color, std::string_view utf8) {
    AddText(id, px, color, UTF8toW(utf8));
}
void TextManager::AddTextf(RegionId id, float px, const float4& color, const wchar_t* fmt, ...) {
    if (fmt == nullptr) { return; }
    wchar_t buf[256];
    va_list args; va_start(args, fmt);
    int len = std::vswprintf(buf, sizeof(buf) / sizeof(wchar_t), fmt, args);
    va_end(args);
    if (len > 0) { AddText(id, px, color, std::wstring_view(buf, (size_t)len)); }
}

// ======== СБОРКА / ОТРИСОВКА ========
void TextManager::Build(Renderer* r, ID3D12GraphicsCommandList* /*cl*/) {
    if (font_ == nullptr) { return; }
    CPU_SCOPE(L"TextManager::Build");

    // 0) Предподсчёт глифов для единого reserve
    size_t totalGlyphs = 0;
    for (const Region& rg : regions_) {
        if (rg.totalLines <= 0) { continue; }
        totalGlyphs += rg.glyphCount;
    }
    if (totalGlyphs) {
        verts_.reserve(verts_.size() + totalGlyphs * 4);
        idx_.reserve(idx_.size() + totalGlyphs * 6);
    }

    // 1) Резерв под потенциальные фоновые прямоугольники
    rectVerts_.reserve(rectVerts_.size() + regions_.size() * 4);
    rectIdx_.reserve(rectIdx_.size() + regions_.size() * 6);

    // 2) Один проход по регионам: фоны и строки
    for (const Region& rg : regions_) {
        if (rg.totalLines <= 0) { continue; }

        // фон
        if (rg.bg.has_value()) {
            const float w = (rg.fixedWidthPx.has_value() ? rg.fixedWidthPx.value() : rg.maxLineWidth)
                + float(rg.padX * 2);
            const float h = float(rg.totalLines * rg.lineStepPx) + float(rg.padY * 2);
            const int   bx = rg.x - rg.padX;
            const int   by = rg.y - rg.padY;
            EmitRect(bx, by, w, h, rg.bg.value());
        }

        // строки
        const float regionW = (rg.fixedWidthPx.has_value() ? rg.fixedWidthPx.value() : rg.maxLineWidth);
        int y = rg.y;
        for (const RegionLine& ln : rg.lines) {
            float xOff = 0.0f;
            switch (rg.align) {
            case Align::Left: { xOff = 0.0f; break; }
            case Align::Center: { xOff = std::max(0.0f, 0.5f * (regionW - ln.widthPx)); break; }
            case Align::Right: { xOff = std::max(0.0f, (regionW - ln.widthPx)); break; }
            }
            EmitGlyphRun(rg.x, y, xOff, ln.color, ln.run);
            y += rg.lineStepPx;
        }
    }

    // 3) Аплоад
    FrameResource* fr = r->GetFrameResource();

    // rects
    {
        const UINT vbBytes = (UINT)(rectVerts_.size() * sizeof(Vertex));
        const UINT ibBytes = (UINT)(rectIdx_.size() * sizeof(uint32_t));
        auto v = fr->AllocDynamic(vbBytes, 16);
        auto i = fr->AllocDynamic(ibBytes, 16);
        if (vbBytes) { std::memcpy(v.cpu, rectVerts_.data(), vbBytes); }
        if (ibBytes) { std::memcpy(i.cpu, rectIdx_.data(), ibBytes); }
        rectVBV_.BufferLocation = v.gpu;
        rectVBV_.StrideInBytes = sizeof(Vertex);
        rectVBV_.SizeInBytes = vbBytes;
        rectIBV_.BufferLocation = i.gpu;
        rectIBV_.Format = DXGI_FORMAT_R32_UINT;
        rectIBV_.SizeInBytes = ibBytes;
    }

    // text
    {
        const UINT vbBytes = (UINT)(verts_.size() * sizeof(Vertex));
        const UINT ibBytes = (UINT)(idx_.size() * sizeof(uint32_t));
        auto v = fr->AllocDynamic(vbBytes, 16);
        auto i = fr->AllocDynamic(ibBytes, 16);
        if (vbBytes) { std::memcpy(v.cpu, verts_.data(), vbBytes); }
        if (ibBytes) { std::memcpy(i.cpu, idx_.data(), ibBytes); }
        vbv_.BufferLocation = v.gpu;
        vbv_.StrideInBytes = sizeof(Vertex);
        vbv_.SizeInBytes = vbBytes;
        ibv_.BufferLocation = i.gpu;
        ibv_.Format = DXGI_FORMAT_R32_UINT;
        ibv_.SizeInBytes = ibBytes;
    }
}

void TextManager::Draw(Renderer* r, ID3D12GraphicsCommandList* cl) {
    CPU_SCOPE(L"TextManager::Draw");
    // 1) фон
    if (!rectVerts_.empty() && !rectIdx_.empty() && matRect_) {
        auto h = r->GetRenderContextPool()->Acquire();
        auto& rc = h.ref();

        std::array<uint32_t, 8> k{};
        auto f2u = [](float f)->uint32_t { uint32_t u; std::memcpy(&u, &f, 4u); return u; };
        k[0] = f2u((float)vpW_);
        k[1] = f2u((float)vpH_);
        rc.constants[1] = { k.begin(), k.end() };

        matRect_->Bind(cl, rc);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->IASetVertexBuffers(0, 1, &rectVBV_);
        cl->IASetIndexBuffer(&rectIBV_);
        cl->DrawIndexedInstanced((UINT)rectIdx_.size(), 1, 0, 0, 0);
    }

    // 2) текст
    if (!verts_.empty() && !idx_.empty() && matText_) {
        auto h = r->GetRenderContextPool()->Acquire();
        auto& rc = h.ref();

        auto tbl = r->StageSrvUavTable({ font_->GetSRVCPU() });
        rc.table[0] = tbl.gpu;

        std::array<uint32_t, 8> k{};
        auto f2u = [](float f)->uint32_t { uint32_t u; std::memcpy(&u, &f, 4u); return u; };
        k[0] = f2u((float)vpW_);
        k[1] = f2u((float)vpH_);
        k[4] = f2u((float)font_->Spread());
        k[5] = f2u((float)font_->PxSize());
        rc.constants[1] = { k.begin(), k.end() };

        const auto samplerDescs = std::array{ SamplerManager::LinearClamp() };
        rc.samplerTable[0] = r->GetSamplerManager()->GetTable(r, samplerDescs);

        matText_->Bind(cl, rc);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->IASetVertexBuffers(0, 1, &vbv_);
        cl->IASetIndexBuffer(&ibv_);
        cl->DrawIndexedInstanced((UINT)idx_.size(), 1, 0, 0, 0);
    }
}

void TextManager::Clear() {
    matText_.reset(); matRect_.reset();
    verts_.clear(); idx_.clear();
    rectVerts_.clear(); rectIdx_.clear();
    RecycleRegionLines();
}

TextManager::RegionLine TextManager::AcquireRegionLine(size_t glyphReserveHint) {
    RegionLine ln;
    if (!regionLinePool_.empty()) {
        ln = std::move(regionLinePool_.back());
        regionLinePool_.pop_back();
    }

    ln.color = {};
    ln.px = 16.0f;
    ln.widthPx = 0.0f;
    ln.glyphCount = 0;

    ln.run.glyphs.clear();
    ln.run.xOffsets.clear();
    ln.run.scale = 1.0f;
    ln.run.ready = false;

    if (glyphReserveHint > 0) {
        if (ln.run.glyphs.capacity() < glyphReserveHint) {
            ln.run.glyphs.reserve(glyphReserveHint);
        }
        if (ln.run.xOffsets.capacity() < glyphReserveHint) {
            ln.run.xOffsets.reserve(glyphReserveHint);
        }
    }

    return ln;
}

void TextManager::RecycleRegionLines() {
    for (Region& rg : regions_) {
        for (RegionLine& ln : rg.lines) {
            ln.run.glyphs.clear();
            ln.run.xOffsets.clear();
            ln.run.ready = false;
            ln.widthPx = 0.0f;
            ln.glyphCount = 0;
            regionLinePool_.push_back(std::move(ln));
        }
        rg.lines.clear();
        rg.maxLineWidth = 0.0f;
        rg.totalLines = 0;
        rg.glyphCount = 0;
        rg.lineStepPx = 18;
    }
}

// ===== приватные =====

// Общий хелпер построения глиф-рана + ширины (один проход)
void TextManager::BuildGlyphRun(std::wstring_view text, float px, GlyphRun& outRun, float& outWidthPx) const {
    outRun.glyphs.clear();
    outRun.xOffsets.clear();
    outRun.scale = px / float(font_->PxSize());
    outRun.ready = false;
    outWidthPx = 0.0f;

    if (font_ == nullptr || text.empty()) { return; }

    const FontAtlas* font = font_;
    const float scale = outRun.scale;

    outRun.glyphs.reserve(text.size());
    outRun.xOffsets.reserve(text.size());

    const FontGlyph* glyphSpace = font->Find((uint32_t)L' ');
    const FontGlyph* glyphTab   = font->Find((uint32_t)L'\t');
    const FontGlyph* glyphN     = font->Find((uint32_t)'n');
    const float emAdvance       = (glyphN ? float(glyphN->xadv) : float(font->PxSize()));
    const float spaceAdvance    = (glyphSpace ? float(glyphSpace->xadv) : emAdvance * 0.5f) * scale;
    const float tabAdvance      = (glyphTab ? float(glyphTab->xadv) : emAdvance * 2.0f) * scale;

    float penX = 0.0f;
    uint32_t prev = 0;

    std::array<const FontGlyph*, 128> asciiGlyphs{};
    std::array<uint8_t, 128> asciiGlyphReady{};
    asciiGlyphReady.fill(0);

    const wchar_t* cur = text.data();
    const wchar_t* const end = cur + text.size();
    for (; cur != end; ++cur) {
        const wchar_t wc = *cur;
        if (wc == L'\n') { break; }
        if (wc == L' ') {
            penX += spaceAdvance;
            prev = 0;
            continue;
        }
        if (wc == L'\t') {
            penX += tabAdvance;
            prev = 0;
            continue;
        }

        const uint32_t cp = (uint32_t)wc;
        const FontGlyph* gph = nullptr;
        if (cp < asciiGlyphs.size()) {
            uint8_t& state = asciiGlyphReady[cp];
            if (!state) {
                asciiGlyphs[cp] = font->Find(cp);
                state = 1;
            }
            gph = asciiGlyphs[cp];
        }
        else {
            gph = font->Find(cp);
        }
        if (!gph) {
            prev = 0;
            continue;
        }

        if (prev) {
            const int kern = font->Kerning(prev, cp);
            if (kern) {
                penX += float(kern) * scale;
            }
        }

        outRun.xOffsets.push_back(penX);
        outRun.glyphs.push_back(gph);

        penX += float(gph->xadv) * scale;
        prev = cp;
    }

    outWidthPx = penX;
    outRun.ready = true;
}

// Быстрый эмит с подготовленного глиф-рана
void TextManager::EmitGlyphRun(int x, int y, float xOffset, const float4& color, const GlyphRun& run) {
    if (!run.ready || run.glyphs.empty()) { return; }

    const float scale = run.scale;
    const float penY = (float)y + float(font_->Ascent()) * scale;

    const size_t n = run.glyphs.size();
    if (n == 0) { return; }

    size_t drawable = 0;
    for (size_t i = 0; i < n; ++i) {
        const FontGlyph* gph = run.glyphs[i];
        if (gph && gph->w != 0 && gph->h != 0) { ++drawable; }
    }
    if (drawable == 0) { return; }

    verts_.reserve(verts_.size() + drawable * 4);
    idx_.reserve(idx_.size() + drawable * 6);

    const size_t baseVert = verts_.size();
    const size_t baseIdx = idx_.size();
    verts_.resize(baseVert + drawable * 4);
    idx_.resize(baseIdx + drawable * 6);

    Vertex* vPtr = verts_.data() + baseVert;
    uint32_t* iPtr = idx_.data() + baseIdx;
    size_t emitted = 0;

    for (size_t i = 0; i < n; ++i) {
        const FontGlyph* gph = run.glyphs[i];
        if (!gph || gph->w == 0 || gph->h == 0) { continue; }
        const float penX = (float)x + xOffset + run.xOffsets[i];

        const float gx = penX + float(gph->xoff) * scale;
        const float gy = penY + float(gph->yoff) * scale;
        const float gw = float(gph->w) * scale;
        const float gh = float(gph->h) * scale;

        Vertex* curV = vPtr + emitted * 4;
        curV[0] = { {gx,      gy,      0}, color, {gph->u0, gph->v0} };
        curV[1] = { {gx + gw, gy,      0}, color, {gph->u1, gph->v0} };
        curV[2] = { {gx + gw, gy + gh, 0}, color, {gph->u1, gph->v1} };
        curV[3] = { {gx,      gy + gh, 0}, color, {gph->u0, gph->v1} };

        const uint32_t base = (uint32_t)(baseVert + emitted * 4);
        uint32_t* curI = iPtr + emitted * 6;
        curI[0] = base + 0u; curI[1] = base + 1u; curI[2] = base + 2u;
        curI[3] = base + 0u; curI[4] = base + 2u; curI[5] = base + 3u;

        ++emitted;
    }
}

// Позиционная отрисовка теперь тоже через BuildGlyphRun + EmitGlyphRun
void TextManager::EmitTextImmediate(int x, int y, const float4& color, float px, std::wstring_view text) {
    if (font_ == nullptr) { return; }
    CPU_SCOPE(L"TextManager::EmitTextImmediate");
    GlyphRun run;
    float width = 0.0f;
    BuildGlyphRun(text, px, run, width);
    EmitGlyphRun(x, y, 0.0f, color, run);
}

void TextManager::EmitRect(int x, int y, float w, float h, const float4& color) {
    const float gx = (float)x, gy = (float)y;
    const float gw = w, gh = h;
    const uint32_t base = (uint32_t)rectVerts_.size();
    rectVerts_.push_back({ {gx,      gy,      0}, color, {0,0} });
    rectVerts_.push_back({ {gx + gw, gy,      0}, color, {0,0} });
    rectVerts_.push_back({ {gx + gw, gy + gh, 0}, color, {0,0} });
    rectVerts_.push_back({ {gx,      gy + gh, 0}, color, {0,0} });
    rectIdx_.push_back(base + 0u); rectIdx_.push_back(base + 1u); rectIdx_.push_back(base + 2u);
    rectIdx_.push_back(base + 0u); rectIdx_.push_back(base + 2u); rectIdx_.push_back(base + 3u);
}