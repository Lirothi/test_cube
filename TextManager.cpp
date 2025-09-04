#include <cstdarg>
#include <cstdio>
#include <optional>
#include <algorithm>
#include <cmath>
#include <array>
#include "TextManager.h"
#include "UploadManager.h"
#include "SamplerManager.h"
#include "InputLayoutManager.h"
#include "Renderer.h"
#include "FontAtlas.h"
#include "Profiler.h"

using Microsoft::WRL::ComPtr;

// форматтер
static std::string VFormat_(const char* fmt, va_list args) {
    if (fmt == nullptr) { return std::string(); }
    va_list copy; va_copy(copy, args);
    int needed = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    if (needed <= 0) { return std::string(); }
    std::string s; s.resize((size_t)needed);
    std::vsnprintf(s.data(), (size_t)needed + 1, fmt, args);
    return s;
}

// utf8 → wide
std::wstring TextManager::UTF8toW(const std::string& s) {
    if (s.empty()) { return L""; }
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w; w.resize((size_t)wlen);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), wlen);
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
    regions_.clear();
}

// позиционные
void TextManager::AddText(int x, int y, const float4& color, float px, const std::string& utf8) {
    EmitTextImmediate(x, y, color, px, utf8);
}
void TextManager::AddTextf(int x, int y, const float4& color, float px, const char* fmt, ...) {
    if (fmt == nullptr) { return; }
    va_list args; va_start(args, fmt);
    std::string s = VFormat_(fmt, args);
    va_end(args);
    if (!s.empty()) { EmitTextImmediate(x, y, color, px, s); }
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

void TextManager::AddText(RegionId id, float px, const float4& color, const std::string& utf8) {
    if (id >= regions_.size() || font_ == nullptr || utf8.empty()) { return; }
    Region& rg = regions_[id];

    RegionLine ln; ln.text = utf8; ln.color = color; ln.px = px;
    if (rg.autoMeasure || (rg.align != Align::Left)) {
        ln.widthPx = MeasureTextWidthPx(utf8, px);
        rg.maxLineWidth = std::max(rg.maxLineWidth, ln.widthPx);
    }
    else {
        ln.widthPx = 0.0f; // не нужен; фон/выравнивание берём из fixedWidthPx
    }
    rg.lines.push_back(std::move(ln));
    rg.totalLines = (int)rg.lines.size();
    rg.lineStepPx = (int)std::round(px + 2.0f);
}
void TextManager::AddTextf(RegionId id, float px, const float4& color, const char* fmt, ...) {
    if (fmt == nullptr) { return; }
    va_list args; va_start(args, fmt);
    std::string s = VFormat_(fmt, args);
    va_end(args);
    if (!s.empty()) { AddText(id, px, color, s); }
}

// сборка/отрисовка
void TextManager::Build(Renderer* r, ID3D12GraphicsCommandList* /*cl*/) {
    if (font_ == nullptr) { return; }
    CPU_SCOPE("TextManager::Build");

    // 1) фоны регионов
    for (const Region& rg : regions_) {
        if (!rg.bg.has_value() || rg.totalLines <= 0) { continue; }
        const float w = (rg.fixedWidthPx.has_value() ? rg.fixedWidthPx.value()
            : rg.maxLineWidth)
            + float(rg.padX * 2);
        const float h = float(rg.totalLines * rg.lineStepPx) + float(rg.padY * 2);
        const int   bx = rg.x - rg.padX;
        const int   by = rg.y - rg.padY;
        EmitRect(bx, by, w, h, rg.bg.value());
    }

    // 2) строки
    for (const Region& rg : regions_) {
        if (rg.totalLines <= 0) { continue; }
        const float regionW = (rg.fixedWidthPx.has_value() ? rg.fixedWidthPx.value()
            : rg.maxLineWidth);
        int y = rg.y;
        for (const RegionLine& ln : rg.lines) {
            float xOff = 0.0f;
            switch (rg.align) {
            case Align::Left:   xOff = 0.0f; break;
            case Align::Center: xOff = std::max(0.0f, 0.5f * (regionW - ln.widthPx)); break;
            case Align::Right:  xOff = std::max(0.0f, (regionW - ln.widthPx)); break;
            }
            EmitTextAt(rg.x, y, xOff, ln.color, ln.px, ln.text);
            y += rg.lineStepPx;
        }
    }

    // 3) аплоад
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
    CPU_SCOPE("TextManager::Draw");
    // 1) фон
    if (!rectVerts_.empty() && !rectIdx_.empty() && matRect_) {
        auto h = r->GetRenderContextPool()->Acquire();
        auto& rc = h.ref();

        // viewport как root-constants (b1)
        std::array<uint32_t, 8> k;
        auto f2u = [](float f) -> uint32_t { uint32_t u; std::memcpy(&u, &f, 4u); return u; };
        k[0] = f2u((float)vpW_);
        k[1] = f2u((float)vpH_);
        k[2] = 0u;
        k[3] = 0u;
        k[4] = 0u;
        k[5] = 0u;
        k[6] = 0u;
        k[7] = 0u;
        rc.constants[1] = {k.begin(), k.end()};

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

        // b1: viewport.xy, spread, pxSize
        std::array<uint32_t, 8> k;
        auto f2u = [](float f) -> uint32_t { uint32_t u; std::memcpy(&u, &f, 4u); return u; };
        k[0] = f2u((float)vpW_);
        k[1] = f2u((float)vpH_);
        k[2] = 0u;
        k[3] = 0u;
        k[4] = f2u((float)font_->Spread());
        k[5] = f2u((float)font_->PxSize());
        k[6] = 0u;
        k[7] = 0u;
        rc.constants[1] = {k.begin(), k.end()};

        rc.samplerTable[0] = r->GetSamplerManager()->GetTable(r, { SamplerManager::LinearClamp() });

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
}

// ===== приватные =====

float TextManager::MeasureTextWidthPx(const std::string& utf8, float px) const {
    if (font_ == nullptr || utf8.empty()) { return 0.0f; }
    CPU_SCOPE("TextManager::MeasureTextWidthPx");

    const float scale = px / float(font_->PxSize());
    float penX = 0.0f;
    std::wstring w = UTF8toW(utf8);
    uint32_t prev = 0;

    for (wchar_t wc : w) {
        if (wc == L'\n') { break; }
        if (wc == L' ' || wc == L'\t') {
            const FontGlyph* gsp = font_->Find((uint32_t)wc);
            float adv = 0.0f;
            if (gsp) { adv = float(gsp->xadv) * scale; }
            else {
                const FontGlyph* gn = font_->Find((uint32_t)'n');
                const float em = (gn ? float(gn->xadv) : float(font_->PxSize()));
                adv = (wc == L'\t' ? em * 2.0f : em * 0.5f) * scale;
            }
            penX += adv; prev = 0; continue;
        }
        const uint32_t cp = (uint32_t)wc;
        const FontGlyph* gph = font_->Find(cp);
        if (!gph) { prev = 0; continue; }
        if (prev) { penX += float(font_->Kerning(prev, cp)) * scale; }
        penX += float(gph->xadv) * scale;
        prev = cp;
    }
    return penX;
}

void TextManager::EmitTextImmediate(int x, int y, const float4& color, float px, const std::string& utf8) {
    if (font_ == nullptr) { return; }
    CPU_SCOPE("TextManager::EmitTextImmediate");

    const float scale = px / float(font_->PxSize());
    float penX = (float)x;
    float penY = (float)y + float(font_->Ascent()) * scale;

    std::wstring w = UTF8toW(utf8);
    uint32_t prev = 0;

    if (!w.empty()) {
        verts_.reserve(verts_.size() + w.size() * 4);
        idx_.reserve(idx_.size() + w.size() * 6);
    }

    for (wchar_t wc : w) {
        if (wc == L'\n') {
            penX = (float)x;
            penY += float(font_->LineAdvance()) * scale;
            prev = 0; continue;
        }
        if (wc == L' ' || wc == L'\t') {
            const FontGlyph* gsp = font_->Find((uint32_t)wc);
            float adv = 0.0f;
            if (gsp) { adv = float(gsp->xadv) * scale; }
            else {
                const FontGlyph* gn = font_->Find((uint32_t)'n');
                const float em = (gn ? float(gn->xadv) : float(font_->PxSize()));
                adv = (wc == L'\t' ? em * 2.0f : em * 0.5f) * scale;
            }
            penX += adv; prev = 0; continue;
        }

        const uint32_t cp = (uint32_t)wc;
        const FontGlyph* gph = font_->Find(cp);
        if (!gph) { prev = 0; continue; }
        if (prev) { penX += float(font_->Kerning(prev, cp)) * scale; }

        if (gph->w == 0 || gph->h == 0) {
            penX += float(gph->xadv) * scale;
            prev = cp; continue;
        }

        const float gx = penX + float(gph->xoff) * scale;
        const float gy = penY + float(gph->yoff) * scale;
        const float gw = float(gph->w) * scale;
        const float gh = float(gph->h) * scale;

        const uint32_t base = (uint32_t)verts_.size();
        verts_.push_back({ {gx,      gy,      0}, color, {gph->u0, gph->v0} });
        verts_.push_back({ {gx + gw, gy,      0}, color, {gph->u1, gph->v0} });
        verts_.push_back({ {gx + gw, gy + gh, 0}, color, {gph->u1, gph->v1} });
        verts_.push_back({ {gx,      gy + gh, 0}, color, {gph->u0, gph->v1} });

        idx_.push_back(base + 0u); idx_.push_back(base + 1u); idx_.push_back(base + 2u);
        idx_.push_back(base + 0u); idx_.push_back(base + 2u); idx_.push_back(base + 3u);

        penX += float(gph->xadv) * scale;
        prev = cp;
    }
}

void TextManager::EmitTextAt(int x, int y, float xOffset, const float4& color, float px, const std::string& utf8) {
    if (font_ == nullptr) { return; }
    CPU_SCOPE("TextManager::EmitTextAt");

    const float scale = px / float(font_->PxSize());
    float penX = (float)x + xOffset;
    float penY = (float)y + float(font_->Ascent()) * scale;

    std::wstring w = UTF8toW(utf8);
    uint32_t prev = 0;

    if (!w.empty()) {
        verts_.reserve(verts_.size() + w.size() * 4);
        idx_.reserve(idx_.size() + w.size() * 6);
    }

    for (wchar_t wc : w) {
        if (wc == L'\n') { break; } // одна строка
        if (wc == L' ' || wc == L'\t') {
            const FontGlyph* gsp = font_->Find((uint32_t)wc);
            float adv = 0.0f;
            if (gsp) { adv = float(gsp->xadv) * scale; }
            else {
                const FontGlyph* gn = font_->Find((uint32_t)'n');
                const float em = (gn ? float(gn->xadv) : float(font_->PxSize()));
                adv = (wc == L'\t' ? em * 2.0f : em * 0.5f) * scale;
            }
            penX += adv; prev = 0; continue;
        }

        const uint32_t cp = (uint32_t)wc;
        const FontGlyph* gph = font_->Find(cp);
        if (!gph) { prev = 0; continue; }
        if (prev) { penX += float(font_->Kerning(prev, cp)) * scale; }

        if (gph->w == 0 || gph->h == 0) {
            penX += float(gph->xadv) * scale;
            prev = cp; continue;
        }

        const float gx = penX + float(gph->xoff) * scale;
        const float gy = penY + float(gph->yoff) * scale;
        const float gw = float(gph->w) * scale;
        const float gh = float(gph->h) * scale;

        const uint32_t base = (uint32_t)verts_.size();
        verts_.push_back({ {gx,      gy,      0}, color, {gph->u0, gph->v0} });
        verts_.push_back({ {gx + gw, gy,      0}, color, {gph->u1, gph->v0} });
        verts_.push_back({ {gx + gw, gy + gh, 0}, color, {gph->u1, gph->v1} });
        verts_.push_back({ {gx,      gy + gh, 0}, color, {gph->u0, gph->v1} });

        idx_.push_back(base + 0u); idx_.push_back(base + 1u); idx_.push_back(base + 2u);
        idx_.push_back(base + 0u); idx_.push_back(base + 2u); idx_.push_back(base + 3u);

        penX += float(gph->xadv) * scale;
        prev = cp;
    }
}

void TextManager::EmitRect(int x, int y, float w, float h, const float4& color) {
    CPU_SCOPE("TextManager::EmitRect");
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