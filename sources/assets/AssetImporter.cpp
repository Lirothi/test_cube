#include "assets/AssetImporter.h"

// Vendored DirectXTex (third_party/DirectXTex) — CPU-path only (no D3D11/D3D12/DirectCompute
// translation units are compiled in; offline compression runs entirely on the CPU).
#include "DirectXTex/DirectXTex.h"

#include <Windows.h>

#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>

#include "third_party/json/json.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace DirectX;

namespace assets {
namespace {

//=============================================================================
// Logging — headless CLI runs in the Windows subsystem with no attached console,
// so mirror progress to a log file (matching the rt_smoke / scene_stress harness
// convention) and to the debugger (DBWIN) for live capture.
//=============================================================================
class Log
{
public:
    explicit Log(const std::string& path) : file_(path, std::ios::trunc) {}

    void Line(const std::string& s)
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (file_) { file_ << s << '\n'; file_.flush(); }
        OutputDebugStringA(("[import] " + s + "\n").c_str());
    }

private:
    std::mutex mu_;
    std::ofstream file_;
};

std::string Narrow(const std::wstring& w)
{
    if (w.empty()) { return {}; }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string LowerExt(const fs::path& p) { return Lower(p.extension().string()); }

bool IsWicImage(const std::string& ext)
{
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" ||
           ext == ".tif" || ext == ".tiff";
}

bool IsConvertibleTexture(const std::string& ext)
{
    return IsWicImage(ext) || ext == ".tga";
}

std::string Hex(HRESULT hr)
{
    char b[16];
    sprintf_s(b, "0x%08X", (unsigned)hr);
    return b;
}

//=============================================================================
// Texture role — decides the color space (sRGB vs linear) and BC target format.
// glTF/Sketchfab and Poly Haven both name maps by suffix, so classify by stem.
//=============================================================================
enum class TexRole { AlbedoSRGB, Normal, LinearData };

const char* RoleName(TexRole r)
{
    switch (r) { case TexRole::AlbedoSRGB: return "albedo(sRGB)";
                 case TexRole::Normal:     return "normal";
                 default:                  return "linear"; }
}

TexRole ClassifyRole(const std::string& stemLower)
{
    const auto has = [&](const char* s) { return stemLower.find(s) != std::string::npos; };
    if (has("normal") || has("_nor") || has("_nrm") || has("_norm")) { return TexRole::Normal; }
    if (has("metallicroughness") || has("metalrough") || has("roughness") || has("metallic") ||
        has("_mr") || has("_arm") || has("_orm") || has("occlusion") || has("_ao") ||
        has("_rough") || has("_metal") || has("displacement") || has("_disp") || has("height"))
    {
        return TexRole::LinearData;
    }
    // baseColor / albedo / diffuse / emissive read as sRGB color; also the safe default
    // (most standalone textures are color maps — a linear map mis-tagged sRGB is a smaller
    // error than the reverse, and the glTF/Poly Haven suffixes above catch the real data maps).
    return TexRole::AlbedoSRGB;
}

// Load any supported image and canonicalize to R8G8B8A8_UNORM (collapses BGRA / palettized /
// 16-bit sources). IGNORE_SRGB keeps raw bytes so the caller owns the color-space decision.
bool LoadRgba8(const fs::path& in, ScratchImage& out, Log& log, const std::string& label)
{
    const std::string ext = LowerExt(in);
    const std::wstring w = in.wstring();
    TexMetadata meta{};
    ScratchImage loaded;
    HRESULT hr = (ext == ".tga")
        ? LoadFromTGAFile(w.c_str(), TGA_FLAGS_NONE, &meta, loaded)
        : LoadFromWICFile(w.c_str(), WIC_FLAGS_IGNORE_SRGB, &meta, loaded);
    if (FAILED(hr)) { log.Line("  FAIL load " + Hex(hr) + "  " + label); return false; }

    if (loaded.GetMetadata().format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = Convert(loaded.GetImages(), loaded.GetImageCount(), loaded.GetMetadata(),
                     DXGI_FORMAT_R8G8B8A8_UNORM, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, out);
        if (FAILED(hr)) { log.Line("  FAIL convert " + Hex(hr) + "  " + label); return false; }
    }
    else { out = std::move(loaded); }
    return true;
}

// Shared compress tail: downscale to budget, tag sRGB, full mip chain, BC compress, save + verify.
bool FinishTextureDds(ScratchImage work, bool srgb, DXGI_FORMAT target, const ImportOptions& opts,
                      const fs::path& out, Log& log, const std::string& label)
{
    const UINT srcW = (UINT)work.GetMetadata().width, srcH = (UINT)work.GetMetadata().height;
    const UINT longest = std::max(srcW, srcH);
    HRESULT hr;
    if (opts.maxTextureSize > 0 && longest > (UINT)opts.maxTextureSize)
    {
        const double s = (double)opts.maxTextureSize / (double)longest;
        const size_t nW = std::max<size_t>(1, (size_t)std::lround(srcW * s));
        const size_t nH = std::max<size_t>(1, (size_t)std::lround(srcH * s));
        ScratchImage t;
        hr = Resize(work.GetImages(), work.GetImageCount(), work.GetMetadata(), nW, nH,
                    (srgb ? TEX_FILTER_SRGB : TEX_FILTER_DEFAULT) | TEX_FILTER_FANT, t);
        if (FAILED(hr)) { log.Line("  FAIL resize " + Hex(hr) + "  " + label); return false; }
        work = std::move(t);
    }

    if (srgb) { work.OverrideFormat(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB); }

    ScratchImage mipped;
    hr = GenerateMipMaps(work.GetImages(), work.GetImageCount(), work.GetMetadata(),
                         srgb ? TEX_FILTER_SRGB : TEX_FILTER_DEFAULT, 0, mipped);
    if (FAILED(hr)) { log.Line("  FAIL mips " + Hex(hr) + "  " + label); return false; }

    // TEX_COMPRESS_PARALLEL spreads BC blocks across cores (DirectXTex OpenMP path, enabled per-TU
    // in the vcxproj). The per-file loop below runs serially so this doesn't nest-oversubscribe.
    TEX_COMPRESS_FLAGS cflags = TEX_COMPRESS_DEFAULT | TEX_COMPRESS_PARALLEL;
    if (!opts.highQuality) { cflags |= TEX_COMPRESS_BC7_QUICK; }
    ScratchImage bc;
    hr = Compress(mipped.GetImages(), mipped.GetImageCount(), mipped.GetMetadata(),
                  target, cflags, TEX_THRESHOLD_DEFAULT, bc);
    if (FAILED(hr)) { log.Line("  FAIL compress " + Hex(hr) + "  " + label); return false; }

    hr = SaveToDDSFile(bc.GetImages(), bc.GetImageCount(), bc.GetMetadata(), DDS_FLAGS_NONE, out.wstring().c_str());
    if (FAILED(hr)) { log.Line("  FAIL save " + Hex(hr) + "  " + label); return false; }

    TexMetadata check{};
    const bool ok = SUCCEEDED(GetMetadataFromDDSFile(out.wstring().c_str(), DDS_FLAGS_NONE, check)) &&
                    check.mipLevels == bc.GetMetadata().mipLevels;
    log.Line(std::string(ok ? "  ok  " : "  WARN ") + label +
             "  " + std::to_string(bc.GetMetadata().width) + "x" + std::to_string(bc.GetMetadata().height) +
             "  fmt=" + std::to_string((int)bc.GetMetadata().format) +
             "  mips=" + std::to_string(bc.GetMetadata().mipLevels) +
             "  -> " + Narrow(out.filename().wstring()));
    return ok;
}

//=============================================================================
// Core conversion: PNG/JPG/TGA -> mipped BC7 (or BC5) DDS sibling.
//=============================================================================
bool ConvertTexture(const fs::path& in, const ImportOptions& opts, Log& log, const std::string& rel)
{
    const TexRole role = ClassifyRole(Lower(in.stem().string()));
    const bool srgb = (role == TexRole::AlbedoSRGB);

    ScratchImage work;
    if (!LoadRgba8(in, work, log, rel)) { return false; }

    // Optional normal-map green flip (OpenGL +Y <-> DirectX -Y). Off by default until the engine's
    // convention is pinned; glTF normals ship +Y and currently render fine.
    if (role == TexRole::Normal && opts.flipGreen)
    {
        ScratchImage t;
        if (SUCCEEDED(TransformImage(work.GetImages(), work.GetImageCount(), work.GetMetadata(),
            [](XMVECTOR* out, const XMVECTOR* in, size_t width, size_t)
            { for (size_t j = 0; j < width; ++j) { out[j] = XMVectorSetY(in[j], 1.0f - XMVectorGetY(in[j])); } }, t)))
        {
            work = std::move(t);
        }
    }

    DXGI_FORMAT target = DXGI_FORMAT_BC7_UNORM;
    if (srgb) { target = DXGI_FORMAT_BC7_UNORM_SRGB; }
    else if (role == TexRole::Normal && opts.bc5Normal) { target = DXGI_FORMAT_BC5_UNORM; }

    fs::path out = in; out.replace_extension(L".dds");
    return FinishTextureDds(std::move(work), srgb, target, opts, out, log, rel + " [" + RoleName(role) + "]");
}

//=============================================================================
// H1c — texture-set import (Poly Haven style: separate grayscale maps, no glTF).
// Synthesizes an ENGINE-layout MR (R=metal, G=rough) from the separate rough/metal
// maps and registers a material preset for the set.
//=============================================================================
struct PresetEntry { std::string name, albedo, mr, normal; };

// Path stored in a preset: relative to the working dir with forward slashes (engine-loadable).
std::string PresetPath(const fs::path& p)
{
    std::error_code ec;
    const fs::path rel = fs::relative(p, fs::current_path(), ec);
    std::string s = Narrow(((!ec && !rel.empty()) ? rel : p).wstring());
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

// Combine separate rough (+ optional metal) grayscale maps into one engine MR texture
// (R=metal — 0 where no metal map exists, the normal case for sand/rock; G=rough) -> BC7 DDS.
bool SynthesizeMR(const fs::path& rough, const fs::path* metal, const ImportOptions& opts,
                  const fs::path& out, Log& log)
{
    ScratchImage rimg;
    if (!LoadRgba8(rough, rimg, log, "mr:rough")) { return false; }
    const size_t W = rimg.GetMetadata().width, H = rimg.GetMetadata().height;

    ScratchImage mimg;
    const Image* md = nullptr;
    if (metal && LoadRgba8(*metal, mimg, log, "mr:metal"))
    {
        if (mimg.GetMetadata().width != W || mimg.GetMetadata().height != H)
        {
            ScratchImage t;
            if (SUCCEEDED(Resize(mimg.GetImages(), mimg.GetImageCount(), mimg.GetMetadata(), W, H, TEX_FILTER_DEFAULT, t)))
            {
                mimg = std::move(t);
            }
        }
        md = mimg.GetImage(0, 0, 0);
    }

    ScratchImage mr;
    if (FAILED(mr.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, 1))) { log.Line("  FAIL mr init"); return false; }
    const Image* rd = rimg.GetImage(0, 0, 0);
    const Image* dst = mr.GetImage(0, 0, 0);
    for (size_t y = 0; y < H; ++y)
    {
        const uint8_t* rr = rd->pixels + y * rd->rowPitch;
        const uint8_t* mm = md ? md->pixels + y * md->rowPitch : nullptr;
        uint8_t* o = dst->pixels + y * dst->rowPitch;
        for (size_t x = 0; x < W; ++x)
        {
            o[x * 4 + 0] = mm ? mm[x * 4 + 0] : 0; // R = metal (0 = dielectric)
            o[x * 4 + 1] = rr[x * 4 + 0];          // G = rough (grayscale rough map)
            o[x * 4 + 2] = 0;
            o[x * 4 + 3] = 255;
        }
    }
    return FinishTextureDds(std::move(mr), /*srgb*/ false, DXGI_FORMAT_BC7_UNORM, opts, out, log, "mr(synth)");
}

// True if `dir` is a texture set (has a diffuse map and a separate rough map). glTF assets pack
// metal+rough together (metallicRoughness) so they lack a standalone `_rough` and won't match.
bool GatherTextureSet(const fs::path& dir, fs::path& diff, fs::path& rough, fs::path& metal, fs::path& nor)
{
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
    {
        if (!e.is_regular_file(ec) || !IsConvertibleTexture(LowerExt(e.path()))) { continue; }
        const std::string s = Lower(e.path().stem().string());
        const auto has = [&](const char* k) { return s.find(k) != std::string::npos; };
        if (diff.empty() && (has("_diff") || has("_albedo") || has("_col") || has("basecolor"))) { diff = e.path(); }
        else if (rough.empty() && has("_rough")) { rough = e.path(); }
        else if (metal.empty() && (has("_metal") || has("metallic"))) { metal = e.path(); }
        else if (nor.empty() && (has("_nor") || has("normal"))) { nor = e.path(); }
    }
    return !diff.empty() && !rough.empty();
}

bool ImportTextureSet(const fs::path& dir, const fs::path& diff, const fs::path& rough,
                      const fs::path& metal, const fs::path& nor, const ImportOptions& opts,
                      Log& log, PresetEntry& out)
{
    // Set name = the map prefix before its role token (e.g. coast_sand_01_diff_2k -> coast_sand_01).
    std::string name = Lower(diff.stem().string());
    for (const char* tok : { "_diff", "_albedo", "_col", "basecolor" })
    {
        const auto p = name.find(tok);
        if (p != std::string::npos) { name = name.substr(0, p); break; }
    }
    if (name.empty()) { name = Lower(dir.filename().string()); }
    log.Line("texture-set '" + name + "'");
    out.name = name;

    ScratchImage a;
    if (!LoadRgba8(diff, a, log, name + " albedo")) { return false; }
    const fs::path aOut = dir / (name + "_albedo.dds");
    if (!FinishTextureDds(std::move(a), true, DXGI_FORMAT_BC7_UNORM_SRGB, opts, aOut, log, name + " [albedo]")) { return false; }
    out.albedo = PresetPath(aOut);

    if (!nor.empty())
    {
        ScratchImage n;
        if (LoadRgba8(nor, n, log, name + " normal"))
        {
            if (opts.flipGreen)
            {
                ScratchImage t;
                if (SUCCEEDED(TransformImage(n.GetImages(), n.GetImageCount(), n.GetMetadata(),
                    [](XMVECTOR* o, const XMVECTOR* i, size_t width, size_t)
                    { for (size_t j = 0; j < width; ++j) { o[j] = XMVectorSetY(i[j], 1.0f - XMVectorGetY(i[j])); } }, t)))
                {
                    n = std::move(t);
                }
            }
            const fs::path nOut = dir / (name + "_normal.dds");
            if (FinishTextureDds(std::move(n), false, DXGI_FORMAT_BC7_UNORM, opts, nOut, log, name + " [normal]"))
            {
                out.normal = PresetPath(nOut);
            }
        }
    }

    const fs::path mrOut = dir / (name + "_mr.dds");
    if (SynthesizeMR(rough, metal.empty() ? nullptr : &metal, opts, mrOut, log)) { out.mr = PresetPath(mrOut); }

    return true;
}

// Register texture-set presets in data/materials.json. The file is already in nlohmann-canonical
// form (sorted keys, 2-space) so parse -> insert -> dump(2) yields a clean minimal diff.
void WritePresets(const std::vector<PresetEntry>& entries, Log& log)
{
    if (entries.empty()) { return; }
    const char* path = "data/materials.json";
    nlohmann::json j = nlohmann::json::object();
    {
        std::ifstream f(path);
        if (f)
        {
            std::stringstream ss; ss << f.rdbuf();
            nlohmann::json parsed = nlohmann::json::parse(ss.str(), nullptr, false, true);
            if (!parsed.is_discarded() && parsed.is_object()) { j = std::move(parsed); }
        }
    }
    if (!j.contains("presets") || !j["presets"].is_object()) { j["presets"] = nlohmann::json::object(); }

    for (const auto& e : entries)
    {
        nlohmann::json p;
        p["albedo"] = e.albedo;
        if (!e.mr.empty()) { p["mr"] = e.mr; }
        if (!e.normal.empty()) { p["normal"] = e.normal; }
        p["normalIsRG"] = false;
        p["useTBN"] = true;
        j["presets"][e.name] = p;
        log.Line("registered preset '" + e.name + "'");
    }

    std::ofstream out(path, std::ios::trunc);
    out << j.dump(2) << "\n";
}

//=============================================================================
// H1d — frame-sequence -> flipbook atlas. Detects *_frame_NN / *_NN sequences,
// resamples each frame to a power-of-two cell, packs a grid, premultiplies alpha,
// and emits <base>.flipbook.json metadata (cols/rows/frames) for the E4 particle presets.
//=============================================================================
struct Sequence { std::string base; std::vector<fs::path> frames; };

std::vector<Sequence> DetectSequences(const fs::path& dir)
{
    struct Item { std::string base; bool hadFrame; int num; fs::path path; };
    std::vector<Item> items;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
    {
        if (!e.is_regular_file(ec) || !IsConvertibleTexture(LowerExt(e.path()))) { continue; }
        std::string stem = e.path().stem().string();
        size_t end = stem.size();
        while (end > 0 && std::isdigit((unsigned char)stem[end - 1])) { --end; }
        if (end == stem.size() || end == 0) { continue; }               // needs trailing digits
        const int num = std::atoi(stem.c_str() + end);
        std::string base = stem.substr(0, end);
        while (!base.empty() && base.back() == '_') { base.pop_back(); } // strip separator
        bool hadFrame = false;
        if (base.size() >= 6 && Lower(base.substr(base.size() - 6)) == "_frame")
        {
            base = base.substr(0, base.size() - 6); hadFrame = true;
            while (!base.empty() && base.back() == '_') { base.pop_back(); }
        }
        items.push_back({ Lower(base), hadFrame, num, e.path() });
    }

    std::map<std::string, std::vector<Item>> groups;
    for (auto& it : items) { groups[it.base].push_back(it); }
    const bool singleGroup = groups.size() == 1;

    std::vector<Sequence> out;
    for (auto& [base, its] : groups)
    {
        if (its.size() < 3) { continue; }
        bool anyFrame = false;
        for (auto& it : its) { anyFrame |= it.hadFrame; }
        if (!anyFrame && !singleGroup) { continue; } // avoid packing distinct sprite variants
        std::sort(its.begin(), its.end(), [](const Item& a, const Item& b) { return a.num < b.num; });
        Sequence s; s.base = base;
        for (auto& it : its) { s.frames.push_back(it.path); }
        out.push_back(std::move(s));
    }
    return out;
}

bool BuildFlipbook(const fs::path& dir, const Sequence& seq, const ImportOptions& opts, Log& log)
{
    const int N = (int)seq.frames.size();
    const int rows = std::max(1, (int)std::floor(std::sqrt((double)N))); // wide grid (8 -> 4x2)
    const int cols = (N + rows - 1) / rows;

    ScratchImage f0;
    if (!LoadRgba8(seq.frames[0], f0, log, seq.base + " frame0")) { return false; }
    const auto nextPot = [](size_t v) { size_t p = 1; while (p < v) { p <<= 1; } return p; };
    const size_t cell = std::min<size_t>(nextPot(std::max(f0.GetMetadata().width, f0.GetMetadata().height)),
                                         (size_t)std::max(64, opts.maxTextureSize));

    ScratchImage atlas;
    if (FAILED(atlas.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, cols * cell, rows * cell, 1, 1)))
    {
        log.Line("  FAIL flipbook atlas init"); return false;
    }
    const Image* ai = atlas.GetImage(0, 0, 0);
    std::memset(ai->pixels, 0, ai->slicePitch);

    for (int i = 0; i < N; ++i)
    {
        ScratchImage fi;
        if (i == 0) { fi = std::move(f0); }
        else if (!LoadRgba8(seq.frames[i], fi, log, seq.base + " frame")) { continue; }

        const ScratchImage* src = &fi;
        ScratchImage rf;
        if (fi.GetMetadata().width != cell || fi.GetMetadata().height != cell)
        {
            if (SUCCEEDED(Resize(fi.GetImages(), fi.GetImageCount(), fi.GetMetadata(), cell, cell, TEX_FILTER_FANT, rf)))
            {
                src = &rf;
            }
        }
        const Image* si = src->GetImage(0, 0, 0);
        const size_t cx = (size_t)(i % cols) * cell, cy = (size_t)(i / cols) * cell;
        for (size_t y = 0; y < cell; ++y)
        {
            const uint8_t* s = si->pixels + y * si->rowPitch;
            uint8_t* d = ai->pixels + (cy + y) * ai->rowPitch + cx * 4;
            for (size_t x = 0; x < cell; ++x)
            {
                const unsigned a = s[x * 4 + 3];
                d[x * 4 + 0] = (uint8_t)(s[x * 4 + 0] * a / 255); // premultiply
                d[x * 4 + 1] = (uint8_t)(s[x * 4 + 1] * a / 255);
                d[x * 4 + 2] = (uint8_t)(s[x * 4 + 2] * a / 255);
                d[x * 4 + 3] = (uint8_t)a;
            }
        }
    }

    // BC7 sRGB, NO mip chain (mips would bleed color across cell boundaries in a packed atlas).
    atlas.OverrideFormat(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    TEX_COMPRESS_FLAGS cflags = TEX_COMPRESS_DEFAULT | TEX_COMPRESS_PARALLEL;
    if (!opts.highQuality) { cflags |= TEX_COMPRESS_BC7_QUICK; }
    ScratchImage bc;
    if (FAILED(Compress(*ai, DXGI_FORMAT_BC7_UNORM_SRGB, cflags, TEX_THRESHOLD_DEFAULT, bc)))
    {
        // OverrideFormat retagged ai in place, so recompress from the (now sRGB) atlas image.
        log.Line("  FAIL flipbook compress"); return false;
    }

    const fs::path atlasOut = dir / (seq.base + "_flipbook.dds");
    if (FAILED(SaveToDDSFile(bc.GetImages(), bc.GetImageCount(), bc.GetMetadata(), DDS_FLAGS_NONE, atlasOut.wstring().c_str())))
    {
        log.Line("  FAIL flipbook save"); return false;
    }

    nlohmann::json m;
    m["atlas"] = PresetPath(atlasOut);
    m["cols"] = cols; m["rows"] = rows; m["frames"] = N;
    m["cellWidth"] = (int)cell; m["cellHeight"] = (int)cell;
    m["premultipliedAlpha"] = true;
    std::ofstream(dir / (seq.base + ".flipbook.json"), std::ios::trunc) << m.dump(2) << "\n";

    log.Line("  ok  flipbook '" + seq.base + "'  " + std::to_string(cols) + "x" + std::to_string(rows) +
             "  " + std::to_string(N) + " frames  cell=" + std::to_string(cell) +
             "  -> " + Narrow(atlasOut.filename().wstring()));
    return true;
}

//=============================================================================
// HDRI skybox: equirectangular .hdr -> 6-face cubemap -> BC6H_UF16 DX10 DDS.
// The cube's face-direction and equirect conventions match the engine's skybox
// sampling (verified by rendering); DirectXTex writes the DX10 TEXTURECUBE flag
// plus the legacy caps2 cube flags the TextureCube loader keys off.
//=============================================================================

// Bilinear equirect sample (wrap longitude, clamp latitude). eq is R32G32B32A32_FLOAT.
void SampleEquirect(const Image& eq, float u, float v, float out[4])
{
    const float fx = u * (float)eq.width - 0.5f;
    const float fy = v * (float)eq.height - 0.5f;
    const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    const float tx = fx - (float)x0, ty = fy - (float)y0;
    const int W = (int)eq.width, H = (int)eq.height;
    const auto wrapX = [W](int x) { x %= W; if (x < 0) { x += W; } return (size_t)x; };
    const auto clampY = [H](int y) { return (size_t)std::min(std::max(y, 0), H - 1); };
    const auto tap = [&](int x, int y, float* c)
    {
        const float* row = reinterpret_cast<const float*>(eq.pixels + clampY(y) * eq.rowPitch);
        const float* px = row + wrapX(x) * 4;
        c[0] = px[0]; c[1] = px[1]; c[2] = px[2]; c[3] = px[3];
    };
    float c00[4], c10[4], c01[4], c11[4];
    tap(x0, y0, c00); tap(x0 + 1, y0, c10); tap(x0, y0 + 1, c01); tap(x0 + 1, y0 + 1, c11);
    for (int i = 0; i < 4; ++i)
    {
        const float a = c00[i] * (1 - tx) + c10[i] * tx;
        const float b = c01[i] * (1 - tx) + c11[i] * tx;
        out[i] = a * (1 - ty) + b * ty;
    }
}

// Standard D3D cube face -> direction (u,v in [-1,1], u right, v down).
XMFLOAT3 CubeFaceDir(int face, float u, float v)
{
    switch (face)
    {
    case 0:  return { 1.0f,   -v,   -u }; // +X
    case 1:  return { -1.0f,  -v,    u }; // -X
    case 2:  return {  u,   1.0f,    v }; // +Y
    case 3:  return {  u,  -1.0f,   -v }; // -Y
    case 4:  return {  u,    -v, 1.0f }; // +Z
    default: return { -u,    -v, -1.0f }; // -Z
    }
}

bool ConvertSkyboxHdr(const fs::path& in, const ImportOptions& opts, Log& log)
{
    const std::wstring w = in.wstring();

    // 1) Load the equirect .hdr as float and force RGBA32F for sampling.
    TexMetadata meta{};
    ScratchImage hdr;
    HRESULT hr = LoadFromHDRFile(w.c_str(), &meta, hdr);
    if (FAILED(hr)) { log.Line("skybox FAIL load " + Hex(hr)); return false; }

    ScratchImage eq;
    if (hdr.GetMetadata().format != DXGI_FORMAT_R32G32B32A32_FLOAT)
    {
        hr = Convert(hdr.GetImages(), hdr.GetImageCount(), hdr.GetMetadata(),
                     DXGI_FORMAT_R32G32B32A32_FLOAT, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, eq);
        if (FAILED(hr)) { log.Line("skybox FAIL convert " + Hex(hr)); return false; }
    }
    else { eq = std::move(hdr); }
    const Image& eqImg = *eq.GetImage(0, 0, 0);

    // 2) Face size — default to source-width/4 (equirect longitude spans 4 faces) capped by option.
    int face = opts.skyboxFaceSize > 0 ? opts.skyboxFaceSize : (int)(eqImg.width / 4);
    face = std::max(64, std::min(face, 2048));

    // 3) Project into a 6-face float cube (faces filled in parallel; the math is per-texel).
    ScratchImage cube;
    hr = cube.InitializeCube(DXGI_FORMAT_R32G32B32A32_FLOAT, (size_t)face, (size_t)face, 1, 1);
    if (FAILED(hr)) { log.Line("skybox FAIL init cube " + Hex(hr)); return false; }

    tbb::parallel_for(0, 6, [&](int f)
    {
        const Image* dst = cube.GetImage(0, (size_t)f, 0);
        for (int y = 0; y < face; ++y)
        {
            float* row = reinterpret_cast<float*>(dst->pixels + (size_t)y * dst->rowPitch);
            const float vv = 2.0f * ((float)y + 0.5f) / (float)face - 1.0f;
            for (int x = 0; x < face; ++x)
            {
                const float uu = 2.0f * ((float)x + 0.5f) / (float)face - 1.0f;
                XMFLOAT3 d = CubeFaceDir(f, uu, vv);
                const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                d.x /= len; d.y /= len; d.z /= len;
                const float lon = std::atan2(d.x, d.z);                  // [-pi, pi]
                const float lat = std::asin(std::min(std::max(d.y, -1.0f), 1.0f));
                const float su = 0.5f + lon / (2.0f * 3.14159265358979f);
                const float sv = 0.5f - lat / 3.14159265358979f;
                SampleEquirect(eqImg, su, sv, row + (size_t)x * 4);
            }
        }
    });

    // 4) Mip chain, then BC6H_UF16 compress (unsigned half — sky radiance is non-negative).
    ScratchImage cubeMips;
    hr = GenerateMipMaps(cube.GetImages(), cube.GetImageCount(), cube.GetMetadata(),
                         TEX_FILTER_DEFAULT, 0, cubeMips);
    if (FAILED(hr)) { log.Line("skybox FAIL mips " + Hex(hr)); return false; }

    fs::path out = in; out.replace_extension(L".dds");
    ScratchImage bc;
    hr = Compress(cubeMips.GetImages(), cubeMips.GetImageCount(), cubeMips.GetMetadata(),
                  DXGI_FORMAT_BC6H_UF16, TEX_COMPRESS_DEFAULT | TEX_COMPRESS_PARALLEL, TEX_THRESHOLD_DEFAULT, bc);
    if (SUCCEEDED(hr))
    {
        hr = SaveToDDSFile(bc.GetImages(), bc.GetImageCount(), bc.GetMetadata(), DDS_FLAGS_NONE, out.wstring().c_str());
    }
    else
    {
        // BC6H unavailable — fall back to an uncompressed RGBA16F cube (loader handles either).
        log.Line("skybox WARN BC6H compress " + Hex(hr) + " — saving RGBA16F fallback");
        ScratchImage half;
        hr = Convert(cubeMips.GetImages(), cubeMips.GetImageCount(), cubeMips.GetMetadata(),
                     DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, half);
        if (SUCCEEDED(hr))
        {
            hr = SaveToDDSFile(half.GetImages(), half.GetImageCount(), half.GetMetadata(), DDS_FLAGS_NONE, out.wstring().c_str());
        }
    }
    if (FAILED(hr)) { log.Line("skybox FAIL save " + Hex(hr)); return false; }

    TexMetadata check{};
    const bool ok = SUCCEEDED(GetMetadataFromDDSFile(out.wstring().c_str(), DDS_FLAGS_NONE, check)) && check.IsCubemap();
    log.Line(std::string(ok ? "  ok  skybox  " : "  WARN skybox ") + std::to_string(face) + "^2 x6" +
             "  fmt=" + std::to_string((int)check.format) + "  mips=" + std::to_string(check.mipLevels) +
             "  -> " + Narrow(out.filename().wstring()));
    return ok;
}

} // namespace

int RunImport(const ImportOptions& opts)
{
    Log log(opts.logPath);
    log.Line("=== asset import (H1) ===");
    log.Line("staging dir : " + (opts.stagingDir.empty() ? "(none)" : opts.stagingDir));
    if (!opts.skyboxHdr.empty()) { log.Line("skybox hdr  : " + opts.skyboxHdr); }
    log.Line("max size    : " + std::to_string(opts.maxTextureSize) +
             (opts.highQuality ? "  quality=high" : "  quality=fast") +
             (opts.flipGreen ? "  flip-green" : "") + (opts.bc5Normal ? "  bc5-normal" : ""));

    // WIC (and the WIC-backed DirectXTex readers) require COM on this thread.
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInited = SUCCEEDED(hrCo);

    int failures = 0;

    // Skybox first (independent of the texture scan; --skybox may point outside the staging dir).
    if (!opts.skyboxHdr.empty())
    {
        std::error_code sec;
        if (!fs::exists(opts.skyboxHdr, sec)) { log.Line("skybox FATAL: file not found"); ++failures; }
        else if (!ConvertSkyboxHdr(opts.skyboxHdr, opts, log)) { ++failures; }
    }

    std::error_code ec;
    const bool haveDir = !opts.stagingDir.empty() && fs::exists(opts.stagingDir, ec) && fs::is_directory(opts.stagingDir, ec);
    if (!haveDir)
    {
        if (opts.skyboxHdr.empty()) { log.Line("FATAL: staging dir does not exist or is not a directory"); failures = 1; }
        log.Line("=== done ===");
        if (coInited) { CoUninitialize(); }
        return failures;
    }

    // Optional file whitelist from the H3 import dialog (empty = convert everything). Matched on the
    // staging-relative, normalized, lowercased path — the panel builds includeRel identically.
    std::set<std::string> includeSet;
    for (const auto& r : opts.includeRel)
    {
        includeSet.insert(Lower(fs::path(r).lexically_normal().generic_string()));
    }
    const auto included = [&](const fs::path& p) -> bool
    {
        if (includeSet.empty()) { return true; }
        std::error_code rec;
        const fs::path rel = fs::relative(p, opts.stagingDir, rec);
        return includeSet.count(Lower(rel.lexically_normal().generic_string())) != 0;
    };

    // Find every directory holding convertible images (for texture-set detection), and count the
    // convertible files so the UI progress bar knows the total workload.
    std::set<fs::path> imageDirs;
    int convertibleCount = 0;
    for (auto it = fs::recursive_directory_iterator(opts.stagingDir, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec) { break; }
        if (it->is_regular_file(ec) && IsConvertibleTexture(LowerExt(it->path())) && included(it->path()))
        {
            imageDirs.insert(it->path().parent_path());
            ++convertibleCount;
        }
    }
    if (opts.progressTotal) { opts.progressTotal->store(convertibleCount); }
    if (opts.progressDone) { opts.progressDone->store(0); }
    const auto bumpProgress = [&opts](int n)
    {
        if (opts.progressDone) { opts.progressDone->fetch_add(n, std::memory_order_relaxed); }
    };

    // H1c: texture-set pass (Poly Haven-style separate maps) — synth MR + register a preset. Skipped
    // when the dialog unchecks "create preset" (every image then converts as a loose DDS instead).
    std::vector<PresetEntry> presets;
    std::set<fs::path> setDirs;       // dirs handled as a set (skip their extras in the loose pass)
    std::set<fs::path> consumedFiles; // individual files already converted (set maps / flipbook frames)
    if (opts.registerPreset)
    {
        for (const auto& d : imageDirs)
        {
            fs::path diff, rough, metal, nor;
            if (!GatherTextureSet(d, diff, rough, metal, nor)) { continue; }
            // Honor the dialog's per-map selection.
            if (!diff.empty()  && !included(diff))  { diff.clear(); }
            if (!rough.empty() && !included(rough)) { rough.clear(); }
            if (!metal.empty() && !included(metal)) { metal.clear(); }
            if (!nor.empty()   && !included(nor))   { nor.clear(); }
            if (diff.empty()) { continue; } // no albedo selected -> not a usable set
            PresetEntry pe;
            if (ImportTextureSet(d, diff, rough, metal, nor, opts, log, pe)) { presets.push_back(pe); setDirs.insert(d); }
            else { ++failures; }
            for (const fs::path* m : { &diff, &rough, &metal, &nor }) { if (!m->empty()) { consumedFiles.insert(*m); } }
            bumpProgress((!diff.empty()) + (!rough.empty()) + (!metal.empty()) + (!nor.empty()));
        }
    }

    // H1d: flipbook pass (frame sequences). Consumed frame files are excluded from the per-file pass.
    int flipbooks = 0;
    for (const auto& d : imageDirs)
    {
        if (setDirs.count(d)) { continue; }
        for (Sequence seq : DetectSequences(d))
        {
            if (!includeSet.empty())
            {
                std::vector<fs::path> keep;
                for (const auto& f : seq.frames) { if (included(f)) { keep.push_back(f); } }
                seq.frames = std::move(keep);
            }
            if (seq.frames.empty()) { continue; }
            if (BuildFlipbook(d, seq, opts, log)) { ++flipbooks; for (const auto& f : seq.frames) { consumedFiles.insert(f); } }
            else { ++failures; }
            bumpProgress(static_cast<int>(seq.frames.size()));
        }
    }

    // Per-file pass (H1b): every convertible image not consumed above. BC7 is CPU-heavy, so one
    // texture per task keeps every core busy. With an explicit dialog selection (includeRel) the
    // whole-set-dir skip is relaxed so hand-picked extras (AO, etc.) still convert; without it, a
    // set dir's non-map files are skipped as before.
    struct Job { fs::path path; std::string rel; };
    std::vector<Job> jobs;
    for (auto it = fs::recursive_directory_iterator(opts.stagingDir, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec) { break; }
        const fs::path& p = it->path();
        if (!it->is_regular_file(ec)) { continue; }
        if (!IsConvertibleTexture(LowerExt(p))) { continue; }
        if (!included(p)) { continue; }
        if (consumedFiles.count(p)) { continue; }
        if (includeSet.empty() && setDirs.count(p.parent_path())) { continue; }
        jobs.push_back({ p, Narrow(fs::relative(p, opts.stagingDir, ec).wstring()) });
    }

    // Serial across textures — each Compress already parallelizes its blocks across cores via
    // TEX_COMPRESS_PARALLEL (OpenMP), so a tbb fan-out here would nest and oversubscribe.
    int converted = 0;
    for (const auto& job : jobs)
    {
        if (ConvertTexture(job.path, opts, log, job.rel)) { ++converted; }
        else { ++failures; }
        bumpProgress(1);
    }

    // Snap the bar to 100% — set/flipbook dirs can hold extra files the per-unit bumps under-count.
    if (opts.progressDone && opts.progressTotal)
    {
        opts.progressDone->store(opts.progressTotal->load(), std::memory_order_relaxed);
    }

    // Register the texture-set presets in one read-modify-write of data/materials.json.
    WritePresets(presets, log);

    log.Line("converted: " + std::to_string(converted) + " loose + " + std::to_string(presets.size()) +
             " sets + " + std::to_string(flipbooks) + " flipbooks, failures: " + std::to_string(failures));
    log.Line("=== done ===");

    if (coInited) { CoUninitialize(); }
    return failures;
}

} // namespace assets
