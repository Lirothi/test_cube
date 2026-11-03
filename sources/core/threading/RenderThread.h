#pragma once
// The render half of the frame on a thread of its own -- game thread plan G8 (docs/game_thread_plan.md),
// `frame.renderThread` 1.
//
// The game thread hands it ONE frame at a time and waits for that frame before the next hand-over, so the game half
// runs at most one frame ahead of what is drawn: UE's FFrameEndSync with r.OneFrameThreadLag 1
// (RenderingThread.cpp:2435-2439, 2506-2510).
//
//     game:   [simulate N+1][wait N][UI, Sync N+1][Kick N+1][simulate N+2][wait N+1] ...
//     render:  [render N ..........]                       [render N+1 .............]
//
// While the game thread waits it pumps the window's messages (plan section 3.9): DXGI may SEND the window a message
// from Present on this thread and block until the window's thread answers it.

#include <atomic>
#include <exception>
#include <functional>
#include <thread>

namespace threading
{
// The switch the game loop reads at the start of each frame (Developer Controls > Frame "Render thread",
// graphics_settings.json frame.renderThread, --render-thread, --set frame.renderThread). Game half only.
inline bool g_renderThreadEnabled = false;

class RenderThread
{
public:
    RenderThread();
    ~RenderThread(); // waits for the frame in flight, then joins
    RenderThread(const RenderThread&) = delete;
    RenderThread& operator=(const RenderThread&) = delete;

    // Game thread, no frame in flight: runs `frame` on the render thread (started on first use, role Render). The
    // game half does not touch what the frame walks until WaitIdle returns.
    void Kick(std::function<void()> frame);
    // Game thread: waits for the frame in flight (none: returns at once), pumping the window's messages, and
    // rethrows what the frame threw. A WM_QUIT met while pumping is posted again for the game loop's own pump.
    void WaitIdle();
    bool InFlight() const noexcept { return inFlight_.load(std::memory_order_acquire); }
    // Waits for the frame in flight and joins the thread; the next Kick starts it again.
    void Stop();

private:
    void Main();
    static void Park();
    static bool InFlightForGame(); // threading::WaitRenderForGame's question: a frame in flight, asked on the game thread

    std::thread thread_;
    std::thread::id owner_; // the game thread: the only one that hands frames over and waits for them
    void* kickEvent_ = nullptr; // HANDLE, auto-reset: a frame (or the stop) is handed over
    void* doneEvent_ = nullptr; // HANDLE, manual-reset (Kick clears it): the frame handed over is done
    std::function<void()> frame_;
    std::exception_ptr error_;
    bool stop_ = false;
    std::atomic<bool> inFlight_{false}; // written by the game thread; a game-half task may ask (InFlightForGame)
    bool waiting_ = false;  // a wait reached again from a message handler waits without pumping
};
} // namespace threading
