#include "rendering/streaming/TextureStreamingIo.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "core/logging/Log.h"

namespace streaming {

TextureStreamingIo::~TextureStreamingIo()
{
    Stop();
}

void TextureStreamingIo::Start()
{
    if (thread_.joinable()) { return; }
    stop_ = false;
    thread_ = std::thread([this] { Run_(); });
}

void TextureStreamingIo::Stop()
{
    if (!thread_.joinable()) { return; }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
    // Whatever is still queued was never started: report it cancelled so the owner can free it.
    for (IoRequest* r : queue_) { r->state.store(static_cast<int>(IoRequest::State::Cancelled), std::memory_order_release); }
    queue_.clear();
}

void TextureStreamingIo::Submit(IoRequest* req)
{
    if (!req) { return; }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.push_back(req);
    }
    cv_.notify_one();
}

std::size_t TextureStreamingIo::Queued() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return queue_.size();
}

void* TextureStreamingIo::OpenCached_(const std::wstring& path)
{
    ++useTick_;
    auto it = files_.find(path);
    if (it != files_.end())
    {
        it->second.lastUse = useTick_;
        return it->second.handle;
    }
    if (files_.size() >= kMaxOpenFiles)
    {
        auto oldest = std::min_element(files_.begin(), files_.end(),
            [](const auto& a, const auto& b) { return a.second.lastUse < b.second.lastUse; });
        CloseHandle(static_cast<HANDLE>(oldest->second.handle));
        files_.erase(oldest);
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) { return h; }
    files_[path] = Cached{ h, useTick_ };
    return h;
}

void TextureStreamingIo::CloseAll_()
{
    for (auto& [path, c] : files_) { (void)path; CloseHandle(static_cast<HANDLE>(c.handle)); }
    files_.clear();
}

void TextureStreamingIo::Run_()
{
    std::vector<std::uint8_t> tight;
    for (;;)
    {
        IoRequest* req = nullptr;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (stop_) { break; }
            req = queue_.front();
            queue_.pop_front();
        }
        if (req->cancel.load(std::memory_order_acquire))
        {
            req->state.store(static_cast<int>(IoRequest::State::Cancelled), std::memory_order_release);
            continue;
        }
        req->state.store(static_cast<int>(IoRequest::State::Reading), std::memory_order_release);

        bool ok = false;
        HANDLE h = static_cast<HANDLE>(OpenCached_(req->path));
        if (h != INVALID_HANDLE_VALUE && req->fileBytes > 0 && req->ringBase)
        {
            tight.resize(static_cast<std::size_t>(req->fileBytes));
            LARGE_INTEGER pos{};
            pos.QuadPart = static_cast<LONGLONG>(req->fileOffset);
            ok = SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) != FALSE;
            std::uint64_t done = 0;
            while (ok && done < req->fileBytes)
            {
                const DWORD want = static_cast<DWORD>(std::min<std::uint64_t>(req->fileBytes - done, 64u << 20));
                DWORD got = 0;
                ok = ReadFile(h, tight.data() + done, want, &got, nullptr) != FALSE && got != 0;
                done += got;
            }
            if (ok)
            {
                for (const IoMipLayout& m : req->mips)
                {
                    const std::uint8_t* src = tight.data() + (m.fileOffset - req->fileOffset);
                    std::uint8_t* dst = req->ringBase + m.ringOffset;
                    for (std::uint32_t y = 0; y < m.rows; ++y)
                    {
                        std::memcpy(dst + static_cast<std::size_t>(y) * m.rowPitch,
                                    src + static_cast<std::size_t>(y) * m.rowBytes, m.rowBytes);
                    }
                }
                bytesRead_.fetch_add(req->fileBytes, std::memory_order_relaxed);
            }
        }
        if (!ok)
        {
            LOG_ERROR(logging::LogCategory::Asset, "texture stream-in read failed: {} ({} bytes at {})",
                      req->path, req->fileBytes, req->fileOffset);
        }
        req->state.store(static_cast<int>(ok ? IoRequest::State::Done : IoRequest::State::Failed),
                         std::memory_order_release);
    }
    CloseAll_();
}

} // namespace streaming
