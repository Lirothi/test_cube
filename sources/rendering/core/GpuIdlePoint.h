#pragma once
// Architecture plan R8.1 (docs/architecture_review_plan.md): the render half's GPU-idle point. What frees or re-makes a
// resource the frames in flight may read takes one: the first need waits (Renderer::WaitForPreviousFrame, counted at the
// point's own site), and every later need finds the GPU idle while no work was handed out since (rhi::Frame::
// WorkHandedOut) -- an upload in between, or the frame's own submission, makes the next need wait again. The frame's
// reconciles (RenderScene::Render: the shadow mode, its capacity, the shadow LOD curve, a shader reload's caster rebuild)
// and the caster set's growth (ShadowGpuData::UpdateForFrame) share one a frame: until R8.1 each waited on its own, three
// or four times in a row on a frame that changed two of them.
#include <cstdint>
#include <source_location>

#include "rendering/core/Renderer.h"

class GpuIdlePoint
{
public:
    explicit GpuIdlePoint(Renderer& renderer, std::source_location where = std::source_location::current())
        : renderer_(renderer), where_(where) {}
    GpuIdlePoint(const GpuIdlePoint&) = delete;
    GpuIdlePoint& operator=(const GpuIdlePoint&) = delete;

    // The GPU idle from here on: a wait, unless this point waited and no work was handed out since.
    void Ensure()
    {
        if (waited_ && renderer_.RhiFrame().WorkHandedOut() == workAtIdle_) { return; }
        renderer_.WaitForPreviousFrame(where_);
        NoteIdle();
    }
    // Someone else made the GPU idle just now (UploadBatch::SubmitAndWait waits for everything handed out, its own too).
    void NoteIdle()
    {
        waited_ = true;
        workAtIdle_ = renderer_.RhiFrame().WorkHandedOut();
    }
    // Did a need of this point wait (or find it idle)?
    bool Waited() const { return waited_; }

private:
    Renderer& renderer_;
    std::source_location where_;
    bool waited_ = false;
    std::uint64_t workAtIdle_ = 0;
};
