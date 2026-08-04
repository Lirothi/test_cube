#include "rendering/core/Screenshot.h"
#include "rendering/core/BarrierTranslation.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

#include <wincodec.h>
#include <wrl/client.h>

#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace
{
    struct ScopedCom
    {
        bool ok = false;
        ScopedCom()
        {
            const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            ok = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
        }
        ~ScopedCom() { if (ok) { CoUninitialize(); } }
    };

    bool EncodePng(const std::vector<std::uint8_t>& bgra, UINT w, UINT h, const std::wstring& path)
    {
        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory))))
        {
            return false;
        }
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> options;
        if (FAILED(factory->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
            FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
            FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
            FAILED(encoder->CreateNewFrame(&frame, &options)) ||
            FAILED(frame->Initialize(options.Get())) ||
            FAILED(frame->SetSize(w, h)))
        {
            return false;
        }
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(frame->SetPixelFormat(&fmt)) ||
            FAILED(frame->WritePixels(h, w * 4, static_cast<UINT>(bgra.size()),
                const_cast<BYTE*>(bgra.data()))) ||
            FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        {
            return false;
        }
        return true;
    }
}

namespace Screenshot
{
bool SaveBackbufferPng(Renderer& renderer, const std::string& path)
{
    ID3D12Device* device = renderer.GetDevice();
    ID3D12Resource* src = renderer.GetLastPresentedBackbuffer();
    if (!device || !src || path.empty())
    {
        return false;
    }

    renderer.WaitForPreviousFrame(); // GPU idle: the presented image is stable in VRAM

    const D3D12_RESOURCE_DESC desc = src->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 rowSize = 0, total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowSize, &total);
    if (total == 0)
    {
        return false;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buf{};
    buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf.Width = total;
    buf.Height = 1;
    buf.DepthOrArraySize = 1;
    buf.MipLevels = 1;
    buf.SampleDesc.Count = 1;
    buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
    {
        return false;
    }

    UploadBatch batch;
    if (!batch.Begin(&renderer))
    {
        return false;
    }
    ID3D12GraphicsCommandList* cl = batch.CommandList();

    // Raw barriers (not the state tracker): the backbuffer is in PRESENT; we borrow it to
    // COPY_SOURCE and restore it to PRESENT, so the tracker's belief stays consistent.
    auto barrier = [cl](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
    {
        D3D12_RESOURCE_BARRIER t{};
        t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        t.Transition.pResource = r;
        t.Transition.StateBefore = a;
        t.Transition.StateAfter = b;
        t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers::EmitOne(cl, t);
    };
    barrier(src, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION s{};
    s.pResource = src;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION d{};
    d.pResource = readback.Get();
    d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    d.PlacedFootprint = fp;
    cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    barrier(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    batch.SubmitAndWait(&renderer);

    const UINT w = static_cast<UINT>(desc.Width);
    const UINT h = desc.Height;
    std::vector<std::uint8_t> px(static_cast<size_t>(w) * h * 4);
    void* mapped = nullptr;
    D3D12_RANGE rr{ 0, static_cast<SIZE_T>(total) };
    if (FAILED(readback->Map(0, &rr, &mapped)))
    {
        return false;
    }
    const auto* srcp = static_cast<const std::uint8_t*>(mapped) + fp.Offset;
    for (UINT y = 0; y < h; ++y)
    {
        std::memcpy(px.data() + static_cast<size_t>(y) * w * 4,
                    srcp + static_cast<size_t>(y) * fp.Footprint.RowPitch,
                    static_cast<size_t>(w) * 4);
    }
    readback->Unmap(0, nullptr);

    // Backbuffer is R8G8B8A8_UNORM (bytes already gamma-encoded LDR); WIC wants BGRA.
    for (size_t i = 0; i + 2 < px.size(); i += 4)
    {
        std::swap(px[i], px[i + 2]);
    }

    ScopedCom com;
    if (!com.ok)
    {
        return false;
    }
    const fs::path out(path);
    std::error_code ec;
    if (!out.parent_path().empty())
    {
        fs::create_directories(out.parent_path(), ec);
    }
    return EncodePng(px, w, h, out.wstring());
}
} // namespace Screenshot
