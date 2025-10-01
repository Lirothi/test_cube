#include "FrameResource.h"
#include "Helpers.h"

void FrameResource::InitUpload(ID3D12Device* dev, UINT bytes) {
    if (bytes == 0u) {
        bytes = 1u << 20; // 1 MB by default
    }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc = { 1, 0 };
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3D12Resource> tmp;
    ThrowIfFailed(dev->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(tmp.ReleaseAndGetAddressOf())));

    upload_ = tmp;
    uploadSize_ = bytes;
    uploadGPU_ = upload_->GetGPUVirtualAddress();
    uploadOffset_.store(0u, std::memory_order_release);

    // Persistent map
    D3D12_RANGE rge{ 0,0 };
    void* p = nullptr;
    ThrowIfFailed(upload_->Map(0, &rge, &p));
    uploadCPU_ = p;

    // Clear fallback chunks
    extraUploads_.clear();
}

void FrameResource::ResetUpload() {
    uploadOffset_.store(0u, std::memory_order_release);
    // Release fallback chunks from the previous frame
    extraUploads_.clear();
}

FrameResource::DynamicAlloc FrameResource::AllocDynamic(UINT size, UINT align) {
    DynamicAlloc out{};
    if (size == 0u) {
        return out;
    }
    if (align == 0u) {
        align = 16u;
    }
    
    auto AlignUp_ = [](UINT v, UINT a) -> UINT {
        const UINT m = a - 1u;
        return (v + m) & ~m;
        };

    // Try the main buffer first
    for (;;) {
        UINT old = uploadOffset_.load(std::memory_order_relaxed);
        UINT aligned = AlignUp_(old, align);
        if (aligned + size <= uploadSize_) {
            if (uploadOffset_.compare_exchange_weak(old, aligned + size,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
                out.cpu = static_cast<uint8_t*>(uploadCPU_) + aligned;
                out.gpu = uploadGPU_ + aligned;
                out.size = size;
                out.offset = aligned;
                return out;
            }
            else {
                continue;
            }
        }
        break;
    }

    // Overflow: take or create a fallback chunk and allocate from it.
    {
        std::lock_guard<std::mutex> lk(uploadGrowMtx_);

        // Try existing chunks
        for (auto& ch : extraUploads_) {
            for (;;) {
                UINT old = ch.offset.load(std::memory_order_relaxed);
                UINT aligned = AlignUp_(old, align);
                if (aligned + size <= ch.size) {
                    if (ch.offset.compare_exchange_weak(old, aligned + size,
                        std::memory_order_acq_rel, std::memory_order_relaxed)) {
                        out.cpu = static_cast<uint8_t*>(ch.cpu) + aligned;
                        out.gpu = ch.gpu + aligned;
                        out.size = size;
                        out.offset = aligned;
                        return out;
                    }
                    else {
                        continue;
                    }
                }
                break;
            }
        }

        // Create a new chunk (grow multiplicatively)
        UploadChunk_ ch{};
        ch.size = std::max<UINT>(AlignUp_(size, 65536u), uploadSize_); // no smaller than the base buffer

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        // For single-GPU we can specify the node mask explicitly
        hp.CreationNodeMask = 1;
        hp.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = ch.size;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc = { 1, 0 };
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = D3D12_RESOURCE_FLAG_NONE;

        // Obtain the device from the existing upload_ resource (it is the same ID3D12Device)
        Microsoft::WRL::ComPtr<ID3D12Device> dev;
        ThrowIfFailed(upload_->GetDevice(IID_PPV_ARGS(dev.GetAddressOf())));

        ThrowIfFailed(dev->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(ch.res.ReleaseAndGetAddressOf())));

        // persistent map
        D3D12_RANGE rge{ 0, 0 };
        void* p = nullptr;
        ThrowIfFailed(ch.res->Map(0, &rge, &p));
        ch.cpu = p;
        ch.gpu = ch.res->GetGPUVirtualAddress();
        ch.offset.store(0u, std::memory_order_release);

        // Immediately carve out space from it
        const UINT aligned = AlignUp_(0u, align);
        ch.offset.store(aligned + size, std::memory_order_release);

        out.cpu = static_cast<uint8_t*>(ch.cpu) + aligned;
        out.gpu = ch.gpu + aligned;
        out.size = size;
        out.offset = aligned;

        extraUploads_.push_back(std::move(ch));
        return out;
    }
}