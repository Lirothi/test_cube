#include "rendering/core/RhiQueue.h"

#include <Windows.h>

#include "core/logging/Log.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "core/threading/ThreadRole.h"
#include "rendering/core/GpuAlloc.h"

RhiQueue::RhiQueue()
{
    workEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    progressEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

RhiQueue::~RhiQueue()
{
    try
    {
        Stop();
    }
    catch (...)
    {
        // An op threw and nobody waited for it: the process is shutting down either way; the thread is joined.
    }
    CloseHandle(static_cast<HANDLE>(workEvent_));
    CloseHandle(static_cast<HANDLE>(progressEvent_));
}

RhiQueue::Ticket RhiQueue::Run(Op op)
{
    Ticket ticket = 0;
    if (Threaded())
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ticket = ++issued_;
            ops_.push_back(Item{ ticket, std::move(op) });
        }
        SetEvent(static_cast<HANDLE>(workEvent_));
        return ticket;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ticket = ++issued_;
    }
    struct Done
    {
        std::atomic<Ticket>& completed;
        Ticket ticket;
        ~Done() { completed.store(ticket, std::memory_order_release); }
    } done{ completed_, ticket };
    const gpu::SubmissionScope submitting;
    op();
    return ticket;
}

void RhiQueue::WaitFor(Ticket ticket)
{
    if (completed_.load(std::memory_order_acquire) < ticket)
    {
        CPU_SCOPE(ProfilerScopes::kRhiThreadWait);
        HANDLE progress = static_cast<HANDLE>(progressEvent_);
        for (;;)
        {
            if (completed_.load(std::memory_order_acquire) >= ticket)
            {
                break;
            }
            ResetEvent(progress);
            if (completed_.load(std::memory_order_acquire) >= ticket)
            {
                break;
            }
            // Two waiters at once (rare) can reset each other's wake-up: the timeout bounds that to 2 ms.
            const DWORD r = MsgWaitForMultipleObjectsEx(1, &progress, 2, QS_SENDMESSAGE, 0);
            if (r == WAIT_OBJECT_0 + 1)
            {
                // Delivers the messages other threads SENT to this thread's windows (DXGI's from Present); the posted
                // ones stay queued for the loop's own pump.
                MSG msg = {};
                PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
            }
        }
    }
    RethrowPending();
}

void RhiQueue::Flush()
{
    Ticket last = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last = issued_;
    }
    WaitFor(last);
}

void RhiQueue::SetThreaded(bool threaded)
{
    if (threaded == Threaded())
    {
        return;
    }
    if (!threaded)
    {
        Stop();
        return;
    }
    Flush();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = false;
    }
    thread_ = std::thread(&RhiQueue::Main, this);
    LOG_INFO(logging::LogCategory::RenderRhi, "rhi thread: started (frame.rhiThread 1)");
}

void RhiQueue::Stop()
{
    if (!thread_.joinable())
    {
        return;
    }
    std::exception_ptr pending; // an op's throw is the caller's, after the join (a joinable std::thread terminates)
    try
    {
        Flush();
    }
    catch (...)
    {
        pending = std::current_exception();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    SetEvent(static_cast<HANDLE>(workEvent_));
    thread_.join();
    LOG_INFO(logging::LogCategory::RenderRhi, "rhi thread: stopped");
    if (pending)
    {
        std::rethrow_exception(pending);
    }
}

void RhiQueue::RethrowPending()
{
    std::exception_ptr error;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::swap(error, error_);
    }
    if (error)
    {
        LOG_ERROR(logging::LogCategory::RenderRhi, "rhi thread: an op threw; rethrown on the thread that waited");
        std::rethrow_exception(error);
    }
}

void RhiQueue::Main()
{
    logging::SetCurrentThreadName("RHI");
    Profiler::Get().SetThreadName("RHIThread");
    Profiler::MarkThreadCrossFrame(); // frame N's submit runs while the game thread closes N and opens N + 1
    threading::SetCurrentRole(threading::ThreadRole::Render); // the render half's queue work, handed over in order
    for (;;)
    {
        Item item;
        bool stop = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!ops_.empty())
            {
                item = std::move(ops_.front());
                ops_.pop_front();
            }
            else
            {
                stop = stop_;
            }
        }
        if (!item.op)
        {
            if (stop)
            {
                break;
            }
            WaitForSingleObject(static_cast<HANDLE>(workEvent_), INFINITE);
            continue;
        }
        {
            CPU_SCOPE(ProfilerScopes::kRhiThreadOp);
            try
            {
                const gpu::SubmissionScope submitting;
                item.op();
            }
            catch (...)
            {
                // Device removal throws from Present (Renderer); the next WaitFor / Flush rethrows the first one.
                std::lock_guard<std::mutex> lock(mutex_);
                if (!error_)
                {
                    error_ = std::current_exception();
                }
            }
        }
        item.op = nullptr; // what the op captured goes before anyone is told it ran
        completed_.store(item.ticket, std::memory_order_release);
        SetEvent(static_cast<HANDLE>(progressEvent_));
    }
    threading::SetCurrentRole(threading::ThreadRole::None);
}
