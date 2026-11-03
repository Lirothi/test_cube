#include "core/threading/RenderThread.h"

#include <Windows.h>

#include <cassert>

#include "core/logging/Log.h"
#include "core/profiling/Profiler.h"
#include "core/threading/ThreadRole.h"

namespace threading
{
namespace
{
RenderThread* g_parkTarget = nullptr; // the game thread's; the one Renderer::WaitForPreviousFrame parks
} // namespace

RenderThread::RenderThread()
{
    kickEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    doneEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr); // manual: a nested wait leaves it set for the outer one
    g_parkTarget = this;
    owner_ = std::this_thread::get_id();
    SetParkRenderHook(&RenderThread::Park, &RenderThread::InFlightForGame);
}

RenderThread::~RenderThread()
{
    try
    {
        Stop();
    }
    catch (...)
    {
        // The process is already unwinding (or the frame threw and nobody asked): the thread is joined either way.
    }
    if (g_parkTarget == this)
    {
        SetParkRenderHook(nullptr);
        g_parkTarget = nullptr;
    }
    CloseHandle(static_cast<HANDLE>(kickEvent_));
    CloseHandle(static_cast<HANDLE>(doneEvent_));
}

void RenderThread::Kick(std::function<void()> frame)
{
    assert(!inFlight_ && "RenderThread::Kick: the previous frame was never waited for");
    if (!thread_.joinable())
    {
        stop_ = false;
        thread_ = std::thread(&RenderThread::Main, this);
        LOG_INFO(logging::LogCategory::Core, "render thread: started (frame.renderThread 1)");
    }
    frame_ = std::move(frame);
    inFlight_ = true;
    ResetEvent(static_cast<HANDLE>(doneEvent_));
    SetEvent(static_cast<HANDLE>(kickEvent_)); // a full barrier: the frame sees everything the hand-over wrote
}

void RenderThread::WaitIdle()
{
    if (!inFlight_)
    {
        return;
    }
    HANDLE done = static_cast<HANDLE>(doneEvent_);
    if (waiting_)
    {
        // Reached from a message handler inside the pump below (a park): the thread is idle once this returns; the
        // outer wait finishes the bookkeeping and rethrows.
        WaitForSingleObject(done, INFINITE);
        return;
    }
    {
        waiting_ = true;
        bool quit = false;
        WPARAM quitCode = 0;
        for (;;)
        {
            const DWORD r = MsgWaitForMultipleObjectsEx(1, &done, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (r == WAIT_OBJECT_0)
            {
                break;
            }
            if (r != WAIT_OBJECT_0 + 1)
            {
                WaitForSingleObject(done, INFINITE); // WAIT_FAILED: no pump, but never a lost frame
                break;
            }
            MSG msg = {};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                if (msg.message == WM_QUIT)
                {
                    quit = true;
                    quitCode = msg.wParam;
                    continue;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        waiting_ = false;
        if (quit)
        {
            PostQuitMessage(static_cast<int>(quitCode));
        }
    }
    inFlight_ = false;
    if (error_)
    {
        std::exception_ptr error = error_;
        error_ = nullptr;
        LOG_ERROR(logging::LogCategory::Core, "render thread: the frame threw; rethrown on the game thread");
        std::rethrow_exception(error);
    }
}

void RenderThread::Stop()
{
    WaitIdle();
    if (thread_.joinable())
    {
        stop_ = true;
        SetEvent(static_cast<HANDLE>(kickEvent_));
        thread_.join();
        LOG_INFO(logging::LogCategory::Core, "render thread: stopped");
    }
}

void RenderThread::Park()
{
    if (g_parkTarget != nullptr && std::this_thread::get_id() == g_parkTarget->owner_)
    {
        g_parkTarget->WaitIdle();
    }
}

bool RenderThread::InFlightForGame()
{
    if (g_parkTarget == nullptr || !g_parkTarget->inFlight_.load(std::memory_order_acquire))
    {
        return false;
    }
    if (std::this_thread::get_id() != g_parkTarget->owner_)
    {
        // A task of the game half: it cannot wait for the frame (only the game thread hands frames over and waits).
        LOG_ERROR_ONCE(logging::LogCategory::Core,
                       "render thread: a game-half TASK reached the renderer during the render thread's frame");
        return false;
    }
    return true;
}

void RenderThread::Main()
{
    logging::SetCurrentThreadName("Render");
    Profiler::Get().SetThreadName("RenderThread");
    SetCurrentRole(ThreadRole::Render); // the role the frame's tasks inherit (plan section 3.4)
    for (;;)
    {
        WaitForSingleObject(static_cast<HANDLE>(kickEvent_), INFINITE);
        if (stop_)
        {
            break;
        }
        try
        {
            frame_();
        }
        catch (...)
        {
            // Device removal throws (Renderer); the game thread rethrows it after its wait, as on one thread.
            error_ = std::current_exception();
        }
        frame_ = nullptr;
        SetEvent(static_cast<HANDLE>(doneEvent_));
    }
    SetCurrentRole(ThreadRole::None);
}
} // namespace threading
