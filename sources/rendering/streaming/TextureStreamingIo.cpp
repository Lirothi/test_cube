#include "rendering/streaming/TextureStreamingIo.h"
#include "core/diagnostics/CrashHandler.h"
#include "core/memory/MemScope.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <format>

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

std::string TextureStreamingIo::CheckLayout(const IoRequest& req, std::uint64_t readBytes, std::uint64_t fileSize)
{
    if (req.expectedFileSize != 0 && fileSize != req.expectedFileSize)
    {
        return std::format("the file holds {} bytes, its mip table {} (it changed after the texture was loaded)",
                           fileSize, req.expectedFileSize);
    }
    if (req.fileOffset > fileSize || readBytes > fileSize - req.fileOffset)
    {
        return std::format("the read of {} bytes at {} runs past the file's {}", readBytes, req.fileOffset, fileSize);
    }
    if (req.ringBegin > req.ringEnd) { return std::format("its ring range [{}, {}) is reversed", req.ringBegin, req.ringEnd); }
    for (std::size_t i = 0; i < req.mips.size(); ++i)
    {
        const IoMipLayout& m = req.mips[i];
        if (m.rows == 0 || m.rowBytes == 0) { return std::format("new mip {} has no rows", i); }
        if (m.rowBytes > m.rowPitch)
        {
            return std::format("new mip {}: a row of {} bytes is wider than its ring pitch {}", i, m.rowBytes, m.rowPitch);
        }
        const std::uint64_t srcBytes = static_cast<std::uint64_t>(m.rows) * m.rowBytes;
        if (m.fileOffset < req.fileOffset || m.fileOffset - req.fileOffset > readBytes
            || srcBytes > readBytes - (m.fileOffset - req.fileOffset))
        {
            return std::format("new mip {}: its {} bytes at file offset {} lie outside the {} bytes read at {}", i, srcBytes,
                               m.fileOffset, readBytes, req.fileOffset);
        }
        const std::uint64_t dstBytes = static_cast<std::uint64_t>(m.rows - 1) * m.rowPitch + m.rowBytes;
        if (m.ringOffset < req.ringBegin || m.ringOffset > req.ringEnd || dstBytes > req.ringEnd - m.ringOffset)
        {
            return std::format("new mip {}: its rows land in [{}, {}), outside its ring range [{}, {})", i, m.ringOffset,
                               m.ringOffset + dstBytes, req.ringBegin, req.ringEnd);
        }
    }
    return {};
}

void TextureStreamingIo::Run_()
{
    MEM_SCOPE(MemTag::TexStream); // the IO thread's buffers (memory profiling M3)
    crash::InstallThisThread(); // std::terminate here reports as on the main thread; a stack overflow has room for the report
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
        std::string refused;
        const vfs::File* file = OpenCached_(req->path);
        if (file != nullptr && req->fileBytes > 0 && req->ringBase)
        {
            // Architecture review 10.5 p. 5: the layout comes from the mip table checked once, at load, and the file
            // may have changed since (a re-import with the editor open). Checked before the read: a request whose rows
            // would be taken from past the read or put past its ring range reads nothing.
            refused = CheckLayout(*req, req->fileBytes, file->Size());
        }
        if (!refused.empty())
        {
            LOG_ERROR(logging::LogCategory::Asset, "texture stream-in refused: {} -- {}; the texture is taken off streaming",
                      req->path, refused);
            req->state.store(static_cast<int>(IoRequest::State::Refused), std::memory_order_release);
            continue;
        }
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
