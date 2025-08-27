#include <cstdarg>
#include <cstdio>
#include "TextManager.h"
#include "UploadManager.h"
#include "SamplerManager.h"
#include "InputLayoutManager.h"

using Microsoft::WRL::ComPtr;

static std::string VFormat_(const char* fmt, va_list args) {
    if (fmt == nullptr) {
        return std::string();
    }
    va_list copy;
    va_copy(copy, args);
    int needed = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);

    if (needed <= 0) {
        return std::string();
    }
    std::string s;
    s.resize((size_t)needed);
    std::vsnprintf(s.data(), (size_t)needed + 1, fmt, args);
    return s;
}

void TextManager::Init(Renderer* r) {
    // материал под SDF
    Material::GraphicsDesc gd;
    gd.shaderFile = L"shaders/font_sdf.hlsl";
    gd.inputLayoutKey = "PosColorUV";
    gd.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gd.depth.DepthEnable = FALSE;
    gd.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    gd.raster.CullMode = D3D12_CULL_MODE_NONE;
    gd.blend.RenderTarget[0].BlendEnable = TRUE;
    gd.blend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    gd.blend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    gd.blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    gd.blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    gd.blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    gd.blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    gd.blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    mat_ = r->GetMaterialManager()->GetOrCreateGraphics(r, gd);
}

void TextManager::Begin(UINT vpW, UINT vpH, float dpiScale) {
    vpW_ = (vpW == 0u ? 1u : vpW);
    vpH_ = (vpH == 0u ? 1u : vpH);
    dpi_ = (dpiScale <= 0.f ? 1.f : dpiScale);
    verts_.clear();
    idx_.clear();
}

static std::wstring UTF8toW(const std::string& s) {
    if (s.empty()) { return L""; }
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w; w.resize((size_t)wlen);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), wlen);
    return w;
}

void TextManager::AddText(int x, int y, uint32_t rgba, float px, const std::string& utf8) {
    if (font_ == nullptr) {
        return;
    }

    const float r = ((rgba >> 24) & 0xFF) / 255.f;
    const float g = ((rgba >> 16) & 0xFF) / 255.f;
    const float b = ((rgba >> 8) & 0xFF) / 255.f;
    const float a = (rgba & 0xFF) / 255.f;

    const float scale = px / float(font_->PxSize());
    float       penX = float(x);
    float       penY = float(y) + float(font_->Ascent()) * scale; // baseline без floor

    std::wstring w = UTF8toW(utf8);
    uint32_t prev = 0;

    if (!w.empty()) {
        verts_.reserve(verts_.size() + w.size() * 4);
        idx_.reserve(idx_.size() + w.size() * 6);
    }

    for (wchar_t wc : w) {
        if (wc == L'\n') {
            penX = float(x);
            penY += float(font_->LineAdvance()) * scale; // без floor
            prev = 0;
            continue;
        }

        // space / tab
        if (wc == L' ' || wc == L'\t') {
            const FontGlyph* gsp = font_->Find((uint32_t)wc);
            float adv = 0.0f;
            if (gsp != nullptr) {
                adv = float(gsp->xadv) * scale;
            }
            else {
                const FontGlyph* gn = font_->Find((uint32_t)'n');
                const float em = (gn != nullptr ? float(gn->xadv) : float(font_->PxSize()));
                adv = (wc == L'\t' ? em * 2.0f : em * 0.5f) * scale;
            }
            penX += adv;
            prev = 0;
            continue;
        }

        const uint32_t cp = (uint32_t)wc;
        const FontGlyph* gph = font_->Find(cp);
        if (gph == nullptr) {
            prev = 0;
            continue;
        }

        if (prev != 0u) {
            penX += float(font_->Kerning(prev, cp)) * scale;
        }

        if (gph->w == 0 || gph->h == 0) {
            penX += float(gph->xadv) * scale;
            prev = cp;
            continue;
        }

        const float gx = penX + float(gph->xoff) * scale; // БЕЗ floor
        const float gy = penY + float(gph->yoff) * scale; // БЕЗ floor
        const float gw = float(gph->w) * scale;
        const float gh = float(gph->h) * scale;

        const uint32_t base = (uint32_t)verts_.size();
        verts_.push_back({ gx,      gy,      0, r,g,b,a, gph->u0, gph->v0 });
        verts_.push_back({ gx + gw, gy,      0, r,g,b,a, gph->u1, gph->v0 });
        verts_.push_back({ gx + gw, gy + gh, 0, r,g,b,a, gph->u1, gph->v1 });
        verts_.push_back({ gx,      gy + gh, 0, r,g,b,a, gph->u0, gph->v1 });

        idx_.push_back(base + 0u); idx_.push_back(base + 1u); idx_.push_back(base + 2u);
        idx_.push_back(base + 0u); idx_.push_back(base + 2u); idx_.push_back(base + 3u);

        penX += float(gph->xadv) * scale;
        prev = cp;
    }
}

void TextManager::AddTextf(int x, int y, uint32_t rgba, float px, const char* fmt, ...) {
    if (fmt == nullptr) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    std::string s = VFormat_(fmt, args);
    va_end(args);
    if (!s.empty()) {
        AddText(x, y, rgba, px, s);
    }
}

void TextManager::AddTextf(int x, int y, float r, float g, float b, float a, float px, const char* fmt, ...) {
    if (fmt == nullptr) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    std::string s = VFormat_(fmt, args);
    va_end(args);
    if (!s.empty()) {
        AddText(x, y, RGBA(r, g, b, a), px, s);
    }
}

void TextManager::Build(Renderer* r, ID3D12GraphicsCommandList* /*cl*/) {
    if (verts_.empty() || idx_.empty() || font_ == nullptr) {
        return;
    }

    FrameResource* fr = r->GetFrameResource();

    const UINT vbBytes = static_cast<UINT>(verts_.size() * sizeof(Vertex));
    const UINT ibBytes = static_cast<UINT>(idx_.size() * sizeof(uint32_t));

    auto v = fr->AllocDynamic(vbBytes, 16);
    auto i = fr->AllocDynamic(ibBytes, 16);

    if (vbBytes > 0u) {
        std::memcpy(v.cpu, verts_.data(), vbBytes);
    }
    if (ibBytes > 0u) {
        std::memcpy(i.cpu, idx_.data(), ibBytes);
    }

    vbv_.BufferLocation = v.gpu;
    vbv_.StrideInBytes = sizeof(Vertex);
    vbv_.SizeInBytes = vbBytes;

    ibv_.BufferLocation = i.gpu;
    ibv_.Format = DXGI_FORMAT_R32_UINT;
    ibv_.SizeInBytes = ibBytes;

    // t0 — SRV атласа
    auto tbl = r->StageSrvUavTable({ font_->GetSRVCPU() });
    rc_.table[0] = tbl.gpu;
}

void TextManager::Draw(Renderer* r, ID3D12GraphicsCommandList* cl) {
    if (verts_.empty() || idx_.empty() || font_ == nullptr) {
        return;
    }

    // root constants: viewport.xy, dummy, spread, pxSize
    std::vector<uint32_t> k;
    k.reserve(8);
    auto f2u = [](float f)->uint32_t { uint32_t u; std::memcpy(&u, &f, 4); return u; };
    k.push_back(f2u((float)vpW_)); k.push_back(f2u((float)vpH_));
    k.push_back(0); k.push_back(0);
    k.push_back(f2u((float)font_->Spread()));
    k.push_back(f2u((float)font_->PxSize()));
    k.push_back(0); k.push_back(0);
    rc_.constants[1] = std::move(k);

    // Самплер (linear clamp)
    rc_.samplerTable[0] = r->GetSamplerManager()->GetTable(r, { SamplerManager::LinearClamp() });

    mat_->Bind(cl, rc_);

    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->IASetVertexBuffers(0, 1, &vbv_);
    cl->IASetIndexBuffer(&ibv_);
    cl->DrawIndexedInstanced((UINT)idx_.size(), 1, 0, 0, 0);
}

void TextManager::Clear()
{
	mat_.reset();
    verts_.clear();
    idx_.clear();
}
