#include "Texture2D.h"
#include "Renderer.h"
#include "DescriptorAllocator.h"
#include "Helpers.h"

using Microsoft::WRL::ComPtr;

void Texture2D::CreateFromRGBA8(Renderer* renderer,
                                ID3D12GraphicsCommandList* uploadCmd,
                                const void* rgba8, UINT width, UINT height,
                                std::vector<ComPtr<ID3D12Resource>>* keepAlive)
{
    DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    UploadRGBA8_(renderer, uploadCmd, rgba8, width, height, keepAlive, fmt);
    CreateCpuSrv_(renderer, fmt);
    stagedFrame_ = UINT(-1);
    srvGPU_.ptr = 0;
}

void Texture2D::UploadRGBA8_(Renderer* r, ID3D12GraphicsCommandList* uploadCmd,
                             const void* rgba8, UINT width, UINT height,
                             std::vector<ComPtr<ID3D12Resource>>* keepAlive,
                             DXGI_FORMAT fmt)
{
    auto* device = r->GetDevice();

    // Default texture
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = width;
    td.Height = height;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_NONE;

    ThrowIfFailed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex_)));

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
    for (UINT y = 0; y < height; ++y) {
        std::memcpy(mapped + fp.Offset + y * fp.Footprint.RowPitch, src + y * width * 4, width * 4);
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
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    uploadCmd->ResourceBarrier(1, &b);

    // Держим upload до исполнения
    if (keepAlive)
    {
	    keepAlive->push_back(upload);
    }

    // Запомнить известный стейт в рендерере (если пользуешься трекером)
    r->SetResourceState(tex_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); // трекер у тебя есть. :contentReference[oaicite:5]{index=5}
}

void Texture2D::CreateCpuSrv_(Renderer* r, DXGI_FORMAT fmt)
{
    auto* device = r->GetDevice();

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = 1;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // ВАЖНО: CPU-only
    ThrowIfFailed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeapCPU_)));
    srvCPU_ = srvHeapCPU_->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = fmt;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(tex_.Get(), &sd, srvCPU_);
}

D3D12_GPU_DESCRIPTOR_HANDLE Texture2D::GetSRVForFrame(Renderer* r)
{
    if (stagedFrame_ == r->GetCurrentFrameIndex() && srvGPU_.ptr != 0) {
        return srvGPU_;
    }
    auto& da = r->GetDescAlloc();                       // shader-visible heap кадра
    auto h = da.Alloc();                                // CPU/GPU пара
    r->GetDevice()->CopyDescriptorsSimple(
        1, h.cpu, srvCPU_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    srvGPU_ = h.gpu;
    stagedFrame_ = r->GetCurrentFrameIndex();
    return srvGPU_;
}