#include "rendering/core/DeferredRelease.h"

#include "core/logging/Log.h"

#include <mutex>
#include <utility>
#include <vector>

namespace render::deferred
{
namespace
{
struct Entry
{
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    std::function<void()> release;
    std::uint64_t bytes = 0;
    std::uint64_t stamp = 0; // released once the GPU has finished this frame number
    const char* what = nullptr;
};

std::mutex g_mutex;
std::vector<Entry> g_waiting;
std::uint64_t g_frame = 0; // the frame number current at the last boundary
std::uint64_t g_bytes = 0;
std::uint64_t g_released = 0;
bool g_shutdown = false;

std::uint64_t ResourceBytes(ID3D12Resource* resource)
{
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (!resource || FAILED(resource->GetDevice(IID_PPV_ARGS(&device)))) { return 0; }
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    return device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
}

// Outside the lock: a release step may retire something else, or take a lock of its owner's.
void ReleaseNow(std::vector<Entry>& entries)
{
    for (Entry& e : entries)
    {
        if (e.release) { e.release(); }
        e.resource.Reset();
    }
    entries.clear();
}

void Add(Entry&& entry)
{
    std::vector<Entry> now;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_shutdown)
        {
            entry.stamp = g_frame + kMargin;
            g_bytes += entry.bytes;
            g_waiting.push_back(std::move(entry));
            return;
        }
        // Shutdown drained the queue with the GPU idle: nothing is in flight any more.
        ++g_released;
        now.push_back(std::move(entry));
    }
    ReleaseNow(now);
}
} // namespace

void Retire(Microsoft::WRL::ComPtr<ID3D12Resource> resource, const char* what)
{
    if (!resource) { return; }
    Entry e;
    e.bytes = ResourceBytes(resource.Get());
    e.resource = std::move(resource);
    e.what = what;
    Add(std::move(e));
}

void Retire(GpuResource&& resource, const char* what)
{
    if (!resource.Get()) { return; }
    const std::uint64_t bytes = ResourceBytes(resource.Get());
    // std::function copies its callable: the move-only resource rides in a shared_ptr.
    auto held = std::make_shared<GpuResource>(std::move(resource));
    Retire([held]() { held->Reset(); }, bytes, what);
}

void Retire(std::function<void()> release, std::uint64_t bytes, const char* what)
{
    if (!release) { return; }
    Entry e;
    e.release = std::move(release);
    e.bytes = bytes;
    e.what = what;
    Add(std::move(e));
}

void BeginFrame(std::uint64_t frameNumber, FrameCompleteFn isComplete, const void* ctx)
{
    // Kept with its capacity: texture slots retire every frame a mip fade moves (thousands a minute on wind_test), and a
    // local vector was an allocation per frame. The game thread's only (the boundary is never re-entered).
    static std::vector<Entry> ready;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_frame = frameNumber;
        // Entries from several threads are only roughly in stamp order: test each (a few dozen at most).
        for (Entry& e : g_waiting)
        {
            if (isComplete && isComplete(ctx, e.stamp))
            {
                g_bytes -= e.bytes;
                ready.push_back(std::move(e));
                e.stamp = 0; // marks the moved-from slot for the erase below
            }
        }
        std::erase_if(g_waiting, [](const Entry& e) { return e.stamp == 0; });
        g_released += ready.size();
    }
    ReleaseNow(ready);
}

void Shutdown()
{
    std::vector<Entry> all;
    std::uint64_t releasedBefore = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_shutdown = true;
        all.swap(g_waiting);
        g_bytes = 0;
        releasedBefore = g_released;
        g_released += all.size();
    }
    const std::size_t drained = all.size();
    ReleaseNow(all);
    LOG_INFO(logging::LogCategory::RenderRhi, "deferred release: {} released at their frames, {} drained at shutdown",
             releasedBefore, drained);
}

std::uint64_t WaitingBytes(const void*)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_bytes;
}

std::uint64_t WaitingCount(const void*)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_waiting.size();
}

void DrainIdle()
{
    std::vector<Entry> all;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        all.swap(g_waiting);
        g_bytes = 0;
        g_released += all.size();
    }
    ReleaseNow(all);
}

Stats GetStats()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    Stats s;
    s.waiting = g_waiting.size();
    s.waitingBytes = g_bytes;
    s.released = g_released;
    return s;
}
} // namespace render::deferred
