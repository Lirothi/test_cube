#include "assets/AssetImporter.h"

// Vendored DirectXTex (third_party/DirectXTex) — CPU-path only (no D3D11/D3D12/DirectCompute
// translation units are compiled in; offline compression runs entirely on the CPU).
#include "DirectXTex/DirectXTex.h"

#include <Windows.h>

#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
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

//=============================================================================
// Core conversion: PNG/JPG/TGA -> mipped BC7 (or BC5) DDS sibling.
//=============================================================================
bool ConvertTexture(const fs::path& in, const ImportOptions& opts, Log& log, const std::string& rel)
{
    const std::string ext = LowerExt(in);
    const TexRole role = ClassifyRole(Lower(in.stem().string()));
    const bool srgb = (role == TexRole::AlbedoSRGB);
    const std::wstring w = in.wstring();

    // 1) Load. HDR/TGA have dedicated readers; everything else via WIC. IGNORE_SRGB keeps the raw
    //    bytes (UNORM) so we own the color-space decision by role rather than trusting a PNG chunk.
    TexMetadata meta{};
    ScratchImage loaded;
    HRESULT hr = (ext == ".tga")
        ? LoadFromTGAFile(w.c_str(), TGA_FLAGS_NONE, &meta, loaded)
        : LoadFromWICFile(w.c_str(), WIC_FLAGS_IGNORE_SRGB, &meta, loaded);
    if (FAILED(hr)) { log.Line("  FAIL load " + Hex(hr) + "  " + rel); return false; }

    // 2) Canonicalize to R8G8B8A8_UNORM (collapses BGRA, palettized, and 16-bit PNG sources).
    ScratchImage work;
    if (loaded.GetMetadata().format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = Convert(loaded.GetImages(), loaded.GetImageCount(), loaded.GetMetadata(),
                     DXGI_FORMAT_R8G8B8A8_UNORM, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, work);
        if (FAILED(hr)) { log.Line("  FAIL convert " + Hex(hr) + "  " + rel); return false; }
    }
    else { work = std::move(loaded); }

    // 3) Optional normal-map green-channel flip (OpenGL +Y <-> DirectX -Y). Off by default until
    //    the engine's expected convention is pinned; glTF normals ship +Y and currently render fine.
    if (role == TexRole::Normal && opts.flipGreen)
    {
        ScratchImage t;
        hr = TransformImage(work.GetImages(), work.GetImageCount(), work.GetMetadata(),
            [](XMVECTOR* out, const XMVECTOR* in, size_t width, size_t)
            {
                for (size_t j = 0; j < width; ++j) { out[j] = XMVectorSetY(in[j], 1.0f - XMVectorGetY(in[j])); }
            }, t);
        if (SUCCEEDED(hr)) { work = std::move(t); }
    }

    // 4) Downscale the longest edge to the max-size budget (staged 4K rock textures -> 2K).
    const UINT srcW = (UINT)work.GetMetadata().width;
    const UINT srcH = (UINT)work.GetMetadata().height;
    const UINT longest = std::max(srcW, srcH);
    if (opts.maxTextureSize > 0 && longest > (UINT)opts.maxTextureSize)
    {
        const double s = (double)opts.maxTextureSize / (double)longest;
        const size_t nW = std::max<size_t>(1, (size_t)std::lround(srcW * s));
        const size_t nH = std::max<size_t>(1, (size_t)std::lround(srcH * s));
        ScratchImage t;
        hr = Resize(work.GetImages(), work.GetImageCount(), work.GetMetadata(), nW, nH,
                    (srgb ? TEX_FILTER_SRGB : TEX_FILTER_DEFAULT) | TEX_FILTER_FANT, t);
        if (FAILED(hr)) { log.Line("  FAIL resize " + Hex(hr) + "  " + rel); return false; }
        work = std::move(t);
    }

    // 5) Tag sRGB so mip filtering is gamma-correct and the BC7 output carries the _SRGB view.
    //    (OverrideFormat only relabels — the stored bytes are unchanged.)
    if (srgb) { work.OverrideFormat(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB); }

    // 6) Full mip chain (box/gamma-correct via the format-driven sRGB filter).
    ScratchImage mipped;
    hr = GenerateMipMaps(work.GetImages(), work.GetImageCount(), work.GetMetadata(),
                         srgb ? TEX_FILTER_SRGB : TEX_FILTER_DEFAULT, 0 /*full chain*/, mipped);
    if (FAILED(hr)) { log.Line("  FAIL mips " + Hex(hr) + "  " + rel); return false; }

    // 7) Compress. Albedo -> BC7_SRGB; normal -> BC7 (or BC5 RG on request); MR/linear -> BC7.
    //    (Per-image compress is serial; the importer parallelizes across textures instead —
    //    DirectXTex's TEX_COMPRESS_PARALLEL needs OpenMP, which this project doesn't enable.)
    DXGI_FORMAT target = DXGI_FORMAT_BC7_UNORM;
    if (srgb) { target = DXGI_FORMAT_BC7_UNORM_SRGB; }
    else if (role == TexRole::Normal && opts.bc5Normal) { target = DXGI_FORMAT_BC5_UNORM; }

    TEX_COMPRESS_FLAGS cflags = TEX_COMPRESS_DEFAULT;
    if (!opts.highQuality) { cflags |= TEX_COMPRESS_BC7_QUICK; }

    ScratchImage bc;
    hr = Compress(mipped.GetImages(), mipped.GetImageCount(), mipped.GetMetadata(),
                  target, cflags, TEX_THRESHOLD_DEFAULT, bc);
    if (FAILED(hr)) { log.Line("  FAIL compress " + Hex(hr) + "  " + rel); return false; }

    // 8) Write the .dds sibling (SaveToDDSFile emits a DX10 header for BC7/BC5).
    fs::path out = in; out.replace_extension(L".dds");
    hr = SaveToDDSFile(bc.GetImages(), bc.GetImageCount(), bc.GetMetadata(), DDS_FLAGS_NONE, out.wstring().c_str());
    if (FAILED(hr)) { log.Line("  FAIL save " + Hex(hr) + "  " + rel); return false; }

    // 9) Round-trip verify: reload the header and confirm the DDS is well-formed and complete.
    TexMetadata check{};
    hr = GetMetadataFromDDSFile(out.wstring().c_str(), DDS_FLAGS_NONE, check);
    const bool ok = SUCCEEDED(hr) && check.mipLevels == mipped.GetMetadata().mipLevels &&
                    check.width == mipped.GetMetadata().width && check.height == mipped.GetMetadata().height;

    log.Line(std::string(ok ? "  ok  " : "  WARN ") + RoleName(role) +
             "  " + std::to_string(bc.GetMetadata().width) + "x" + std::to_string(bc.GetMetadata().height) +
             "  fmt=" + std::to_string((int)bc.GetMetadata().format) +
             "  mips=" + std::to_string(bc.GetMetadata().mipLevels) +
             "  -> " + Narrow(out.filename().wstring()));
    return true;
}

} // namespace

int RunImport(const ImportOptions& opts)
{
    Log log(opts.logPath);
    log.Line("=== asset import (H1) ===");
    log.Line("staging dir : " + opts.stagingDir);
    if (!opts.skyboxHdr.empty()) { log.Line("skybox hdr  : " + opts.skyboxHdr + " (H1e — not yet implemented)"); }
    log.Line("max size    : " + std::to_string(opts.maxTextureSize) +
             (opts.highQuality ? "  quality=high" : "  quality=fast") +
             (opts.flipGreen ? "  flip-green" : "") + (opts.bc5Normal ? "  bc5-normal" : ""));

    // WIC (and the WIC-backed DirectXTex readers) require COM on this thread.
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInited = SUCCEEDED(hrCo);

    std::error_code ec;
    if (!fs::exists(opts.stagingDir, ec) || !fs::is_directory(opts.stagingDir, ec))
    {
        log.Line("FATAL: staging dir does not exist or is not a directory");
        if (coInited) { CoUninitialize(); }
        return 1;
    }

    // Collect the convertible textures first, then compress them across worker threads. BC7 is
    // CPU-heavy, so one texture per task keeps every core busy on a folder import (the plan's
    // "compression on background threads (tbb)"). WIC needs COM per worker thread.
    struct Job { fs::path path; std::string rel; };
    std::vector<Job> jobs;
    for (auto it = fs::recursive_directory_iterator(opts.stagingDir, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec) { break; }
        const fs::path& p = it->path();
        if (!it->is_regular_file(ec)) { continue; }
        if (!IsConvertibleTexture(LowerExt(p))) { continue; }
        jobs.push_back({ p, Narrow(fs::relative(p, opts.stagingDir, ec).wstring()) });
    }

    std::atomic<int> converted{0};
    std::atomic<int> failures{0};
    tbb::parallel_for(tbb::blocked_range<size_t>(0, jobs.size()),
        [&](const tbb::blocked_range<size_t>& r)
        {
            const bool threadCo = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
            for (size_t i = r.begin(); i != r.end(); ++i)
            {
                if (ConvertTexture(jobs[i].path, opts, log, jobs[i].rel)) { ++converted; }
                else { ++failures; }
            }
            if (threadCo) { CoUninitialize(); }
        });

    log.Line("converted: " + std::to_string(converted.load()) + ", failures: " + std::to_string(failures.load()));
    log.Line("=== done ===");

    if (coInited) { CoUninitialize(); }
    return failures.load();
}

} // namespace assets
