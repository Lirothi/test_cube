#include "core/profiling/Profiler.h"
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
#include "rendering/core/GpuAlloc.h"
#include <Windows.h>

namespace {

uint64_t ToMicroseconds(Profiler::CpuClock::time_point tp) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count());
}

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
const Profiler::ScopeNameKey kGpuFrameKey = Profiler::RegisterTraceLiteral(L"GPU.Frame");
// Async-compute plan step 3: TWO GPU tracks. In a single-track trace an overlap is
// indistinguishable from a reordering, so this has to exist before any pass moves — otherwise
// there is no way to see whether async compute is doing anything at all.
//
// The GPU rows take tids 0 and 1 and every CPU thread shifts to +2, so the two GPU tracks stay
// adjacent at the top of the viewer. Trace-reading scripts that hard-coded `tid == 0` for "the GPU"
// must now read 0 AND 1.
constexpr uint32_t kGpuTraceThreadIndex = 0u;           // direct queue
constexpr uint32_t kGpuTraceThreadIndexCompute = 1u;    // async compute queue
constexpr uint32_t kCpuTraceThreadIndexBase = 2u;       // CPU threads start after both GPU rows
constexpr const char* kGpuTraceThreadName = "GPU Queue";
constexpr const char* kGpuTraceThreadNameCompute = "GPU Queue (async compute)";
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
                ev.tsUs = ev.tsUs >= startUs ? ev.tsUs - startUs : 0;
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
        traceRecording_.store(true, std::memory_order_release);
        traceStartUs_ = ToMicroseconds(frameCpuStart_);
        traceFirstFrame_ = frameNo;
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
            TraceEvent frame;
            frame.key = RegisterTraceDynamic(L"Frame " + std::to_wstring(frameNo_));
            frame.tsUs = startUs - traceStartUs_;
            frame.durUs = endUs - startUs;
            frame.threadIndex = GetThreadIndex_Locked(std::this_thread::get_id());
            frame.frameNumber = frameNo_;
            frame.hasFrameNumber = true;
            traceEvents_.push_back(frame);
            TraceEvent handling;
            handling.key = kTraceHandlingKey;
            handling.tsUs = endUs - traceStartUs_;
            handling.durUs = ToMicroseconds(CpuClock::now()) - endUs;
            handling.threadIndex = frame.threadIndex;
            traceEvents_.push_back(handling);
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
            if (gpuQueue_[q] && FAILED(gpuQueue_[q]->Signal(gpuDrainFence_[q].Get(), batch.fenceValue))) {
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
    if (!GetEnabled() || sampleEpoch_.load(std::memory_order_acquire) != epoch) { return; }
    auto& buffer = GetThreadSamples();
    std::lock_guard<std::mutex> lock(buffer.mutex);
    // The frame may have closed while the producer was finding/locking its buffer.
    if (sampleEpoch_.load(std::memory_order_acquire) != epoch) { return; }
    buffer.samples.push_back({ key, std::chrono::duration<double, std::milli>(end - start).count() });
    if (traceCapturingAtomic_.load(std::memory_order_acquire)) {
        TraceEvent ev;
        ev.key = key;
        ev.tsUs = ToMicroseconds(start);
        ev.durUs = ToMicroseconds(end) - ev.tsUs;
        ev.threadIndex = buffer.threadIndex;
        buffer.traces.push_back(ev);
    }
}

#if PROF_GPU_ENABLED
bool Profiler::CaptureGpuTraceCalibration(UINT queueIndex, GpuTraceCalibration& calibration) const {
    if (queueIndex >= kGpuQueueCount) { return false; }
    if (!gpuInitialized_ || !gpuQueue_[queueIndex] || gpuFreq_[queueIndex] == 0 || gpuTraceQpcFreq_ == 0) {
        return false;
    }
    UINT64 gpuTimestamp = 0;
    UINT64 cpuQpc = 0;
    if (FAILED(gpuQueue_[queueIndex]->GetClockCalibration(&gpuTimestamp, &cpuQpc))) {
        return false;
    }
    calibration.gpuTimestamp = gpuTimestamp;
    calibration.cpuQpc = cpuQpc;
    return true;
}

uint64_t Profiler::GpuTimestampToCpuUs(UINT64 gpuTimestamp, const GpuTraceCalibration& calibration,
                                       UINT queueIndex) const {
    if (queueIndex >= kGpuQueueCount || gpuFreq_[queueIndex] == 0 || gpuTraceQpcFreq_ == 0) {
        return 0;
    }

    const double gpuDelta = static_cast<double>(gpuTimestamp) - static_cast<double>(calibration.gpuTimestamp);
    const double cpuQpc =
        static_cast<double>(calibration.cpuQpc) +
        gpuDelta * static_cast<double>(gpuTraceQpcFreq_) / static_cast<double>(gpuFreq_[queueIndex]);
    const double cpuUs =
        static_cast<double>(gpuTraceCpuOriginUs_) +
        (cpuQpc - static_cast<double>(gpuTraceQpcOrigin_)) * 1000000.0 / static_cast<double>(gpuTraceQpcFreq_);
    if (cpuUs <= 0.0) {
        return 0;
    }
    return static_cast<uint64_t>(std::llround(cpuUs));
}

void Profiler::CollectGpuResolvedSamples(std::vector<ScopeSample>& sampleOut,
    std::vector<TraceEvent>* traceOut, uint64_t traceStartUs) {
    if (!gpuInitialized_) { return; }
    UINT64 completed = UINT64_MAX;
    for (UINT q = 0; q < kGpuQueueCount; ++q) {
        if (gpuDrainFence_[q]) { completed = (std::min)(completed, gpuDrainFence_[q]->GetCompletedValue()); }
    }
    std::array<UINT, kGpuReadbackFrameSlots> ready{};
    size_t count = 0;
    for (UINT i = 0; i < kGpuReadbackFrameSlots; ++i) {
        const auto& batch = gpuResolvedBatches_[i];
        if (batch.queryCount && batch.fenceValue != UINT64_MAX && completed >= batch.fenceValue) { ready[count++] = i; }
    }
    if (!count) { return; }
    std::sort(ready.begin(), ready.begin() + count, [this](UINT a, UINT b) {
        return gpuResolvedBatches_[a].fenceValue < gpuResolvedBatches_[b].fenceValue;
    });
    GpuTraceCalibration calibration[kGpuQueueCount]{};
    bool calibrated[kGpuQueueCount] = {};
    // Durations need frequency only; calibration is solely for captured events.
    if (traceOut && traceStartUs) {
        for (UINT q = 0; q < kGpuQueueCount; ++q) { calibrated[q] = CaptureGpuTraceCalibration(q, calibration[q]); }
    }
    for (size_t i = 0; i < count; ++i) {
        const UINT slot = ready[i];
        auto& batch = gpuResolvedBatches_[slot];
        const size_t base = static_cast<size_t>(slot) * maxGpuQueries_;
        for (const auto& range : batch.ranges) {
            const UINT q = range.queueIndex;
            if (!range.completed || q >= kGpuQueueCount || !gpuMappedReadback_[q] || !gpuFreq_[q]) { continue; }
            const UINT64 a = gpuMappedReadback_[q][base + range.start];
            const UINT64 b = gpuMappedReadback_[q][base + range.end];
            if (b < a) { continue; }
            sampleOut.push_back({ range.key, static_cast<double>(b - a) * 1000.0 / gpuFreq_[q], range.frameNo });
            if (!traceOut || !calibrated[q] || range.frameNo < traceFirstFrame_ || range.frameNo > traceLastFrame_) { continue; }
            const uint64_t start = GpuTimestampToCpuUs(a, calibration[q], q);
            const uint64_t end = GpuTimestampToCpuUs(b, calibration[q], q);
            if (end <= traceStartUs || end < start) { continue; }
            TraceEvent ev;
            ev.key = range.key;
            if (range.key.name == kGpuFrameKey.name) {
                ev.key = RegisterTraceDynamic(L"GPU.Frame " + std::to_wstring(range.frameNo));
            }
            ev.frameNumber = range.frameNo;
            ev.hasFrameNumber = true;
            ev.tsUs = (std::max)(start, traceStartUs) - traceStartUs;
            ev.durUs = end - (std::max)(start, traceStartUs);
            ev.threadIndex = q == 0 ? kGpuTraceThreadIndex : kGpuTraceThreadIndexCompute;
            ev.category = TraceEvent::Category::Gpu;
            traceOut->push_back(ev);
        }
        batch.ranges.clear(); // retain per-slot capacity
        batch.queryCount = 0;
        batch.fenceValue = 0;
    }
}

void Profiler::InitGpu(ID3D12Device* device, ID3D12CommandQueue* queue,
    ID3D12CommandQueue* computeQueue, UINT maxQueries) {
    if (!device || !queue || maxQueries < 2) { return; }
    maxGpuQueries_ = maxQueries & ~1u;
    gpuQueue_[0] = queue;
    gpuQueue_[1] = computeQueue;
    D3D12_QUERY_HEAP_DESC queryDesc{};
    queryDesc.Count = maxGpuQueries_;
    queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC resource{};
    resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource.Width = static_cast<UINT64>(maxGpuQueries_) * sizeof(UINT64) * kGpuReadbackFrameSlots;
    resource.Height = resource.DepthOrArraySize = resource.MipLevels = resource.SampleDesc.Count = 1;
    resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (UINT q = 0; q < kGpuQueueCount; ++q) {
        if (!gpuQueue_[q]) { continue; }
        const D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(resource.Width) };
        if (FAILED(gpu::CreateQueryHeap(device, &queryDesc, MemTag::Profiler,
                q == 0 ? L"Profiler.QueryHeap.Graphics" : L"Profiler.QueryHeap.Compute",
                IID_PPV_ARGS(&gpuQueryHeap_[q]))) ||
            FAILED(gpu::CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &resource,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, MemTag::Profiler,
                q == 0 ? L"Profiler.Readback.Graphics" : L"Profiler.Readback.Compute",
                IID_PPV_ARGS(&gpuReadback_[q]))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpuDrainFence_[q]))) ||
            FAILED(gpuQueue_[q]->GetTimestampFrequency(&gpuFreq_[q])) ||
            FAILED(gpuReadback_[q]->Map(0, &readRange, reinterpret_cast<void**>(&gpuMappedReadback_[q])))) {
            LOG_ERROR(logging::LogCategory::Profiling, "GPU profiler initialization failed on queue {}", q);
            ShutdownGpu();
            return;
        }
    }
    gpuPending_.resize(maxGpuQueries_ / 2);
    for (auto& batch : gpuResolvedBatches_) { batch.ranges.reserve(maxGpuQueries_ / 2); }
    LARGE_INTEGER frequency{}, now{};
    if (QueryPerformanceFrequency(&frequency) && QueryPerformanceCounter(&now)) {
        gpuTraceQpcFreq_ = frequency.QuadPart;
        gpuTraceQpcOrigin_ = now.QuadPart;
        gpuTraceCpuOriginUs_ = ToMicroseconds(CpuClock::now());
    }
    gpuInitialized_ = true;
}

void Profiler::CollectGpuResults() {
    bool capture = false;
    uint64_t startUs = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        capture = traceCapturing_ || traceDraining_;
        startUs = traceStartUs_;
    }
    std::vector<TraceEvent> events;
    CollectGpuResolvedSamples(gpuFrameSamples_, capture ? &events : nullptr, startUs);
    if (!events.empty()) {
        std::lock_guard<std::mutex> lock(mtx_);
        traceEvents_.insert(traceEvents_.end(), events.begin(), events.end());
    }
    // BeginFrame precedes the renderer's fence wait: recheck after readback.
    // Never overwrite an unread ring slot when a queue is more than 8 frames late.
    gpuRecordingReady_ = gpuInitialized_ && frameOpen_ && gpuResolvedBatches_[gpuRecordingReadbackSlot_].queryCount == 0;
    FinishTraceIfReady();
}

void Profiler::BeginGpuFrame(ID3D12GraphicsCommandList* cl) {
    gpuFrameSampleIdx_ = BeginGpuSample(cl, kGpuFrameKey);
}

void Profiler::EndGpuFrame(ID3D12GraphicsCommandList* cl) {
    EndGpuSample(cl, gpuFrameSampleIdx_);
    gpuFrameSampleIdx_ = SIZE_MAX;
}

size_t Profiler::BeginGpuSample(ID3D12GraphicsCommandList* cl, const ScopeNameKey& key) {
    if (!gpuInitialized_ || !gpuRecordingReady_ || !cl || !GetEnabled() || !sampleEpoch_.load(std::memory_order_acquire)) { return SIZE_MAX; }
    const UINT q = cl->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE ? 1u : 0u;
    if (!gpuQueryHeap_[q]) { return SIZE_MAX; }
    const UINT start = nextGpuQuery_.fetch_add(2, std::memory_order_relaxed);
    if (start >= maxGpuQueries_) { return SIZE_MAX; }
    auto& groups = GetThreadSamples().gpuGroups;
    GpuResolveGroup* group = nullptr;
    for (auto& candidate : groups) {
        if (candidate->depth && candidate->cl == cl) { group = candidate.get(); break; }
    }
    if (!group) {
        for (auto& candidate : groups) {
            if (!candidate->depth) { group = candidate.get(); break; }
        }
        if (!group) {
            groups.push_back(std::make_unique<GpuResolveGroup>());
            group = groups.back().get();
            group->starts.reserve(16);
        }
        group->cl = cl;
        group->starts.clear();
    }
    ++group->depth;
    group->starts.push_back(start);
    const size_t idx = start / 2;
    gpuPending_[idx] = { key, start, start + 1, false, frameNo_, q, group };
    cl->EndQuery(gpuQueryHeap_[q].Get(), D3D12_QUERY_TYPE_TIMESTAMP, start);
    return idx;
}

void Profiler::EndGpuSample(ID3D12GraphicsCommandList* cl, size_t idx) {
    if (!gpuInitialized_ || !cl || idx == SIZE_MAX || idx >= gpuPending_.size()) { return; }
    auto& range = gpuPending_[idx];
    if (range.completed) { return; }
    const UINT q = range.queueIndex;
    cl->EndQuery(gpuQueryHeap_[q].Get(), D3D12_QUERY_TYPE_TIMESTAMP, range.end);
    range.completed = true;
    auto& group = *range.group;
    if (--group.depth) { return; }
    // Coalesce only consecutive pairs of completed nested scopes on this list.
    // Other recording threads can leave gaps; those queries must not be resolved.
    for (size_t i = 0; i < group.starts.size();) {
        const UINT first = group.starts[i++];
        UINT count = 2;
        while (i < group.starts.size() && group.starts[i] == first + count) { count += 2; ++i; }
        const UINT64 offset = (static_cast<UINT64>(gpuRecordingReadbackSlot_) * maxGpuQueries_ + first) * sizeof(UINT64);
        cl->ResolveQueryData(gpuQueryHeap_[q].Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, count, gpuReadback_[q].Get(), offset);
    }
}

void Profiler::ShutdownGpu() {
    // The renderer joins both queues before releasing GPU resources. Only
    // shutdown joins file I/O; interactive Stop merely retires completed batches.
    CollectGpuResults();
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (traceCapturing_) {
            traceCapturing_ = false;
            traceRecording_.store(false, std::memory_order_release);
            traceDraining_ = true;
            traceLastFrame_ = frameNo_;
            traceCapturingAtomic_.store(false, std::memory_order_release);
        }
    }
    FinishTraceIfReady();
    StopTraceWriter();
    TaskSystem::Get().Wait(overlayTask_);
    TaskSystem::Get().Release(overlayTask_);
    gpuFrameSampleIdx_ = SIZE_MAX;
    for (UINT q = 0; q < kGpuQueueCount; ++q) {
        if (gpuMappedReadback_[q] && gpuReadback_[q]) {
            const D3D12_RANGE written{ 0, 0 };
            gpuReadback_[q]->Unmap(0, &written);
        }
        gpuMappedReadback_[q] = nullptr;
        gpuQueryHeap_[q].Reset();
        gpuReadback_[q].Reset();
        gpuDrainFence_[q].Reset();
        gpuQueue_[q] = nullptr;
        gpuFreq_[q] = 0;
    }
    for (auto& batch : gpuResolvedBatches_) { batch.ranges.clear(); batch.queryCount = 0; batch.fenceValue = 0; }
    gpuPending_.clear();
    gpuTraceQpcFreq_ = gpuTraceQpcOrigin_ = gpuTraceCpuOriginUs_ = 0;
    gpuDrainFenceValue_ = 1;
    maxGpuQueries_ = gpuRecordingReadbackSlot_ = 0;
    nextGpuQuery_.store(0, std::memory_order_relaxed);
    gpuInitialized_ = gpuRecordingReady_ = false;
}
#endif // PROF_GPU_ENABLED

std::vector<Profiler::OverlaySample> Profiler::OverlaySnapshot() const {
    std::vector<OverlaySample> out;
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    out.reserve(overlayRows_.size() + gpuOverlayRows_.size());
    auto addRows = [&](const auto& rows, bool gpu) {
        for (const auto& row : rows) {
            out.push_back({ WideToUtf8(row.name), row.avgMs, row.maxMs, row.usages, gpu });
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
    out << "{\n\"traceEvents\":[\n";
    bool first = true;
    auto separator = [&]() { if (!first) { out << ",\n"; } first = false; };
    auto threadName = [&](size_t tid, const std::string& name) {
        separator();
        out << "  {\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":0,\"tid\":" << tid
            << ",\"args\":{\"name\":\"" << EscapeJson(name) << "\"}}";
    };
#if PROF_GPU_ENABLED
    threadName(kGpuTraceThreadIndex, kGpuTraceThreadName);
    threadName(kGpuTraceThreadIndexCompute, kGpuTraceThreadNameCompute);
#endif
    for (size_t tid = 0; tid < dump.threadNames.size(); ++tid) {
        if (!dump.threadNames[tid].empty()) { threadName(tid + kCpuTraceThreadIndexBase, dump.threadNames[tid]); }
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
            << ",\"pid\":0,\"tid\":" << TraceOutputThreadIndex(ev);
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
    LOG_INFO(logging::LogCategory::Profiling, "trace saved: {} ({} events)", fileName, dump.events.size());
}

#endif // PROF_ENABLED
