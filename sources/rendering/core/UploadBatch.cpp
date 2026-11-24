#include "rendering/core/UploadBatch.h"

#include "rendering/core/Renderer.h"
#include "rendering/core/DeferredRelease.h"

UploadBatch::~UploadBatch()
{
    // Begun but never submitted (e.g. a failed load): close the list so the
    // recorded work is dropped cleanly, without executing it.
    if (open_ && cmdList_)
    {
        cmdList_->Close();
    }
}

bool UploadBatch::Begin(Renderer* renderer)
{
    if (open_ || !renderer || !renderer->GetDevice())
    {
        return false;
    }

    ID3D12Device* device = renderer->GetDevice();
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_))))
    {
        return false;
    }
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr, IID_PPV_ARGS(&cmdList_))))
    {
        allocator_.Reset();
        return false;
    }

    keepAlive_.clear();
    open_ = true;
    return true;
}

bool UploadBatch::CloseAndExecute(Renderer* renderer)
{
    if (!open_ || !renderer)
    {
        return false;
    }

    if (FAILED(cmdList_->Close()))
    {
        open_ = false;
        return false;
    }
    // Game thread plan G7: from the game half, unparked, the list waits for the hand-over; SubmitAndWait's wait sends it
    // first (Renderer::WaitForPreviousFrame flushes the hand-over submits before it idles the GPU).
    renderer->ExecuteOrDefer(cmdList_.Get());
    open_ = false;
    return true;
}

bool UploadBatch::Submit(Renderer* renderer)
{
    if (!CloseAndExecute(renderer))
    {
        return false;
    }
    // R16.8: the list, its allocator and the intermediates go to the deferred-release service -- the batch's owner no
    // longer keeps them (it may drop the batch at once). The service stamps them with the frame number current now plus
    // kMargin: a list deferred to the hand-over executes with the NEXT frame's work, which that margin covers -- by the
    // number of the call alone the allocator would be released under a list that has not reached the queue yet.
    std::uint64_t bytes = 0;
    for (const Microsoft::WRL::ComPtr<ID3D12Resource>& resource : keepAlive_)
    {
        if (resource) { bytes += resource->GetDesc().Width; }
    }
    render::deferred::Retire([allocator = std::move(allocator_), list = std::move(cmdList_),
                              keep = std::move(keepAlive_)]() mutable {
        keep.clear();
        list.Reset();
        allocator.Reset();
    }, bytes, "upload batch");
    keepAlive_.clear();
    return true;
}

void UploadBatch::SubmitAndWait(Renderer* renderer, std::source_location where)
{
    if (!CloseAndExecute(renderer))
    {
        return;
    }
    renderer->WaitForPreviousFrame(where);
    // The GPU is idle and has run this list: everything goes now, as before R16.8.
    keepAlive_.clear();
    cmdList_.Reset();
    allocator_.Reset();
}
