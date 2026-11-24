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

#include "core/vfs/Vfs.h"

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
    // Refused: the file or the layout no longer matches the mip table the texture was registered with (architecture
    // review 10.5 p. 5) -- nothing was read into the ring, and the texture is taken off streaming.
    enum class State : int { Queued, Reading, Done, Failed, Cancelled, Refused };

    std::wstring path;
    std::uint64_t fileOffset = 0;      // the tight [first new mip, last new mip] range in the file
    std::uint64_t fileBytes = 0;
    std::uint64_t expectedFileSize = 0; // the whole file per its mip table (header + every mip), as at load
    std::vector<IoMipLayout> mips;
    std::uint8_t* ringBase = nullptr;
    std::uint64_t ringBegin = 0;       // the ring range this request owns: every row lands inside [begin, end)
    std::uint64_t ringEnd = 0;
    std::atomic<int> state{ static_cast<int>(State::Queued) };
    std::atomic<bool> cancel{ false };

    State GetState() const { return static_cast<State>(state.load(std::memory_order_acquire)); }
    bool Finished() const { const State s = GetState(); return s != State::Queued && s != State::Reading; }
};

// Plan A2.2: one worker thread reads mip ranges from DDS files straight into the upload ring
// (a tight read, then rows laid out at the D3D pitch). Cancel is honoured before the read starts;
// a read already running finishes and the requester discards it. Files stay open in a 64-entry LRU, read
// through the VFS at their offsets.
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

    // Empty when every row of `req` lies inside the `readBytes` read for it and inside its ring range, and the file is
    // the size its table says; otherwise why not. The worker refuses such a request instead of copying by it.
    static std::string CheckLayout(const IoRequest& req, std::uint64_t readBytes, std::uint64_t fileSize);

    // `--texstream-selftest=layout|size` (architecture review M14): the next stream-in request is built wrong -- its
    // last mip moved one row past what is read, or the file's expected size off by 4 KB (a file changed after the
    // texture was registered). The requester takes the arm (TakeSelfTest) once.
    enum class SelfTest : int { None, Layout, Size };
    static void ArmSelfTest(SelfTest arm) { selfTest_.store(static_cast<int>(arm), std::memory_order_release); }
    static SelfTest TakeSelfTest() { return static_cast<SelfTest>(selfTest_.exchange(0, std::memory_order_acq_rel)); }

private:
    static inline std::atomic<int> selfTest_{ 0 };
    static constexpr std::size_t kMaxOpenFiles = 64;
    void Run_();
    const vfs::File* OpenCached_(const std::wstring& path); // null when it cannot be opened
    void CloseAll_();

    std::thread thread_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<IoRequest*> queue_;
    bool stop_ = false;

    struct Cached { vfs::File file; std::uint64_t lastUse = 0; };
    std::unordered_map<std::wstring, Cached> files_; // worker thread only
    std::uint64_t useTick_ = 0;
    std::atomic<std::uint64_t> bytesRead_{ 0 };
};

} // namespace streaming
