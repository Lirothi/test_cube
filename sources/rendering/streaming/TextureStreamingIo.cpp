#include "rendering/streaming/TextureStreamingIo.h"
#include "core/memory/MemScope.h"

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

const vfs::File* TextureStreamingIo::OpenCached_(const std::wstring& path)
{
    ++useTick_;
    auto it = files_.find(path);
    if (it != files_.end())
    {
        it->second.lastUse = useTick_;
        return &it->second.file;
    }
    if (files_.size() >= kMaxOpenFiles)
    {
        auto oldest = std::min_element(files_.begin(), files_.end(),
            [](const auto& a, const auto& b) { return a.second.lastUse < b.second.lastUse; });
        files_.erase(oldest);
    }
    vfs::File file = vfs::Open(path);
    if (!file) { return nullptr; }
    Cached& cached = files_[path];
    cached.file = std::move(file);
    cached.lastUse = useTick_;
    return &cached.file;
}

void TextureStreamingIo::CloseAll_()
{
    files_.clear();
}

void TextureStreamingIo::Run_()
{
    MEM_SCOPE(MemTag::TexStream); // the IO thread's buffers (memory profiling M3)
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
        const vfs::File* file = OpenCached_(req->path);
        if (file != nullptr && req->fileBytes > 0 && req->ringBase)
        {
            tight.resize(static_cast<std::size_t>(req->fileBytes));
            ok = file->Read(req->fileOffset, tight.data(), tight.size());
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
