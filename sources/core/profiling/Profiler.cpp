#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerDetail.h"
#include "core/memory/MemScope.h"
#include <cwchar>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string_view>
#include <algorithm>
#include <cmath>
#include <memory>
#include <limits>
#include "third_party/robin_hood.h"
#include "core/logging/Log.h"
#include "core/threading/ThreadRole.h"
#include <Windows.h>

using namespace profiler_detail;

namespace {

std::string EscapeJson(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char c : input) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                std::ostringstream oss;
                oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)(unsigned char)c;
                out += oss.str();
            }
            else {
                out += c;
            }
            break;
        }
    }
    return out;
}

const Profiler::ScopeNameKey kTraceHandlingKey = Profiler::RegisterTraceLiteral(L"TraceHandling");
// The two GPU track indices (0, 1) and why the CPU threads start at 2: core/profiling/ProfilerDetail.h.
constexpr uint32_t kCpuTraceThreadIndexBase = 2u;       // CPU threads start after both GPU rows
constexpr const char* kGpuTraceThreadName = "GPU Graphics";       // the direct queue
constexpr const char* kGpuTraceThreadNameCompute = "GPU Compute";  // the async compute queue
// Built-in whole-CPU-frame counter, emitted automatically each EndFrame so no
// manual CPU_SCOPE is needed; spans the profiler's BeginFrame..EndFrame bracket,
// which includes Renderer::BeginFrame's fence wait.
const Profiler::ScopeNameKey kCpuFrameKey = Profiler::RegisterTraceLiteral(L"CPU.Frame");

std::wstring BuildWideName(const Profiler::ScopeNameKey& key) {
    if (!key.name || key.name->empty()) {
        return L"unknown";
    }
    return *key.name;
}

class TraceStringTable {
public:
    Profiler::ScopeNameKey RegisterLiteral(const wchar_t* name) {
#if PROF_ENABLED
        std::lock_guard<std::mutex> lock(mtx_);

        std::wstring literal = (name && *name) ? std::wstring(name) : std::wstring();
        if (literal.empty()) {
            literal = L"unknown";
        }

        auto valueIt = valueLookup_.find(literal);
        if (valueIt != valueLookup_.end()) {
            uint32_t idx = valueIt->second;
            if (idx < entries_.size() && entries_[idx]) {
                return entries_[idx]->key;
            }
        }

        const uint32_t index = static_cast<uint32_t>(entries_.size());
        auto entry = std::make_unique<Profiler::TraceNameEntry>();
        entry->displayName = std::move(literal);
        entry->key.name = &entry->displayName;
        entry->isDynamic = false;
        entry->dynamicInUse = false;
        auto* stored = entry.get();
        entries_.push_back(std::move(entry));
        valueLookup_.emplace(stored->displayName, index);
        keyLookup_.emplace(stored->key.name, index);
        return stored->key;
#else
        (void)name;
        return {};
#endif
    }

    Profiler::ScopeNameKey RegisterDynamic(std::wstring name, uint32_t* outIndex) {
#if PROF_ENABLED
        std::lock_guard<std::mutex> lock(mtx_);

        std::wstring display = std::move(name);
        if (display.empty()) {
            display = L"unknown";
        }

        uint32_t index = std::numeric_limits<uint32_t>::max();
        Profiler::TraceNameEntry* entry = nullptr;

        if (!dynamicFree_.empty()) {
            index = dynamicFree_.back();
            dynamicFree_.pop_back();
            if (index >= entries_.size()) {
                entries_.resize(index + 1);
            }
            if (!entries_[index]) {
                entries_[index] = std::make_unique<Profiler::TraceNameEntry>();
            }
            entry = entries_[index].get();
            entry->displayName = std::move(display);
            entry->isDynamic = true;
            entry->dynamicInUse = true;
            if (!entry->key.name) {
                entry->key.name = &entry->displayName;
            }
        }
        else {
            index = static_cast<uint32_t>(entries_.size());
            auto newEntry = std::make_unique<Profiler::TraceNameEntry>();
            newEntry->displayName = std::move(display);
            newEntry->key.name = &newEntry->displayName;
            newEntry->isDynamic = true;
            newEntry->dynamicInUse = true;
            entry = newEntry.get();
            entries_.push_back(std::move(newEntry));
        }

        keyLookup_[entry->key.name] = index;

        if (outIndex) {
            *outIndex = index;
        }
        captureDynamicKeys_.push_back(entry->key);
        return entry->key;
#else
        (void)name; (void)outIndex;
        return {};
#endif
    }

    void ReleaseDynamicKeys(const std::vector<Profiler::ScopeNameKey>& keys) {
#if PROF_ENABLED
        if (keys.empty()) { return; }
        std::lock_guard<std::mutex> lock(mtx_);
        for (const Profiler::ScopeNameKey& key : keys) {
            if (!key.name) { continue; }
            auto it = keyLookup_.find(key.name);
            if (it == keyLookup_.end()) { continue; }
            const uint32_t idx = it->second;
            if (idx >= entries_.size()) { continue; }
            auto* entry = entries_[idx].get();
            if (!entry || !entry->isDynamic || !entry->dynamicInUse) { continue; }
            entry->dynamicInUse = false;
            dynamicFree_.push_back(idx);
        }
#else
        (void)keys;
#endif
    }

    std::vector<Profiler::ScopeNameKey> TakeCaptureKeys() {
#if PROF_ENABLED
        std::lock_guard<std::mutex> lock(mtx_);
        std::vector<Profiler::ScopeNameKey> out;
        out.swap(captureDynamicKeys_);
        return out;
#else
        return {};
#endif
    }

private:
    std::mutex mtx_;
    robin_hood::unordered_flat_map<const std::wstring*, uint32_t> keyLookup_;
    robin_hood::unordered_flat_map<std::wstring, uint32_t> valueLookup_;
    std::vector<std::unique_ptr<Profiler::TraceNameEntry>> entries_;
    std::vector<uint32_t> dynamicFree_;
    std::vector<Profiler::ScopeNameKey> captureDynamicKeys_;
};

TraceStringTable& GetTraceStringTable() {
    static TraceStringTable table;
    return table;
}

std::string WideToUtf8(const std::wstring& input) {
    if (input.empty()) { return std::string(); }
    int bytes = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.c_str(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr );
    if (bytes <= 0) {
        return std::string();
    }
    std::string out(static_cast<size_t>(bytes), '\0');
    int written = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.c_str(), static_cast<int>(input.size()), out.data(), bytes, nullptr, nullptr);
    if (written != bytes) {
        return std::string();
    }
    return out;
}

uint64_t TraceOutputThreadIndex(const Profiler::TraceEvent& ev) {
    if (ev.category == Profiler::TraceEvent::Category::Gpu) {
        // Step 3: a GPU event carries its QUEUE index in threadIndex (0 direct / 1 compute), which
        // is already the row number. Previously this ignored threadIndex and forced everything onto
        // one row — the single-track blindness this step removes.
        return static_cast<uint64_t>(ev.threadIndex);
    }
    return static_cast<uint64_t>(ev.threadIndex) + kCpuTraceThreadIndexBase;
}

// A name WriteTraceJson makes: trace_YYYYMMDD_HHMMSS_<...>.json. A trace the owner renamed is never pruned.
bool IsWrittenTraceName(std::wstring_view name) {
    constexpr std::wstring_view prefix = L"trace_", suffix = L".json";
    if (name.size() < prefix.size() + 16 + suffix.size() || !name.starts_with(prefix) || !name.ends_with(suffix)) {
        return false;
    }
    const std::wstring_view stamp = name.substr(prefix.size(), 16); // YYYYMMDD_HHMMSS_
    for (size_t i = 0; i < stamp.size(); ++i) {
        const bool ok = (i == 8 || i == 15) ? stamp[i] == L'_' : (stamp[i] >= L'0' && stamp[i] <= L'9');
        if (!ok) { return false; }
    }
    return true;
}

// Keeps the newest `keepCount` written traces in `folder` by write time (the name breaks a tie: within one process
// it counts up); `written` is always kept. Architecture review 10.16 p. 4: traces/ grew without end.
void PruneOldTraces(const std::filesystem::path& folder, const std::filesystem::path& written, size_t keepCount) {
    namespace fs = std::filesystem;
    struct Item { fs::path path; fs::file_time_type time; };
    std::vector<Item> items;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code itemError;
        if (!it->is_regular_file(itemError) || !IsWrittenTraceName(it->path().filename().wstring())) { continue; }
        const fs::file_time_type time = it->last_write_time(itemError);
        if (!itemError) { items.push_back({ it->path(), time }); }
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return a.time != b.time ? a.time > b.time : a.path.filename() > b.path.filename();
    });
    size_t kept = 0, deleted = 0;
    for (const Item& item : items) {
        if (item.path.filename() == written.filename() || kept < keepCount) { ++kept; continue; }
        std::error_code removeError;
        if (fs::remove(item.path, removeError)) { ++deleted; }
        else {
            LOG_WARNING(logging::LogCategory::Profiling, "trace retention: cannot delete {} ({})",
                item.path.generic_string(), removeError.message());
        }
    }
    if (deleted != 0) {
        LOG_INFO(logging::LogCategory::Profiling, "trace retention: deleted {} older traces in {}, keeping the newest {}",
            deleted, folder.generic_string(), kept);
    }
}

} // namespace


#if PROF_ENABLED

Profiler::Profiler() {
    // Make the task system outlive this singleton even in a standalone harness.
    (void)TaskSystem::Get();
}

Profiler::~Profiler() {
    sampleEpoch_.store(0, std::memory_order_release);
    TaskSystem::Get().Wait(overlayTask_);
    TaskSystem::Get().Release(overlayTask_);
    StopTraceWriter();
}

Profiler::ScopeNameKey Profiler::RegisterTraceLiteral(const wchar_t* name) {
    return GetTraceStringTable().RegisterLiteral(name);
}

Profiler::ScopeNameKey Profiler::RegisterTraceDynamic(std::wstring name, uint32_t* outIndex) {
    return GetTraceStringTable().RegisterDynamic(std::move(name), outIndex);
}

void Profiler::ReleaseTraceNameKeys(const std::vector<ScopeNameKey>& keys) {
    GetTraceStringTable().ReleaseDynamicKeys(keys);
}

Profiler::ThreadSamples& Profiler::GetThreadSamples() {
    // The singleton owns buffers until producers have shut down. Thread exit does
    // not leave dangling registrations or reclaim a buffer under the collector.
    static thread_local ThreadSamples* local = nullptr;
    if (!local) {
        auto buffer = std::make_unique<ThreadSamples>();
        buffer->samples.reserve(256);
        buffer->drainedSamples.reserve(256);
        {
            std::lock_guard<std::mutex> lock(mtx_);
            buffer->threadIndex = GetThreadIndex_Locked(std::this_thread::get_id());
        }
        std::lock_guard<std::mutex> lock(buffersMutex_);
        local = buffer.get();
        threadSamples_.push_back(std::move(buffer));
    }
    return *local;
}

void Profiler::DrainThreadSamples(bool capture, uint64_t startUs) {
    std::lock_guard<std::mutex> registryLock(buffersMutex_);
    for (auto& buffer : threadSamples_) {
        {
            std::lock_guard<std::mutex> lock(buffer->mutex);
            buffer->samples.swap(buffer->drainedSamples);
            buffer->traces.swap(buffer->drainedTraces);
        }
        asyncCpuSamples_.insert(asyncCpuSamples_.end(), buffer->drainedSamples.begin(), buffer->drainedSamples.end());
        buffer->drainedSamples.clear();
        if (capture) {
            for (auto ev : buffer->drainedTraces) {
                if (ev.tsUs < startUs) {
                    // Begun before the capture (a MarkThreadCrossFrame thread's scope, the RHI thread's submit):
                    // keep the part inside it. Moved to 0 with its whole duration, its end overlapped the scope after
                    // it -- Perfetto's SLICE_SPILL_OVERLAPPING_COMPLETE_EVENT on Service3 / Service4 (2026-11-03).
                    const uint64_t endUs = ev.tsUs + ev.durUs;
                    if (endUs <= startUs) { continue; }
                    ev.durUs = endUs - startUs;
                    ev.tsUs = 0;
                } else {
                    ev.tsUs -= startUs;
                }
                if (TraceEventRoom_Locked(1) == 0) { continue; }
                traceEvents_.push_back(ev);
            }
        }
        buffer->drainedTraces.clear();
    }
}

Profiler& Profiler::Get() {
    static Profiler g;
    return g;
}

#if PROF_GPU_ENABLED
Profiler::ScopedGpu::ScopedGpu(ID3D12GraphicsCommandList* cl, ScopeNameKey key) : cl_(cl) {
    idx_ = Profiler::Get().BeginGpuSample(cl, key);
}
Profiler::ScopedGpu::~ScopedGpu() {
    Profiler::Get().EndGpuSample(cl_, idx_);
}
#endif

void Profiler::BeginFrame(uint64_t frameNo) {
    // R15.7 (docs/architecture_review_plan.md 10.16, item 1): the frame's boundaries are drawn while the render thread
    // draws nothing -- otherwise its scopes of the frame before land in this one, and the GPU samples swap under it.
    TC_CHECK_THREAD(!threading::RenderThreadInFlight(), "the render thread idle", "Profiler::BeginFrame");
    if (!GetEnabled()) { return; }
    std::lock_guard<std::mutex> lock(mtx_);
    frameNo_ = frameNo;
    frameOpen_ = true;
    frameCpuStart_ = CpuClock::now();
    if (traceCaptureRequested_ && !traceCapturing_ && !traceDraining_) {
        traceCaptureRequested_ = false;
        traceCapturing_ = true;
        traceFramesRemaining_ = traceRequestFrameCount_;
        traceFramesRecorded_ = 0;
        traceEvents_.clear();
        traceCounters_.clear();
        traceEventsDropped_ = 0;
        traceLimitReached_ = false;
        traceRecording_.store(true, std::memory_order_release);
        traceStartUs_ = ToMicroseconds(frameCpuStart_);
        traceFirstFrame_ = frameNo;
        for (UINT q = 0; q < kGpuQueueCount; ++q) { gpuTraceCalibrated_[q] = false; }
        traceLastFrame_ = UINT64_MAX;
        traceStopRequested_ = false;
    }
    traceCapturingAtomic_.store(traceCapturing_, std::memory_order_release);
    sampleEpoch_.store(++nextSampleEpoch_, std::memory_order_release);
#if PROF_GPU_ENABLED
    nextGpuQuery_.store(0, std::memory_order_relaxed);
    gpuFrameSampleIdx_ = SIZE_MAX;
    gpuRecordingReady_ = gpuInitialized_ && gpuResolvedBatches_[gpuRecordingReadbackSlot_].queryCount == 0;
#endif
}

void Profiler::EndFrame() {
    MEM_SCOPE(MemTag::Profiler); // memory profiling M3
    TC_CHECK_THREAD(!threading::RenderThreadInFlight(), "the render thread idle", "Profiler::EndFrame"); // R15.7
    if (!frameOpen_) { return; }
    auto& tasks = TaskSystem::Get();
    tasks.Wait(overlayTask_);
    tasks.Release(overlayTask_);
    asyncCpuSamples_.clear();
    asyncGpuSamples_.clear();
    asyncGpuSamples_.swap(gpuFrameSamples_);
    const auto frameEnd = CpuClock::now();
    sampleEpoch_.store(0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mtx_);
        DrainThreadSamples(traceCapturing_, traceStartUs_);
        if (traceCapturing_) {
            const uint64_t startUs = ToMicroseconds(frameCpuStart_);
            const uint64_t endUs = ToMicroseconds(frameEnd);
            const uint32_t threadIndex = GetThreadIndex_Locked(std::this_thread::get_id());
            if (TraceEventRoom_Locked(1) != 0) {
                TraceEvent frame;
                frame.key = RegisterTraceDynamic(L"Frame " + std::to_wstring(frameNo_));
                frame.tsUs = startUs - traceStartUs_;
                frame.durUs = endUs - startUs;
                frame.threadIndex = threadIndex;
                frame.frameNumber = frameNo_;
                frame.hasFrameNumber = true;
                traceEvents_.push_back(frame);
            }
            if (TraceEventRoom_Locked(1) != 0) {
                TraceEvent handling;
                handling.key = kTraceHandlingKey;
                handling.tsUs = endUs - traceStartUs_;
                handling.durUs = ToMicroseconds(CpuClock::now()) - endUs;
                handling.threadIndex = threadIndex;
                traceEvents_.push_back(handling);
            }
            ++traceFramesRecorded_;
            if (!traceOpenEnded_ && traceFramesRemaining_) { --traceFramesRemaining_; }
            if (traceStopRequested_ || (!traceOpenEnded_ && traceFramesRemaining_ == 0)) {
                traceCapturing_ = false;
                traceRecording_.store(false, std::memory_order_release);
                traceDraining_ = true;
                traceLastFrame_ = frameNo_;
                traceStopRequested_ = false;
            }
        }
        frameOpen_ = false;
        traceCapturingAtomic_.store(false, std::memory_order_release);
    }
    asyncCpuSamples_.push_back({ kCpuFrameKey,
        std::chrono::duration<double, std::milli>(frameEnd - frameCpuStart_).count(), frameNo_ });
#if PROF_GPU_ENABLED
    const UINT queryCount = (std::min)(nextGpuQuery_.load(std::memory_order_relaxed), maxGpuQueries_);
    if (gpuRecordingReady_ && queryCount) {
        auto& batch = gpuResolvedBatches_[gpuRecordingReadbackSlot_];
        batch.ranges.assign(gpuPending_.begin(), gpuPending_.begin() + queryCount / 2);
        batch.queryCount = queryCount;
        batch.frameNo = frameNo_;
        batch.fenceValue = gpuDrainFenceValue_++;
        for (UINT q = 0; q < kGpuQueueCount; ++q) {
            if (!gpuQueue_[q]) { continue; }
            const bool signalled = queueSignalHook_
                ? queueSignalHook_(gpuQueue_[q], gpuDrainFence_[q].Get(), batch.fenceValue) // G10: the queues' road
                : SUCCEEDED(gpuQueue_[q]->Signal(gpuDrainFence_[q].Get(), batch.fenceValue));
            if (!signalled) {
                LOG_ERROR(logging::LogCategory::Profiling, "GPU profiler fence signal failed on queue {}", q);
                batch.fenceValue = UINT64_MAX; // never reuse a slot whose completion is unknown
                break;
            }
        }
        gpuRecordingReadbackSlot_ = (gpuRecordingReadbackSlot_ + 1) % kGpuReadbackFrameSlots;
    }
    gpuRecordingReady_ = false;
#endif
    FinishTraceIfReady();

    auto overlayJob = [this]() {
        const auto t0 = CoolClock::now();
        const double cooldown = maxResetIntervalSec_.load(std::memory_order_relaxed);
        if (resetMaxRequested_.exchange(false, std::memory_order_acq_rel) ||
            std::chrono::duration<double>(t0 - lastMaxReset_).count() >= cooldown) {
            ResetMax_Unsafe();
            lastMaxReset_ = t0;
        }
        auto accumulate = [this](const auto& samples, auto& stats, auto& order, uint32_t& nextId, bool gpu) {
            auto commit = [&]() { for (auto& kv : stats) { kv.second.stats.CommitFrame(emaAlpha_); } };
            uint64_t frame = samples.empty() ? 0 : samples.front().frameNumber;
            for (const auto& sample : samples) {
                if (gpu && sample.frameNumber != frame) {
                    commit();
                    frame = sample.frameNumber;
                }
                auto [it, inserted] = stats.try_emplace(sample.key);
                if (inserted) {
                    it->second.wideName = BuildWideName(sample.key);
                    it->second.overlayId = nextId++;
                    order.push_back(sample.key);
                }
                it->second.stats.Accumulate(sample.ms);
            }
            if (!gpu || !samples.empty()) { commit(); }
        };
        accumulate(asyncCpuSamples_, stats_, overlayOrder_, nextOverlayId_, false);
#if PROF_GPU_ENABLED
        accumulate(asyncGpuSamples_, gpuStats_, gpuOverlayOrder_, nextGpuOverlayId_, true);
#endif
        const bool resort = std::chrono::duration<double>(t0 - lastOverlaySort_).count() >=
            overlayResortIntervalSec_.load(std::memory_order_relaxed);
        auto sort = [&](auto& order, const auto& stats) {
            if (!resort) { return; }
            std::sort(order.begin(), order.end(), [&](ScopeNameKey a, ScopeNameKey b) {
                const auto& x = stats.at(a);
                const auto& y = stats.at(b);
                if ((x.stats.lastCount == 0) != (y.stats.lastCount == 0)) { return x.stats.lastCount != 0; }
                if (x.stats.avgMs != y.stats.avgMs) { return x.stats.avgMs > y.stats.avgMs; }
                return x.overlayId < y.overlayId;
            });
        };
        sort(overlayOrder_, stats_);
#if PROF_GPU_ENABLED
        sort(gpuOverlayOrder_, gpuStats_);
#endif
        if (resort) { lastOverlaySort_ = t0; }
        auto publish = [](auto& rows, const auto& order, const auto& stats, size_t offset) {
            rows.resize(order.size() + offset);
            for (size_t i = 0; i < order.size(); ++i) {
                const auto& entry = stats.at(order[i]);
                auto& row = rows[i + offset];
                if (row.entryId != entry.overlayId) {
                    row.entryId = entry.overlayId;
                    row.name = entry.wideName;
                }
                row.avgMs = entry.stats.avgMs;
                row.maxMs = entry.stats.maxMs;
                row.lastMs = entry.stats.lastMs;
                row.usages = entry.stats.lastCount;
            }
        };
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        publish(overlayRows_, overlayOrder_, stats_, 1);
#if PROF_GPU_ENABLED
        publish(gpuOverlayRows_, gpuOverlayOrder_, gpuStats_, 0);
#endif
        const double dt = std::chrono::duration<double, std::milli>(CoolClock::now() - t0).count();
        endFrameAsyncAvgMs_ = endFrameAsyncAvgMs_ <= 0 ? dt : endFrameAsyncAvgMs_ * emaAlpha_ + dt * (1 - emaAlpha_);
        endFrameAsyncMaxMs_ = (std::max)(endFrameAsyncMaxMs_, dt);
        auto& self = overlayRows_.front();
        if (self.name.empty()) { self.name = L"Profiler::EndFrame.Async"; }
        self.avgMs = endFrameAsyncAvgMs_;
        self.maxMs = endFrameAsyncMaxMs_;
        self.usages = 1;
    };
#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
    overlayTask_ = tasks.Submit(std::move(overlayJob));
#else
    overlayJob();
#endif
}

void Profiler::PushSample(const ScopeNameKey& key, CpuClock::time_point start, CpuClock::time_point end, uint64_t epoch) {
    const bool anyEpoch = epoch == kAnyEpoch; // MarkThreadCrossFrame: the frame that drains it takes it
    if (!GetEnabled() || (!anyEpoch && sampleEpoch_.load(std::memory_order_acquire) != epoch)) { return; }
    auto& buffer = GetThreadSamples();
    std::lock_guard<std::mutex> lock(buffer.mutex);
    // The frame may have closed while the producer was finding/locking its buffer.
    if (!anyEpoch && sampleEpoch_.load(std::memory_order_acquire) != epoch) { return; }
    buffer.samples.push_back({ key, std::chrono::duration<double, std::milli>(end - start).count() });
    // A thread that lies across the frames may end a scope between two of them (the physics step, in the wait for the GPU): the capture is open, the frame is not.
    if (anyEpoch ? traceRecording_.load(std::memory_order_acquire) : traceCapturingAtomic_.load(std::memory_order_acquire)) {
        TraceEvent ev;
        ev.key = key;
        ev.tsUs = ToMicroseconds(start);
        ev.durUs = ToMicroseconds(end) - ev.tsUs;
        ev.threadIndex = buffer.threadIndex;
        buffer.traces.push_back(ev);
    }
}

std::vector<Profiler::OverlaySample> Profiler::OverlaySnapshot() const {
    std::vector<OverlaySample> out;
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    out.reserve(overlayRows_.size() + gpuOverlayRows_.size());
    auto addRows = [&](const auto& rows, bool gpu) {
        for (const auto& row : rows) {
            out.push_back({ WideToUtf8(row.name), row.avgMs, row.maxMs, row.usages, gpu, row.lastMs });
        }
    };
    addRows(overlayRows_, false);
    addRows(gpuOverlayRows_, true);
    return out;
}

bool Profiler::DumpOverlay(const std::string& path) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { return false; }
    const std::vector<OverlaySample> rows = OverlaySnapshot();
    for (const bool gpu : { false, true }) {
        f << (gpu ? "[GPU]" : "[CPU]") << " frame=" << frameNo_ << "\n";
        for (const auto& r : rows) {
            if (r.gpu != gpu) { continue; }
            char line[256];
            std::snprintf(line, sizeof(line), "%-36s avg:%8.3f max:%8.3f usages:%u\n",
                r.name.c_str(), r.avgMs, r.maxMs, r.usages);
            f << line;
        }
    }
    return f.good();
}

void Profiler::SetMaxCooldownSeconds(double sec) {
    maxResetIntervalSec_.store((std::max)(0.1, sec), std::memory_order_relaxed);
}

double Profiler::GetMaxCooldownSeconds() const {
    return maxResetIntervalSec_.load(std::memory_order_relaxed);
}

void Profiler::ResetMaxNow() {
    resetMaxRequested_.store(true, std::memory_order_release);
}

void Profiler::ResetMax_Unsafe() {
    for (auto& kv : stats_) { kv.second.stats.maxMs = 0.0; }
#if PROF_GPU_ENABLED
    for (auto& kv : gpuStats_) { kv.second.stats.maxMs = 0.0; }
#endif
    endFrameAsyncMaxMs_ = 0.0;
}

size_t Profiler::TraceEventRoom_Locked(size_t count) {
    const size_t held = traceEvents_.size() + traceCounters_.size();
    const size_t room = held < traceEventLimit_ ? (std::min)(count, traceEventLimit_ - held) : 0;
    if (room == count) { return room; }
    traceEventsDropped_ += count - room;
    if (!traceLimitReached_) {
        traceLimitReached_ = true;
        if (traceCapturing_) { traceStopRequested_ = true; } // ends at this frame's close, as Stop would
        LOG_WARNING(logging::LogCategory::Profiling,
            "trace capture: the recording reached its limit of {} events at frame {} -- stopped there; the file keeps "
            "what was recorded", traceEventLimit_, frameNo_);
    }
    return room;
}

void Profiler::FinishTraceIfReady() {
    TraceDumpData dump;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!traceDraining_) { return; }
#if PROF_GPU_ENABLED
        for (const auto& batch : gpuResolvedBatches_) {
            if (batch.queryCount && batch.frameNo >= traceFirstFrame_ && batch.frameNo <= traceLastFrame_) { return; }
        }
#endif
        dump.events.swap(traceEvents_);
        dump.counters.swap(traceCounters_);
        dump.dynamicKeys = GetTraceStringTable().TakeCaptureKeys();
        dump.threadNames = threadIndexToName_;
        dump.droppedEvents = traceEventsDropped_;
        traceDraining_ = false;
        traceStartUs_ = 0;
        traceRequestFrameCount_ = traceFramesRemaining_ = 0;
        traceOpenEnded_ = false;
    }
    QueueTraceWrite(std::move(dump));
}

void Profiler::QueueTraceWrite(TraceDumpData dump) {
    std::lock_guard<std::mutex> lock(traceWriterMutex_);
    traceWriteQueue_.push_back(std::move(dump));
    if (!traceWriter_.joinable()) {
        traceWriterStop_ = false;
        traceWriter_ = std::thread([this]() {
            MEM_SCOPE(MemTag::Profiler); // memory profiling M3
            logging::SetCurrentThreadName("ProfilerTraceWriter");
            for (;;) {
                TraceDumpData next;
                {
                    std::unique_lock<std::mutex> lock(traceWriterMutex_);
                    traceWriterCv_.wait(lock, [this]() { return traceWriterStop_ || !traceWriteQueue_.empty(); });
                    if (traceWriteQueue_.empty()) { return; }
                    next = std::move(traceWriteQueue_.front());
                    traceWriteQueue_.pop_front();
                }
                try { WriteTraceJson(next); }
                catch (const std::exception& e) {
                    LOG_ERROR(logging::LogCategory::Profiling, "trace capture export failed: {}", e.what());
                }
                ReleaseTraceNameKeys(next.dynamicKeys);
            }
        });
    }
    traceWriterCv_.notify_one();
}

void Profiler::StopTraceWriter() {
    {
        std::lock_guard<std::mutex> lock(traceWriterMutex_);
        traceWriterStop_ = true;
    }
    traceWriterCv_.notify_all();
    if (traceWriter_.joinable()) { traceWriter_.join(); }
}

void Profiler::RequestTraceCapture(uint32_t frameCount) {
    if (frameCount == 0) { return; }
    std::lock_guard<std::mutex> lk(mtx_);
    if (traceDraining_) { return; }
    if (traceCapturing_) {
        traceStopRequested_ = true;
        LOG_INFO(logging::LogCategory::Profiling, "trace capture stop requested");
        return;
    }
    if (traceCaptureRequested_) {
        traceCaptureRequested_ = false;
        traceRequestFrameCount_ = 0;
        LOG_INFO(logging::LogCategory::Profiling, "trace capture canceled");
        return;
    }
    traceCaptureRequested_ = true;
    traceRequestFrameCount_ = frameCount;
    traceOpenEnded_ = false;
    traceStopRequested_ = false;
    LOG_INFO(logging::LogCategory::Profiling, "trace capture requested: {} frames", frameCount);
}

void Profiler::BeginTraceCapture() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (traceCapturing_ || traceCaptureRequested_ || traceDraining_) { return; }
    traceCaptureRequested_ = true;
    traceRequestFrameCount_ = 1; // the start path requires a non-zero budget; openEnded ignores it
    traceOpenEnded_ = true;
    traceStopRequested_ = false;
}

void Profiler::StopTraceCapture() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (traceCapturing_) {
        traceStopRequested_ = true;
        return;
    }
    // Not started yet: cancel the pending request instead of leaving it armed.
    traceCaptureRequested_ = false;
    traceRequestFrameCount_ = 0;
    traceOpenEnded_ = false;
}

bool Profiler::IsTraceCapturing() const {
    return traceRecording_.load(std::memory_order_acquire);
}

void Profiler::RecordTraceCounters(const char* track, const char* const* names, const double* values, int count) {
    if (track == nullptr || names == nullptr || values == nullptr || count <= 0) { return; }
    TraceCounter c;
    c.track = track;
    c.count = count < kMaxTraceCounterSeries ? count : kMaxTraceCounterSeries;
    for (int i = 0; i < c.count; ++i) {
        c.names[i] = names[i];
        c.values[i] = values[i];
    }
    const uint64_t nowUs = ToMicroseconds(CpuClock::now());
    std::lock_guard<std::mutex> lk(mtx_);
    if (!traceCapturing_) { return; }
    if (TraceEventRoom_Locked(1) == 0) { return; }
    c.tsUs = nowUs > traceStartUs_ ? nowUs - traceStartUs_ : 0;
    traceCounters_.push_back(c);
}

Profiler::TraceCaptureStatus Profiler::GetTraceCaptureStatus() const {
    TraceCaptureStatus s;
    std::lock_guard<std::mutex> lk(mtx_);
    s.active = traceCapturing_;
    s.pending = traceCaptureRequested_ || traceDraining_;
    s.openEnded = traceOpenEnded_;
    s.framesRemaining = traceFramesRemaining_;
    s.framesRecorded = traceFramesRecorded_;
    s.events = traceEvents_.size();
    s.droppedEvents = traceEventsDropped_;
    s.lastPath = traceLastPath_;
    return s;
}

void Profiler::SetThreadName(const std::string& name) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto id = std::this_thread::get_id();
    threadNames_[id] = name;
    auto it = threadIndices_.find(id);
    if (it != threadIndices_.end()) {
        const uint32_t idx = it->second;
        if (threadIndexToName_.size() <= idx) { threadIndexToName_.resize(idx + 1); }
        threadIndexToName_[idx] = name;
    }
}

uint32_t Profiler::GetThreadIndex_Locked(std::thread::id id) {
    auto it = threadIndices_.find(id);
    if (it != threadIndices_.end()) {
        return it->second;
    }
    const uint32_t idx = static_cast<uint32_t>(threadIndices_.size());
    threadIndices_[id] = idx;
    if (threadIndexToName_.size() <= idx) { threadIndexToName_.resize(idx + 1); }
    auto nameIt = threadNames_.find(id);
    if (nameIt == threadNames_.end()) {
        std::string autoName = "Thread " + std::to_string(idx);
        threadNames_[id] = autoName;
        threadIndexToName_[idx] = std::move(autoName);
    }
    else {
        threadIndexToName_[idx] = nameIt->second;
    }
    return idx;
}

void Profiler::WriteTraceJson(const TraceDumpData& dump) {
    if (dump.events.empty()) { return; }
    const uint32_t fileIdx = traceFileCounter_.fetch_add(1u, std::memory_order_relaxed);
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &tt);
    std::ostringstream fname;
#if defined(NDEBUG)
    constexpr const char* suffix = "_release";
#else
    constexpr const char* suffix = "_debug";
#endif
    fname << "traces/trace_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << suffix
          << "_" << GetCurrentProcessId() << "_" << std::setw(3) << std::setfill('0') << fileIdx << ".json";
    const std::string fileName = fname.str();
    std::filesystem::create_directory("traces");
    std::ofstream out(fileName, std::ios::binary);
    if (!out) {
        LOG_ERROR(logging::LogCategory::Profiling, "trace capture FAILED: cannot write {}", fileName);
        return;
    }
    // Rows in the order they are read (owner, 2026-11-03): GPU graphics, GPU compute, the main thread, the render thread
    // (game thread plan G8), the RHI thread (G10b), then every other thread in the order it first recorded. The tid follows that order -- the
    // order every viewer falls back to -- and each row says it again as thread_sort_index (chrome://tracing, Perfetto).
    std::vector<uint64_t> cpuTid(dump.threadNames.size(), 0u);
    {
        uint64_t next = kCpuTraceThreadIndexBase;
        const auto place = [&](const char* name) {
            for (size_t i = 0; i < dump.threadNames.size(); ++i) {
                if (cpuTid[i] == 0u && dump.threadNames[i] == name) { cpuTid[i] = next++; }
            }
        };
        place("MainThread");
        place("RenderThread");
        place("RHIThread"); // G10b
        for (uint64_t& tid : cpuTid) {
            if (tid == 0u) { tid = next++; }
        }
    }
    const auto outputTid = [&](const TraceEvent& ev) -> uint64_t {
        if (ev.category == TraceEvent::Category::Gpu) { return TraceOutputThreadIndex(ev); }
        // A thread the name table does not know goes after every one it does.
        return ev.threadIndex < cpuTid.size() ? cpuTid[ev.threadIndex]
                                              : TraceOutputThreadIndex(ev) + cpuTid.size();
    };
    out << "{\n\"traceEvents\":[\n";
    bool first = true;
    auto separator = [&]() { if (!first) { out << ",\n"; } first = false; };
    auto threadName = [&](uint64_t tid, const std::string& name) {
        separator();
        out << "  {\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":0,\"tid\":" << tid
            << ",\"args\":{\"name\":\"" << EscapeJson(name) << "\"}}";
        separator();
        out << "  {\"name\":\"thread_sort_index\",\"ph\":\"M\",\"pid\":0,\"tid\":" << tid
            << ",\"args\":{\"sort_index\":" << tid << "}}";
    };
#if PROF_GPU_ENABLED
    threadName(kGpuTraceThreadIndex, kGpuTraceThreadName);
    threadName(kGpuTraceThreadIndexCompute, kGpuTraceThreadNameCompute);
#endif
    for (size_t i = 0; i < dump.threadNames.size(); ++i) {
        if (!dump.threadNames[i].empty()) { threadName(cpuTid[i], dump.threadNames[i]); }
    }
    // Cache escaped names once per distinct name, not once per event; do not
    // reserve an events-sized hash table for a capture with millions of events.
    robin_hood::unordered_flat_map<const std::wstring*, std::string> names;
    names.reserve(256);
    for (const auto& ev : dump.events) {
        auto [it, inserted] = names.try_emplace(ev.key.name);
        if (inserted) { it->second = EscapeJson(ev.key.name ? WideToUtf8(*ev.key.name) : "unknown"); }
        separator();
        out << "  {\"name\":\"" << it->second << "\",\"cat\":\""
            << (ev.category == TraceEvent::Category::Gpu ? "GPU" : "CPU")
            << "\",\"ph\":\"X\",\"ts\":" << ev.tsUs << ",\"dur\":" << ev.durUs
            << ",\"pid\":0,\"tid\":" << outputTid(ev);
        if (ev.hasFrameNumber) { out << ",\"args\":{\"frame\":" << ev.frameNumber << "}"; }
        out << "}";
    }
    // Counter tracks: one event per sample, every series of the track in its args.
    for (const TraceCounter& c : dump.counters) {
        separator();
        out << "  {\"name\":\"" << EscapeJson(c.track) << "\",\"ph\":\"C\",\"ts\":" << c.tsUs
            << ",\"pid\":0,\"tid\":0,\"args\":{";
        for (int i = 0; i < c.count; ++i) {
            if (i) { out << ","; }
            out << "\"" << EscapeJson(c.names[i] ? c.names[i] : "?") << "\":" << c.values[i];
        }
        out << "}}";
    }
    out << "\n],\n\"displayTimeUnit\":\"us\"\n}\n";
    out.close();
    if (!out) {
        LOG_ERROR(logging::LogCategory::Profiling, "trace capture FAILED: incomplete write {}", fileName);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mtx_);
        traceLastPath_ = fileName;
    }
    if (dump.droppedEvents != 0) {
        LOG_INFO(logging::LogCategory::Profiling, "trace saved: {} ({} events; {} past the limit dropped)", fileName,
            dump.events.size(), dump.droppedEvents);
    } else {
        LOG_INFO(logging::LogCategory::Profiling, "trace saved: {} ({} events)", fileName, dump.events.size());
    }
    PruneOldTraces("traces", fileName, kTraceFilesKept);
}

#endif // PROF_ENABLED
