#include "rendering/core/UploadBatch.h"

#include "rendering/core/Renderer.h"

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

void UploadBatch::SubmitAndWait(Renderer* renderer)
{
    if (!open_ || !renderer)
    {
        return;
    }

    cmdList_->Close();
    ID3D12CommandList* lists[] = { cmdList_.Get() };
    renderer->GetCommandQueue()->ExecuteCommandLists(1, lists);
    renderer->WaitForPreviousFrame();

    keepAlive_.clear();
    open_ = false;
}
