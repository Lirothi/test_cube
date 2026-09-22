#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace streaming {

// One mip of a stream-in: where it sits in the file (tight rows) and where it lands in the ring
// (D3D placed-footprint rows, pitch a multiple of 256).
struct IoMipLayout
{
    std::uint64_t fileOffset = 0;
    std::uint64_t ringOffset = 0;
    std::uint32_t rows = 0;
    std::uint32_t rowBytes = 0;
    std::uint32_t rowPitch = 0;
};

// Owned by the requester; the worker only touches `state`, `cancel` and the ring bytes. Freed by
// the requester once Finished().
struct IoRequest
{
    enum class State : int { Queued, Reading, Done, Failed, Cancelled };

    std::wstring path;
    std::uint64_t fileOffset = 0;      // the tight [first new mip, last new mip] range in the file
    std::uint64_t fileBytes = 0;
    std::vector<IoMipLayout> mips;
    std::uint8_t* ringBase = nullptr;
    std::atomic<int> state{ static_cast<int>(State::Queued) };
    std::atomic<bool> cancel{ false };

    State GetState() const { return static_cast<State>(state.load(std::memory_order_acquire)); }
    bool Finished() const { const State s = GetState(); return s != State::Queued && s != State::Reading; }
};

// Plan A2.2: one worker thread reads mip ranges from DDS files straight into the upload ring
// (a tight read, then rows laid out at the D3D pitch). Cancel is honoured before the read starts;
// a read already running finishes and the requester discards it. Files stay open in a 64-entry LRU.
class TextureStreamingIo
{
public:
    ~TextureStreamingIo();
    void Start();
    void Stop();
    bool Running() const { return thread_.joinable(); }

    void Submit(IoRequest* req);
    std::size_t Queued() const;
    std::uint64_t BytesRead() const { return bytesRead_.load(std::memory_order_relaxed); }

private:
    static constexpr std::size_t kMaxOpenFiles = 64;
    void Run_();
    void* OpenCached_(const std::wstring& path); // HANDLE, or INVALID_HANDLE_VALUE
    void CloseAll_();

    std::thread thread_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<IoRequest*> queue_;
    bool stop_ = false;

    struct Cached { void* handle = nullptr; std::uint64_t lastUse = 0; };
    std::unordered_map<std::wstring, Cached> files_; // worker thread only
    std::uint64_t useTick_ = 0;
    std::atomic<std::uint64_t> bytesRead_{ 0 };
};

} // namespace streaming
