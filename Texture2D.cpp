#include "Texture2D.h"
#include "Renderer.h"
#include "DescriptorAllocator.h"
#include "Helpers.h"

#include <wrl.h>
#include <wincodec.h>
#include <algorithm>
#include <fstream>
#include <cstring>
#include <cwctype>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

// ========================= helpers =========================
static bool CreateWICFactory(ComPtr<IWICImagingFactory>& out)
{
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&out));
    return SUCCEEDED(hr);
}

static bool EndsWithNoCase(const std::wstring& s, const std::wstring& suf)
{
    if (s.size() < suf.size()) {
        return false;
    }
    for (size_t i = 0; i < suf.size(); ++i) {
        if (std::towlower(s[s.size() - suf.size() + i]) != std::towlower(suf[i])) {
            return false;
        }
    }
    return true;
}

// ========================= WIC loader (to RGBA8) =========================
bool Texture2D::LoadRGBA8_WIC_(const std::wstring& path, std::vector<uint8_t>& outRGBA, UINT& outW, UINT& outH)
{
    outRGBA.clear(); outW = outH = 0;

    ComPtr<IWICImagingFactory> factory;
    if (!CreateWICFactory(factory)) {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = factory->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        return false;
    }

    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h))) {
        return false;
    }

    ComPtr<IWICFormatConverter> conv;
    if (FAILED(factory->CreateFormatConverter(&conv))) {
        return false;
    }

    // Конвертируем в RGBA8 (линейное пространство — SRGB обрабатываем выбором SRV формата)
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        return false;
    }

    outRGBA.resize(size_t(w) * size_t(h) * 4);
    const UINT stride = w * 4;
    const UINT bufSz = stride * h;

    if (FAILED(conv->CopyPixels(nullptr, stride, bufSz, reinterpret_cast<BYTE*>(outRGBA.data())))) {
        outRGBA.clear();
        return false;
    }

    outW = w; outH = h;
    return true;
}

// ========================= DDS loader (direct to GPU format) =========================
namespace {
#pragma pack(push,1)
    struct DDS_PIXELFORMAT {
        uint32_t size; uint32_t flags; uint32_t fourCC; uint32_t RGBBitCount;
        uint32_t RBitMask; uint32_t GBitMask; uint32_t BBitMask; uint32_t ABitMask;
    };
    struct DDS_HEADER {
        uint32_t size; uint32_t flags; uint32_t height; uint32_t width; uint32_t pitchOrLinearSize;
        uint32_t depth; uint32_t mipMapCount; uint32_t reserved1[11]; DDS_PIXELFORMAT ddspf;
        uint32_t caps; uint32_t caps2; uint32_t caps3; uint32_t caps4; uint32_t reserved2;
    };
    struct DDS_HEADER_DXT10 {
        DXGI_FORMAT dxgiFormat; uint32_t resourceDimension; uint32_t miscFlag; uint32_t arraySize; uint32_t miscFlags2;
    };
#pragma pack(pop)

    static constexpr uint32_t DDS_MAGIC = 0x20534444u; // 'DDS '
    static constexpr uint32_t DDPF_FOURCC = 0x4;
    static constexpr uint32_t FOURCC(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        return (a) | ((b) << 8) | ((c) << 16) | ((d) << 24);
    }

    struct FormatPair { DXGI_FORMAT resTypeless; DXGI_FORMAT srvUnorm; DXGI_FORMAT srvSRGB; bool isBC; uint32_t bytesPerBlockOrPixel; };

    static bool MapDXGIToPair(DXGI_FORMAT fmt, FormatPair& out)
    {
        switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            out = { DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false, 4 }; return true;
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
            out = { DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM_SRGB, true, 8 }; return true;
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
            out = { DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM_SRGB, true, 16 }; return true;
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
            out = { DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM_SRGB, true, 16 }; return true;
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            out = { DXGI_FORMAT_BC4_TYPELESS, DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_UNKNOWN /*no sRGB*/, true, 8 }; return true;
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
            out = { DXGI_FORMAT_BC5_TYPELESS, DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_UNKNOWN /*no sRGB*/, true, 16 }; return true;
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            out = { DXGI_FORMAT_BC7_TYPELESS, DXGI_FORMAT_BC7_UNORM, DXGI_FORMAT_BC7_UNORM_SRGB, true, 16 }; return true;
        default:
            return false;
        }
    }

    static bool MapLegacyFourCCToPair(uint32_t fourCC, FormatPair& out)
    {
        switch (fourCC) {
        case FOURCC('D', 'X', 'T', '1'): return MapDXGIToPair(DXGI_FORMAT_BC1_UNORM, out);
        case FOURCC('D', 'X', 'T', '3'): return MapDXGIToPair(DXGI_FORMAT_BC2_UNORM, out);
        case FOURCC('D', 'X', 'T', '5'): return MapDXGIToPair(DXGI_FORMAT_BC3_UNORM, out);
        case FOURCC('B', 'C', '4', 'U'): return MapDXGIToPair(DXGI_FORMAT_BC4_UNORM, out);
        case FOURCC('B', 'C', '5', 'U'): return MapDXGIToPair(DXGI_FORMAT_BC5_UNORM, out);
        default: return false;
        }
    }

    static size_t MipByteSize(const FormatPair& fp, UINT w, UINT h)
    {
        if (fp.isBC) {
            UINT bw = std::max(1u, (w + 3u) / 4u);
            UINT bh = std::max(1u, (h + 3u) / 4u);
            return size_t(bw) * size_t(bh) * fp.bytesPerBlockOrPixel;
        }
        else {
            return size_t(w) * size_t(h) * fp.bytesPerBlockOrPixel;
        }
    }
}

bool Texture2D::CreateFromDDS_(Renderer* r, ID3D12GraphicsCommandList* uploadCmd,
    const CreateDesc& desc,
    std::vector<ComPtr<ID3D12Resource>>* keepAlive)
{
    // 1) Читаем файл целиком
    std::ifstream f(desc.path, std::ios::binary);
    if (!f) {
        return false;
    }
    uint32_t magic = 0; f.read(reinterpret_cast<char*>(&magic), 4);
    if (magic != DDS_MAGIC) {
        return false;
    }

    DDS_HEADER hdr{}; f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (hdr.size != sizeof(DDS_HEADER) || hdr.ddspf.size != sizeof(DDS_PIXELFORMAT)) {
        return false;
    }

    // Поддержка только 2D, без кубов и массивов
    const bool isCube = (hdr.caps2 & 0x00000200u) != 0; // DDSCAPS2_CUBEMAP
    if (isCube) {
        return false;
    }

    DXGI_FORMAT fileFmt = DXGI_FORMAT_UNKNOWN;
    FormatPair fp{};

    bool hasDX10 = (hdr.ddspf.flags & DDPF_FOURCC) && (hdr.ddspf.fourCC == FOURCC('D', 'X', '1', '0'));
    DDS_HEADER_DXT10 dxt10{};
    if (hasDX10) {
        f.read(reinterpret_cast<char*>(&dxt10), sizeof(dxt10));
        if (dxt10.resourceDimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dxt10.arraySize != 1) {
            return false;
        }
        fileFmt = dxt10.dxgiFormat;
        if (!MapDXGIToPair(fileFmt, fp)) {
            return false;
        }
    }
    else {
        // Легаси FourCC
        if (hdr.ddspf.flags & DDPF_FOURCC) {
            if (!MapLegacyFourCCToPair(hdr.ddspf.fourCC, fp)) {
                return false;
            }
        }
        else {
            // Предположим RGBA8
            if (hdr.ddspf.RGBBitCount == 32 && hdr.ddspf.RBitMask == 0x00FF0000 && hdr.ddspf.GBitMask == 0x0000FF00 && hdr.ddspf.BBitMask == 0x000000FF && hdr.ddspf.ABitMask == 0xFF000000) {
                MapDXGIToPair(DXGI_FORMAT_R8G8B8A8_UNORM, fp);
            }
            else {
                return false; // неподдерживаемый формат
            }
        }
    }

    const UINT mipCount = std::max(1u, hdr.mipMapCount);
    const UINT width = std::max(1u, hdr.width);
    const UINT height = std::max(1u, hdr.height);

    // 2) Загружаем данные в память
    std::vector<uint8_t> fileData;
    f.seekg(0, std::ios::end);
    const std::streamoff fileSize = f.tellg();
    std::streamoff dataStart = 4 + sizeof(DDS_HEADER) + (hasDX10 ? sizeof(DDS_HEADER_DXT10) : 0);
    const std::streamoff dataSize = fileSize - dataStart;
    fileData.resize(static_cast<size_t>(dataSize));
    f.seekg(dataStart, std::ios::beg);
    f.read(reinterpret_cast<char*>(fileData.data()), dataSize);

    // 3) Создаём ресурс (TYPELESS, чтобы SRGB/UNORM SRV на выбор)
    auto* device = r->GetDevice();

    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = width;
    td.Height = height;
    td.DepthOrArraySize = 1;
    td.MipLevels = static_cast<UINT16>(mipCount);
    td.Format = fp.resTypeless;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_NONE;

    ThrowIfFailed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex_)));
    tex_->SetName(L"Tex2D_RESOURCE_DDS");

    // 4) Вычисляем footprints для всех мипов
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fps(mipCount);
    std::vector<UINT> numRows(mipCount);
    std::vector<UINT64> rowSizes(mipCount);
    UINT64 uploadTotal = 0;
    device->GetCopyableFootprints(&td, 0, mipCount, 0, fps.data(), numRows.data(), rowSizes.data(), &uploadTotal);

    // 5) Upload buffer
    D3D12_HEAP_PROPERTIES hpUp{}; hpUp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC upDesc{};
    upDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upDesc.Width = uploadTotal;
    upDesc.Height = 1;
    upDesc.DepthOrArraySize = 1;
    upDesc.MipLevels = 1;
    upDesc.Format = DXGI_FORMAT_UNKNOWN;
    upDesc.SampleDesc.Count = 1;
    upDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> upload;
    ThrowIfFailed(device->CreateCommittedResource(&hpUp, D3D12_HEAP_FLAG_NONE, &upDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));

    // 6) Копируем по мипам, с учётом BC-строя
    uint8_t* mapped = nullptr;
    D3D12_RANGE rge{ 0, 0 };
    ThrowIfFailed(upload->Map(0, &rge, reinterpret_cast<void**>(&mapped)));

    size_t srcOffset = 0;
    UINT mw = width, mh = height;
    for (UINT m = 0; m < mipCount; ++m) {
        const UINT rows = numRows[m];
        size_t srcRowPitch = 0;
        if (fp.isBC) {
            const UINT bw = std::max(1u, (mw + 3u) / 4u);
            srcRowPitch = size_t(bw) * fp.bytesPerBlockOrPixel;
        }
        else {
            srcRowPitch = size_t(mw) * fp.bytesPerBlockOrPixel;
        }
        const uint8_t* srcMip = fileData.data() + srcOffset;
        for (UINT y = 0; y < rows; ++y) {
            std::memcpy(mapped + fps[m].Offset + size_t(y) * fps[m].Footprint.RowPitch,
                srcMip + size_t(y) * srcRowPitch,
                srcRowPitch);
        }
        srcOffset += MipByteSize(fp, mw, mh);
        mw = std::max(1u, mw >> 1);
        mh = std::max(1u, mh >> 1);
    }
    upload->Unmap(0, nullptr);

    // 7) Copy -> resource
    for (UINT m = 0; m < mipCount; ++m) {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = tex_.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = m;

        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = upload.Get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = fps[m];

        uploadCmd->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
    }

    // 8) Barrier COPY_DEST -> PIXEL_SHADER_RESOURCE
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex_.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        uploadCmd->ResourceBarrier(1, &b);
    }

    if (keepAlive) {
        keepAlive->push_back(upload);
    }
    r->SetResourceState(tex_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // 9) Выбираем SRV формат по назначению (sRGB для Albedo)
    DXGI_FORMAT srvFmt = fp.srvUnorm;
    if (desc.usage == Usage::AlbedoSRGB && fp.srvSRGB != DXGI_FORMAT_UNKNOWN) {
        srvFmt = fp.srvSRGB;
    }

    CreateCpuSrv_(r, srvFmt, mipCount);

    width_ = width; height_ = height; mipLevels_ = mipCount;
    resourceFormat_ = td.Format; srvFormat_ = srvFmt;
    stagedFrame_ = UINT(-1); srvGPU_.ptr = 0;

    return true;
}

// ========================= Public API =========================
bool Texture2D::CreateFromFile(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmd,
    const CreateDesc& desc,
    std::vector<ComPtr<ID3D12Resource>>* keepAlive)
{
    // DDS — отдельный путь
    if (EndsWithNoCase(desc.path, L".dds")) {
        if (!CreateFromDDS_(renderer, uploadCmd, desc, keepAlive)) {
            OutputDebugStringW((L"[Texture2D] DDS load failed: " + desc.path + L"\n").c_str());
            return false;
        }
        return true;
    }

    // 1) WIC → RGBA8
    std::vector<uint8_t> rgba;
    UINT w = 0, h = 0;
    if (!LoadRGBA8_WIC_(desc.path, rgba, w, h)) {
        OutputDebugStringW((L"[Texture2D] WIC load failed: " + desc.path + L"\n").c_str());
        return false;
    }

    // 2) Если это NormalMap и normalIsRG=true — «обнулим» B, чтобы не мусорил
    if (desc.usage == Usage::NormalMap && desc.normalIsRG) {
        for (size_t i = 0; i < rgba.size(); i += 4) {
            rgba[i + 2] = 0;   // B = 0
        }
    }

    // 3) Выбираем форматы: ресурс TYPELESS, SRV UNORM/SRGB
    DXGI_FORMAT resourceFmt = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    DXGI_FORMAT srvFmt = (desc.usage == Usage::AlbedoSRGB) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
        : DXGI_FORMAT_R8G8B8A8_UNORM;

    // 4) Upload
    UploadRGBA8_(renderer, uploadCmd, rgba.data(), w, h, keepAlive, resourceFmt);
    CreateCpuSrv_(renderer, srvFmt, /*mips*/1);

    width_ = w; height_ = h; mipLevels_ = 1;
    resourceFormat_ = resourceFmt; srvFormat_ = srvFmt;

    // сброс staged кэша
    stagedFrame_ = UINT(-1); srvGPU_.ptr = 0;

    return true;
}

void Texture2D::CreateFromRGBA8(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmd,
    const void* rgba8, UINT width, UINT height,
    std::vector<ComPtr<ID3D12Resource>>* keepAlive)
{
    // По умолчанию — линейный UNORM SRV (подойдёт для normal/MR/linear)
    DXGI_FORMAT resFmt = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    DXGI_FORMAT srvFmt = DXGI_FORMAT_R8G8B8A8_UNORM;

    UploadRGBA8_(renderer, uploadCmd, rgba8, width, height, keepAlive, resFmt);
    CreateCpuSrv_(renderer, srvFmt, /*mips*/1);

    width_ = width; height_ = height; mipLevels_ = 1;
    resourceFormat_ = resFmt; srvFormat_ = srvFmt;

    stagedFrame_ = UINT(-1); srvGPU_.ptr = 0;
}

D3D12_GPU_DESCRIPTOR_HANDLE Texture2D::GetSRVForFrame(Renderer* r)
{
    if (stagedFrame_ == r->GetCurrentFrameIndex() && srvGPU_.ptr != 0) {
        return srvGPU_;
    }

    auto& da = r->GetDescAlloc();
    auto h = da.Alloc(); // CPU/GPU пара
    r->GetDevice()->CopyDescriptorsSimple(1, h.cpu, srvCPU_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    srvGPU_ = h.gpu;
    stagedFrame_ = r->GetCurrentFrameIndex();
    return srvGPU_;
}

// ========================= Internal: upload & SRV =========================
void Texture2D::UploadRGBA8_(Renderer* r, ID3D12GraphicsCommandList* uploadCmd,
    const void* rgba8, UINT width, UINT height,
    std::vector<ComPtr<ID3D12Resource>>* keepAlive,
    DXGI_FORMAT resourceFmt)
{
    auto* device = r->GetDevice();

    // Default texture (TYPELESS)
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = width;
    td.Height = height;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1; // TODO: мипы по желанию (compute/graphics)
    td.Format = resourceFmt; // TYPELESS
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_NONE;

    ThrowIfFailed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex_)));
    tex_->SetName(L"Tex2D_RESOURCE");

    // Footprint
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT numRows = 0; UINT64 rowSize = 0, total = 0;
    device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &numRows, &rowSize, &total);

    // Upload buffer
    D3D12_HEAP_PROPERTIES hpUp{}; hpUp.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC upDesc{};
    upDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upDesc.Width = total;
    upDesc.Height = 1;
    upDesc.DepthOrArraySize = 1;
    upDesc.MipLevels = 1;
    upDesc.Format = DXGI_FORMAT_UNKNOWN;
    upDesc.SampleDesc.Count = 1;
    upDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> upload;
    ThrowIfFailed(device->CreateCommittedResource(&hpUp, D3D12_HEAP_FLAG_NONE, &upDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));

    // Fill upload rows
    uint8_t* mapped = nullptr;
    D3D12_RANGE rge{ 0, 0 };
    ThrowIfFailed(upload->Map(0, &rge, reinterpret_cast<void**>(&mapped)));

    const uint8_t* src = reinterpret_cast<const uint8_t*>(rgba8);
    const size_t srcPitch = size_t(width) * 4;

    for (UINT y = 0; y < height; ++y) {
        std::memcpy(mapped + fp.Offset + y * fp.Footprint.RowPitch, src + y * srcPitch, srcPitch);
    }
    upload->Unmap(0, nullptr);

    // Copy
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = tex_.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = upload.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = fp;

    uploadCmd->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);

    // Barrier COPY_DEST -> PIXEL_SHADER_RESOURCE
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex_.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    uploadCmd->ResourceBarrier(1, &b);

    // Держим upload до исполнения
    if (keepAlive) {
        keepAlive->push_back(upload);
    }

    // Запомнить стейт в трекере
    r->SetResourceState(tex_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void Texture2D::CreateCpuSrv_(Renderer* r, DXGI_FORMAT srvFmt, UINT mipLevels)
{
    auto* device = r->GetDevice();

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = 1;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // CPU-only
    ThrowIfFailed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeapCPU_)));
    srvCPU_ = srvHeapCPU_->GetCPUDescriptorHandleForHeapStart();
    srvHeapCPU_->SetName(L"Tex2D_DESC_HEAP");

    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = srvFmt; // UNORM или SRGB
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = mipLevels;
    sd.Texture2D.MostDetailedMip = 0;
    sd.Texture2D.ResourceMinLODClamp = 0.f;
    device->CreateShaderResourceView(tex_.Get(), &sd, srvCPU_);

    srvFormat_ = srvFmt;
}
