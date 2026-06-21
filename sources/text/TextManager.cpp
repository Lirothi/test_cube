#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <optional>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <string_view>
#include <DirectXPackedVector.h>
#include "text/TextManager.h"
#include "materials/UploadManager.h"
#include "rendering/descriptors/SamplerManager.h"
#include "rendering/descriptors/InputLayoutManager.h"
#include "rendering/core/Renderer.h"
#include "text/FontAtlas.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"

using Microsoft::WRL::ComPtr;

#ifndef TEXT_MANAGER_FINE_PROFILING
#define TEXT_MANAGER_FINE_PROFILING 0
#endif

#if TEXT_MANAGER_FINE_PROFILING
#define TEXT_MANAGER_FINE_CPU_SCOPE(keyExpr) CPU_SCOPE(keyExpr)
#else
#define TEXT_MANAGER_FINE_CPU_SCOPE(keyExpr) do { } while (0)
#endif

#if TEXT_MANAGER_PERF_STATS
namespace {
class TextPerfScope {
public:
    TextPerfScope(double& targetUs, bool enabled) noexcept
        : targetUs_(targetUs), enabled_(enabled) {
        if (enabled_) {
            start_ = Clock::now();
        }
    }

    ~TextPerfScope() {
        if (!enabled_) { return; }
        const auto end = Clock::now();
        targetUs_ += std::chrono::duration<double, std::micro>(end - start_).count();
    }

private:
    using Clock = std::chrono::steady_clock;
    double& targetUs_;
    Clock::time_point start_;
    bool enabled_ = false;
};
} // namespace

#define TEXT_PERF_JOIN2(a, b) a##b
#define TEXT_PERF_JOIN(a, b) TEXT_PERF_JOIN2(a, b)
#define TEXT_PERF_SCOPE(field) TextPerfScope TEXT_PERF_JOIN(_textPerfScope_, __LINE__)(framePerf_.field, perfStatsEnabled_)
#define TEXT_PERF_ADD(field, value) do { if (perfStatsEnabled_) { framePerf_.field += (value); } } while (0)
#else
#define TEXT_PERF_SCOPE(field) do { } while (0)
#define TEXT_PERF_ADD(field, value) do { } while (0)
#endif

namespace {
uint32_t PackColorUnorm8(const float4& color) {
    const auto toByte = [](float value) -> uint32_t {
        const float clamped = std::clamp(value, 0.0f, 1.0f);
        return static_cast<uint32_t>(clamped * 255.0f + 0.5f);
    };

    const uint32_t r = toByte(color.x);
    const uint32_t g = toByte(color.y);
    const uint32_t b = toByte(color.z);
    const uint32_t a = toByte(color.w);
    return r | (g << 8) | (b << 16) | (a << 24);
}

uint32_t PackHalf2(float x, float y) {
    using DirectX::PackedVector::XMConvertFloatToHalf;
    const uint32_t hx = static_cast<uint32_t>(XMConvertFloatToHalf(x));
    const uint32_t hy = static_cast<uint32_t>(XMConvertFloatToHalf(y));
    return hx | (hy << 16);
}
} // namespace
// utf8 to wide
std::wstring TextManager::UTF8toW(std::string_view s) {
    if (s.empty()) { return L""; }
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w; w.resize((size_t)wlen);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), wlen);
    return w;
}

void TextManager::SetShadow(const ShadowDesc& desc) {
    shadow_ = desc;
}

void TextManager::DisableShadow() {
    shadow_.reset();
}

void TextManager::SetFont(FontAtlas* f) {
    if (font_ != f) {
        cachedGlyphRuns_.clear();
        cachedGlyphRunsFont_ = nullptr;
    }
    font_ = f;
}

void TextManager::Init(Renderer* r) {
    auto createTextMaterial = [&](const wchar_t* shaderPath) -> std::shared_ptr<Material> {
        Material::GraphicsDesc gd;
        gd.shaderFile = shaderPath;
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
        return r->GetMaterialManager()->GetOrCreateGraphics(r, gd);
    };

    matTextSdf_ = createTextMaterial(L"shaders/font_sdf.hlsl");
    matTextCoverage_ = createTextMaterial(L"shaders/font_pixelperfect.hlsl");
    // Rectangle material (background)
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
#if TEXT_MANAGER_PERF_STATS
    if (perfStatsEnabled_) {
        lastPerf_ = framePerf_;
        framePerf_ = {};
    }
#endif
    TEXT_PERF_SCOPE(beginUs);

    vpW_ = (vpW == 0u ? 1u : vpW);
    vpH_ = (vpH == 0u ? 1u : vpH);
    dpi_ = (dpiScale <= 0.f ? 1.f : dpiScale);

    verts_.clear();
    rectVerts_.clear(); rectIdx_.clear();
    frameRegionGlyphCount_ = 0;
    frameBackgroundRectCount_ = 0;
    RecycleRegionLines();
    for (Region& rg : regions_) {
        regionPool_.push_back(std::move(rg));
    }
    regions_.clear();
}

// Positional text (non-cached, legacy behavior)
void TextManager::AddText(int x, int y, float px, const float4& color, std::wstring_view text) {
    AddText(x, y, px, color, text, false);
}
void TextManager::AddText(int x, int y, float px, const float4& color, std::string_view utf8) {
    AddText(x, y, px, color, utf8, false);
}
void TextManager::AddText(int x, int y, float px, const float4& color, std::wstring_view text, bool enableShadow) {
    TEXT_PERF_ADD(positionalTextCalls, 1u);
    TEXT_PERF_ADD(inputChars, static_cast<uint32_t>(std::min<size_t>(text.size(), std::numeric_limits<uint32_t>::max())));
    EmitTextImmediate(x, y, px, color, text, enableShadow);
}
void TextManager::AddText(int x, int y, float px, const float4& color, std::string_view utf8, bool enableShadow) {
    AddText(x, y, px, color, UTF8toW(utf8), enableShadow);
}
void TextManager::AddTextf(int x, int y, float px, const float4& color, const wchar_t* fmt, ...) {
    if (fmt == nullptr) { return; }
    TEXT_PERF_ADD(addTextfCalls, 1u);
    wchar_t buf[256];
    int len = 0;
    {
        TEXT_PERF_SCOPE(formatUs);
        va_list args; va_start(args, fmt);
        len = std::vswprintf(buf, sizeof(buf) / sizeof(wchar_t), fmt, args);
        va_end(args);
    }
    if (len > 0) { AddText(x, y, px, color, std::wstring_view(buf, (size_t)len)); }
}
void TextManager::AddTextfShadow(int x, int y, float px, const float4& color, bool enableShadow, const wchar_t* fmt, ...) {
    if (fmt == nullptr) { return; }
    TEXT_PERF_ADD(addTextfCalls, 1u);
    wchar_t buf[256];
    int len = 0;
    {
        TEXT_PERF_SCOPE(formatUs);
        va_list args; va_start(args, fmt);
        len = std::vswprintf(buf, sizeof(buf) / sizeof(wchar_t), fmt, args);
        va_end(args);
    }
    if (len > 0) { AddText(x, y, px, color, std::wstring_view(buf, (size_t)len), enableShadow); }
}

// Regions
TextManager::RegionId TextManager::CreateRegion(int x, int y, Align align) {
    TEXT_PERF_ADD(regions, 1u);

    Region r;
    if (!regionPool_.empty()) {
        r = std::move(regionPool_.back());
        regionPool_.pop_back();
    }
    r.x = x;
    r.y = y;
    r.align = align;
    r.padX = 8;
    r.padY = 6;
    r.bg.reset();
    r.fixedWidthPx.reset();
    r.autoMeasure = true;
    r.kerning = true;
    r.lines.clear();
    r.maxLineWidth = 0.0f;
    r.totalLines = 0;
    r.lineStepPx = 18;
    r.glyphCount = 0;
    regions_.push_back(std::move(r));
    return (RegionId)(regions_.size() - 1);
}
void TextManager::RegionSetBackground(RegionId id, std::optional<float4> color) {
    if (id >= regions_.size()) { return; }
    Region& rg = regions_[id];
    const bool hadBackground = rg.bg.has_value();
    const bool hasBackground = color.has_value();
    if (!hadBackground && hasBackground) {
        ++frameBackgroundRectCount_;
        TEXT_PERF_ADD(backgrounds, 1u);
    }
    else if (hadBackground && !hasBackground && frameBackgroundRectCount_ > 0) {
        --frameBackgroundRectCount_;
    }
    rg.bg = color;
}
void TextManager::RegionSetPadding(RegionId id, int padX, int padY) {
    if (id >= regions_.size()) { return; }
    regions_[id].padX = padX; regions_[id].padY = padY;
}
void TextManager::RegionSetAlign(RegionId id, Align a) {
    if (id >= regions_.size()) { return; }
    // Alignment decides whether lines are emitted directly (Left) or deferred
    // (Center/Right) at AddText time, so it must be set before any text is added
    // to the region; otherwise already-emitted Left lines cannot be re-aligned.
    assert(regions_[id].lines.empty() && "RegionSetAlign must be called before AddText");
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
void TextManager::RegionSetKerning(RegionId id, bool enabled) {
    if (id >= regions_.size()) { return; }
    regions_[id].kerning = enabled;
}

void TextManager::AddText(RegionId id, float px, const float4& color, std::wstring_view text) {
    AddText(id, px, color, text, false);
}
void TextManager::AddText(RegionId id, float px, const float4& color, std::string_view utf8) {
    AddText(id, px, color, UTF8toW(utf8), false);
}
void TextManager::AddText(RegionId id, float px, const float4& color, std::string_view utf8, bool enableShadow) {
    AddText(id, px, color, UTF8toW(utf8), enableShadow);
}
void TextManager::AddText(RegionId id, float px, const float4& color, std::wstring_view text, bool enableShadow) {
    TEXT_MANAGER_FINE_CPU_SCOPE(ProfilerScopes::kTextManagerAddText);
    TEXT_PERF_SCOPE(addTextUs);
    TEXT_PERF_ADD(addTextCalls, 1u);
    TEXT_PERF_ADD(inputChars, static_cast<uint32_t>(std::min<size_t>(text.size(), std::numeric_limits<uint32_t>::max())));

    if (id >= regions_.size() || font_ == nullptr || text.empty()) { return; }
    Region& rg = regions_[id];

    const size_t charCount = text.size();
    const bool emitDirect = CanEmitRegionLineImmediately(rg);
    RegionLine* ln = AcquireRegionLine(charCount);
    if (!ln) {
        return;
    }

    ln->color = color;
    ln->px = px;
    ln->shadowEnabled = enableShadow;
    ln->lineIndex = rg.totalLines;

    const int lineStepPx = (int)std::round(px + 2.0f);
    SetRegionLineStep(rg, lineStepPx);

    if (emitDirect) {
        ln->emittedDirect = true;
        ln->emittedY = rg.y + (ln->lineIndex * rg.lineStepPx);
        ln->directVertexFirst = verts_.size();
        const size_t emittedGlyphs = EmitTextDirect(rg.x, ln->emittedY, px, color, text, enableShadow, rg.kerning, &ln->widthPx);
        ln->directVertexCount = verts_.size() - ln->directVertexFirst;
        ln->glyphCount = (uint32_t)std::min<size_t>(emittedGlyphs, std::numeric_limits<uint32_t>::max());
        rg.glyphCount += emittedGlyphs;
        // emitDirect implies Align::Left; track width for the background rect when
        // there is no fixed width to fall back on.
        if (rg.autoMeasure) {
            rg.maxLineWidth = std::max(rg.maxLineWidth, ln->widthPx);
        }
        TEXT_PERF_ADD(directLines, 1u);
    }
    else {
        BuildGlyphRun(text, ln->px, ln->run, ln->widthPx);
        const size_t glyphReserve = (ln->run.ready ? ln->run.glyphCount : charCount);
        ln->glyphCount = (uint32_t)std::min<size_t>(glyphReserve, std::numeric_limits<uint32_t>::max());

        if (rg.autoMeasure || (rg.align != Align::Left)) {
            rg.maxLineWidth = std::max(rg.maxLineWidth, ln->widthPx);
        }

        rg.glyphCount += glyphReserve;
        frameRegionGlyphCount_ += glyphReserve;
        TEXT_PERF_ADD(deferredLines, 1u);
    }
    rg.lines.push_back(ln);
    rg.totalLines = (int)rg.lines.size();
}

void TextManager::AddCachedText(RegionId id, float px, const float4& color, std::wstring_view text, bool enableShadow) {
    TEXT_PERF_SCOPE(addCachedTextUs);
    TEXT_PERF_ADD(addCachedTextCalls, 1u);
    TEXT_PERF_ADD(inputChars, static_cast<uint32_t>(std::min<size_t>(text.size(), std::numeric_limits<uint32_t>::max())));

    if (id >= regions_.size() || font_ == nullptr || text.empty()) { return; }
    Region& rg = regions_[id];

    const CachedGlyphRun& cached = GetCachedGlyphRun(text, px);
    const size_t glyphReserve = (cached.run.ready ? cached.run.glyphCount : text.size());
    RegionLine* ln = AcquireRegionLine(glyphReserve);
    if (!ln) {
        return;
    }

    ln->color = color;
    ln->px = px;
    ln->shadowEnabled = enableShadow;
    ln->run = cached.run;
    ln->widthPx = cached.widthPx;
    ln->glyphCount = (uint32_t)std::min<size_t>(glyphReserve, std::numeric_limits<uint32_t>::max());
    ln->lineIndex = rg.totalLines;

    if (rg.autoMeasure || (rg.align != Align::Left)) {
        rg.maxLineWidth = std::max(rg.maxLineWidth, ln->widthPx);
    }

    const int lineStepPx = (int)std::round(px + 2.0f);
    SetRegionLineStep(rg, lineStepPx);

    rg.glyphCount += glyphReserve;
    if (CanEmitRegionLineImmediately(rg)) {
        ln->emittedDirect = true;
        ln->emittedY = rg.y + (ln->lineIndex * rg.lineStepPx);
        ln->directVertexFirst = verts_.size();
        EmitGlyphRun(rg.x, ln->emittedY, 0.0f, ln->color, ln->run, ln->shadowEnabled);
        ln->directVertexCount = verts_.size() - ln->directVertexFirst;
        TEXT_PERF_ADD(directLines, 1u);
    }
    else {
        frameRegionGlyphCount_ += glyphReserve;
        TEXT_PERF_ADD(deferredLines, 1u);
    }
    rg.lines.push_back(ln);
    rg.totalLines = (int)rg.lines.size();
}

void TextManager::AddTextf(RegionId id, float px, const float4& color, const wchar_t* fmt, ...) {
    if (fmt == nullptr) { return; }
    TEXT_PERF_ADD(addTextfCalls, 1u);
    wchar_t buf[256];
    int len = 0;
    {
        TEXT_PERF_SCOPE(formatUs);
        va_list args; va_start(args, fmt);
        len = std::vswprintf(buf, sizeof(buf) / sizeof(wchar_t), fmt, args);
        va_end(args);
    }
    if (len > 0) { AddText(id, px, color, std::wstring_view(buf, (size_t)len)); }
}
void TextManager::AddTextfShadow(RegionId id, float px, const float4& color, bool enableShadow, const wchar_t* fmt, ...) {
    if (fmt == nullptr) { return; }
    TEXT_PERF_ADD(addTextfCalls, 1u);
    wchar_t buf[256];
    int len = 0;
    {
        TEXT_PERF_SCOPE(formatUs);
        va_list args; va_start(args, fmt);
        len = std::vswprintf(buf, sizeof(buf) / sizeof(wchar_t), fmt, args);
        va_end(args);
    }
    if (len > 0) { AddText(id, px, color, std::wstring_view(buf, (size_t)len), enableShadow); }
}

// ======== BUILD / RENDER ========
void TextManager::Build(Renderer* r, ID3D12GraphicsCommandList* /*cl*/) {
    CollectRetiredTextIndexBuffers(r);

    if (font_ == nullptr) { return; }
    CPU_SCOPE(ProfilerScopes::kTextManagerBuild);
    TEXT_PERF_SCOPE(buildUs);

    // CPU: reserve + emit region backgrounds and deferred lines into the arrays.
    BuildVerticesCPU();

    // 3) Upload to GPU
    FrameResource* fr = r->GetFrameResource();

    // rects
    {
        TEXT_PERF_SCOPE(uploadRectUs);
        const UINT vbBytes = (UINT)(rectVerts_.size() * sizeof(Vertex));
        const UINT ibBytes = (UINT)(rectIdx_.size() * sizeof(uint32_t));
        if (vbBytes > 0 && ibBytes > 0) {
            auto v = fr->AllocDynamic(vbBytes, 16);
            auto i = fr->AllocDynamic(ibBytes, 16);
            std::memcpy(v.cpu, rectVerts_.data(), vbBytes);
            std::memcpy(i.cpu, rectIdx_.data(), ibBytes);
            rectVBV_.BufferLocation = v.gpu;
            rectVBV_.StrideInBytes = sizeof(Vertex);
            rectVBV_.SizeInBytes = vbBytes;
            rectIBV_.BufferLocation = i.gpu;
            rectIBV_.Format = DXGI_FORMAT_R32_UINT;
            rectIBV_.SizeInBytes = ibBytes;
        }
        else {
            rectVBV_ = {};
            rectIBV_ = {};
        }
    }

    // text
    {
        TEXT_PERF_SCOPE(uploadTextUs);
        const UINT vbBytes = (UINT)(verts_.size() * sizeof(Vertex));
        const size_t textQuadCount = verts_.size() / 4u;
        if (vbBytes > 0 && textQuadCount > 0) {
            EnsureTextIndexCapacity(r, textQuadCount);
            auto v = fr->AllocDynamic(vbBytes, 16);
            std::memcpy(v.cpu, verts_.data(), vbBytes);
            vbv_.BufferLocation = v.gpu;
            vbv_.StrideInBytes = sizeof(Vertex);
            vbv_.SizeInBytes = vbBytes;
        }
        else {
            vbv_ = {};
        }
    }
}

void TextManager::BuildVerticesCPU() {
    // 0) Reserve once from frame counters maintained during AddText/RegionSetBackground.
    {
        TEXT_PERF_SCOPE(buildReserveUs);
        if (frameRegionGlyphCount_) {
            verts_.ensureAdditional(frameRegionGlyphCount_ * 4);
        }

        // 1) Reserve space for known background rectangles.
        rectVerts_.ensureAdditional(frameBackgroundRectCount_ * 4);
        rectIdx_.ensureAdditional(frameBackgroundRectCount_ * 6);
    }

    // 2) Single pass over regions: backgrounds and lines
    {
        TEXT_PERF_SCOPE(buildRegionsUs);
        for (const Region& rg : regions_) {
            if (rg.totalLines <= 0) { continue; }

            // Background
            if (rg.bg.has_value()) {
                const float w = (rg.fixedWidthPx.has_value() ? rg.fixedWidthPx.value() : rg.maxLineWidth)
                    + float(rg.padX * 2);
                const float h = float(rg.totalLines * rg.lineStepPx) + float(rg.padY * 2);
                const int   bx = rg.x - rg.padX;
                const int   by = rg.y - rg.padY;
                EmitRectReserved(bx, by, w, h, rg.bg.value());
            }

            // Lines
            int y = rg.y;
            if (rg.align == Align::Left) {
                for (const RegionLine* ln : rg.lines) {
                    if (!ln) { continue; }
                    if (ln->emittedDirect) {
                        y += rg.lineStepPx;
                        continue;
                    }
                    EmitGlyphRunReserved(rg.x, y, 0.0f, ln->color, ln->run, ln->shadowEnabled);
                    y += rg.lineStepPx;
                }
            }
            else {
                const float regionW = (rg.fixedWidthPx.has_value() ? rg.fixedWidthPx.value() : rg.maxLineWidth);
                for (const RegionLine* ln : rg.lines) {
                    if (!ln) { continue; }
                    if (ln->emittedDirect) {
                        y += rg.lineStepPx;
                        continue;
                    }
                    const float xOff = (rg.align == Align::Center)
                        ? std::max(0.0f, 0.5f * (regionW - ln->widthPx))
                        : std::max(0.0f, (regionW - ln->widthPx));
                    EmitGlyphRunReserved(rg.x, y, xOff, ln->color, ln->run, ln->shadowEnabled);
                    y += rg.lineStepPx;
                }
            }
        }
    }
}

void TextManager::Draw(Renderer* r, ID3D12GraphicsCommandList* cl) {
    CPU_SCOPE(ProfilerScopes::kTextManagerDraw);
    TEXT_PERF_SCOPE(drawUs);
    // 1) Background
    if (!rectVerts_.empty() && !rectIdx_.empty() && matRect_) {
        auto h = r->GetRenderContextPool()->Acquire();
        auto& rc = h.ref();

        rc.constants[0] = { FloatToUint32((float)vpW_), FloatToUint32((float)vpH_), 0u, 0u };

        matRect_->Bind(cl, rc);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->IASetVertexBuffers(0, 1, &rectVBV_);
        cl->IASetIndexBuffer(&rectIBV_);
        cl->DrawIndexedInstanced((UINT)rectIdx_.size(), 1, 0, 0, 0);
    }

    // 2) Text
    if (!verts_.empty() && textIndexQuadCapacity_ > 0 && font_) {
        auto h = r->GetRenderContextPool()->Acquire();
        auto& rc = h.ref();

        auto tbl = r->StageSrvUavTable({ font_->GetSRVCPU() });
        rc.srvTable[0] = tbl.gpu;

        const bool useCoverage = font_->IsCoverage();
        //const D3D12_SAMPLER_DESC samplerDesc = useCoverage ? *SamplerManager::PointClamp() : *SamplerManager::LinearClamp();
        rc.samplerTable[0] = r->GetSamplerManager()->GetTable(r, *SamplerManager::LinearClamp());

        const std::shared_ptr<Material>& mat = useCoverage ? matTextCoverage_ : matTextSdf_;
        if (!mat) {
            return;
        }

        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->IASetVertexBuffers(0, 1, &vbv_);
        cl->IASetIndexBuffer(&textIBV_);

        const float invAtlasW = (font_->AtlasWidth() > 0) ? (1.0f / float(font_->AtlasWidth())) : 0.0f;
        const float invAtlasH = (font_->AtlasHeight() > 0) ? (1.0f / float(font_->AtlasHeight())) : 0.0f;
        float2 shadowBaseOffset = { 0.0f, 0.0f };
        float3 shadowColorRgb = { 0.0f, 0.0f, 0.0f };
        if (shadow_.has_value()) {
            const ShadowDesc& desc = shadow_.value();
            shadowBaseOffset = { desc.offsetX, desc.offsetY };
            shadowColorRgb = { desc.color.x, desc.color.y, desc.color.z };
        }

        rc.constants[0] = {
            FloatToUint32((float)vpW_), FloatToUint32((float)vpH_),
            FloatToUint32(invAtlasW), FloatToUint32(invAtlasH),
            FloatToUint32(shadowBaseOffset.x), FloatToUint32(shadowBaseOffset.y),
            0u, 0u,
            FloatToUint32(shadowColorRgb.x), FloatToUint32(shadowColorRgb.y),
            FloatToUint32(shadowColorRgb.z), 0u
        };

        mat->Bind(cl, rc);

        const UINT indexCount = (UINT)((verts_.size() / 4u) * 6u);
        cl->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
    }
}

void TextManager::Clear() {
    matTextSdf_.reset();
    matTextCoverage_.reset();
    matRect_.reset();
    verts_.clear();
    rectVerts_.clear(); rectIdx_.clear();
    frameRegionGlyphCount_ = 0;
    frameBackgroundRectCount_ = 0;
    framePerf_ = {};
    lastPerf_ = {};
    textIndexBuffer_.Reset();
    for (auto& buffers : retiredTextIndexBuffers_) {
        buffers.clear();
    }
    retiredTextIndexFrameMask_ = 0;
    textIBV_ = {};
    textIndexQuadCapacity_ = 0;
    RecycleRegionLines();
    nextUnusedRegionLine_ = 0;
    freeRegionLines_.clear();
    for (Region& rg : regions_) {
        regionPool_.push_back(std::move(rg));
    }
    regions_.clear();
    regionPool_.clear();
}

TextManager::RegionLine* TextManager::AcquireRegionLine(size_t glyphReserveHint) {
    RegionLine* ln = nullptr;

    if (!freeRegionLines_.empty()) {
        ln = freeRegionLines_.back();
        freeRegionLines_.pop_back();
    } else if (nextUnusedRegionLine_ < regionLinePool_.size()) {
        ln = &regionLinePool_[nextUnusedRegionLine_++];
    }

    if (!ln) {
        assert(false && "RegionLine pool overflow");
        return nullptr;
    }

    ln->inUse = true;
    ln->color = {};
    ln->px = 16.0f;
    ln->widthPx = 0.0f;
    ln->directVertexFirst = 0;
    ln->directVertexCount = 0;
    ln->glyphCount = 0;
    ln->lineIndex = 0;
    ln->emittedY = 0;
    ln->run.Reset();
    ln->shadowEnabled = false;
    ln->emittedDirect = false;

    assert(glyphReserveHint <= GlyphRun::kDefaultCapacity);
    (void)glyphReserveHint;

    return ln;
}

void TextManager::RecycleRegionLines() {
    const RegionLine* poolBegin = regionLinePool_.data();
    const RegionLine* poolEnd = poolBegin + regionLinePool_.size();
    for (Region& rg : regions_) {
        for (RegionLine* ln : rg.lines) {
            if (!ln) { continue; }
            assert(ln >= poolBegin && ln < poolEnd);
            ln->run.Reset();
            ln->widthPx = 0.0f;
            ln->directVertexFirst = 0;
            ln->directVertexCount = 0;
            ln->glyphCount = 0;
            ln->lineIndex = 0;
            ln->emittedY = 0;
            ln->inUse = false;
            ln->shadowEnabled = false;
            ln->emittedDirect = false;
            freeRegionLines_.push_back(ln);
        }
        rg.lines.clear();
        rg.maxLineWidth = 0.0f;
        rg.totalLines = 0;
        rg.glyphCount = 0;
        rg.lineStepPx = 18;
    }
    assert(freeRegionLines_.size() <= regionLinePool_.size());
}

void TextManager::EnsureTextIndexCapacity(Renderer* r, size_t quadCount) {
    if (r == nullptr || quadCount == 0 || quadCount <= textIndexQuadCapacity_) {
        return;
    }

    size_t newCapacity = (textIndexQuadCapacity_ != 0) ? textIndexQuadCapacity_ : 256u;
    while (newCapacity < quadCount) {
        const size_t next = newCapacity * 2u;
        if (next <= newCapacity) {
            newCapacity = quadCount;
            break;
        }
        newCapacity = next;
    }

    const size_t indexCount = newCapacity * 6u;
    const size_t byteSize = indexCount * sizeof(uint32_t);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = byteSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    ThrowIfFailed(r->GetDevice()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(buffer.GetAddressOf())));

    D3D12_RANGE readRange{ 0, 0 };
    uint32_t* mapped = nullptr;
    ThrowIfFailed(buffer->Map(0, &readRange, reinterpret_cast<void**>(&mapped)));
    for (size_t i = 0; i < newCapacity; ++i) {
        const uint32_t base = static_cast<uint32_t>(i * 4u);
        uint32_t* dst = mapped + i * 6u;
        dst[0] = base + 0u;
        dst[1] = base + 1u;
        dst[2] = base + 2u;
        dst[3] = base + 0u;
        dst[4] = base + 2u;
        dst[5] = base + 3u;
    }
    buffer->Unmap(0, nullptr);

    RetireTextIndexBuffer(r);

    textIndexBuffer_ = std::move(buffer);
    textIndexQuadCapacity_ = newCapacity;
    textIBV_.BufferLocation = textIndexBuffer_->GetGPUVirtualAddress();
    textIBV_.Format = DXGI_FORMAT_R32_UINT;
    textIBV_.SizeInBytes = static_cast<UINT>(byteSize);
}

void TextManager::CollectRetiredTextIndexBuffers(Renderer* r) {
    if (r == nullptr || retiredTextIndexFrameMask_ == 0u) {
        return;
    }

    const UINT frameIndex = r->GetCurrentFrameIndex();
    if (frameIndex >= retiredTextIndexBuffers_.size() || frameIndex >= 32u) {
        return;
    }

    const uint32_t frameBit = 1u << frameIndex;
    if ((retiredTextIndexFrameMask_ & frameBit) == 0u) {
        return;
    }

    retiredTextIndexBuffers_[frameIndex].clear();
    retiredTextIndexFrameMask_ &= ~frameBit;
}

void TextManager::RetireTextIndexBuffer(Renderer* r) {
    if (!textIndexBuffer_) {
        return;
    }

    if (r != nullptr) {
        const UINT frameIndex = r->GetCurrentFrameIndex();
        if (frameIndex < retiredTextIndexBuffers_.size() && frameIndex < 32u) {
            retiredTextIndexBuffers_[frameIndex].push_back(std::move(textIndexBuffer_));
            retiredTextIndexFrameMask_ |= (1u << frameIndex);
            return;
        }
    }

    retiredTextIndexBuffers_[0].push_back(std::move(textIndexBuffer_));
    retiredTextIndexFrameMask_ |= 1u;
}

// ===== Private helpers =====

const TextManager::CachedGlyphRun& TextManager::GetCachedGlyphRun(std::wstring_view text, float px) {
    if (cachedGlyphRunsFont_ != font_) {
        cachedGlyphRuns_.clear();
        cachedGlyphRunsFont_ = font_;
    }

    for (const CachedGlyphRun& entry : cachedGlyphRuns_) {
        if (entry.px == px && entry.text == text) {
            return entry;
        }
    }

    CachedGlyphRun entry;
    entry.text.assign(text.data(), text.size());
    entry.px = px;
    BuildGlyphRun(entry.text, entry.px, entry.run, entry.widthPx);

    cachedGlyphRuns_.push_back(std::move(entry));
    return cachedGlyphRuns_.back();
}

bool TextManager::CanEmitRegionLineImmediately(const Region& rg) noexcept {
    // Left-aligned glyphs sit at x = rg.x regardless of line/region width, so
    // they can be emitted in a single pass at AddText time. The background rect
    // (which does depend on width) is emitted later in Build from the running
    // maxLineWidth / fixedWidthPx, so it stays correct. Center/Right need the
    // width up front and therefore must defer to Build (two passes).
    return rg.align == Align::Left;
}

void TextManager::SetRegionLineStep(Region& rg, int lineStepPx) {
    if (rg.lineStepPx == lineStepPx) { return; }
    TEXT_PERF_SCOPE(lineRetargetUs);

    rg.lineStepPx = lineStepPx;
    for (RegionLine* ln : rg.lines) {
        if (!ln || !ln->emittedDirect || ln->directVertexCount == 0) { continue; }

        const int newY = rg.y + (ln->lineIndex * rg.lineStepPx);
        const float dy = float(newY - ln->emittedY);
        if (dy == 0.0f) {
            ln->emittedY = newY;
            continue;
        }

        Vertex* v = verts_.data() + ln->directVertexFirst;
        for (size_t i = 0; i < ln->directVertexCount; ++i) {
            v[i].pos.y += dy;
        }
        TEXT_PERF_ADD(retargetedVertices, static_cast<uint32_t>(std::min<size_t>(ln->directVertexCount, std::numeric_limits<uint32_t>::max())));
        ln->emittedY = newY;
    }
}

size_t TextManager::EmitTextDirect(int x, int y, float px, const float4& color, std::wstring_view text, bool enableShadow, bool useKerning, float* outWidthPx) {
    TEXT_PERF_SCOPE(directEmitUs);
    TEXT_PERF_ADD(directEmitCalls, 1u);

    if (outWidthPx) { *outWidthPx = 0.0f; }
    if (font_ == nullptr || text.empty()) { return 0; }

    const FontAtlas* font = nullptr;
    float scale = 1.0f;
    float penY = 0.0f;
    float spaceAdvance = 0.0f;
    float tabAdvance = 0.0f;
    float shadowScale = 0.0f;
    float shadowAlpha = 0.0f;

    {
        TEXT_PERF_SCOPE(directEmitSetupUs);
        font = font_;
        scale = px / float(font->PxSize());
        penY = (float)y + float(font->Ascent()) * scale;

        const FontGlyph* glyphSpace = font->FindFast((uint32_t)L' ');
        const FontGlyph* glyphTab = font->FindFast((uint32_t)L'\t');
        const FontGlyph* glyphN = font->FindFast((uint32_t)'n');
        const float emAdvance = (glyphN ? float(glyphN->xadv) : float(font->PxSize()));
        spaceAdvance = (glyphSpace ? float(glyphSpace->xadv) : emAdvance * 0.5f) * scale;
        tabAdvance = (glyphTab ? float(glyphTab->xadv) : emAdvance * 2.0f) * scale;

        if (enableShadow && shadow_.has_value()) {
            const ShadowDesc& desc = shadow_.value();
            float offsetScale = (desc.scaleWithTextSize ? 1.0f / scale : 1.0f);
            if (desc.scaleWithDpi) {
                offsetScale *= dpi_;
            }
            else {
                offsetScale = 0.0f;
            }

            const float baseAlpha = desc.color.w * desc.alphaMultiplier * color.w;
            const float finalAlpha = std::clamp(baseAlpha, 0.0f, 1.0f);
            if (finalAlpha > 0.0f) {
                shadowScale = offsetScale;
                shadowAlpha = finalAlpha;
            }
        }
    }

    const size_t maxGlyphs = text.size();
    size_t vertexBase = 0;
    {
        TEXT_PERF_SCOPE(directEmitReserveUs);
        vertexBase = verts_.appendUninitialized(maxGlyphs * 4);
    }

    Vertex* const vData = verts_.data() + vertexBase;
    const uint32_t packedColor = PackColorUnorm8(color);
    const uint32_t packedShadow = PackHalf2(shadowScale, shadowAlpha);
    const float baseX = static_cast<float>(x);

    const bool hasKerning = useKerning && font->HasKerning();
    size_t emittedGlyphs = 0;

    {
        TEXT_PERF_SCOPE(directEmitLoopUs);
        const wchar_t* cur = text.data();
        const wchar_t* const end = cur + text.size();
        float penX = 0.0f;

        if (!hasKerning) {
            for (; cur != end; ++cur) {
                const wchar_t wc = *cur;
                if (wc == L'\n') { break; }
                if (wc == L' ') {
                    penX += spaceAdvance;
                    continue;
                }
                if (wc == L'\t') {
                    penX += tabAdvance;
                    continue;
                }

                const FontGlyph* gph = font->FindFast(static_cast<uint32_t>(wc));
                if (!gph) { continue; }

                const float gx = baseX + penX + float(gph->xoff) * scale;
                const float gy = penY + float(gph->yoff) * scale;
                const float gw = float(gph->w) * scale;
                const float gh = float(gph->h) * scale;

                Vertex* curV = vData + emittedGlyphs * 4;

                Vertex v;
                v.col = packedColor;
                v.shadowParams = packedShadow;

                v.pos.x = gx; v.pos.y = gy;
                v.uv.x = gph->u0; v.uv.y = gph->v0;
                curV[0] = v;

                v.pos.x = gx + gw;
                v.uv.x = gph->u1;
                curV[1] = v;

                v.pos.y = gy + gh;
                v.uv.y = gph->v1;
                curV[2] = v;

                v.pos.x = gx;
                v.uv.x = gph->u0;
                curV[3] = v;

                penX += float(gph->xadv) * scale;
                ++emittedGlyphs;
            }
        }
        else {
            uint32_t prev = 0;
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

                const uint32_t cp = static_cast<uint32_t>(wc);
                const FontGlyph* gph = font->FindFast(cp);
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

                const float gx = baseX + penX + float(gph->xoff) * scale;
                const float gy = penY + float(gph->yoff) * scale;
                const float gw = float(gph->w) * scale;
                const float gh = float(gph->h) * scale;

                Vertex* curV = vData + emittedGlyphs * 4;

                Vertex v;
                v.col = packedColor;
                v.shadowParams = packedShadow;

                v.pos.x = gx; v.pos.y = gy;
                v.uv.x = gph->u0; v.uv.y = gph->v0;
                curV[0] = v;

                v.pos.x = gx + gw;
                v.uv.x = gph->u1;
                curV[1] = v;

                v.pos.y = gy + gh;
                v.uv.y = gph->v1;
                curV[2] = v;

                v.pos.x = gx;
                v.uv.x = gph->u0;
                curV[3] = v;

                penX += float(gph->xadv) * scale;
                prev = cp;
                ++emittedGlyphs;
            }
        }

        if (outWidthPx) { *outWidthPx = penX; }
    }

    verts_.resizeReserved(vertexBase + emittedGlyphs * 4);

    TEXT_PERF_ADD(directGlyphs, static_cast<uint32_t>(std::min<size_t>(emittedGlyphs, std::numeric_limits<uint32_t>::max())));
    return emittedGlyphs;
}

// Shared helper to build a glyph run and compute width in a single pass
void TextManager::BuildGlyphRun(std::wstring_view text, float px, GlyphRun& outRun, float& outWidthPx) const {
    TEXT_MANAGER_FINE_CPU_SCOPE(ProfilerScopes::kTextManagerBuildGlyphRun);
    TEXT_PERF_SCOPE(buildGlyphRunUs);
    TEXT_PERF_ADD(glyphRunBuilds, 1u);

    outRun.Reset();
    outRun.scale = px / float(font_->PxSize());
    outRun.ready = false;
    outWidthPx = 0.0f;

    if (font_ == nullptr || text.empty()) { return; }

    const FontAtlas* font = font_;
    const float scale = outRun.scale;

    if (text.size() > GlyphRun::kDefaultCapacity) {
        assert(text.size() <= GlyphRun::kDefaultCapacity);
    }

    const FontGlyph* glyphSpace = font->FindFast((uint32_t)L' ');
    const FontGlyph* glyphTab   = font->FindFast((uint32_t)L'\t');
    const FontGlyph* glyphN     = font->FindFast((uint32_t)'n');
    const float emAdvance       = (glyphN ? float(glyphN->xadv) : float(font->PxSize()));
    const float spaceAdvance    = (glyphSpace ? float(glyphSpace->xadv) : emAdvance * 0.5f) * scale;
    const float tabAdvance      = (glyphTab ? float(glyphTab->xadv) : emAdvance * 2.0f) * scale;

    float penX = 0.0f;
    uint32_t prev = 0;
    bool hasKerning = font->HasKerning();

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
        const FontGlyph* gph = font->FindFast(cp);
        if (!gph) {
            prev = 0;
            continue;
        }

        if (hasKerning && prev) {
            const int kern = font->Kerning(prev, cp);
            if (kern) {
                penX += float(kern) * scale;
            }
        }

        if (outRun.glyphCount >= GlyphRun::kDefaultCapacity) {
            assert(outRun.glyphCount < GlyphRun::kDefaultCapacity);
            break;
        }

        outRun.Append(gph, penX);

        penX += float(gph->xadv) * scale;
        prev = cp;
    }

    outWidthPx = penX;
    outRun.ready = true;
}

// Fast emission for a prepared glyph run
void TextManager::EmitGlyphRun(int x, int y, float xOffset, const float4& color, const GlyphRun& run, bool enableShadow) {
    EmitGlyphRunImpl(x, y, xOffset, color, run, enableShadow, false);
}

void TextManager::EmitGlyphRunReserved(int x, int y, float xOffset, const float4& color, const GlyphRun& run, bool enableShadow) {
    EmitGlyphRunImpl(x, y, xOffset, color, run, enableShadow, true);
}

void TextManager::EmitGlyphRunImpl(int x, int y, float xOffset, const float4& color, const GlyphRun& run, bool enableShadow, bool reservedAppend) {
    if (!run.ready || run.glyphCount == 0) { return; }
    TEXT_PERF_SCOPE(runEmitUs);
    TEXT_PERF_ADD(runEmitCalls, 1u);

    const float scale = run.scale;
    const float penY = (float)y + float(font_->Ascent()) * scale;
    const float baseX = static_cast<float>(x) + xOffset;

    const size_t n = run.glyphCount;
    if (n == 0) { return; }
    TEXT_PERF_ADD(runGlyphs, static_cast<uint32_t>(std::min<size_t>(n, std::numeric_limits<uint32_t>::max())));

    const size_t baseVert = reservedAppend ? verts_.appendUninitializedReserved(n * 4) : verts_.appendUninitialized(n * 4);
    Vertex* const vData = verts_.data() + baseVert;

    float shadowScale = 0.0f;
    float shadowAlpha = 0.0f;

    if (enableShadow && shadow_.has_value()) {
        const ShadowDesc& desc = shadow_.value();
        float offsetScale = (desc.scaleWithTextSize ? 1.0f / scale : 1.0f);
        if (desc.scaleWithDpi) {
            offsetScale *= dpi_;
        }
        else {
            offsetScale = 0.0f;
        }

        const float baseAlpha = desc.color.w * desc.alphaMultiplier * color.w;
        const float finalAlpha = std::clamp(baseAlpha, 0.0f, 1.0f);
        if (finalAlpha > 0.0f) {
            shadowScale = offsetScale;
            shadowAlpha = finalAlpha;
        }
    }

    const uint32_t packedColor = PackColorUnorm8(color);
    const uint32_t packedShadow = PackHalf2(shadowScale, shadowAlpha);

    for (size_t i = 0; i < n; ++i) {
        const FontGlyph* gph = run.GlyphAt(i);
        assert(gph != nullptr);
        if (!gph) {
            Vertex* curV = vData + i * 4;
            Vertex v{};
            curV[0] = v;
            curV[1] = v;
            curV[2] = v;
            curV[3] = v;

            continue;
        }
        const float penX = baseX + run.XOffsetAt(i);

        const float gx = penX + float(gph->xoff) * scale;
        const float gy = penY + float(gph->yoff) * scale;
        const float gw = float(gph->w) * scale;
        const float gh = float(gph->h) * scale;

        Vertex* curV = vData + i * 4;
        Vertex v;
        v.col = packedColor;
        v.shadowParams = packedShadow;

        v.pos.x = gx; v.pos.y = gy;
        v.uv.x = gph->u0; v.uv.y = gph->v0;
        curV[0] = v;

        //v.pos = { gx + gw, gy };
        //v.uv = { gph->u1, gph->v0 };
        v.pos.x = gx + gw;
        v.uv.x = gph->u1;
        curV[1] = v;

        //v.pos = { gx + gw, gy + gh };
        //v.uv = { gph->u1, gph->v1 };
        v.pos.y = gy + gh;
        v.uv.y = gph->v1;
        curV[2] = v;

        //v.pos = { gx, gy + gh };
        //v.uv = { gph->u0, gph->v1 };
        v.pos.x = gx;
        v.uv.x = gph->u0;
        curV[3] = v;

    }

}

// Positional text is always left-aligned at an explicit (x, y), so there is
// nothing to measure first: emit straight into the vertex buffer in a single
// pass (like ImGui's AddText) instead of building an intermediate GlyphRun and
// walking the glyphs twice. useKerning=true keeps the previous behavior of
// honoring the font's kerning when present.
void TextManager::EmitTextImmediate(int x, int y, float px, const float4& color, std::wstring_view text, bool enableShadow) {
    if (font_ == nullptr) { return; }
    TEXT_MANAGER_FINE_CPU_SCOPE(ProfilerScopes::kTextManagerEmitImmediate);
    EmitTextDirect(x, y, px, color, text, enableShadow, /*useKerning*/ true, /*outWidthPx*/ nullptr);
}

void TextManager::EmitRect(int x, int y, float w, float h, const float4& color) {
    EmitRectImpl(x, y, w, h, color, false);
}

void TextManager::EmitRectReserved(int x, int y, float w, float h, const float4& color) {
    EmitRectImpl(x, y, w, h, color, true);
}

void TextManager::EmitRectImpl(int x, int y, float w, float h, const float4& color, bool reservedAppend) {
    const float gx = (float)x, gy = (float)y;
    const float gw = w, gh = h;
    const size_t baseVert = reservedAppend ? rectVerts_.appendUninitializedReserved(4) : rectVerts_.appendUninitialized(4);
    Vertex* vData = rectVerts_.data() + baseVert;

    Vertex v;
    v.col = PackColorUnorm8(color);
    v.uv = { 0.0f, 0.0f };
    v.shadowParams = PackHalf2(0.0f, 0.0f);

    v.pos = { gx, gy };
    vData[0] = v;

    v.pos = { gx + gw, gy };
    vData[1] = v;

    v.pos = { gx + gw, gy + gh };
    vData[2] = v;

    v.pos = { gx, gy + gh };
    vData[3] = v;

    const size_t baseIdx = reservedAppend ? rectIdx_.appendUninitializedReserved(6) : rectIdx_.appendUninitialized(6);
    uint32_t* iData = rectIdx_.data() + baseIdx;
    const uint32_t base = static_cast<uint32_t>(baseVert);
    iData[0] = base + 0u; iData[1] = base + 1u; iData[2] = base + 2u;
    iData[3] = base + 0u; iData[4] = base + 2u; iData[5] = base + 3u;
}

int RunTextManagerBenchmark(const char* outputPath) {
    const char* path = (outputPath && outputPath[0]) ? outputPath : "textmanager_benchmark.csv";
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return 2;
    }

    auto atlas = std::make_unique<FontAtlas>();
    atlas->InitAsciiBenchmarkFont();

    auto tm = std::make_unique<TextManager>();
    tm->SetFont(atlas.get());
    auto shadowDesc = TextManager::ShadowDesc();
    shadowDesc.offsetX = 4.0f;
    shadowDesc.offsetY = 4.0f;
    shadowDesc.color.w = 0.9f;
    shadowDesc.scaleWithTextSize = true;
    tm->SetShadow(shadowDesc);

    // 22 profiler-style rows. Pre-formatted: formatting (vswprintf) is excluded
    // from the timed region so we measure glyph emission, not printf.
    std::vector<std::wstring> rows;
    rows.reserve(22);
    for (int i = 0; i < 22; ++i) {
        wchar_t buf[192];
        std::swprintf(buf, sizeof(buf) / sizeof(wchar_t),
            L"TextManagerBenchmark.Row%02d          avg:%6.2f  max:%6.2f  p/u:%6.3f  usages:%u",
            i, 0.10 + double(i) * 0.01, 0.20 + double(i) * 0.015, 0.010 + double(i) * 0.001, 1u + static_cast<uint32_t>(i));
        rows.emplace_back(buf);
    }

    constexpr UINT kVpW = 1920, kVpH = 1080;
    constexpr int  kWarmupFrames = 256;
    constexpr int  kSampleFrames = 30000;
    using Clock = std::chrono::steady_clock;

    const float4 colA(1.0f, 1.0f, 1.0f, 0.92f);
    const float4 colB(0.5f, 0.5f, 0.5f, 0.92f);

    out << "scenario,usPerFrame,directLines,deferredLines,positionalCalls,regionAddTextCalls,"
        << "inputChars,directGlyphs,runGlyphs,buildGlyphRunUs,directEmitUs,runEmitUs,retargetUs\n";

    // Each frame: Begin -> add one frame of text -> BuildVerticesCPU (the CPU
    // half of Build). Timing the emit pass too keeps the deferred path's second
    // pass counted fairly against the single-pass direct path. Wall-clock timing
    // runs with PerfStats DISABLED so the internal TEXT_PERF_SCOPE timers don't
    // pollute the measurement; one extra instrumented frame captures the
    // fine-grained breakdown (populated only when built -DTEXT_MANAGER_PERF_STATS=1).
    auto runScenario = [&](const char* name, auto&& addFrame) {
        tm->SetPerfStatsEnabled(false);

        for (int f = 0; f < kWarmupFrames; ++f) {
            tm->Begin(kVpW, kVpH, 1.0f);
            addFrame();
            tm->BuildVerticesCPU();
        }

        double totalUs = 0.0;
        for (int f = 0; f < kSampleFrames; ++f) {
            tm->Begin(kVpW, kVpH, 1.0f);
            const auto t0 = Clock::now();
            addFrame();
            tm->BuildVerticesCPU();
            const auto t1 = Clock::now();
            totalUs += std::chrono::duration<double, std::micro>(t1 - t0).count();
        }
        const double usPerFrame = totalUs / double(kSampleFrames);

        // One instrumented frame for the breakdown (no-op fields when PERF_STATS off).
        tm->SetPerfStatsEnabled(true);
        tm->Begin(kVpW, kVpH, 1.0f);
        addFrame();
        tm->BuildVerticesCPU();
        tm->Begin(kVpW, kVpH, 1.0f);
        const TextManager::PerfStats s = tm->GetPerfStats();

        out << name << ',' << usPerFrame << ','
            << s.directLines << ',' << s.deferredLines << ','
            << s.positionalTextCalls << ',' << s.addTextCalls << ','
            << s.inputChars << ',' << s.directGlyphs << ',' << s.runGlyphs << ','
            << s.buildGlyphRunUs << ',' << s.directEmitUs << ',' << s.runEmitUs << ','
            << s.lineRetargetUs << '\n';
    };

    // S1: positional AddText. Always single-pass (left-aligned at explicit x,y).
    runScenario("positional", [&] {
        int y = 64;
        for (size_t i = 0; i < rows.size(); ++i) {
            tm->AddText(16, y, 16.0f, (i & 1) ? colA : colB, rows[i], true);
            y += 18;
        }
    });

    // S2: Left region, auto-measure, no fixed width.
    // Before the optimization this deferred to Build (two passes); now direct.
    runScenario("left_automeasure", [&] {
        auto reg = tm->CreateRegion(16, 64, TextManager::Align::Left);
        tm->RegionSetPadding(reg, 8, 6);
        tm->RegionSetBackground(reg, float4(0.0f, 0.0f, 0.05f, 0.75f));
        tm->RegionSetKerning(reg, false);
        for (size_t i = 0; i < rows.size(); ++i) {
            tm->AddText(reg, 16.0f, (i & 1) ? colA : colB, rows[i], true);
        }
    });

    // S3: Left region, fixed width, no auto-measure. Already direct before and
    // after; serves as an unchanged-baseline sanity check.
    runScenario("left_fixedwidth", [&] {
        auto reg = tm->CreateRegion(16, 64, TextManager::Align::Left);
        tm->RegionSetPadding(reg, 8, 6);
        tm->RegionSetBackground(reg, float4(0.0f, 0.0f, 0.05f, 0.75f));
        tm->RegionSetFixedWidth(reg, 760.0f);
        tm->RegionSetAutoMeasure(reg, false);
        tm->RegionSetKerning(reg, false);
        for (size_t i = 0; i < rows.size(); ++i) {
            tm->AddText(reg, 16.0f, (i & 1) ? colA : colB, rows[i], true);
        }
    });

    return 0;
}
