#pragma once
// The one road to the GPU queues for a frame in flight -- game thread plan G10 (docs/game_thread_plan.md).
//
// Every queue operation of the running frame -- the frame's submission (its lists, the waits and signals between the
// queues, Present, the frame fences), the game half's deferred GPU work, the profiler's drain signal -- is handed to
// Run, in order. Inline (frame.rhiThread 0, the default): Run executes it at once on the calling thread. Threaded
// (G10b, UE's r.RHIThread.Enable): the RHI thread executes them in the order handed over, and WaitFor / Flush are where
// a caller waits for them -- the slot's frame fences (BeginFrame), an idle GPU, an ImGui atlas upload, a resize.
//
//     render:  [record N][record N+1 ..........][record N+2 ...
//     rhi:              [submit N, Present N]   [submit N+1, Present N+1]
//
// While a caller waits it delivers the messages SENT to its windows (not the posted ones): DXGI may send the window a
// message from Present on the RHI thread and block until the window's thread answers it.

#include <atomic>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

class RhiQueue
{
public:
    using Op = std::function<void()>;
    using Ticket = std::uint64_t;

    RhiQueue();
    ~RhiQueue(); // runs what is left, then joins
    RhiQueue(const RhiQueue&) = delete;
    RhiQueue& operator=(const RhiQueue&) = delete;

    // Executes `op` after everything handed over before it; its ticket is what WaitFor waits for. Inline: at once,
    // and what it throws reaches the caller here; threaded: on the RHI thread, and what it throws is rethrown by the
    // next WaitFor / Flush (the ops after a throw still run: the device is gone, and each says so itself).
    Ticket Run(Op op);
    // Returns once the op with that ticket (and every one before it) has run.
    void WaitFor(Ticket ticket);
    void Flush();
    bool Threaded() const { return thread_.joinable(); }
    // Starts or stops the RHI thread, after everything handed over has run. Only where no other thread hands over.
    void SetThreaded(bool threaded);

private:
    struct Item
    {
        Ticket ticket = 0;
        Op op;
    };
    void Main();
    void Stop();
    void RethrowPending();

    std::thread thread_;
    std::mutex mutex_;                      // ops_, stop_, error_, issued_
    void* workEvent_ = nullptr;             // HANDLE, auto-reset: an op (or the stop) was handed over
    void* progressEvent_ = nullptr;         // HANDLE, manual-reset: an op ran (a waiter resets it before it sleeps)
    std::deque<Item> ops_;
    bool stop_ = false;
    std::exception_ptr error_;
    Ticket issued_ = 0;                     // the last ticket handed out
    std::atomic<Ticket> completed_{ 0 };    // every op up to this one has run
};
