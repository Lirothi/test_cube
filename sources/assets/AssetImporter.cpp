#include "assets/AssetImporter.h"

// Vendored DirectXTex (third_party/DirectXTex) — CPU-path only (no D3D11/D3D12/DirectCompute
// translation units are compiled in; offline compression runs entirely on the CPU).
#include "DirectXTex/DirectXTex.h"

#include <Windows.h>

#pragma comment(lib, "d3d11.lib") // H5: importer-private D3D11 device for GPU BC6H/BC7 encode

#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>

#include "third_party/json/json.hpp"
#include "third_party/cgltf/cgltf.h" // H6: harvest per-texture glTF factors for import-time baking

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
// H5 — GPU block compression. BC6H/BC7 encode on a private D3D11 compute device
// (DirectXTex BCDirectCompute — the texconv -gpu path). This is offline file
// conversion, so the device is wholly separate from the engine's D3D12 one; the
// CPU path stays as the fallback (--cpu, headless without a GPU, older HW).
// RunImport is single-threaded per run (the editor serializes jobs via its
// running_ atomic, the CLI is one-shot), so a file-scope device is safe.
//=============================================================================
ID3D11Device* g_gpuDevice = nullptr;

bool IsGpuCompressibleFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
    case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
        return true;
    default:
        return false;
    }
}

// Single chokepoint for block compression: GPU when available and the target is
// BC6H/BC7 (everything else — BC5 normals — is cheap on CPU), CPU otherwise.
// A GPU failure mid-run (device removed etc.) degrades to CPU instead of
// failing the import.
HRESULT CompressBlocks(const Image* images, size_t nimages, const TexMetadata& meta,
                       DXGI_FORMAT target, TEX_COMPRESS_FLAGS cflags, ScratchImage& out,
                       Log& log, const std::string& label)
{
    if (g_gpuDevice && IsGpuCompressibleFormat(target))
    {
        // PARALLEL / BC7_QUICK are CPU-path speed knobs; the GPU encoder always
        // runs its full mode search (quality ~= CPU default, way above QUICK).
        const TEX_COMPRESS_FLAGS gpuFlags =
            cflags & ~(TEX_COMPRESS_PARALLEL | TEX_COMPRESS_BC7_QUICK);
        const HRESULT hr = Compress(g_gpuDevice, images, nimages, meta, target, gpuFlags,
                                    TEX_ALPHA_WEIGHT_DEFAULT, out);
        if (SUCCEEDED(hr)) { return hr; }
        log.Line("  WARN gpu compress " + Hex(hr) + " — CPU fallback  " + label);
    }
    return Compress(images, nimages, meta, target, cflags, TEX_THRESHOLD_DEFAULT, out);
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

// H6 — packed metallic-roughness textures (glTF "metallicRoughness", ORM/ARM packs). Their
// channel order is G=rough, B=metal (R = AO or filler). The importer repacks them into the
// FINAL engine layout (R=metal, G=rough, B=0) before compression — including baking the glTF
// material's metallic/roughness FACTORS into the channels — so imported DDS are plain engine
// data needing no glTF-specific runtime semantics. Convention: a .dds MR is ALWAYS final
// engine-layout (this importer is the only .dds producer); a raw .png MR stays glTF-layout
// (the MR_LAYOUT_GLTF shader branch exists only for raw-staging previews).
bool IsPackedMrStem(const std::string& stemLower)
{
    const auto has = [&](const char* s) { return stemLower.find(s) != std::string::npos; };
    return has("metallicroughness") || has("metalrough") || has("_mr") ||
           has("_orm") || has("_arm");
}

//=============================================================================
// H6 — glTF factor harvest. A light cgltf parse of every staged glTF maps each
// referenced texture file to its material's factors, so ConvertTexture can bake
// them (baseColorFactor into albedo RGB, metallic/roughnessFactor into MR).
// The alpha factor is NOT baked — it feeds the runtime alpha test (baseColor.a)
// identically for raw and imported assets.
//=============================================================================
struct GltfTexFactors
{
    bool  isAlbedo = false, isMr = false;
    float baseColor[4] = { 1.f, 1.f, 1.f, 1.f };
    float metallic = 1.f, roughness = 1.f;
    bool  conflict = false;
};

std::string FactorKey(const fs::path& p)
{
    std::error_code ec;
    const fs::path abs = fs::absolute(p, ec);
    return Lower((ec ? p : abs).lexically_normal().generic_string());
}

void HarvestGltfFactors(const fs::path& stagingDir,
                        std::map<std::string, GltfTexFactors>& out, Log& log)
{
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(stagingDir, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec) { break; }
        std::error_code fec;
        if (!it->is_regular_file(fec)) { continue; }
        const std::string ext = LowerExt(it->path());
        if (ext != ".gltf" && ext != ".glb") { continue; }

        cgltf_options opt{};
        cgltf_data* data = nullptr;
        if (cgltf_parse_file(&opt, it->path().string().c_str(), &data) != cgltf_result_success)
        {
            continue;
        }
        const fs::path gltfDir = it->path().parent_path();

        const auto resolveKey = [&](const cgltf_texture_view& view) -> std::string
        {
            if (!view.texture || !view.texture->image || !view.texture->image->uri) { return {}; }
            std::string uri = view.texture->image->uri;
            if (uri.rfind("data:", 0) == 0) { return {}; } // embedded — never hits the loose pass
            cgltf_decode_uri(uri.data());
            uri.resize(std::strlen(uri.c_str()));
            return FactorKey(gltfDir / uri);
        };

        for (cgltf_size m = 0; m < data->materials_count; ++m)
        {
            const cgltf_material& mat = data->materials[m];
            if (!mat.has_pbr_metallic_roughness) { continue; }
            const cgltf_pbr_metallic_roughness& pbr = mat.pbr_metallic_roughness;

            const std::string albedoKey = resolveKey(pbr.base_color_texture);
            if (!albedoKey.empty())
            {
                GltfTexFactors& f = out[albedoKey];
                if (f.isAlbedo &&
                    std::memcmp(f.baseColor, pbr.base_color_factor, sizeof(f.baseColor)) != 0)
                {
                    f.conflict = true;
                }
                f.isAlbedo = true;
                std::memcpy(f.baseColor, pbr.base_color_factor, sizeof(f.baseColor));
            }

            const std::string mrKey = resolveKey(pbr.metallic_roughness_texture);
            if (!mrKey.empty())
            {
                GltfTexFactors& f = out[mrKey];
                if (f.isMr && (f.metallic != pbr.metallic_factor ||
                               f.roughness != pbr.roughness_factor))
                {
                    f.conflict = true;
                }
                f.isMr = true;
                f.metallic = pbr.metallic_factor;
                f.roughness = pbr.roughness_factor;
            }
        }
        cgltf_free(data);
    }

    for (const auto& [key, f] : out)
    {
        if (f.conflict)
        {
            log.Line("  WARN texture shared by materials with DIFFERENT factors (last wins): " + key);
        }
    }
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
    hr = CompressBlocks(mipped.GetImages(), mipped.GetImageCount(), mipped.GetMetadata(),
                        target, cflags, bc, log, label);
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

// Re-center a tangent-space normal map that carries a baked-in DC "lean" — a purple cast where the
// flat baseline strays from (128,128) instead of pointing straight up. A uniform lean skews lighting
// (badly under grazing lights: it shoves the lit disc off-center), so shift tangent XY back to
// neutral and re-normalize each texel, keeping the surface detail. Threshold-gated: already-neutral
// maps (like a good Poly Haven set) are left untouched. Operates on the pre-mip R8G8B8A8 image.
static void CenterNormalMap(ScratchImage& work, Log& log, const std::string& label)
{
    const Image* img = work.GetImage(0, 0, 0);
    if (!img || img->format != DXGI_FORMAT_R8G8B8A8_UNORM) { return; }
    const size_t px = img->width * img->height;
    if (px == 0) { return; }
    double sumR = 0.0, sumG = 0.0;
    for (size_t y = 0; y < img->height; ++y)
    {
        const uint8_t* row = img->pixels + y * img->rowPitch;
        for (size_t x = 0; x < img->width; ++x) { sumR += row[x * 4]; sumG += row[x * 4 + 1]; }
    }
    const float avgR = static_cast<float>(sumR / static_cast<double>(px));
    const float avgG = static_cast<float>(sumG / static_cast<double>(px));
    const float mx = avgR / 127.5f - 1.0f; // mean lean in tangent X
    const float my = avgG / 127.5f - 1.0f; // mean lean in tangent Y
    const float lean = std::sqrt(mx * mx + my * my);
    if (lean <= 0.03f) { return; } // ~1.7 deg: within noise, leave neutral maps alone

    ScratchImage t;
    if (FAILED(TransformImage(work.GetImages(), work.GetImageCount(), work.GetMetadata(),
        [mx, my](XMVECTOR* out, const XMVECTOR* in, size_t width, size_t)
        {
            for (size_t j = 0; j < width; ++j)
            {
                float nx = XMVectorGetX(in[j]) * 2.0f - 1.0f - mx;
                float ny = XMVectorGetY(in[j]) * 2.0f - 1.0f - my;
                float r2 = nx * nx + ny * ny;
                constexpr float cap = 0.98f; // keep a little Z headroom (never fully flat)
                if (r2 > cap) { const float s = std::sqrt(cap / r2); nx *= s; ny *= s; r2 = cap; }
                const float nz = std::sqrt(fmaxf(1e-4f, 1.0f - r2));
                out[j] = XMVectorSet(nx * 0.5f + 0.5f, ny * 0.5f + 0.5f, nz * 0.5f + 0.5f, XMVectorGetW(in[j]));
            }
        }, t)))
    {
        return;
    }
    work = std::move(t);
    const int leanDeg = static_cast<int>(std::atan(lean) * 57.2958f + 0.5f);
    log.Line("  [normal centered] lean " + std::to_string(leanDeg) + " deg (avg R=" +
             std::to_string(static_cast<int>(avgR + 0.5f)) + " G=" +
             std::to_string(static_cast<int>(avgG + 0.5f)) + " -> 128)  " + label);
}

//=============================================================================
// Core conversion: PNG/JPG/TGA -> mipped BC7 (or BC5) DDS sibling.
//=============================================================================
bool ConvertTexture(const fs::path& in, const ImportOptions& opts, Log& log, const std::string& rel,
                    const std::map<std::string, GltfTexFactors>& gltfFactors)
{
    const std::string stemLower = Lower(in.stem().string());
    const TexRole role = ClassifyRole(stemLower);
    const bool srgb = (role == TexRole::AlbedoSRGB);

    ScratchImage work;
    if (!LoadRgba8(in, work, log, rel)) { return false; }

    const auto facIt = gltfFactors.find(FactorKey(in));
    const GltfTexFactors* fac = facIt != gltfFactors.end() ? &facIt->second : nullptr;

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

    if (role == TexRole::Normal && opts.centerNormals) { CenterNormalMap(work, log, rel); }

    // H6: bake packed MR (glTF/ORM/ARM: G=rough, B=metal) into FINAL engine data:
    // R = metal*metallicFactor, G = rough*roughnessFactor, B = 0. The DDS needs no
    // glTF-specific runtime handling — it is byte-equivalent to a synthesized set MR.
    std::string roleTag = " [" + std::string(RoleName(role)) + "]";
    const bool packedMr = (fac && fac->isMr) ||
        (role == TexRole::LinearData && IsPackedMrStem(stemLower));
    if (packedMr)
    {
        const float metalF = fac ? fac->metallic : 1.0f;
        const float roughF = fac ? fac->roughness : 1.0f;
        ScratchImage t;
        if (FAILED(TransformImage(work.GetImages(), work.GetImageCount(), work.GetMetadata(),
            [metalF, roughF](XMVECTOR* out, const XMVECTOR* in, size_t width, size_t)
            {
                for (size_t j = 0; j < width; ++j)
                {
                    // in = (AO, rough, metal, a) -> out = (metal*mF, rough*rF, 0, 1)
                    out[j] = XMVectorSet(
                        XMVectorGetZ(in[j]) * metalF,
                        XMVectorGetY(in[j]) * roughF,
                        0.0f, 1.0f);
                }
            }, t)))
        {
            log.Line("  FAIL mr repack  " + rel);
            return false;
        }
        work = std::move(t);
        char tag[64];
        snprintf(tag, sizeof(tag), " [mr baked mF=%.2f rF=%.2f]", metalF, roughF);
        roleTag = tag;
    }
    else if (fac && fac->isAlbedo &&
             (fac->baseColor[0] != 1.0f || fac->baseColor[1] != 1.0f || fac->baseColor[2] != 1.0f))
    {
        // Bake baseColorFactor.rgb into the albedo. The texture is sRGB-encoded while the factor
        // is linear, so decode -> multiply -> re-encode. Alpha is left untouched (the alpha factor
        // stays a runtime parameter feeding the alpha test).
        const XMVECTOR factor = XMVectorSet(fac->baseColor[0], fac->baseColor[1], fac->baseColor[2], 1.0f);
        ScratchImage t;
        if (FAILED(TransformImage(work.GetImages(), work.GetImageCount(), work.GetMetadata(),
            [factor](XMVECTOR* out, const XMVECTOR* in, size_t width, size_t)
            {
                for (size_t j = 0; j < width; ++j)
                {
                    const XMVECTOR linear = XMVectorMultiply(XMColorSRGBToRGB(in[j]), factor);
                    out[j] = XMVectorSetW(XMColorRGBToSRGB(linear), XMVectorGetW(in[j]));
                }
            }, t)))
        {
            log.Line("  FAIL albedo factor bake  " + rel);
            return false;
        }
        work = std::move(t);
        roleTag = " [albedo(sRGB) x factor]";
    }

    DXGI_FORMAT target = DXGI_FORMAT_BC7_UNORM;
    if (srgb) { target = DXGI_FORMAT_BC7_UNORM_SRGB; }
    else if (role == TexRole::Normal && opts.bc5Normal) { target = DXGI_FORMAT_BC5_UNORM; }

    fs::path out = in; out.replace_extension(L".dds");
    return FinishTextureDds(std::move(work), srgb, target, opts, out, log, rel + roleTag);
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
                      bool importAlbedo, bool importMr, bool importNormal,
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

    bool success = true;
    if (importAlbedo)
    {
        ScratchImage a;
        const fs::path aOut = dir / (name + "_albedo.dds");
        if (!LoadRgba8(diff, a, log, name + " albedo") ||
            !FinishTextureDds(std::move(a), true, DXGI_FORMAT_BC7_UNORM_SRGB,
                opts, aOut, log, name + " [albedo]"))
        {
            success = false;
        }
        else
        {
            out.albedo = PresetPath(aOut);
        }
    }

    if (importNormal && !nor.empty())
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
            if (opts.centerNormals) { CenterNormalMap(n, log, name + " [normal]"); }
            const fs::path nOut = dir / (name + "_normal.dds");
            if (FinishTextureDds(std::move(n), false, DXGI_FORMAT_BC7_UNORM, opts, nOut, log, name + " [normal]"))
            {
                out.normal = PresetPath(nOut);
            }
            else { success = false; }
        }
        else { success = false; }
    }

    if (importMr)
    {
        const fs::path mrOut = dir / (name + "_mr.dds");
        if (SynthesizeMR(rough, metal.empty() ? nullptr : &metal, opts, mrOut, log))
        {
            out.mr = PresetPath(mrOut);
        }
        else { success = false; }
    }

    return success;
}

// I0: register texture-set materials as per-file assets — one data/materials/<name>.json per
// set (schema v2, flat object). Existing files are merged per key so a partial set reimport
// (only some maps selected) preserves the untouched texture entries and any authored params.
void WritePresets(const std::vector<PresetEntry>& entries, Log& log)
{
    if (entries.empty()) { return; }
    std::error_code ec;
    fs::create_directories("data/materials", ec);

    for (const auto& e : entries)
    {
        const fs::path path = fs::path("data/materials") / (e.name + ".json");
        nlohmann::json p = nlohmann::json::object();
        {
            std::ifstream f(path);
            if (f)
            {
                std::stringstream ss; ss << f.rdbuf();
                nlohmann::json parsed = nlohmann::json::parse(ss.str(), nullptr, false, true);
                if (!parsed.is_discarded() && parsed.is_object()) { p = std::move(parsed); }
            }
        }
        if (!e.albedo.empty()) { p["albedo"] = e.albedo; }
        if (!e.mr.empty()) { p["mr"] = e.mr; }
        if (!e.normal.empty()) { p["normal"] = e.normal; }
        p["normalIsRG"] = false;

        std::ofstream out(path, std::ios::trunc);
        out << p.dump(2) << "\n";
        log.Line("registered material '" + e.name + "' -> " + path.generic_string());
    }
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
    if (FAILED(CompressBlocks(atlas.GetImages(), atlas.GetImageCount(), atlas.GetMetadata(),
                              DXGI_FORMAT_BC7_UNORM_SRGB, cflags, bc, log,
                              "flipbook '" + seq.base + "'")))
    {
        // OverrideFormat retagged the atlas in place, so this compresses the (now sRGB) image.
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

//=============================================================================
// F7 — offline IBL derivatives for split-sum image-based lighting.
//
// The display cube's ordinary mip chain is a BOX FILTER, not a GGX prefilter. The engine has been
// treating it as one (`skyMip = roughness * kSkyRoughMaxMip` in the ocean, the same trick in
// compose), which is why rough reflections read as "small sky" rather than as a broadened lobe.
// These three resources are what a correct split-sum evaluation needs:
//
//   <stem>_spec.dds     GGX-prefiltered radiance. Mip m corresponds to roughness m/(mips-1), so the
//                       runtime indexes it by perceptual roughness directly instead of guessing.
//   <stem>_diffuse.dds  cosine-convolved irradiance, tiny (it is a 9-coefficient-worth signal), read
//                       with the surface normal to replace the flat ambient constant.
//   textures/brdf_lut.dds  the split-sum environment BRDF, indexed by (NoV, roughness). Scene
//                       independent, generated once and shared by every level.
//
// Everything here is deterministic: fixed Hammersley sequences, fixed sample counts, no RNG. Two
// imports of the same source produce byte-identical outputs, which is what makes the "did the
// importer change?" question answerable by hashing.
//=============================================================================

// Scan a float32 image set for the failures that make an IBL bake silently useless: NaN/Inf from a
// divide by a zero weight, and negatives from a filter that overshot. Also reports mean luminance,
// which is the energy check -- a prefiltered mip must stay near the level below it, and a cosine
// convolution must land near the source's average. Cheap enough to run on every bake.
struct FloatStats { double meanLuma; float maxComp; size_t nonFinite; size_t negative; };

FloatStats ScanFloatImages(const Image* images, size_t count)
{
    FloatStats s{ 0.0, 0.0f, 0, 0 };
    double sum = 0.0;
    size_t n = 0;
    for (size_t i = 0; i < count; ++i)
    {
        const Image& img = images[i];
        for (size_t y = 0; y < img.height; ++y)
        {
            const float* row = reinterpret_cast<const float*>(img.pixels + y * img.rowPitch);
            for (size_t x = 0; x < img.width; ++x)
            {
                const float* p = row + x * 4;
                for (int c = 0; c < 3; ++c)
                {
                    if (!std::isfinite(p[c])) { ++s.nonFinite; continue; }
                    if (p[c] < 0.0f) { ++s.negative; }
                    if (p[c] > s.maxComp) { s.maxComp = p[c]; }
                }
                if (std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]))
                {
                    sum += 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
                    ++n;
                }
            }
        }
    }
    s.meanLuma = (n > 0) ? sum / (double)n : 0.0;
    return s;
}

std::string StatsLine(const FloatStats& s)
{
    char buf[160];
    std::snprintf(buf, sizeof(buf), "meanLuma=%.5f max=%.3f nonFinite=%zu negative=%zu",
                  s.meanLuma, s.maxComp, s.nonFinite, s.negative);
    return buf;
}

// Direction -> cube face + face-local uv in [0,1]. The inverse of CubeFaceDir above; the two must
// agree exactly or every prefiltered texel lands in the wrong place.
void DirToFaceUV(const XMFLOAT3& d, int& face, float& u, float& v)
{
    const float ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
    float ma;
    float sc, tc;
    if (ax >= ay && ax >= az)
    {
        ma = ax;
        if (d.x > 0.0f) { face = 0; sc = -d.z; tc = -d.y; }
        else            { face = 1; sc =  d.z; tc = -d.y; }
    }
    else if (ay >= az)
    {
        ma = ay;
        if (d.y > 0.0f) { face = 2; sc = d.x; tc =  d.z; }
        else            { face = 3; sc = d.x; tc = -d.z; }
    }
    else
    {
        ma = az;
        if (d.z > 0.0f) { face = 4; sc =  d.x; tc = -d.y; }
        else            { face = 5; sc = -d.x; tc = -d.y; }
    }
    u = 0.5f * (sc / ma + 1.0f);
    v = 0.5f * (tc / ma + 1.0f);
}

// Bilinear sample of one mip of a float32 cube, by direction. Edges clamp inside the face rather
// than reaching across the seam: the alternative is a per-edge neighbour table, and at the face
// sizes and blur radii here the visible difference is nil while the bug surface is not.
void SampleCubeDir(const ScratchImage& cube, size_t mip, const XMFLOAT3& dir, float out[3])
{
    XMFLOAT3 d = dir;
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len > 0.0f) { d.x /= len; d.y /= len; d.z /= len; }

    int face; float u, v;
    DirToFaceUV(d, face, u, v);

    const Image* img = cube.GetImage(mip, (size_t)face, 0);
    const float fx = u * (float)img->width - 0.5f;
    const float fy = v * (float)img->height - 0.5f;
    const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    const float tx = fx - (float)x0, ty = fy - (float)y0;

    const auto clampi = [](int a, int lo, int hi) { return a < lo ? lo : (a > hi ? hi : a); };
    const int xs[2] = { clampi(x0, 0, (int)img->width - 1), clampi(x0 + 1, 0, (int)img->width - 1) };
    const int ys[2] = { clampi(y0, 0, (int)img->height - 1), clampi(y0 + 1, 0, (int)img->height - 1) };

    float acc[3] = { 0.0f, 0.0f, 0.0f };
    const float wx[2] = { 1.0f - tx, tx };
    const float wy[2] = { 1.0f - ty, ty };
    for (int j = 0; j < 2; ++j)
    {
        const float* row = reinterpret_cast<const float*>(img->pixels + (size_t)ys[j] * img->rowPitch);
        for (int i = 0; i < 2; ++i)
        {
            const float w = wx[i] * wy[j];
            const float* p = row + (size_t)xs[i] * 4;
            acc[0] += p[0] * w; acc[1] += p[1] * w; acc[2] += p[2] * w;
        }
    }
    out[0] = acc[0]; out[1] = acc[1]; out[2] = acc[2];
}

// Van der Corput radical inverse — the low-discrepancy half of Hammersley.
float RadicalInverseVdC(uint32_t bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return (float)bits * 2.3283064365386963e-10f;
}

// GGX importance sample around +Z, then rotated into the frame of N by the caller.
XMFLOAT3 ImportanceSampleGGX(float u1, float u2, float roughness, const XMFLOAT3& n)
{
    const float a = roughness * roughness;
    const float phi = 6.2831853071795864f * u1;
    const float cosTheta = std::sqrt((1.0f - u2) / (1.0f + (a * a - 1.0f) * u2));
    const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));

    const XMFLOAT3 h{ sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta };

    const XMFLOAT3 up = std::fabs(n.z) < 0.999f ? XMFLOAT3{ 0.0f, 0.0f, 1.0f } : XMFLOAT3{ 1.0f, 0.0f, 0.0f };
    XMFLOAT3 tx{ up.y * n.z - up.z * n.y, up.z * n.x - up.x * n.z, up.x * n.y - up.y * n.x };
    const float tl = std::sqrt(tx.x * tx.x + tx.y * tx.y + tx.z * tx.z);
    tx.x /= tl; tx.y /= tl; tx.z /= tl;
    const XMFLOAT3 ty{ n.y * tx.z - n.z * tx.y, n.z * tx.x - n.x * tx.z, n.x * tx.y - n.y * tx.x };

    return { tx.x * h.x + ty.x * h.y + n.x * h.z,
             tx.y * h.x + ty.y * h.y + n.y * h.z,
             tx.z * h.x + ty.z * h.y + n.z * h.z };
}

// GGX-prefiltered radiance cube. Mip m <-> roughness m / (mipCount - 1); mip 0 is a straight copy so
// a mirror surface still reads the sharp sky. N = V = R is the standard split-sum approximation --
// it loses stretched grazing reflections and is what every real-time implementation ships.
bool BuildPrefilteredSpecular(const ScratchImage& srcCubeMips, int baseFace, ScratchImage& out, Log& log)
{
    size_t mipCount = 1;
    while ((baseFace >> mipCount) >= 8 && mipCount < 9) { ++mipCount; }

    HRESULT hr = out.InitializeCube(DXGI_FORMAT_R32G32B32A32_FLOAT, (size_t)baseFace, (size_t)baseFace, 1, mipCount);
    if (FAILED(hr)) { log.Line("ibl FAIL init spec cube " + Hex(hr)); return false; }

    const size_t srcMips = srcCubeMips.GetMetadata().mipLevels;

    // Roughness per mip: the EXACT INVERSE of `IblMipFromRoughness` in shaders/ibl_common.hlsli,
    // which is Unreal's logarithmic mapping (ReflectionEnvironmentShared.ush). Mirrored here rather
    // than shared because the importer is CPU-side; if either side changes, both change, and the
    // symptom of them disagreeing is invisible -- the reflections merely look slightly wrong.
    const auto roughnessFromMip = [mipCount](size_t mip)
    {
        constexpr float kRoughestMip = 1.0f;
        constexpr float kMipScale = 1.2f;
        const float levelFrom1x1 = (float)mipCount - 1.0f - (float)mip;
        return std::min(1.0f, std::exp2((kRoughestMip - levelFrom1x1) / kMipScale));
    };

    for (size_t mip = 0; mip < mipCount; ++mip)
    {
        // Mip 0 stays a straight copy. Their mapping puts it at roughness ~0.03, which for a GGX
        // lobe is within a hair of a mirror, and a copy is both sharper and cheaper.
        const float roughness = (mip == 0) ? 0.0f : roughnessFromMip(mip);
        const int size = std::max(1, baseFace >> (int)mip);
        // Sample budget grows with the lobe: a mirror needs one tap, a rough lobe needs many.
        const int sampleCount = (mip == 0) ? 1 : (int)std::min<size_t>(512, 64u << std::min<size_t>(mip, 3));

        tbb::parallel_for(0, 6, [&](int f)
        {
            const Image* dst = out.GetImage(mip, (size_t)f, 0);
            for (int y = 0; y < size; ++y)
            {
                float* row = reinterpret_cast<float*>(dst->pixels + (size_t)y * dst->rowPitch);
                const float vv = 2.0f * ((float)y + 0.5f) / (float)size - 1.0f;
                for (int x = 0; x < size; ++x)
                {
                    const float uu = 2.0f * ((float)x + 0.5f) / (float)size - 1.0f;
                    XMFLOAT3 n = CubeFaceDir(f, uu, vv);
                    const float nl = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                    n.x /= nl; n.y /= nl; n.z /= nl;

                    float* px = row + (size_t)x * 4;
                    if (mip == 0)
                    {
                        SampleCubeDir(srcCubeMips, 0, n, px);
                        px[3] = 1.0f;
                        continue;
                    }

                    float acc[3] = { 0.0f, 0.0f, 0.0f };
                    float wsum = 0.0f;
                    for (int s = 0; s < sampleCount; ++s)
                    {
                        const float u1 = (float)s / (float)sampleCount;
                        const float u2 = RadicalInverseVdC((uint32_t)s);
                        const XMFLOAT3 h = ImportanceSampleGGX(u1, u2, roughness, n);
                        const float ndoth = n.x * h.x + n.y * h.y + n.z * h.z;
                        // L = reflect(-N, H) with V == N
                        const XMFLOAT3 l{ 2.0f * ndoth * h.x - n.x,
                                          2.0f * ndoth * h.y - n.y,
                                          2.0f * ndoth * h.z - n.z };
                        const float ndotl = n.x * l.x + n.y * l.y + n.z * l.z;
                        if (ndotl <= 0.0f) { continue; }

                        // Read from a coarser source mip for the wide lobes: a box-filtered tap of an
                        // already-blurred level is a far better estimate of that solid angle than a
                        // point sample of mip 0, and it is what keeps the fireflies out.
                        const size_t srcMip = std::min(srcMips - 1, (size_t)(roughness * (float)(srcMips - 1) * 0.75f));
                        float c[3];
                        SampleCubeDir(srcCubeMips, srcMip, l, c);
                        acc[0] += c[0] * ndotl; acc[1] += c[1] * ndotl; acc[2] += c[2] * ndotl;
                        wsum += ndotl;
                    }
                    const float inv = (wsum > 0.0f) ? 1.0f / wsum : 0.0f;
                    px[0] = acc[0] * inv; px[1] = acc[1] * inv; px[2] = acc[2] * inv; px[3] = 1.0f;
                }
            }
        });
        {
            const Image* mipImgs[6];
            Image mipCopy[6];
            for (int f = 0; f < 6; ++f) { mipCopy[f] = *out.GetImage(mip, (size_t)f, 0); }
            (void)mipImgs;
            const FloatStats st = ScanFloatImages(mipCopy, 6);
            log.Line("  ibl spec   mip " + std::to_string(mip) + "  " + std::to_string(size) + "^2" +
                     "  rough=" + std::to_string(roughness) + "  samples=" + std::to_string(sampleCount) +
                     "  " + StatsLine(st));
        }
    }
    return true;
}

// Cosine-convolved irradiance cube. Brute-force over a coarse source level with real solid-angle
// weights rather than importance sampling: at 32^2 output against a 32^2 source that is 6k x 6k
// dot products, which is nothing, and it removes sampling noise from the one resource whose whole
// job is to be smooth.
bool BuildIrradianceCube(const ScratchImage& srcCubeMips, int outFace, ScratchImage& out, Log& log)
{
    HRESULT hr = out.InitializeCube(DXGI_FORMAT_R32G32B32A32_FLOAT, (size_t)outFace, (size_t)outFace, 1, 1);
    if (FAILED(hr)) { log.Line("ibl FAIL init irradiance cube " + Hex(hr)); return false; }

    // Pick the source mip whose face is closest to 32 texels: fine enough for the sky's low-frequency
    // content, coarse enough that the double loop stays trivial.
    const size_t srcMips = srcCubeMips.GetMetadata().mipLevels;
    size_t srcMip = 0;
    while (srcMip + 1 < srcMips && (int)srcCubeMips.GetImage(srcMip, 0, 0)->width > 32) { ++srcMip; }
    const int ss = (int)srcCubeMips.GetImage(srcMip, 0, 0)->width;

    // Precompute source directions, radiance and solid angles once.
    struct Tap { XMFLOAT3 dir; float rgb[3]; float sa; };
    std::vector<Tap> taps;
    taps.reserve((size_t)ss * ss * 6);
    for (int f = 0; f < 6; ++f)
    {
        const Image* img = srcCubeMips.GetImage(srcMip, (size_t)f, 0);
        for (int y = 0; y < ss; ++y)
        {
            const float* row = reinterpret_cast<const float*>(img->pixels + (size_t)y * img->rowPitch);
            const float vv = 2.0f * ((float)y + 0.5f) / (float)ss - 1.0f;
            for (int x = 0; x < ss; ++x)
            {
                const float uu = 2.0f * ((float)x + 0.5f) / (float)ss - 1.0f;
                XMFLOAT3 d = CubeFaceDir(f, uu, vv);
                const float l2 = d.x * d.x + d.y * d.y + d.z * d.z;
                const float l = std::sqrt(l2);
                Tap t;
                t.dir = { d.x / l, d.y / l, d.z / l };
                // Differential solid angle of a cube texel: (2/n)^2 * (1 / |d|^3), with |d| the
                // UN-normalised face direction. Dropping this weights the corners like the centres
                // and tilts the whole integral.
                const float texel = 2.0f / (float)ss;
                t.sa = (texel * texel) / (l2 * l);
                const float* p = row + (size_t)x * 4;
                t.rgb[0] = p[0]; t.rgb[1] = p[1]; t.rgb[2] = p[2];
                taps.push_back(t);
            }
        }
    }

    tbb::parallel_for(0, 6, [&](int f)
    {
        const Image* dst = out.GetImage(0, (size_t)f, 0);
        for (int y = 0; y < outFace; ++y)
        {
            float* row = reinterpret_cast<float*>(dst->pixels + (size_t)y * dst->rowPitch);
            const float vv = 2.0f * ((float)y + 0.5f) / (float)outFace - 1.0f;
            for (int x = 0; x < outFace; ++x)
            {
                const float uu = 2.0f * ((float)x + 0.5f) / (float)outFace - 1.0f;
                XMFLOAT3 n = CubeFaceDir(f, uu, vv);
                const float nl = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                n.x /= nl; n.y /= nl; n.z /= nl;

                float acc[3] = { 0.0f, 0.0f, 0.0f };
                for (const Tap& t : taps)
                {
                    const float ndotl = n.x * t.dir.x + n.y * t.dir.y + n.z * t.dir.z;
                    if (ndotl <= 0.0f) { continue; }
                    const float w = ndotl * t.sa;
                    acc[0] += t.rgb[0] * w; acc[1] += t.rgb[1] * w; acc[2] += t.rgb[2] * w;
                }
                // Store IRRADIANCE / PI, i.e. the value a Lambertian surface multiplies its albedo
                // by directly. Folding the 1/pi in here keeps the shader from having to remember it.
                const float k = 1.0f / 3.14159265358979f;
                float* px = row + (size_t)x * 4;
                px[0] = acc[0] * k; px[1] = acc[1] * k; px[2] = acc[2] * k; px[3] = 1.0f;
            }
        }
    });
    {
        const FloatStats st = ScanFloatImages(out.GetImages(), out.GetImageCount());
        // The source's own average over ALL SIX faces at the mip we integrated -- a single face is
        // not the cube's average and comparing against it makes the energy check meaningless.
        Image srcFaces[6];
        for (int f = 0; f < 6; ++f) { srcFaces[f] = *srcCubeMips.GetImage(srcMip, (size_t)f, 0); }
        const FloatStats src = ScanFloatImages(srcFaces, 6);
        log.Line("  ibl diffuse " + std::to_string(outFace) + "^2 x6  from src mip " +
                 std::to_string(srcMip) + " (" + std::to_string(ss) + "^2)  " + StatsLine(st) +
                 "  [src face0 meanLuma=" + std::to_string(src.meanLuma) + "]");
    }
    return true;
}

// Split-sum environment BRDF: x = scale on F0, y = bias. Scene independent, so it is written once to
// textures/brdf_lut.dds and shared. Smith-GGX height-correlated visibility, matching the direct
// lighting in lighting_cs.
bool BuildBrdfLut(const fs::path& out, int size, Log& log)
{
    ScratchImage lut;
    HRESULT hr = lut.Initialize2D(DXGI_FORMAT_R32G32_FLOAT, (size_t)size, (size_t)size, 1, 1);
    if (FAILED(hr)) { log.Line("ibl FAIL init brdf lut " + Hex(hr)); return false; }

    const Image* img = lut.GetImage(0, 0, 0);
    tbb::parallel_for(0, size, [&](int y)
    {
        float* row = reinterpret_cast<float*>(img->pixels + (size_t)y * img->rowPitch);
        const float roughness = ((float)y + 0.5f) / (float)size;
        for (int x = 0; x < size; ++x)
        {
            const float ndotv = ((float)x + 0.5f) / (float)size;
            const XMFLOAT3 v{ std::sqrt(std::max(0.0f, 1.0f - ndotv * ndotv)), 0.0f, ndotv };
            const XMFLOAT3 n{ 0.0f, 0.0f, 1.0f };

            float a = 0.0f, b = 0.0f;
            const int kSamples = 1024;
            const float alpha = roughness * roughness;
            // IBL variant of the Smith k is alpha/2, with alpha = roughness^2. Squaring alpha here
            // (k = roughness^4 / 2) makes k far too small, G far too large, and the LUT returns
            // A + B up to 7.7 against a physical ceiling of 1 -- which is exactly what the first
            // bake produced. The spec cube looked perfect at the same time; only the A+B check
            // caught it.
            const float k = alpha / 2.0f;
            for (int s = 0; s < kSamples; ++s)
            {
                const float u1 = (float)s / (float)kSamples;
                const float u2 = RadicalInverseVdC((uint32_t)s);
                const XMFLOAT3 h = ImportanceSampleGGX(u1, u2, roughness, n);
                const float vdoth = v.x * h.x + v.y * h.y + v.z * h.z;
                const XMFLOAT3 l{ 2.0f * vdoth * h.x - v.x, 2.0f * vdoth * h.y - v.y, 2.0f * vdoth * h.z - v.z };
                const float ndotl = std::max(0.0f, l.z);
                const float ndoth = std::max(0.0f, h.z);
                const float vh = std::max(0.0f, vdoth);
                if (ndotl <= 0.0f) { continue; }

                const float gv = ndotv / (ndotv * (1.0f - k) + k);
                const float gl = ndotl / (ndotl * (1.0f - k) + k);
                const float g = gv * gl;
                const float gVis = (g * vh) / std::max(1e-6f, ndoth * ndotv);
                const float fc = std::pow(1.0f - vh, 5.0f);
                a += (1.0f - fc) * gVis;
                b += fc * gVis;
            }
            row[(size_t)x * 2 + 0] = a / (float)kSamples;
            row[(size_t)x * 2 + 1] = b / (float)kSamples;
        }
    });

    // RG16_FLOAT as the plan specifies: the values live in [0,1] and half precision is ample, while
    // the file stays 128 KB instead of 512 KB.
    ScratchImage half;
    hr = Convert(*img, DXGI_FORMAT_R16G16_FLOAT, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, half);
    if (FAILED(hr)) { log.Line("ibl FAIL convert brdf lut " + Hex(hr)); return false; }

    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    // FORCE_DX10: RG16_FLOAT has a legacy FourCC (112), so DirectXTex would write the old header
    // form and Texture2D's DDS loader -- which requires DX10 -- would refuse the file. That is
    // exactly how the first F8 run ended up with lut=0 and a silent fallback.
    hr = SaveToDDSFile(*half.GetImage(0, 0, 0), DDS_FLAGS_FORCE_DX10_EXT, out.wstring().c_str());
    if (FAILED(hr)) { log.Line("ibl FAIL save brdf lut " + Hex(hr)); return false; }
    log.Line("  ok  ibl brdf " + std::to_string(size) + "^2 RG16F -> " + Narrow(out.filename().wstring()));
    return true;
}

// Save a float32 cube as BC6H_UF16 with the same RGBA16F fallback the display cube uses.
// The ceiling of every HDR container this importer writes: BC6H_UF16 encodes its endpoints as
// 16-bit floats, and the RGBA16F fallback is 16-bit floats outright. Same number for both.
constexpr float kMaxHalfFloat = 65504.0f;

// CLAMP WHERE THE DATA ENTERS THE CONTAINER -- UE's rule, and the reason is not "tidiness".
//
// BC6H builds a 4x4 block from two endpoints. An endpoint the format cannot represent does not
// merely round: it takes the whole block with it, and the block collapses to one flat extreme
// value. A coarse mip then magnifies that block across a large solid angle, which is how a sky
// whose sun sits above the ceiling paints solid achromatic slabs into reflections and fog. It was
// measured here as spec mip 0 at 524,711 and mip 1 at 135,530 against a ceiling of 65,504; the
// previous sky never showed it because its peak was ~34.
//
// Nothing is lost that the engine could have carried: scene colour is R16G16B16A16_FLOAT, so the
// same ceiling applies at the other end of the pipe. What is above it was never going to arrive --
// today it arrives as garbage instead of as a clamp.
//
// UE do exactly this, in ReflectionEnvironmentShaders.usf:219, gated by a flag whose call site
// (ReflectionEnvironmentCapture.cpp:728) is commented "Rendering into an FP16 texture." Their
// MaxHalfFloat is the same 65504 (Common.ush:142). They clamp on the way INTO the scratch cube and
// filter from the clamped copy, so the mip chain and the prefilter both see clamped data; this
// mirrors that, which is why it runs on the mip chain rather than on each file as it is written.
void ClampToFp16Range(ScratchImage& img, Log& log, const char* tag)
{
    if (img.GetMetadata().format != DXGI_FORMAT_R32G32B32A32_FLOAT)
    {
        log.Line(std::string("  ") + tag + " clamp SKIPPED: not R32G32B32A32_FLOAT");
        return;
    }

    size_t clamped = 0;
    float peak = 0.0f;
    const Image* images = img.GetImages();
    for (size_t i = 0; i < img.GetImageCount(); ++i)
    {
        const Image& im = images[i];
        for (size_t y = 0; y < im.height; ++y)
        {
            float* row = reinterpret_cast<float*>(im.pixels + y * im.rowPitch);
            for (size_t x = 0; x < im.width; ++x)
            {
                float* px = row + x * 4;
                // RGB only. Alpha is 1 here and UE clamp theirs to 1 for the same reason.
                for (int c = 0; c < 3; ++c)
                {
                    if (px[c] > peak) { peak = px[c]; }
                    if (px[c] > kMaxHalfFloat) { px[c] = kMaxHalfFloat; ++clamped; }
                }
            }
        }
    }

    char msg[192];
    std::snprintf(msg, sizeof(msg), "  %s clamp  ceiling %.0f  peak was %.1f  channels clamped %zu%s",
                  tag, kMaxHalfFloat, peak, clamped,
                  clamped ? "  <-- the container could not have held these" : "  (nothing to do)");
    log.Line(msg);
}

bool SaveHdrCube(ScratchImage& cube, const fs::path& out, Log& log, const char* tag, bool compress)
{
    // The container boundary itself, so no future caller can write an HDR cube past the ceiling.
    // With the source chain already clamped this normally finds nothing -- a weighted average of
    // clamped values cannot exceed the clamp -- and that is the point: it is an invariant, not a
    // fix-up.
    ClampToFp16Range(cube, log, tag);

    HRESULT hr = E_FAIL;
    if (compress)
    {
        ScratchImage bc;
        hr = CompressBlocks(cube.GetImages(), cube.GetImageCount(), cube.GetMetadata(),
                            DXGI_FORMAT_BC6H_UF16, TEX_COMPRESS_DEFAULT | TEX_COMPRESS_PARALLEL,
                            bc, log, tag);
        if (SUCCEEDED(hr))
        {
            hr = SaveToDDSFile(bc.GetImages(), bc.GetImageCount(), bc.GetMetadata(), DDS_FLAGS_NONE, out.wstring().c_str());
            if (SUCCEEDED(hr)) { return true; }
        }
        log.Line(std::string(tag) + " WARN BC6H " + Hex(hr) + " — saving RGBA16F fallback");
    }
    ScratchImage half;
    hr = Convert(cube.GetImages(), cube.GetImageCount(), cube.GetMetadata(),
                 DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, half);
    if (SUCCEEDED(hr))
    {
        // FORCE_DX10 for the same reason as the LUT: RGBA16F has a legacy FourCC (113) and the
        // TextureCube loader requires a DX10 header.
        hr = SaveToDDSFile(half.GetImages(), half.GetImageCount(), half.GetMetadata(),
                           DDS_FLAGS_FORCE_DX10_EXT, out.wstring().c_str());
    }
    if (FAILED(hr)) { log.Line(std::string(tag) + " FAIL save " + Hex(hr)); return false; }
    return true;
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

    // 3b) CALIBRATE. See ImportOptions::skyTargetMedianLuma for why this happens here and not on
    // the camera. Applied to the float cube BEFORE the mip chain, so the display texture, its mips
    // and every F7 derivative inherit one consistent scale.
    if (opts.skyTargetMedianLuma > 0.0f)
    {
        std::vector<float> lum;
        lum.reserve((size_t)face * face * 6);
        for (int f = 0; f < 6; ++f)
        {
            const Image* img = cube.GetImage(0, (size_t)f, 0);
            for (int y = 0; y < face; ++y)
            {
                const float* row = reinterpret_cast<const float*>(img->pixels + (size_t)y * img->rowPitch);
                for (int x = 0; x < face; ++x)
                {
                    const float* px = row + (size_t)x * 4;
                    lum.push_back(0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2]);
                }
            }
        }
        const size_t mid = lum.size() / 2;
        std::nth_element(lum.begin(), lum.begin() + mid, lum.end());
        const float median = lum[mid];
        if (median > 1e-6f)
        {
            const float scale = opts.skyTargetMedianLuma / median;
            tbb::parallel_for(0, 6, [&](int f)
            {
                const Image* img = cube.GetImage(0, (size_t)f, 0);
                for (int y = 0; y < face; ++y)
                {
                    float* row = reinterpret_cast<float*>(img->pixels + (size_t)y * img->rowPitch);
                    for (int x = 0; x < face; ++x)
                    {
                        float* px = row + (size_t)x * 4;
                        px[0] *= scale; px[1] *= scale; px[2] *= scale;
                    }
                }
            });
            char msg[192];
            std::snprintf(msg, sizeof(msg),
                "  sky calib  median %.4f -> %.4f  scale x%.4f  (%+.2f EV)",
                median, opts.skyTargetMedianLuma, scale, std::log2(scale));
            log.Line(msg);
        }
        else
        {
            log.Line("  sky calib  SKIPPED: source median luminance is ~0");
        }
    }

    // 4) Mip chain, then BC6H_UF16 compress (unsigned half — sky radiance is non-negative).
    ScratchImage cubeMips;
    hr = GenerateMipMaps(cube.GetImages(), cube.GetImageCount(), cube.GetMetadata(),
                         TEX_FILTER_DEFAULT, 0, cubeMips);
    if (FAILED(hr)) { log.Line("skybox FAIL mips " + Hex(hr)); return false; }

    // Everything downstream -- this file AND the prefilter that reads `cubeMips` as its source --
    // works from the clamped chain, which is the shape of UE's CopyCubemapToScratchCubemap.
    ClampToFp16Range(cubeMips, log, "skybox");

    fs::path out = in; out.replace_extension(L".dds");
    ScratchImage bc;
    hr = CompressBlocks(cubeMips.GetImages(), cubeMips.GetImageCount(), cubeMips.GetMetadata(),
                        DXGI_FORMAT_BC6H_UF16, TEX_COMPRESS_DEFAULT | TEX_COMPRESS_PARALLEL,
                        bc, log, "skybox");
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
    if (!ok) { return false; }

    // F7: the split-sum derivatives, from the same float32 cube the display texture came from. The
    // display cube is untouched -- these are SIBLINGS, so a level that has not been converted keeps
    // loading exactly what it loads today and the runtime falls back (F8's job).
    {
        fs::path specOut = out; specOut.replace_filename(out.stem().wstring() + L"_spec.dds");
        fs::path diffOut = out; diffOut.replace_filename(out.stem().wstring() + L"_diffuse.dds");

        ScratchImage spec;
        if (BuildPrefilteredSpecular(cubeMips, face, spec, log))
        {
            if (SaveHdrCube(spec, specOut, log, "ibl spec", true))
            {
                TexMetadata sm{};
                const bool sok = SUCCEEDED(GetMetadataFromDDSFile(specOut.wstring().c_str(), DDS_FLAGS_NONE, sm)) && sm.IsCubemap();
                log.Line(std::string(sok ? "  ok  ibl spec " : "  WARN ibl spec ") + std::to_string(face) + "^2 x6" +
                         "  fmt=" + std::to_string((int)sm.format) + "  mips=" + std::to_string(sm.mipLevels) +
                         "  -> " + Narrow(specOut.filename().wstring()));
            }
        }

        ScratchImage irr;
        // 32^2 faces: the cosine convolution is a low-frequency signal, and anything larger is
        // storing noise-free duplicates of its own neighbours. Left UNCOMPRESSED -- BC6H on a
        // 32^2 cube saves 100 KB and costs banding on the one resource that must stay smooth.
        if (BuildIrradianceCube(cubeMips, 32, irr, log))
        {
            if (SaveHdrCube(irr, diffOut, log, "ibl diffuse", false))
            {
                TexMetadata dm{};
                const bool dok = SUCCEEDED(GetMetadataFromDDSFile(diffOut.wstring().c_str(), DDS_FLAGS_NONE, dm)) && dm.IsCubemap();
                log.Line(std::string(dok ? "  ok  ibl diff " : "  WARN ibl diff ") + "32^2 x6" +
                         "  fmt=" + std::to_string((int)dm.format) +
                         "  -> " + Narrow(diffOut.filename().wstring()));
            }
        }

        // Scene independent, so it is written next to the cube AND is safe to copy to textures/ once.
        // Regenerated on every skybox import because it is cheap (~1024 samples x 256^2) and because
        // a stale LUT is invisible until it is wrong.
        fs::path lutOut = out; lutOut.replace_filename(L"brdf_lut.dds");
        BuildBrdfLut(lutOut, 256, log);
    }
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

    // H5: bring up the GPU encoder (D3D11 DirectCompute BC6H/BC7). Hardware only — WARP
    // compute would lose to the OpenMP CPU path. Released on every RunImport exit.
    struct GpuDeviceGuard
    {
        ~GpuDeviceGuard()
        {
            if (g_gpuDevice) { g_gpuDevice->Release(); g_gpuDevice = nullptr; }
        }
    } gpuDeviceGuard;
    if (opts.useGpu)
    {
        const D3D_FEATURE_LEVEL wanted = D3D_FEATURE_LEVEL_11_0;
        ID3D11Device* dev = nullptr;
        const HRESULT hrDev = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            &wanted, 1, D3D11_SDK_VERSION, &dev, nullptr, nullptr);
        if (SUCCEEDED(hrDev)) { g_gpuDevice = dev; log.Line("gpu encode  : on (D3D11 BC6H/BC7 compute)"); }
        else { log.Line("gpu encode  : unavailable " + Hex(hrDev) + " — CPU path"); }
    }
    else
    {
        log.Line("gpu encode  : off — CPU path");
    }

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

    // H6: harvest glTF material factors so converted textures come out FINAL (factors baked in).
    std::map<std::string, GltfTexFactors> gltfTexFactors;
    HarvestGltfFactors(opts.stagingDir, gltfTexFactors, log);

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
            // Each generated texture is an independent import resource. A
            // partial reimport selects only the maps required by that output.
            const bool importAlbedo = includeSet.empty() || included(diff);
            const bool importMr = includeSet.empty() || included(rough) ||
                (!metal.empty() && included(metal));
            const bool importNormal = !nor.empty() &&
                (includeSet.empty() || included(nor));
            if (!importAlbedo && !importMr && !importNormal) { continue; }
            PresetEntry pe;
            if (ImportTextureSet(d, diff, rough, metal, nor, opts,
                    importAlbedo, importMr, importNormal, log, pe))
            {
                presets.push_back(pe);
                setDirs.insert(d);
            }
            else { ++failures; }
            if (importAlbedo) { consumedFiles.insert(diff); }
            if (importMr)
            {
                consumedFiles.insert(rough);
                if (!metal.empty()) { consumedFiles.insert(metal); }
            }
            if (importNormal) { consumedFiles.insert(nor); }
            bumpProgress(importAlbedo + importMr + importNormal);
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
        if (ConvertTexture(job.path, opts, log, job.rel, gltfTexFactors)) { ++converted; }
        else { ++failures; }
        bumpProgress(1);
    }

    // Snap the bar to 100% — set/flipbook dirs can hold extra files the per-unit bumps under-count.
    if (opts.progressDone && opts.progressTotal)
    {
        opts.progressDone->store(opts.progressTotal->load(), std::memory_order_relaxed);
    }

    // Register the texture-set materials as per-file assets (data/materials/<set>.json, I0).
    WritePresets(presets, log);

    log.Line("converted: " + std::to_string(converted) + " loose + " + std::to_string(presets.size()) +
             " sets + " + std::to_string(flipbooks) + " flipbooks, failures: " + std::to_string(failures));
    log.Line("=== done ===");

    if (coInited) { CoUninitialize(); }
    return failures;
}

} // namespace assets
