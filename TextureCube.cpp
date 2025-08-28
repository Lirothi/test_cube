#include "TextureCube.h"
#include "Renderer.h"
#include "DescriptorAllocator.h"
#include "Helpers.h"

#include <fstream>
#include <cstring>
#include <algorithm>

using Microsoft::WRL::ComPtr;

#pragma pack(push,1)
struct DDS_PIXELFORMAT {
    uint32_t size;
    uint32_t flags;
    uint32_t fourCC;
    uint32_t RGBBitCount;
    uint32_t RBitMask;
    uint32_t GBitMask;
    uint32_t BBitMask;
    uint32_t ABitMask;
};

struct DDS_HEADER {
    uint32_t size;
    uint32_t flags;
    uint32_t height;
    uint32_t width;
    uint32_t pitchOrLinearSize;
    uint32_t depth;
    uint32_t mipMapCount;
    uint32_t reserved1[11];
    DDS_PIXELFORMAT ddspf;
    uint32_t caps;
    uint32_t caps2;
    uint32_t caps3;
    uint32_t caps4;
    uint32_t reserved2;
};

struct DDS_HEADER_DXT10 {
    DXGI_FORMAT               dxgiFormat;
    D3D12_RESOURCE_DIMENSION  resourceDimension;
    uint32_t                  miscFlag;    // D3D11_RESOURCE_MISC_TEXTURECUBE = 0x4
    uint32_t                  arraySize;   // кратно 6 для cubemap arrays
    uint32_t                  miscFlags2;  // alpha mode и пр.
};
#pragma pack(pop)

#ifndef MAKEFOURCC
#define MAKEFOURCC(ch0, ch1, ch2, ch3) \
    ((uint32_t)(uint8_t)(ch0)        | ((uint32_t)(uint8_t)(ch1) << 8) | \
     ((uint32_t)(uint8_t)(ch2) << 16)| ((uint32_t)(uint8_t)(ch3) << 24))
#endif

static constexpr uint32_t DDS_MAGIC = MAKEFOURCC('D', 'D', 'S', ' ');
static constexpr uint32_t DDS_FOURCC = 0x00000004u;          // ddspf.flags
static constexpr uint32_t FOURCC_DX10 = MAKEFOURCC('D', 'X', '1', '0');

static constexpr uint32_t DDSCAPS2_CUBEMAP = 0x00000200u;
static constexpr uint32_t DDSCAPS2_CUBEMAP_POSITIVEX = 0x00000400u;
static constexpr uint32_t DDSCAPS2_CUBEMAP_NEGATIVEX = 0x00000800u;
static constexpr uint32_t DDSCAPS2_CUBEMAP_POSITIVEY = 0x00001000u;
static constexpr uint32_t DDSCAPS2_CUBEMAP_NEGATIVEY = 0x00002000u;
static constexpr uint32_t DDSCAPS2_CUBEMAP_POSITIVEZ = 0x00004000u;
static constexpr uint32_t DDSCAPS2_CUBEMAP_NEGATIVEZ = 0x00008000u;

//=============================================================================
// Public API
//=============================================================================

bool TextureCube::CreateFromDDS(Renderer* r,
                                ID3D12GraphicsCommandList* uploadCmd,
                                const std::wstring& path,
                                std::vector<ComPtr<ID3D12Resource>>* keepAlive)
{
    if (!r || !uploadCmd) {
        return false;
    }

    // 1) читаем файл
    std::vector<uint8_t> file;
    if (!LoadFileToMemory_(path, file)) {
        OutputDebugStringW((L"[TextureCube] failed to read: " + path + L"\n").c_str());
        return false;
    }

    // 2) парсим DDS (требуем DX10 header для современных форматов)
    UINT w=0, h=0, mips=1, arr=0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    size_t dataOfs = 0;
    bool isCube = false;

    if (!ParseDDS_(file.data(), file.size(), w, h, mips, arr, fmt, dataOfs, isCube)) {
        OutputDebugStringW((L"[TextureCube] unsupported DDS (need DX10 cubemap): " + path + L"\n").c_str());
        return false;
    }
    if (!isCube || arr == 0 || (arr % 6) != 0) {
        OutputDebugStringW(L"[TextureCube] DDS is not a cubemap (arraySize not multiple of 6)\n");
        return false;
    }

    width_ = w; height_ = h; mipLevels_ = mips; arraySize_ = arr; format_ = fmt;

    // 3) создаём GPU ресурс (Texture2D array, arraySize = 6*N)
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = static_cast<UINT16>(arr);
    td.MipLevels = static_cast<UINT16>(mips);
    td.Format = fmt;
    td.SampleDesc = {1,0};
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    ThrowIfFailed(r->GetDevice()->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex_)));

    // 4) подготавливаем upload и раскладку сабресурсов
    const UINT subresources = mips * arr;

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(subresources);
    std::vector<UINT>   numRows(subresources);
    std::vector<UINT64> rowSizes(subresources);
    UINT64 uploadBytes = 0;

    r->GetDevice()->GetCopyableFootprints(&td, 0, subresources, 0,
        layouts.data(), numRows.data(), rowSizes.data(), &uploadBytes);

    D3D12_HEAP_PROPERTIES hpUp{}; hpUp.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC upDesc{};
    upDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upDesc.Width = uploadBytes;
    upDesc.Height = 1;
    upDesc.DepthOrArraySize = 1;
    upDesc.MipLevels = 1;
    upDesc.Format = DXGI_FORMAT_UNKNOWN;
    upDesc.SampleDesc = {1,0};
    upDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> upload;
    ThrowIfFailed(r->GetDevice()->CreateCommittedResource(
        &hpUp, D3D12_HEAP_FLAG_NONE, &upDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));

    // 5) копируем линейные данные DDS в upload-буфер (с учётом Footprint.RowPitch)
    uint8_t* mapped = nullptr;
    ThrowIfFailed(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));

    const uint8_t* src = file.data() + dataOfs;
    size_t srcOfs = 0;

    for (UINT s = 0; s < subresources; ++s) {
        const auto& lay = layouts[s];
        const UINT rows = numRows[s];
        const UINT64 rowBytes = rowSizes[s];

        for (UINT y = 0; y < rows; ++y) {
            std::memcpy(mapped + lay.Offset + size_t(y) * lay.Footprint.RowPitch,
                        src + srcOfs, (size_t)rowBytes);
            srcOfs += (size_t)rowBytes;
        }
    }
    upload->Unmap(0, nullptr);

    // 6) CopyTextureRegion всех сабресурсов
    for (UINT s = 0; s < subresources; ++s) {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = tex_.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = s;

        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = upload.Get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = layouts[s];

        uploadCmd->CopyTextureRegion(&dst, 0,0,0, &srcLoc, nullptr);
    }

    // 7) барьер COPY_DEST -> PS SRV
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex_.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        uploadCmd->ResourceBarrier(1, &b);
    }

    if (keepAlive) {
        keepAlive->push_back(upload);
    }

    r->SetResourceState(tex_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // 8) создаём CPU SRV (TextureCube или TextureCubeArray, формат = как у ресурса)
    CreateSrvCPU_(r, format_, mips, arr);

    // сброс staged-кэша
    stagedFrame_ = UINT(-1);
    srvGPU_.ptr = 0;

    return true;
}

D3D12_GPU_DESCRIPTOR_HANDLE TextureCube::GetSRVForFrame(Renderer* r)
{
    if (stagedFrame_ == r->GetCurrentFrameIndex() && srvGPU_.ptr != 0) {
        return srvGPU_;
    }
    auto& da = r->GetDescAlloc();
    auto h = da.Alloc();
    r->GetDevice()->CopyDescriptorsSimple(
        1, h.cpu, srvCPU_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    srvGPU_ = h.gpu;
    stagedFrame_ = r->GetCurrentFrameIndex();
    return srvGPU_;
}

//=============================================================================
// Internals
//=============================================================================

bool TextureCube::LoadFileToMemory_(const std::wstring& path, std::vector<uint8_t>& data)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    f.seekg(0, std::ios::end);
    const auto size = size_t(f.tellg());
    f.seekg(0, std::ios::beg);
    data.resize(size);
    if (size > 0) {
        f.read(reinterpret_cast<char*>(data.data()), size);
    }
    return true;
}

bool TextureCube::ParseDDS_(const uint8_t* bytes, size_t size,
    UINT& outW, UINT& outH, UINT& outMips, UINT& outArray,
    DXGI_FORMAT& outFmt, size_t& outDataOffset, bool& outIsCube)
{
    if (size < 4 + sizeof(DDS_HEADER)) {
        return false;
    }

    const uint32_t magic = *reinterpret_cast<const uint32_t*>(bytes);
    if (magic != DDS_MAGIC) {
        return false;
    }

    const auto* hdr = reinterpret_cast<const DDS_HEADER*>(bytes + 4);
    if (hdr->size != sizeof(DDS_HEADER)) {
        return false;
    }

    outW = hdr->width;
    outH = hdr->height;
    outMips = std::max(1u, hdr->mipMapCount);

    const bool hasFourCC = (hdr->ddspf.flags & DDS_FOURCC) != 0;
    const uint32_t fourcc = hdr->ddspf.fourCC;
    const bool hasDX10 = hasFourCC && (fourcc == FOURCC_DX10);

    if (hasDX10) {
        // DX10 header path (modern formats)
        if (size < 4 + sizeof(DDS_HEADER) + sizeof(DDS_HEADER_DXT10)) return false;
        const auto* hx = reinterpret_cast<const DDS_HEADER_DXT10*>(bytes + 4 + sizeof(DDS_HEADER));
        if (hx->resourceDimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return false;

        outIsCube = (hx->miscFlag & 0x4) != 0; // D3D11_RESOURCE_MISC_TEXTURECUBE
        outW = hdr->width;
        outH = hdr->height;
        outMips = std::max(1u, hdr->mipMapCount);
        outFmt = hx->dxgiFormat;

        // некоторые тулзы пишут arraySize=1 для куба и надеются на legacy-флаги caps2
        const uint32_t faceMask =
            0x00000400u | 0x00000800u | 0x00001000u | 0x00002000u | 0x00004000u | 0x00008000u; // +X -X +Y -Y +Z -Z
        const bool legacyCube = (hdr->caps2 & 0x00000200u) != 0 && ((hdr->caps2 & faceMask) == faceMask); // DDSCAPS2_CUBEMAP

        outArray = std::max(1u, hx->arraySize);
        if (outIsCube) {
            if (outArray == 1 && legacyCube) {
                // поблажка: сломанный DX10 заголовок, но legacy-флаги говорят "куб" → считаем 1 куб = 6 слоёв
                outArray = 6;
            }
            if ((outArray % 6) != 0) {
                // всё ещё мусор — лучше отказать, чтобы не копировать данные мимо
                return false;
            }
        }

        outDataOffset = 4 + sizeof(DDS_HEADER) + sizeof(DDS_HEADER_DXT10);
        return true;
    }

    // --- Legacy DDS path (без DX10). Делаем минимально полезную поддержку ---
    // Определим, куб это или нет, и пытаемся сопоставить распространённые форматы.
    const bool capsCube = (hdr->caps2 & DDSCAPS2_CUBEMAP) != 0;
    const uint32_t faceMask =
        DDSCAPS2_CUBEMAP_POSITIVEX | DDSCAPS2_CUBEMAP_NEGATIVEX |
        DDSCAPS2_CUBEMAP_POSITIVEY | DDSCAPS2_CUBEMAP_NEGATIVEY |
        DDSCAPS2_CUBEMAP_POSITIVEZ | DDSCAPS2_CUBEMAP_NEGATIVEZ;

    outIsCube = capsCube && ((hdr->caps2 & faceMask) == faceMask);
    outArray = outIsCube ? 6u : 1u;

    // Формат:
    // a) Сжатые DXT — маппим на BC1/2/3 UNORM
    if (hasFourCC) {
        switch (fourcc) {
        case MAKEFOURCC('D', 'X', 'T', '1'): outFmt = DXGI_FORMAT_BC1_UNORM; break;
        case MAKEFOURCC('D', 'X', 'T', '3'): outFmt = DXGI_FORMAT_BC2_UNORM; break;
        case MAKEFOURCC('D', 'X', 'T', '5'): outFmt = DXGI_FORMAT_BC3_UNORM; break;
        default: outFmt = DXGI_FORMAT_UNKNOWN; break;
        }
    }
    // b) Несжатые 32-битные
    if (outFmt == DXGI_FORMAT_UNKNOWN && hdr->ddspf.RGBBitCount == 32) {
        const auto R = hdr->ddspf.RBitMask;
        const auto G = hdr->ddspf.GBitMask;
        const auto B = hdr->ddspf.BBitMask;
        const auto A = hdr->ddspf.ABitMask;
        // A8R8G8B8 (D3D9) -> BGRA8
        if (R == 0x00FF0000 && G == 0x0000FF00 && B == 0x000000FF && A == 0xFF000000) {
            outFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
        }
        // A8B8G8R8 -> RGBA8
        else if (R == 0x000000FF && G == 0x0000FF00 && B == 0x00FF0000 && A == 0xFF000000) {
            outFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
        }
    }

    if (outFmt == DXGI_FORMAT_UNKNOWN) {
        // Не поддерживаем этот старый формат — лучше перегнать DDS в DX10/HDR/BC6H.
        return false;
    }

    outDataOffset = 4 + sizeof(DDS_HEADER);
    return true;
}

void TextureCube::CreateSrvCPU_(Renderer* r, DXGI_FORMAT srvFmt, UINT mipLevels, UINT arraySize)
{
    // CPU-only heap на 1 SRV
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = 1;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(r->GetDevice()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeapCPU_)));
    srvCPU_ = srvHeapCPU_->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = srvFmt;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    if (arraySize > 6) {
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        sd.TextureCubeArray.MostDetailedMip = 0;
        sd.TextureCubeArray.MipLevels = mipLevels;
        sd.TextureCubeArray.NumCubes = arraySize / 6;
        sd.TextureCubeArray.First2DArrayFace = 0;
    } else {
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sd.TextureCube.MostDetailedMip = 0;
        sd.TextureCube.MipLevels = mipLevels;
    }

    r->GetDevice()->CreateShaderResourceView(tex_.Get(), &sd, srvCPU_);
}