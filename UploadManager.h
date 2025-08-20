#pragma once
#include <wrl.h>
#include <d3d12.h>
#include <vector>
#include <cstdint>
#include "Helpers.h"

using Microsoft::WRL::ComPtr;

class UploadManager {
public:
    UploadManager(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList)
        : device_(device), cmdList_(cmdList) {
    }

    ComPtr<ID3D12Resource> CreateBufferWithData(const void* srcData, size_t byteSize,
        D3D12_RESOURCE_FLAGS flags,
        D3D12_RESOURCE_STATES afterCopy)
    {
        // --- Default (GPU) ---
        D3D12_HEAP_PROPERTIES heapDefault{};
        heapDefault.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = byteSize;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = flags;

        ComPtr<ID3D12Resource> defaultBuf;
        ThrowIfFailed(device_->CreateCommittedResource(
            &heapDefault, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&defaultBuf)));

        // --- Upload ---
        D3D12_HEAP_PROPERTIES heapUpload{};
        heapUpload.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC descUpload = desc;
        descUpload.Flags = D3D12_RESOURCE_FLAG_NONE;

        ComPtr<ID3D12Resource> uploadBuf;
        ThrowIfFailed(device_->CreateCommittedResource(
            &heapUpload, D3D12_HEAP_FLAG_NONE, &descUpload,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&uploadBuf)));

        // Map+memcpy
        void* dst = nullptr;
        D3D12_RANGE range{ 0, 0 };
        ThrowIfFailed(uploadBuf->Map(0, &range, &dst));
        std::memcpy(dst, srcData, byteSize);
        uploadBuf->Unmap(0, nullptr);

        // Copy + barrier
        cmdList_->CopyResource(defaultBuf.Get(), uploadBuf.Get());

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = defaultBuf.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = afterCopy;
        cmdList_->ResourceBarrier(1, &barrier);

        keepAlive_.push_back(uploadBuf);
        return defaultBuf;
    }

    void StealKeepAlive(std::vector<ComPtr<ID3D12Resource>>* out) {
        if (out) {
            out->insert(out->end(), keepAlive_.begin(), keepAlive_.end());
            keepAlive_.clear();
        }
    }

private:
    ID3D12Device* device_ = nullptr;
    ID3D12GraphicsCommandList* cmdList_ = nullptr;
    std::vector<ComPtr<ID3D12Resource>> keepAlive_;
};