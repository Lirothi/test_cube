#include "rendering/visibility/HzbCullSelfTest.h"
#include "rendering/visibility/HzbCull.h"
#include "core/logging/Log.h"
#include "core/math/Math.h"

#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "dxcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace {

// Everything this harness says is a session-log EVENT (AGENTS.md, "Session Log"): Info for
// progress and per-case results, Error for a failed verdict. No file of its own.
constexpr logging::LogCategory kCat = logging::LogCategory::RenderValidation;

void Log(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    LOG_INFO(kCat, "{}", buf);
}

// The one line a gate reads: "hzb cull self-test: PASS ..." / "hzb cull self-test: FAIL ...".
int Verdict(const char* text, int failures)
{
    if (failures != 0) { LOG_ERROR(kCat, "hzb cull self-test: {}", text); }
    else { LOG_INFO(kCat, "hzb cull self-test: {}", text); }
    return failures;
}

// ---- GPU-side layouts: mirror hzb_cull_selftest_cs.hlsl to the byte ----------------------------

struct SelfTestConstants
{
    Math::mat4 localToWorld;
    Math::mat4 worldToClip;
    Math::mat4 viewToClip;
    int viewRect[4];
    std::uint32_t hzbSize[2];
    std::uint32_t boxCount;
    std::uint32_t footprint;
    Math::mat4 orthoWorldToClip; // S5b: the ortho cases' projection (reverse-Z), own pyramid in t2
};
static_assert(sizeof(SelfTestConstants) == 288, "SelfTestCB layout drifted from the shader");

struct TestBox
{
    float center[4];
    float extent[4];
};
static_assert(sizeof(TestBox) == 32, "TestBox stride");

struct TestResult
{
    hzb::Int4 pixels;
    hzb::Int4 hzbTexels;
    int level;
    float minDepth;
    std::uint32_t flags;
    std::uint32_t visible;
    float depth;
    float rectMinZ;
    float pad[2];
};
static_assert(sizeof(TestResult) == 64, "TestResult stride");

constexpr std::uint32_t kFlagCrossesNear    = 1u;
constexpr std::uint32_t kFlagCrossesFar     = 2u;
constexpr std::uint32_t kFlagSideCulled     = 4u;
constexpr std::uint32_t kFlagFrustumVisible = 8u;
constexpr std::uint32_t kFlagOverlapsPixel  = 16u;

// ---- The synthetic scene ----------------------------------------------------------------------
//
// A 1234x717 view (deliberately odd, not a power of two: every fold rule of hzb_build_cs.hlsl has
// to fire), reverse-Z perspective, an occluder PLANE across the whole screen at 10 m of view depth
// with a rectangular HOLE of sky (depth 0 = far) in it. Boxes are authored in view space; the
// matrices are still non-trivial -- a translated camera and a translated local frame that cancel --
// so the transform path is exercised with numbers that are exact in float.

constexpr int   kViewW = 1234;
constexpr int   kViewH = 717;
constexpr float kNear = 0.1f;
constexpr float kFar = 100.0f;
constexpr float kPlaneViewZ = 10.0f;
constexpr int   kHoleX0 = 700, kHoleX1 = 800; // [x0, x1) in full-resolution pixels
constexpr int   kHoleY0 = 300, kHoleY1 = 400;
constexpr int   kFootprint = 4;

// Device depth the rasteriser would write for a surface at view depth z under the reverse-Z
// projection below: clip.z / clip.w = (z * P22 + P32) / z.
float DeviceDepth(float viewZ)
{
    const float a = kNear / (kNear - kFar);
    const float b = (kNear * kFar) / (kFar - kNear);
    return a + b / viewZ;
}

// S5b: the ORTHO cases' projection -- reverse-Z (near -> 1, far -> 0) over the same view rect and
// the same occluder plane, but a device depth means something else under it, so the ortho cases
// test against a second pyramid built with this mapping. The window is 40 m wide at the view's
// aspect; XMMatrixOrthographicOffCenterLH with near/far SWAPPED is the reverse-Z ortho.
constexpr float kOrthoNear = 1.0f;
constexpr float kOrthoFar = 100.0f;
constexpr float kOrthoHalfW = 20.0f;
constexpr float kOrthoHalfH = kOrthoHalfW * static_cast<float>(kViewH) / static_cast<float>(kViewW);
float OrthoDepth(float viewZ)
{
    return (kOrthoFar - viewZ) / (kOrthoFar - kOrthoNear);
}

struct Pyramid
{
    int levels = 0;
    std::vector<int> w, h;
    std::vector<std::vector<float>> mip;
};

// hzb_build_cs.hlsl on the CPU: 2x2 min with the source clamped to its edge, the odd leftover
// column/row folded into the last destination texel.
template <typename LoadFn>
std::vector<float> ReduceLevel(LoadFn&& load, int srcW, int srcH, int dstW, int dstH)
{
    std::vector<float> out(static_cast<size_t>(dstW) * dstH);
    const auto src = [&](int x, int y)
    {
        x = std::min(std::max(x, 0), srcW - 1);
        y = std::min(std::max(y, 0), srcH - 1);
        return load(x, y);
    };
    for (int y = 0; y < dstH; ++y)
    {
        for (int x = 0; x < dstW; ++x)
        {
            const int sx = x * 2, sy = y * 2;
            float z = std::min(std::min(src(sx, sy), src(sx + 1, sy)), std::min(src(sx, sy + 1), src(sx + 1, sy + 1)));
            const bool oddX = (srcW & 1) != 0 && x == dstW - 1;
            const bool oddY = (srcH & 1) != 0 && y == dstH - 1;
            if (oddX) { z = std::min(z, std::min(src(sx + 2, sy), src(sx + 2, sy + 1))); }
            if (oddY) { z = std::min(z, std::min(src(sx, sy + 2), src(sx + 1, sy + 2))); }
            if (oddX && oddY) { z = std::min(z, src(sx + 2, sy + 2)); }
            out[static_cast<size_t>(y) * dstW + x] = z;
        }
    }
    return out;
}

Pyramid BuildPyramid(float plane)
{
    const auto fullRes = [&](int x, int y)
    {
        const bool inHole = x >= kHoleX0 && x < kHoleX1 && y >= kHoleY0 && y < kHoleY1;
        return inHole ? 0.0f : plane;
    };

    Pyramid p;
    const int w0 = (kViewW + 1) / 2;
    const int h0 = (kViewH + 1) / 2;
    int levels = 0;
    while (((w0 >> levels) > 0 || (h0 >> levels) > 0) && levels < 16) { ++levels; }
    p.levels = levels;
    p.w.resize(levels);
    p.h.resize(levels);
    p.mip.resize(levels);
    for (int l = 0; l < levels; ++l)
    {
        p.w[l] = std::max(1, w0 >> l);
        p.h[l] = std::max(1, h0 >> l);
    }
    p.mip[0] = ReduceLevel(fullRes, kViewW, kViewH, p.w[0], p.h[0]);
    for (int l = 1; l < levels; ++l)
    {
        const std::vector<float>& src = p.mip[l - 1];
        const int sw = p.w[l - 1];
        p.mip[l] = ReduceLevel([&](int x, int y) { return src[static_cast<size_t>(y) * sw + x]; },
                               p.w[l - 1], p.h[l - 1], p.w[l], p.h[l]);
    }
    return p;
}

struct TestCase
{
    const char* name;
    Math::float3 center;
    Math::float3 extent;
    bool expectVisible;
    const char* why;
    bool ortho = false; // S5b: HzbBoxCullFrustumOrtho against the ortho pyramid (t2)
};

// Hand-set verdicts. Positions keep a margin from every pixel-centre and depth boundary on
// purpose: the GPU may contract mul+add into an FMA where the CPU does not, and a case sitting
// exactly on a boundary would test the compiler, not the library.
const TestCase kCases[] = {
    { "front_small",        { 0.0f, 0.0f, 5.0f },       { 0.5f, 0.5f, 0.5f },    true,  "in front of the occluder plane" },
    { "behind_covered",     { -5.0f, 0.0f, 20.0f },     { 1.0f, 1.0f, 1.0f },    false, "behind the plane, away from the hole" },
    { "behind_in_hole",     { 7.4f, 0.5f, 20.0f },      { 0.5f, 0.5f, 0.5f },    true,  "behind the plane, seen through the hole" },
    { "behind_hole_edge",   { 4.5f, 0.0f, 20.0f },      { 0.5f, 0.5f, 0.5f },    true,  "straddles the hole's left edge" },
    { "near_crossing",      { 0.0f, 0.0f, 0.15f },      { 0.3f, 0.3f, 0.3f },    true,  "crosses the near plane: visible without the HZB" },
    { "off_screen_left",    { -100.0f, 0.0f, 20.0f },   { 1.0f, 1.0f, 1.0f },    false, "entirely left of the frustum" },
    { "behind_far",         { 0.0f, 0.0f, 200.0f },     { 1.0f, 1.0f, 1.0f },    false, "beyond the far plane" },
    { "subpixel_behind",    { 2.0f, 1.0f, 20.0f },      { 0.05f, 0.05f, 0.05f }, false, "1-2 px, behind the plane" },
    { "subpixel_front",     { 0.3f, 0.2f, 5.0f },       { 0.01f, 0.01f, 0.01f }, true,  "1-2 px, in front of the plane" },
    { "big_behind",         { -15.0f, 0.0f, 50.0f },    { 8.0f, 8.0f, 8.0f },    false, "large rect -> coarse level, still covered" },
    { "corner_fold",        { 33.5f, -19.5f, 20.0f },   { 0.3f, 0.3f, 0.3f },    false, "bottom-right corner: level coords clamp to the folded texel" },
    { "thin_just_in_front", { 0.0f, 0.0f, 9.99f },      { 0.5f, 0.5f, 0.005f },  true,  "5 mm in front of the plane" },
    { "thin_just_behind",   { 0.0f, 0.0f, 10.02f },     { 0.5f, 0.5f, 0.005f },  false, "15 mm behind the plane" },
    // S5b: the orthographic cull (a cascade's light view). Same plane and hole; the hole in the
    // ortho window is view x in [2.69, 5.93], y in [-1.35, 1.90].
    { "ortho_front",          { 0.0f, 0.0f, 5.0f },     { 0.5f, 0.5f, 0.5f },    true,  "ortho: in front of the plane", true },
    { "ortho_behind_covered", { -5.0f, 0.0f, 20.0f },   { 1.0f, 1.0f, 1.0f },    false, "ortho: behind the plane, away from the hole", true },
    { "ortho_behind_in_hole", { 4.3f, 0.3f, 20.0f },    { 0.5f, 0.5f, 0.5f },    true,  "ortho: behind the plane, inside the hole", true },
    { "ortho_near_crossing",  { 0.0f, 0.0f, 1.0f },     { 0.5f, 0.5f, 0.5f },    true,  "ortho: crosses the near plane (pancaked caster): visible without the HZB", true },
    { "ortho_off_left",       { -100.0f, 0.0f, 20.0f }, { 1.0f, 1.0f, 1.0f },    false, "ortho: entirely left of the window", true },
    { "ortho_beyond_far",     { 0.0f, 0.0f, 150.0f },   { 1.0f, 1.0f, 1.0f },    false, "ortho: beyond the far plane", true },
    { "ortho_big_behind",     { -12.0f, 0.0f, 50.0f },  { 6.0f, 6.0f, 6.0f },    false, "ortho: large rect -> coarse level, still covered", true },
    { "ortho_thin_front",     { 0.0f, 0.0f, 9.99f },    { 0.5f, 0.5f, 0.005f },  true,  "ortho: 5 mm in front of the plane", true },
};
constexpr std::uint32_t kCaseCount = static_cast<std::uint32_t>(sizeof(kCases) / sizeof(kCases[0]));

// The CPU mirror, applying the same consumer rules as the shader's CSMain.
TestResult Mirror(const TestCase& c, const SelfTestConstants& k, const Pyramid& pyrPersp, const Pyramid& pyrOrtho)
{
    TestResult r{};
    r.level = -1;
    const Pyramid& pyr = c.ortho ? pyrOrtho : pyrPersp;
    const hzb::FrustumCull cull = c.ortho
        ? hzb::BoxCullFrustumOrtho(c.center, c.extent, k.localToWorld, k.orthoWorldToClip, false, false)
        : hzb::BoxCullFrustumPerspective(c.center, c.extent, k.localToWorld, k.worldToClip, k.viewToClip, false);
    r.flags |= cull.crossesNearPlane ? kFlagCrossesNear : 0u;
    r.flags |= cull.crossesFarPlane ? kFlagCrossesFar : 0u;
    r.flags |= cull.frustumSideCulled ? kFlagSideCulled : 0u;
    r.flags |= cull.isVisible ? kFlagFrustumVisible : 0u;
    r.depth = cull.rectMax.z;
    r.rectMinZ = cull.rectMin.z;
    if (!cull.isVisible)
    {
        r.visible = 0u;
    }
    else if (cull.crossesNearPlane)
    {
        r.visible = 1u;
    }
    else
    {
        const hzb::Int4 viewRect{ k.viewRect[0], k.viewRect[1], k.viewRect[2], k.viewRect[3] };
        const hzb::ScreenRect rect = hzb::GetScreenRect(viewRect, cull.rectMin, cull.rectMax, static_cast<int>(k.footprint));
        r.pixels = rect.pixels;
        r.hzbTexels = rect.hzbTexels;
        r.level = rect.hzbLevel;
        r.flags |= rect.overlapsPixelCenter ? kFlagOverlapsPixel : 0u;
        const hzb::Int2 size{ static_cast<int>(k.hzbSize[0]), static_cast<int>(k.hzbSize[1]) };
        r.minDepth = hzb::GetMinDepth(size, rect, [&](int x, int y, int level)
        {
            return pyr.mip[level][static_cast<size_t>(y) * pyr.w[level] + x];
        });
        r.visible = (rect.overlapsPixelCenter && rect.depth >= r.minDepth) ? 1u : 0u;
    }
    return r;
}

// ---- D3D12 plumbing ------------------------------------------------------------------------------

ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, D3D12_HEAP_TYPE heap, UINT64 bytes,
                                    D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags);

// A synthetic pyramid on the GPU: the R32 mip chain, its staging copy and the upload. Two of
// them since S5b (perspective and ortho), so the plumbing lives here once.
struct GpuPyramid
{
    D3D12_RESOURCE_DESC desc{};
    ComPtr<ID3D12Resource> tex;
    ComPtr<ID3D12Resource> staging;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    int levels = 0;

    bool Create(ID3D12Device* device, const Pyramid& pyr)
    {
        levels = pyr.levels;
        desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = static_cast<UINT64>(pyr.w[0]);
        desc.Height = static_cast<UINT>(pyr.h[0]);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = static_cast<UINT16>(pyr.levels);
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex));
        }
        footprints.assign(static_cast<size_t>(pyr.levels), {});
        std::vector<UINT> rowCounts(static_cast<size_t>(pyr.levels));
        std::vector<UINT64> rowBytes(static_cast<size_t>(pyr.levels));
        UINT64 stagingBytes = 0;
        device->GetCopyableFootprints(&desc, 0, static_cast<UINT>(pyr.levels), 0, footprints.data(), rowCounts.data(), rowBytes.data(), &stagingBytes);
        staging = CreateBuffer(device, D3D12_HEAP_TYPE_UPLOAD, stagingBytes, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
        if (!tex || !staging) { return false; }
        void* mapped = nullptr;
        const D3D12_RANGE noRead{ 0, 0 };
        if (FAILED(staging->Map(0, &noRead, &mapped)) || !mapped) { return false; }
        for (int l = 0; l < pyr.levels; ++l)
        {
            std::uint8_t* dst = static_cast<std::uint8_t*>(mapped) + footprints[l].Offset;
            for (UINT row = 0; row < rowCounts[l]; ++row)
            {
                std::memcpy(dst + static_cast<size_t>(row) * footprints[l].Footprint.RowPitch,
                            pyr.mip[l].data() + static_cast<size_t>(row) * pyr.w[l],
                            static_cast<size_t>(pyr.w[l]) * sizeof(float));
            }
        }
        staging->Unmap(0, nullptr);
        return true;
    }

    void RecordUpload(ID3D12GraphicsCommandList* cl) const
    {
        for (int l = 0; l < levels; ++l)
        {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = tex.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = static_cast<UINT>(l);
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = staging.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = footprints[l];
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        cl->ResourceBarrier(1, &b);
    }

    void CreateSrv(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE cpu) const
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC ts{};
        ts.Format = DXGI_FORMAT_R32_FLOAT;
        ts.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        ts.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        ts.Texture2D.MipLevels = static_cast<UINT>(levels);
        device->CreateShaderResourceView(tex.Get(), &ts, cpu);
    }
};

ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, D3D12_HEAP_TYPE heap, UINT64 bytes,
                                    D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = std::max<UINT64>(bytes, 256);
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    ComPtr<ID3D12Resource> r;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r))))
    {
        return nullptr;
    }
    return r;
}

bool WriteUpload(ID3D12Resource* buffer, const void* data, size_t bytes)
{
    void* mapped = nullptr;
    const D3D12_RANGE noRead{ 0, 0 };
    if (!buffer || FAILED(buffer->Map(0, &noRead, &mapped)) || !mapped) { return false; }
    std::memcpy(mapped, data, bytes);
    buffer->Unmap(0, nullptr);
    return true;
}

void Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    cl->ResourceBarrier(1, &b);
}

// The engine's own DXC flags (Material.cpp): row-major packing, HLSL 2021. `-I shaders` resolves
// the library include the same way the runtime does, from the working directory.
ComPtr<IDxcBlob> CompileCs(const wchar_t* path, const wchar_t* entry, std::string& messages)
{
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    ComPtr<IDxcIncludeHandler> includes;
    if (FAILED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) ||
        FAILED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) ||
        FAILED(utils->CreateDefaultIncludeHandler(&includes)))
    {
        messages = "dxc unavailable";
        return nullptr;
    }
    ComPtr<IDxcBlobEncoding> src;
    if (FAILED(utils->LoadFile(path, nullptr, &src)))
    {
        messages = "cannot load the shader source (run from the project root)";
        return nullptr;
    }
    LPCWSTR args[] = { path, L"-E", entry, L"-T", L"cs_6_0", L"-Zpr", L"-HV", L"2021", L"-I", L"shaders", L"-O3" };
    DxcBuffer buf{ src->GetBufferPointer(), src->GetBufferSize(), DXC_CP_ACP };
    ComPtr<IDxcResult> result;
    if (FAILED(compiler->Compile(&buf, args, static_cast<UINT32>(sizeof(args) / sizeof(args[0])), includes.Get(), IID_PPV_ARGS(&result))))
    {
        messages = "dxc Compile call failed";
        return nullptr;
    }
    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    if (errors && errors->GetStringLength() > 0) { messages = errors->GetStringPointer(); }
    HRESULT status = S_OK;
    result->GetStatus(&status);
    if (FAILED(status)) { return nullptr; }
    ComPtr<IDxcBlob> object;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
    return object;
}

const char* Yn(std::uint32_t v) { return v ? "vis" : "OCC"; }

} // namespace

int RunHzbCullSelfTest()
{
    Log("hzb cull self-test (occlusion plan S2): view %dx%d, plane at %.1f m (device z %.6g), hole px [%d,%d)x[%d,%d)",
        kViewW, kViewH, kPlaneViewZ, DeviceDepth(kPlaneViewZ), kHoleX0, kHoleX1, kHoleY0, kHoleY1);

#ifdef _DEBUG
    {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
        {
            dbg->EnableDebugLayer();
            Log("debug layer enabled");
        }
    }
#endif

    // --- Shader first: a compile error is the most likely failure and needs no device. ---
    std::string messages;
    ComPtr<IDxcBlob> dxil = CompileCs(L"shaders/hzb_cull_selftest_cs.hlsl", L"CSMain", messages);
    if (!messages.empty()) { logging::WriteRawLines(logging::LogLevel::Warning, kCat, messages.c_str()); }
    if (!dxil) { return Verdict("FAIL shader-compile", 1); }

    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
    {
        return Verdict("FAIL device-create", 1);
    }

    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fence;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    const bool ready =
        SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) &&
        SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
        SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cl))) &&
        SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE evt = ready ? CreateEventW(nullptr, FALSE, FALSE, nullptr) : nullptr;
    if (!ready || !evt)
    {
        if (evt) { CloseHandle(evt); }
        return Verdict("FAIL cmdlist", 1);
    }

    // --- Root signature (embedded in the DXIL) + PSO ---
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(device->CreateRootSignature(0, dxil->GetBufferPointer(), dxil->GetBufferSize(), IID_PPV_ARGS(&rootSig))))
    {
        CloseHandle(evt);
        return Verdict("FAIL root-signature", 1);
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rootSig.Get();
    pd.CS.pShaderBytecode = dxil->GetBufferPointer();
    pd.CS.BytecodeLength = dxil->GetBufferSize();
    if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso))))
    {
        CloseHandle(evt);
        return Verdict("FAIL pso", 1);
    }

    // --- Scene: matrices exact in float, boxes in view space ---
    const Math::float3 eye(3.0f, 2.0f, -5.0f);
    const Math::mat4 view = Math::mat4::LookAtLH(eye, eye + Math::float3(0.0f, 0.0f, 1.0f), Math::float3(0.0f, 1.0f, 0.0f));
    const Math::mat4 proj = Math::mat4::PerspectiveFovLHReverseZ(DirectX::XM_PIDIV2,
                                                                 static_cast<float>(kViewW) / static_cast<float>(kViewH),
                                                                 kNear, kFar);
    const Pyramid pyr = BuildPyramid(DeviceDepth(kPlaneViewZ));
    const Pyramid pyrOrtho = BuildPyramid(OrthoDepth(kPlaneViewZ));
    Log("pyramid: mip 0 %dx%d, %d levels (perspective plane %.7g, ortho plane %.7g)", pyr.w[0], pyr.h[0], pyr.levels,
        static_cast<double>(DeviceDepth(kPlaneViewZ)), static_cast<double>(OrthoDepth(kPlaneViewZ)));

    SelfTestConstants k{};
    k.localToWorld = Math::mat4::Translation(eye); // local == view exactly: +eye then -eye
    k.worldToClip = view * proj;
    k.viewToClip = proj;
    // Near/far swapped on purpose: reverse-Z ortho (see kOrthoNear).
    k.orthoWorldToClip = view * Math::mat4::OrthoOffCenterLH(-kOrthoHalfW, kOrthoHalfW, -kOrthoHalfH, kOrthoHalfH,
                                                             kOrthoFar, kOrthoNear);
    k.viewRect[0] = 0; k.viewRect[1] = 0; k.viewRect[2] = kViewW; k.viewRect[3] = kViewH;
    k.hzbSize[0] = static_cast<std::uint32_t>(pyr.w[0]);
    k.hzbSize[1] = static_cast<std::uint32_t>(pyr.h[0]);
    k.boxCount = kCaseCount;
    k.footprint = kFootprint;

    std::vector<TestBox> boxes(kCaseCount);
    for (std::uint32_t i = 0; i < kCaseCount; ++i)
    {
        const TestCase& c = kCases[i];
        boxes[i] = TestBox{ { c.center.x, c.center.y, c.center.z, c.ortho ? 1.0f : 0.0f }, { c.extent.x, c.extent.y, c.extent.z, 0.0f } };
    }

    // --- Resources ---
    ComPtr<ID3D12Resource> cbBuf = CreateBuffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(SelfTestConstants), D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    ComPtr<ID3D12Resource> boxBuf = CreateBuffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, sizeof(TestBox) * kCaseCount, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    ComPtr<ID3D12Resource> resultBuf = CreateBuffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, sizeof(TestResult) * kCaseCount, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> readback = CreateBuffer(device.Get(), D3D12_HEAP_TYPE_READBACK, sizeof(TestResult) * kCaseCount, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);

    GpuPyramid gpuPyr, gpuPyrOrtho;
    const bool pyramidsOk = gpuPyr.Create(device.Get(), pyr) && gpuPyrOrtho.Create(device.Get(), pyrOrtho);

    ComPtr<ID3D12DescriptorHeap> heap;
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 4; // t0 boxes, t1 pyramid, t2 ortho pyramid, u0 results
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
    }
    if (!cbBuf || !boxBuf || !resultBuf || !readback || !pyramidsOk || !heap)
    {
        CloseHandle(evt);
        return Verdict("FAIL resources", 1);
    }

    WriteUpload(cbBuf.Get(), &k, sizeof(k));
    WriteUpload(boxBuf.Get(), boxes.data(), sizeof(TestBox) * kCaseCount);

    const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = kCaseCount;
        sd.Buffer.StructureByteStride = sizeof(TestBox);
        device->CreateShaderResourceView(boxBuf.Get(), &sd, cpu);            // t0
        cpu.ptr += inc;
        gpuPyr.CreateSrv(device.Get(), cpu);                                  // t1
        cpu.ptr += inc;
        gpuPyrOrtho.CreateSrv(device.Get(), cpu);                             // t2
        cpu.ptr += inc;
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = kCaseCount;
        ud.Buffer.StructureByteStride = sizeof(TestResult);
        device->CreateUnorderedAccessView(resultBuf.Get(), nullptr, &ud, cpu); // u0
    }

    // --- Record: upload the pyramids, dispatch, copy the results out ---
    gpuPyr.RecordUpload(cl.Get());
    gpuPyrOrtho.RecordUpload(cl.Get());

    ID3D12DescriptorHeap* heaps[] = { heap.Get() };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(rootSig.Get());
    cl->SetPipelineState(pso.Get());
    cl->SetComputeRootConstantBufferView(0, cbBuf->GetGPUVirtualAddress());
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap->GetGPUDescriptorHandleForHeapStart();
    cl->SetComputeRootDescriptorTable(1, gpu);
    gpu.ptr += 3ull * inc;
    cl->SetComputeRootDescriptorTable(2, gpu);
    cl->Dispatch((kCaseCount + 63u) / 64u, 1, 1);

    Transition(cl.Get(), resultBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->CopyResource(readback.Get(), resultBuf.Get());
    cl->Close();
    {
        ID3D12CommandList* lists[] = { cl.Get() };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), 1);
        if (fence->GetCompletedValue() < 1)
        {
            fence->SetEventOnCompletion(1, evt);
            WaitForSingleObject(evt, INFINITE);
        }
    }
    const HRESULT removed = device->GetDeviceRemovedReason();
    if (FAILED(removed))
    {
        Log("device removed: 0x%08lX", static_cast<unsigned long>(removed));
        CloseHandle(evt);
        return Verdict("FAIL device-removed", 1);
    }

    // --- Compare ---
    std::vector<TestResult> gpuResults(kCaseCount);
    {
        void* mapped = nullptr;
        const D3D12_RANGE range{ 0, sizeof(TestResult) * kCaseCount };
        if (FAILED(readback->Map(0, &range, &mapped)) || !mapped)
        {
            CloseHandle(evt);
            return Verdict("FAIL readback-map", 1);
        }
        std::memcpy(gpuResults.data(), mapped, sizeof(TestResult) * kCaseCount);
        const D3D12_RANGE noWrite{ 0, 0 };
        readback->Unmap(0, &noWrite);
    }
    CloseHandle(evt);

    int failures = 0;
    for (std::uint32_t i = 0; i < kCaseCount; ++i)
    {
        const TestCase& c = kCases[i];
        const TestResult& g = gpuResults[i];
        const TestResult m = Mirror(c, k, pyr, pyrOrtho);
        const bool sameRect = std::memcmp(&g.pixels, &m.pixels, sizeof(g.pixels)) == 0 &&
                              std::memcmp(&g.hzbTexels, &m.hzbTexels, sizeof(g.hzbTexels)) == 0;
        const bool sameDepth = std::memcmp(&g.minDepth, &m.minDepth, sizeof(float)) == 0;
        const bool gpuEqCpu = sameRect && g.level == m.level && sameDepth && g.flags == m.flags && g.visible == m.visible;
        const bool expected = (m.visible != 0u) == c.expectVisible;
        const bool ok = gpuEqCpu && expected;
        if (!ok) { ++failures; }
        Log("%-4s %-19s expect=%s gpu=%s cpu=%s  level=%d/%d px=[%d,%d..%d,%d]/[%d,%d..%d,%d] tex=[%d,%d..%d,%d]/[%d,%d..%d,%d] minDepth=%.7g/%.7g depth=%.7g/%.7g flags=%u/%u  -- %s%s%s",
            ok ? "ok" : "FAIL", c.name, Yn(c.expectVisible ? 1u : 0u), Yn(g.visible), Yn(m.visible),
            g.level, m.level,
            g.pixels.x, g.pixels.y, g.pixels.z, g.pixels.w, m.pixels.x, m.pixels.y, m.pixels.z, m.pixels.w,
            g.hzbTexels.x, g.hzbTexels.y, g.hzbTexels.z, g.hzbTexels.w, m.hzbTexels.x, m.hzbTexels.y, m.hzbTexels.z, m.hzbTexels.w,
            static_cast<double>(g.minDepth), static_cast<double>(m.minDepth),
            static_cast<double>(g.depth), static_cast<double>(m.depth),
            g.flags, m.flags, c.why,
            gpuEqCpu ? "" : " [gpu != cpu mirror]", expected ? "" : " [cpu verdict != expectation]");
    }

    char verdict[96];
    if (failures == 0)
    {
        std::snprintf(verdict, sizeof(verdict), "PASS %u cases, gpu == cpu mirror", kCaseCount);
    }
    else
    {
        std::snprintf(verdict, sizeof(verdict), "FAIL %d of %u cases", failures, kCaseCount);
    }
    return Verdict(verdict, failures);
}
