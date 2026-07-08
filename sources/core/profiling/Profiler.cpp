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
#include "text/TextManager.h"
#include "input/InputManager.h"
#include "app/Systems.h"

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
const Profiler::ScopeNameKey kProfilerEmitOverlayKey = Profiler::RegisterTraceLiteral(L"Profiler::EmitOverlay");
const Profiler::ScopeNameKey kGpuFrameKey = Profiler::RegisterTraceLiteral(L"GPU.Frame");
constexpr uint32_t kGpuTraceThreadIndex = 0u;
constexpr const char* kGpuTraceThreadName = "GPU Queue";
constexpr UINT kGpuReadbackFrameSlots = 8u;
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

void FormatOverlayRow(Profiler::OverlayRow& row, const std::wstring* name) {
    const double perUse = (row.usages ? (row.avgMs / static_cast<double>(row.usages)) : 0.0);
    wchar_t buf[192];
    std::swprintf(buf, sizeof(buf) / sizeof(wchar_t),
        L"%-50s  avg:%6.2f  max:%6.2f  p/u:%6.3f  usages:%u",
        name->c_str(), row.avgMs, row.maxMs, perUse, row.usages);
    row.formatted.assign(buf);
}

uint64_t TraceOutputThreadIndex(const Profiler::TraceEvent& ev) {
    if (ev.category == Profiler::TraceEvent::Category::Gpu) {
        return kGpuTraceThreadIndex;
    }
    return static_cast<uint64_t>(ev.threadIndex) + 1u;
}

} // namespace


#if PROF_ENABLED

Profiler::SampleNode* const Profiler::kSampleListClosed = reinterpret_cast<Profiler::SampleNode*>(1);
Profiler::TraceSampleNode* const Profiler::kTraceListClosed = reinterpret_cast<Profiler::TraceSampleNode*>(1);

Profiler::Profiler() {
    frameSampleHead_.store(kSampleListClosed, std::memory_order_relaxed);
#if PROF_GPU_ENABLED
    gpuFrameSampleHead_.store(kSampleListClosed, std::memory_order_relaxed);
#endif
    traceSampleHead_.store(kTraceListClosed, std::memory_order_relaxed);
}

Profiler::~Profiler() {
    auto destroyList = [](SampleNode* head) {
        while (head) {
            SampleNode* next = head->next;
            delete head;
            head = next;
        }
    };

    SampleNode* head = frameSampleHead_.exchange(nullptr, std::memory_order_acq_rel);
    if (head == kSampleListClosed) { head = nullptr; }
    destroyList(head);
#if PROF_GPU_ENABLED
    SampleNode* gpuHead = gpuFrameSampleHead_.exchange(nullptr, std::memory_order_acq_rel);
    if (gpuHead == kSampleListClosed) { gpuHead = nullptr; }
    destroyList(gpuHead);
#endif

    SampleNode* pool = sampleNodePool_.exchange(nullptr, std::memory_order_acq_rel);
    destroyList(pool);

    auto destroyTraceList = [](TraceSampleNode* head) {
        while (head) {
            TraceSampleNode* next = head->next;
            delete head;
            head = next;
        }
    };

    TraceSampleNode* traceHead = traceSampleHead_.exchange(nullptr, std::memory_order_acq_rel);
    if (traceHead == kTraceListClosed) { traceHead = nullptr; }
    destroyTraceList(traceHead);

    TraceSampleNode* tracePool = traceSamplePool_.exchange(nullptr, std::memory_order_acq_rel);
    destroyTraceList(tracePool);
}

Profiler::ScopeNameKey Profiler::RegisterTraceLiteral(const wchar_t* name) {
    return GetTraceStringTable().RegisterLiteral(name);
}

Profiler::ScopeNameKey Profiler::RegisterTraceDynamic(std::wstring name, uint32_t* outIndex) {
    return GetTraceStringTable().RegisterDynamic(std::move(name), outIndex);
}

void Profiler::ReleaseTraceNameKeys(const std::vector<Profiler::ScopeNameKey>& keys) {
    GetTraceStringTable().ReleaseDynamicKeys(keys);
}

Profiler::SampleNode* Profiler::AcquireSampleNode() {
    SampleNode* node = sampleNodePool_.load(std::memory_order_acquire);
    while (node && !sampleNodePool_.compare_exchange_weak(node, node->next,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
    }
    if (!node) {
        node = new SampleNode();
    }
    node->next = nullptr;
    return node;
}

void Profiler::ReleaseSampleNode(SampleNode* node) {
    if (!node) { return; }
    SampleNode* head = sampleNodePool_.load(std::memory_order_relaxed);
    do {
        node->next = head;
    } while (!sampleNodePool_.compare_exchange_weak(head, node,
                std::memory_order_release, std::memory_order_relaxed));
}

void Profiler::ReleaseSampleList(SampleNode* head) {
    if (!head || head == kSampleListClosed) { return; }
    while (head) {
        SampleNode* next = head->next;
        ReleaseSampleNode(head);
        head = next;
    }
}

Profiler::TraceSampleNode* Profiler::AcquireTraceSampleNode() {
    TraceSampleNode* node = traceSamplePool_.load(std::memory_order_acquire);
    while (node && !traceSamplePool_.compare_exchange_weak(node, node->next,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
    }
    if (!node) {
        node = new TraceSampleNode();
    }
    node->next = nullptr;
    return node;
}

void Profiler::ReleaseTraceSampleNode(TraceSampleNode* node) {
    if (!node) { return; }
    TraceSampleNode* head = traceSamplePool_.load(std::memory_order_relaxed);
    do {
        node->next = head;
    } while (!traceSamplePool_.compare_exchange_weak(head, node,
                std::memory_order_release, std::memory_order_relaxed));
}

void Profiler::ReleaseTraceSampleList(TraceSampleNode* head) {
    if (!head || head == kTraceListClosed) { return; }
    while (head) {
        TraceSampleNode* next = head->next;
        ReleaseTraceSampleNode(head);
        head = next;
    }
}

bool Profiler::PushTraceSampleNode(TraceSampleNode* node) {
    TraceSampleNode* head = traceSampleHead_.load(std::memory_order_acquire);
    while (head != kTraceListClosed) {
        node->next = head;
        if (traceSampleHead_.compare_exchange_weak(head, node,
                std::memory_order_release, std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

void Profiler::DrainTraceSampleNodes(uint64_t traceStartUs) {
    TraceSampleNode* head = traceSampleHead_.exchange(kTraceListClosed, std::memory_order_acq_rel);
    if (head == kTraceListClosed) { return; }
    while (head) {
        TraceSampleNode* next = head->next;
        TraceEvent ev = head->sample;
        if (ev.tsUs >= traceStartUs) {
            ev.tsUs -= traceStartUs;
        }
        else {
            ev.tsUs = 0;
        }
        traceEvents_.push_back(std::move(ev));
        ReleaseTraceSampleNode(head);
        head = next;
    }
}

bool Profiler::PushSampleNode(std::atomic<SampleNode*>& headAtomic, SampleNode* node) {
    SampleNode* head = headAtomic.load(std::memory_order_acquire);
    while (head != kSampleListClosed) {
        node->next = head;
        if (headAtomic.compare_exchange_weak(head, node,
                std::memory_order_release, std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

void Profiler::DrainSampleNodes(std::atomic<SampleNode*>& headAtomic, std::vector<ScopeSample>& out) {
    SampleNode* head = headAtomic.exchange(kSampleListClosed, std::memory_order_acq_rel);
    if (head == kSampleListClosed) { return; }
    while (head) {
        out.push_back(head->sample);
        SampleNode* next = head->next;
        ReleaseSampleNode(head);
        head = next;
    }
}

uint32_t Profiler::GetThreadIndexForCurrentThread() {
    static thread_local uint32_t cached = std::numeric_limits<uint32_t>::max();
    uint32_t idx = cached;
    if (idx != std::numeric_limits<uint32_t>::max()) {
        return idx;
    }
    std::lock_guard<std::mutex> lk(mtx_);
    idx = GetThreadIndex_Locked(std::this_thread::get_id());
    cached = idx;
    return idx;
}

Profiler& Profiler::Get() {
    static Profiler g;
    return g;
}

#if PROF_GPU_ENABLED
Profiler::ScopedGpu::ScopedGpu(ID3D12GraphicsCommandList* cl, ScopeNameKey key)
#if PROF_ENABLED
    : cl_(cl)
#endif
{
#if PROF_ENABLED
    idx_ = Profiler::Get().BeginGpuSample(cl, key);
#else
    (void)cl;
    (void)key;
#endif
}

Profiler::ScopedGpu::~ScopedGpu() {
#if PROF_ENABLED
    Profiler::Get().EndGpuSample(cl_, idx_);
#endif
}
#endif

void Profiler::BeginFrame(uint64_t frameNo) {
    if (!GetEnabled()) { return; }
    std::lock_guard<std::mutex> lk(mtx_);
    frameSamples_.clear();
    SampleNode* stale = frameSampleHead_.exchange(nullptr, std::memory_order_acq_rel);
    if (stale && stale != kSampleListClosed) {
        ReleaseSampleList(stale);
    }
#if PROF_GPU_ENABLED
    SampleNode* gpuStale = gpuFrameSampleHead_.exchange(nullptr, std::memory_order_acq_rel);
    if (gpuStale && gpuStale != kSampleListClosed) {
        ReleaseSampleList(gpuStale);
    }
#endif
    TraceSampleNode* traceStale = traceSampleHead_.exchange(nullptr, std::memory_order_acq_rel);
    if (traceStale && traceStale != kTraceListClosed) {
        ReleaseTraceSampleList(traceStale);
    }
#if PROF_GPU_ENABLED
    gpuFrameSamples_.clear();
    gpuFrameSampleIdx_.store(SIZE_MAX, std::memory_order_relaxed);
#endif
    frameNo_ = frameNo;
    frameOpen_ = true;
    frameSampleHead_.store(nullptr, std::memory_order_release);
#if PROF_GPU_ENABLED
    gpuFrameSampleHead_.store(nullptr, std::memory_order_release);
#endif
    traceSampleHead_.store(nullptr, std::memory_order_release);
    frameOpenFlag_.store(true, std::memory_order_release);
    frameCpuStart_ = CpuClock::now();

    if (traceCaptureRequested_ && !traceCapturing_ && traceRequestFrameCount_ > 0) {
        std::lock_guard<std::mutex> traceLock(traceMtx_);
        traceCaptureRequested_ = false;
        traceCapturing_ = true;
        traceFramesRemaining_ = traceRequestFrameCount_;
        traceEvents_.clear();
        traceStartUs_ = 0;
        traceStartSet_ = false;
        traceStopRequested_ = false;
        auto leftoverKeys = GetTraceStringTable().TakeCaptureKeys();
        if (!leftoverKeys.empty()) {
            GetTraceStringTable().ReleaseDynamicKeys(leftoverKeys);
        }
    }

    if (traceCapturing_ && !traceStartSet_) {
        std::lock_guard<std::mutex> traceLock(traceMtx_);
        if (!traceStartSet_) {
            traceStartUs_ = ToMicroseconds(frameCpuStart_);
            traceStartSet_ = true;
        }
    }
    traceCapturingAtomic_.store(traceCapturing_, std::memory_order_release);

    if (lastMaxReset_.time_since_epoch().count() == 0) {
        lastMaxReset_ = CoolClock::now();
    }
    if (lastOverlaySort_.time_since_epoch().count() == 0) {
        lastOverlaySort_ = CoolClock::now();
    }
}

void Profiler::EndFrame() {
    if (!GetEnabled()) { return; }

    // 0) Wait for the previous async build (minimal work on the main thread)
    TaskSystem& tasks = TaskSystem::Get();
    tasks.Wait(overlayTask_);
    tasks.Release(overlayTask_);

    // 1) Pull samples from the current frame and close it out quickly
    auto& samples = asyncCpuSamples_;
    samples.clear();
#if PROF_GPU_ENABLED
    auto& gpuSamples = asyncGpuSamples_;
    gpuSamples.clear();
#endif
    auto& traceDump = traceDumpData_;
    traceDump.events.clear();
    traceDump.dynamicKeys.clear();
    bool haveTraceDump = false;
    bool finishTraceNow = false;
    uint64_t finishedTraceStartUs = 0;
    const auto frameEnd = CpuClock::now();
    frameSamples_.clear();
#if PROF_GPU_ENABLED
    gpuFrameSamples_.clear();
#endif
    frameOpenFlag_.store(false, std::memory_order_release);
    DrainSampleNodes(frameSampleHead_, frameSamples_);
#if PROF_GPU_ENABLED
    DrainSampleNodes(gpuFrameSampleHead_, gpuFrameSamples_);
#endif
    samples.swap(frameSamples_);
#if PROF_GPU_ENABLED
    gpuSamples.swap(gpuFrameSamples_);
#endif
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!frameOpen_) {
            frameSamples_.swap(samples);
#if PROF_GPU_ENABLED
            gpuFrameSamples_.swap(gpuSamples);
#endif
            return;
        }
        if (traceCapturing_) {
            std::lock_guard<std::mutex> traceLock(traceMtx_);
            const uint64_t startUs = ToMicroseconds(frameCpuStart_);
            const uint64_t endUs = ToMicroseconds(frameEnd);
            const uint64_t durUs = (endUs > startUs) ? (endUs - startUs) : 0;
            const uint32_t threadIdx = GetThreadIndex_Locked(std::this_thread::get_id());
            TraceEvent fev;
            std::wstring frameLabel = L"Frame ";
            frameLabel += std::to_wstring(frameNo_);
            Profiler::ScopeNameKey frameKey = RegisterTraceDynamic(std::move(frameLabel));
            fev.key = frameKey;
            fev.tsUs = (startUs >= traceStartUs_) ? (startUs - traceStartUs_) : 0;
            fev.durUs = durUs;
            fev.threadIndex = threadIdx;
            traceEvents_.push_back(std::move(fev));

            DrainTraceSampleNodes(traceStartUs_);

            TraceEvent hev;
            hev.key = kTraceHandlingKey;
            hev.tsUs = endUs - traceStartUs_;
            hev.durUs = ToMicroseconds(CoolClock::now()) - endUs;
            hev.threadIndex = threadIdx;
            traceEvents_.push_back(std::move(hev));

            const bool stopNow = traceStopRequested_;
            traceStopRequested_ = false;

            bool finishTrace = false;
            if (stopNow) {
                traceFramesRemaining_ = 0;
                finishTrace = true;
            }
            else if (traceFramesRemaining_ > 0) {
                traceFramesRemaining_--;
                if (traceFramesRemaining_ == 0) {
                    finishTrace = true;
                }
            }

            if (finishTrace) {
                finishTraceNow = true;
                finishedTraceStartUs = traceStartUs_;
                traceCapturing_ = false;
                traceStartUs_ = 0;
                traceStartSet_ = false;
                traceRequestFrameCount_ = 0;
            }
        }
        frameOpen_ = false;
    }
    traceCapturingAtomic_.store(traceCapturing_, std::memory_order_release);

    // Whole-CPU-frame counter: the full BeginFrame..EndFrame span (includes
    // Renderer::BeginFrame's fence wait). Pushed into this frame's CPU samples
    // so it accumulates into stats_ / the overlay like any scope — past the
    // early-out above, so the frame was open. Excludes only the profiler's own
    // post-frameEnd work.
    {
        const double wholeFrameMs =
            std::chrono::duration<double, std::milli>(frameEnd - frameCpuStart_).count();
        asyncCpuSamples_.push_back(ScopeSample{ kCpuFrameKey, wholeFrameMs });
    }

#if PROF_GPU_ENABLED
    // 2) prepare gpu samples for next frame
    {
        std::lock_guard<std::mutex> lk(gpuMtx_);
        if (!gpuPending_.empty() && nextGpuQuery_ > 0 && gpuQueue_ && gpuDrainFence_) {
            const UINT64 fenceValue = gpuDrainFenceValue_++;
            if (SUCCEEDED(gpuQueue_->Signal(gpuDrainFence_.Get(), fenceValue))) {
                GpuResolvedBatch batch;
                batch.ranges = std::move(gpuPending_);
                batch.queryCount = nextGpuQuery_;
                batch.readbackSlot = gpuRecordingReadbackSlot_;
                batch.fenceValue = fenceValue;
                gpuResolvedBatches_.push_back(std::move(batch));
            }
        }
        gpuPending_.clear();
        nextGpuQuery_ = 0;
        gpuRecordingReadbackSlot_ = (gpuRecordingReadbackSlot_ + 1u) % kGpuReadbackFrameSlots;
    }

    if (finishTraceNow && WaitForGpuProfilerIdle()) {
        std::vector<TraceEvent> finalGpuTraceEvents;
        CollectGpuResolvedSamples(&gpuSamples, &finalGpuTraceEvents, finishedTraceStartUs);
        if (!finalGpuTraceEvents.empty()) {
            std::lock_guard<std::mutex> traceLock(traceMtx_);
            for (auto& ev : finalGpuTraceEvents) {
                traceEvents_.push_back(std::move(ev));
            }
        }
    }
#endif

    if (finishTraceNow) {
        std::lock_guard<std::mutex> traceLock(traceMtx_);
        traceDump.events = std::move(traceEvents_);
        traceDump.dynamicKeys = GetTraceStringTable().TakeCaptureKeys();
        haveTraceDump = true;
    }

    auto overlayJob = [this, haveTraceDump]() mutable {
        const auto t0 = CoolClock::now();

        // A) Reduce samples into stats_ (lock-free) + EMA/lastCount
        auto accumulateSamples = [&](const std::vector<ScopeSample>& src,
                                     auto& statsMap,
                                     uint32_t& nextOverlayId) {
            for (const auto& s : src) {
                auto [it, inserted] = statsMap.try_emplace(s.key);
                auto& entry = it->second;
                if (inserted) {
                    entry.wideName = BuildWideName(it->first);
                    entry.overlayId = nextOverlayId++;
                    if (entry.overlayId == 0) { entry.overlayId = nextOverlayId++; }
                }
                entry.stats.Accumulate(s.ms);
            }
            for (auto& kv : statsMap) {
                kv.second.stats.CommitFrame(emaAlpha_);
            }
        };

        accumulateSamples(asyncCpuSamples_, stats_, nextOverlayId_);
        asyncCpuSamples_.clear();

#if PROF_GPU_ENABLED
        accumulateSamples(asyncGpuSamples_, gpuStats_, nextGpuOverlayId_);
        asyncGpuSamples_.clear();
#endif

        // B) Build the current set of rows, reusing allocated buffers
        auto buildOverlayScratch = [&](auto& statsMap,
                                       std::vector<OverlayRow>& scratchRows,
                                       robin_hood::unordered_flat_map<uint32_t, size_t>& lookup,
                                       std::vector<uint8_t>& usedFlags,
                                       std::vector<const StatsEntry*>& entryPtrs,
                                       std::vector<size_t>& orderBuffer) {
            entryPtrs.clear();
            entryPtrs.reserve(statsMap.size());
            for (auto& kv : statsMap) {
                if (kv.second.overlayId == 0) { continue; }
                // Drop scopes not recorded this frame (lastCount == 0): CommitFrame freezes their
                // avg/max, so otherwise they linger forever with stale values (e.g. VSM passes in
                // Legacy mode). The entry stays in statsMap, so it reappears seamlessly if it resumes.
                if (kv.second.stats.lastCount == 0) { continue; }
                entryPtrs.push_back(&kv.second);
            }
            const size_t count = entryPtrs.size();
            scratchRows.resize(count);
            lookup.clear();
            lookup.reserve(count);
            usedFlags.assign(count, 0u);
            orderBuffer.resize(count);
            if (count == 0) {
                return;
            }
            auto fillRow = [&](size_t idx) {
                const StatsEntry* entry = entryPtrs[idx];
                OverlayRow row;
                row.entryId = entry->overlayId;
                row.avgMs = entry->stats.avgMs;
                row.maxMs = entry->stats.maxMs;
                row.usages = entry->stats.lastCount;
                FormatOverlayRow(row, &entry->wideName);
                scratchRows[idx] = std::move(row);
            };
            constexpr size_t kParallelThreshold = 512;
            const size_t jobCount = count;
            if (jobCount >= kParallelThreshold) {
                TaskSystem::ParallelFor(jobCount, [&](size_t idx) { fillRow(idx); }, 64);
            }
            else {
                for (size_t idx = 0; idx < jobCount; ++idx) {
                    fillRow(idx);
                }
            }
            for (size_t idx = 0; idx < jobCount; ++idx) {
                lookup.emplace(scratchRows[idx].entryId, idx);
                orderBuffer[idx] = idx;
            }
        };

        buildOverlayScratch(stats_, overlayScratchRows_, overlayScratchLookup_, overlayScratchUsed_, overlayEntryPtrs_, overlayScratchOrder_);
#if PROF_GPU_ENABLED
        buildOverlayScratch(gpuStats_, gpuOverlayScratchRows_, gpuOverlayScratchLookup_, gpuOverlayScratchUsed_, gpuOverlayEntryPtrs_, gpuOverlayScratchOrder_);
#endif

        // C) Rare sort or stable order update
        const auto now = CoolClock::now();
        const double secSinceSort = std::chrono::duration<double>(now - lastOverlaySort_).count();
        const bool needResort = (secSinceSort >= overlayResortIntervalSec_);

        const int readIdx = overlayReadBuf_.load(std::memory_order_acquire);
        const int writeIdx = readIdx ^ 1;
        auto& readRows = overlayRows_[readIdx];
        auto& writeRows = overlayRows_[writeIdx];
        writeRows.clear();
        writeRows.reserve(overlayScratchRows_.size() + 1u);

#if PROF_GPU_ENABLED
        const int gpuReadIdx = gpuOverlayReadBuf_.load(std::memory_order_acquire);
        const int gpuWriteIdx = gpuReadIdx ^ 1;
        auto& gpuReadRows = gpuOverlayRows_[gpuReadIdx];
        auto& gpuWriteRows = gpuOverlayRows_[gpuWriteIdx];
        gpuWriteRows.clear();
        gpuWriteRows.reserve(gpuOverlayScratchRows_.size());
#endif

        if (needResort) {
            auto& order = overlayScratchOrder_;
            auto cmpIdx = [&](size_t a, size_t b) {
                const auto& ra = overlayScratchRows_[a];
                const auto& rb = overlayScratchRows_[b];
                if (ra.avgMs == rb.avgMs) {
                    return ra.entryId < rb.entryId;
                }
                return ra.avgMs > rb.avgMs;
            };
            std::sort(order.begin(), order.end(), cmpIdx);
            for (size_t idx : order) {
                writeRows.push_back(std::move(overlayScratchRows_[idx]));
            }

#if PROF_GPU_ENABLED
            auto& gorder = gpuOverlayScratchOrder_;
            auto gcmpIdx = [&](size_t a, size_t b) {
                const auto& ra = gpuOverlayScratchRows_[a];
                const auto& rb = gpuOverlayScratchRows_[b];
                if (ra.avgMs == rb.avgMs) {
                    return ra.entryId < rb.entryId;
                }
                return ra.avgMs > rb.avgMs;
            };
            std::sort(gorder.begin(), gorder.end(), gcmpIdx);
            for (size_t idx : gorder) {
                gpuWriteRows.push_back(std::move(gpuOverlayScratchRows_[idx]));
            }
#endif
            lastOverlaySort_ = now;
        }
        else {
            for (const OverlayRow& prev : readRows) {
                const uint32_t key = prev.entryId;
                if (key == 0) { continue; }
                auto it = overlayScratchLookup_.find(key);
                if (it != overlayScratchLookup_.end()) {
                    const size_t idx = it->second;
                    writeRows.push_back(std::move(overlayScratchRows_[idx]));
                    overlayScratchUsed_[idx] = 1;
                }
            }
            for (size_t idx = 0; idx < overlayScratchRows_.size(); ++idx) {
                if (!overlayScratchUsed_[idx]) {
                    writeRows.push_back(std::move(overlayScratchRows_[idx]));
                }
            }

#if PROF_GPU_ENABLED
            for (const OverlayRow& prev : gpuReadRows) {
                const uint32_t key = prev.entryId;
                if (key == 0) { continue; }
                auto it = gpuOverlayScratchLookup_.find(key);
                if (it != gpuOverlayScratchLookup_.end()) {
                    const size_t idx = it->second;
                    gpuWriteRows.push_back(std::move(gpuOverlayScratchRows_[idx]));
                    gpuOverlayScratchUsed_[idx] = 1;
                }
            }
            for (size_t idx = 0; idx < gpuOverlayScratchRows_.size(); ++idx) {
                if (!gpuOverlayScratchUsed_[idx]) {
                    gpuWriteRows.push_back(std::move(gpuOverlayScratchRows_[idx]));
                }
            }
#endif
        }

        // D) Cooldown for resetting maxima (under the stats_ lock)
        {
            const double secSinceMax = std::chrono::duration<double>(now - lastMaxReset_).count();
            if (secSinceMax >= maxResetIntervalSec_) {
                std::lock_guard<std::mutex> lk(mtx_);
                ResetMax_Unsafe();
                lastMaxReset_ = now;
                endFrameAsyncMaxMs_ = 0.0;
            }
        }

        // E) Measure the async phase itself and insert it as the first entry
        {
            const double dtMs = std::chrono::duration<double, std::milli>(CoolClock::now() - t0).count();
            endFrameAsyncLastMs_ = dtMs;
            if (endFrameAsyncAvgMs_ <= 0.0) { endFrameAsyncAvgMs_ = dtMs; }
            else { endFrameAsyncAvgMs_ = endFrameAsyncAvgMs_ * emaAlpha_ + dtMs * (1.0 - emaAlpha_); }
            if (dtMs > endFrameAsyncMaxMs_) { endFrameAsyncMaxMs_ = dtMs; }

            static const std::wstring kAsyncName = L"Profiler::EndFrame.Async";
            OverlayRow self;
            self.entryId = 0;
            self.avgMs = endFrameAsyncAvgMs_;
            self.maxMs = endFrameAsyncMaxMs_;
            self.usages = 1u;
            FormatOverlayRow(self, &kAsyncName);
            writeRows.insert(writeRows.begin(), std::move(self));
        }

#if PROF_GPU_ENABLED
        // F) Flip the read buffers
        overlayReadBuf_.store(writeIdx, std::memory_order_release);
        gpuOverlayReadBuf_.store(gpuWriteIdx, std::memory_order_release);
#else
        overlayReadBuf_.store(writeIdx, std::memory_order_release);
#endif

        if (haveTraceDump) {
            auto& traceDump = traceDumpData_;
            if (!traceDump.events.empty()) {
                WriteTraceJson(traceDump.events);
            }
            ReleaseTraceNameKeys(traceDump.dynamicKeys);
            traceDump.events.clear();
            traceDump.dynamicKeys.clear();
        }
    };

#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
    overlayTask_ = TaskSystem::Get().Submit(std::move(overlayJob));
#else
    overlayTask_ = nullptr;
    overlayJob();
#endif
}

void Profiler::PushSample(const ScopeNameKey& key, CpuClock::time_point start, CpuClock::time_point end) {
    if (!GetEnabled()) { return; }
    if (!frameOpenFlag_.load(std::memory_order_acquire)) { return; }
    const double ms = std::chrono::duration<double, std::milli>(end - start).count();
    SampleNode* node = AcquireSampleNode();
    node->sample.key = key;
    node->sample.ms = ms;
    if (!PushSampleNode(frameSampleHead_, node)) {
        ReleaseSampleNode(node);
        return;
    }

    if (!traceCapturingAtomic_.load(std::memory_order_acquire)) {
        return;
    }

    const uint64_t startUs = ToMicroseconds(start);
    const uint64_t endUs = ToMicroseconds(end);
    const uint64_t durUs = (endUs > startUs) ? (endUs - startUs) : 0;
    const uint32_t threadIdx = GetThreadIndexForCurrentThread();
    TraceSampleNode* traceNode = AcquireTraceSampleNode();
    traceNode->sample.key = key;
    traceNode->sample.tsUs = startUs;
    traceNode->sample.durUs = durUs;
    traceNode->sample.threadIndex = threadIdx;
    if (!PushTraceSampleNode(traceNode)) {
        ReleaseTraceSampleNode(traceNode);
    }
}
#if PROF_GPU_ENABLED
void Profiler::PushGpuSample(const ScopeNameKey& key, double ms) {
    if (!GetEnabled()) { return; }
    if (!frameOpenFlag_.load(std::memory_order_acquire)) { return; }
    SampleNode* node = AcquireSampleNode();
    node->sample.key = key;
    node->sample.ms = ms;
    if (!PushSampleNode(gpuFrameSampleHead_, node)) {
        ReleaseSampleNode(node);
    }
}

bool Profiler::CaptureGpuTraceCalibration(GpuTraceCalibration& calibration) const {
    if (!gpuInitialized_ || !gpuQueue_ || gpuFreq_ == 0 || gpuTraceQpcFreq_ == 0) {
        return false;
    }
    UINT64 gpuTimestamp = 0;
    UINT64 cpuQpc = 0;
    if (FAILED(gpuQueue_->GetClockCalibration(&gpuTimestamp, &cpuQpc))) {
        return false;
    }
    calibration.gpuTimestamp = gpuTimestamp;
    calibration.cpuQpc = cpuQpc;
    return true;
}

uint64_t Profiler::GpuTimestampToCpuUs(UINT64 gpuTimestamp, const GpuTraceCalibration& calibration) const {
    if (gpuFreq_ == 0 || gpuTraceQpcFreq_ == 0) {
        return 0;
    }

    const double gpuDelta = static_cast<double>(gpuTimestamp) - static_cast<double>(calibration.gpuTimestamp);
    const double cpuQpc =
        static_cast<double>(calibration.cpuQpc) +
        gpuDelta * static_cast<double>(gpuTraceQpcFreq_) / static_cast<double>(gpuFreq_);
    const double cpuUs =
        static_cast<double>(gpuTraceCpuOriginUs_) +
        (cpuQpc - static_cast<double>(gpuTraceQpcOrigin_)) * 1000000.0 / static_cast<double>(gpuTraceQpcFreq_);
    if (cpuUs <= 0.0) {
        return 0;
    }
    return static_cast<uint64_t>(std::llround(cpuUs));
}

void Profiler::CollectGpuResolvedSamples(std::vector<ScopeSample>* sampleOut,
    std::vector<TraceEvent>* traceOut,
    uint64_t traceStartUs) {
    if (!gpuInitialized_) { return; }

    std::vector<GpuResolvedBatch> readyBatches;
    {
        std::lock_guard<std::mutex> lk(gpuMtx_);
        if (gpuResolvedBatches_.empty() || !gpuDrainFence_) { return; }

        const UINT64 completedFence = gpuDrainFence_->GetCompletedValue();
        auto writeIt = gpuResolvedBatches_.begin();
        for (auto readIt = gpuResolvedBatches_.begin(); readIt != gpuResolvedBatches_.end(); ++readIt) {
            if (readIt->fenceValue != 0 && completedFence >= readIt->fenceValue) {
                readyBatches.push_back(std::move(*readIt));
            }
            else {
                if (writeIt != readIt) {
                    *writeIt = std::move(*readIt);
                }
                ++writeIt;
            }
        }
        gpuResolvedBatches_.erase(writeIt, gpuResolvedBatches_.end());
    }

    if (readyBatches.empty()) {
        return;
    }

    GpuTraceCalibration calibration{};
    const bool collectTrace = (traceOut != nullptr && traceStartUs != 0 && CaptureGpuTraceCalibration(calibration));

    for (const GpuResolvedBatch& batch : readyBatches) {
        if (batch.queryCount == 0 || batch.readbackSlot >= kGpuReadbackFrameSlots) {
            continue;
        }

        const SIZE_T slotBase = static_cast<SIZE_T>(batch.readbackSlot) *
            static_cast<SIZE_T>(maxGpuQueries_) * sizeof(UINT64);
        UINT64* data = nullptr;
        D3D12_RANGE range{ slotBase, slotBase + static_cast<SIZE_T>(batch.queryCount) * sizeof(UINT64) };
        if (SUCCEEDED(gpuReadback_->Map(0, &range, reinterpret_cast<void**>(&data)))) {
            UINT64* slotData = reinterpret_cast<UINT64*>(reinterpret_cast<uint8_t*>(data) + slotBase);
            for (const auto& s : batch.ranges) {
                if (!s.completed || s.start >= batch.queryCount || s.end >= batch.queryCount) {
                    continue;
                }

                const UINT64 a = slotData[s.start];
                const UINT64 b = slotData[s.end];
                if (b <= a || gpuFreq_ == 0) {
                    continue;
                }

                const double ms = static_cast<double>(b - a) * 1000.0 / static_cast<double>(gpuFreq_);
                if (sampleOut) {
                    sampleOut->push_back(ScopeSample{ s.key, ms });
                }
                else {
                    PushGpuSample(s.key, ms);
                }

                if (collectTrace) {
                    const uint64_t startCpuUs = GpuTimestampToCpuUs(a, calibration);
                    const uint64_t endCpuUs = GpuTimestampToCpuUs(b, calibration);
                    if (endCpuUs <= traceStartUs || endCpuUs <= startCpuUs) {
                        continue;
                    }

                    const uint64_t clippedStartUs = std::max(startCpuUs, traceStartUs);
                    TraceEvent ev;
                    ev.key = s.key;
                    if (s.key.name == kGpuFrameKey.name) {
                        std::wstring frameName = L"GPU.Frame ";
                        frameName += std::to_wstring(s.frameNo);
                        ev.key = RegisterTraceDynamic(std::move(frameName));
                        ev.frameNumber = s.frameNo;
                        ev.hasFrameNumber = true;
                    }
                    ev.tsUs = clippedStartUs - traceStartUs;
                    ev.durUs = endCpuUs - clippedStartUs;
                    ev.threadIndex = kGpuTraceThreadIndex;
                    ev.category = TraceEvent::Category::Gpu;
                    traceOut->push_back(std::move(ev));
                }
            }
            D3D12_RANGE writtenRange{ 0, 0 };
            gpuReadback_->Unmap(0, &writtenRange);
        }
    }
}

bool Profiler::WaitForGpuProfilerIdle() {
    if (!gpuInitialized_ || !gpuQueue_ || !gpuDrainFence_ || !gpuDrainFenceEvent_) {
        return false;
    }

    const UINT64 value = gpuDrainFenceValue_++;
    if (FAILED(gpuQueue_->Signal(gpuDrainFence_.Get(), value))) {
        return false;
    }
    if (gpuDrainFence_->GetCompletedValue() < value) {
        if (FAILED(gpuDrainFence_->SetEventOnCompletion(value, gpuDrainFenceEvent_))) {
            return false;
        }
        WaitForSingleObject(gpuDrainFenceEvent_, INFINITE);
    }
    return true;
}

void Profiler::InitGpu(ID3D12Device* device, ID3D12CommandQueue* queue, UINT maxQueries) {
#if PROF_ENABLED
    if (!device || !queue) { return; }
    gpuQueue_ = queue;
    maxGpuQueries_ = maxQueries;

    D3D12_QUERY_HEAP_DESC qh{};
    qh.Count = maxQueries;
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.NodeMask = 0;
    device->CreateQueryHeap(&qh, IID_PPV_ARGS(&gpuQueryHeap_));

    UINT64 bufSize = (UINT64)maxQueries * sizeof(UINT64) * kGpuReadbackFrameSlots;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bufSize;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.SampleDesc.Count = 1;
    device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&gpuReadback_));

    queue->GetTimestampFrequency(&gpuFreq_);
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpuDrainFence_));
    gpuDrainFenceEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    LARGE_INTEGER qpcFreq{};
    LARGE_INTEGER qpcNow{};
    if (QueryPerformanceFrequency(&qpcFreq) && QueryPerformanceCounter(&qpcNow)) {
        gpuTraceQpcFreq_ = static_cast<UINT64>(qpcFreq.QuadPart);
        gpuTraceQpcOrigin_ = static_cast<UINT64>(qpcNow.QuadPart);
        gpuTraceCpuOriginUs_ = ToMicroseconds(CpuClock::now());
    }
    gpuInitialized_ = true;
#else
    (void)device; (void)queue; (void)maxQueries;
#endif
}

void Profiler::CollectGpuResults() {
#if PROF_ENABLED
    if (!gpuInitialized_) { return; }

    bool collectTrace = false;
    uint64_t traceStartUs = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        collectTrace = traceCapturing_ && traceStartSet_;
        traceStartUs = traceStartUs_;
    }

    std::vector<TraceEvent> gpuTraceEvents;
    CollectGpuResolvedSamples(nullptr, collectTrace ? &gpuTraceEvents : nullptr, traceStartUs);
    if (!gpuTraceEvents.empty()) {
        std::lock_guard<std::mutex> traceLock(traceMtx_);
        for (auto& ev : gpuTraceEvents) {
            traceEvents_.push_back(std::move(ev));
        }
    }
#endif
}

void Profiler::BeginGpuFrame(ID3D12GraphicsCommandList* cl) {
#if PROF_ENABLED
    if (!GetEnabled()) {
        gpuFrameSampleIdx_.store(SIZE_MAX, std::memory_order_relaxed);
        return;
    }
    const size_t idx = BeginGpuSample(cl, kGpuFrameKey);
    gpuFrameSampleIdx_.store(idx, std::memory_order_relaxed);
#else
    (void)cl;
#endif
}

void Profiler::EndGpuFrame(ID3D12GraphicsCommandList* cl) {
#if PROF_ENABLED
    const size_t idx = gpuFrameSampleIdx_.load(std::memory_order_relaxed);
    if (idx != SIZE_MAX) {
        EndGpuSample(cl, idx);
    }
    gpuFrameSampleIdx_.store(SIZE_MAX, std::memory_order_relaxed);
#else
    (void)cl;
#endif
}

size_t Profiler::BeginGpuSample(ID3D12GraphicsCommandList* cl, const ScopeNameKey& key) {
#if PROF_ENABLED
    if (!gpuInitialized_ || !cl) { return SIZE_MAX; }
    UINT start = 0;
    UINT end = 0;
    size_t idx = SIZE_MAX;
    {
        std::lock_guard<std::mutex> lk(gpuMtx_);
        if (nextGpuQuery_ + 1 >= maxGpuQueries_) { return SIZE_MAX; }
        start = nextGpuQuery_;
        end = start + 1;
        nextGpuQuery_ += 2;
        idx = gpuPending_.size();
        gpuPending_.push_back({ key, start, end, false, frameNo_ });
    }
    cl->EndQuery(gpuQueryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, start);
    return idx;
#else
    (void)cl; (void)key; return SIZE_MAX;
#endif
}

void Profiler::EndGpuSample(ID3D12GraphicsCommandList* cl, size_t idx) {
#if PROF_ENABLED
    if (!gpuInitialized_ || !cl || idx == SIZE_MAX) { return; }
    UINT start = 0;
    UINT end = 0;
    {
        std::lock_guard<std::mutex> lk(gpuMtx_);
        if (idx >= gpuPending_.size()) { return; }
        auto& range = gpuPending_[idx];
        if (range.completed) { return; }
        start = range.start;
        end = range.end;
        range.completed = true;
    }
    cl->EndQuery(gpuQueryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, end);
    const UINT64 readbackOffset =
        (static_cast<UINT64>(gpuRecordingReadbackSlot_) * static_cast<UINT64>(maxGpuQueries_) +
            static_cast<UINT64>(start)) * sizeof(UINT64);
    cl->ResolveQueryData(gpuQueryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
        start, 2, gpuReadback_.Get(), readbackOffset);
#else
    (void)cl; (void)idx;
#endif
}

void Profiler::ShutdownGpu() {
#if PROF_ENABLED
    if (!gpuInitialized_) {
        return;
    }

    {
        std::lock_guard<std::mutex> lk(gpuMtx_);
        gpuPending_.clear();
        gpuResolvedBatches_.clear();
    }

    gpuFrameSampleIdx_.store(SIZE_MAX, std::memory_order_relaxed);
    gpuQueryHeap_.Reset();
    gpuReadback_.Reset();
    gpuDrainFence_.Reset();
    if (gpuDrainFenceEvent_) {
        CloseHandle(gpuDrainFenceEvent_);
        gpuDrainFenceEvent_ = nullptr;
    }
    gpuQueue_ = nullptr;
    gpuFreq_ = 0;
    gpuTraceQpcFreq_ = 0;
    gpuTraceQpcOrigin_ = 0;
    gpuTraceCpuOriginUs_ = 0;
    gpuDrainFenceValue_ = 1;
    maxGpuQueries_ = 0;
    nextGpuQuery_ = 0;
    gpuRecordingReadbackSlot_ = 0;
    gpuInitialized_ = false;
#endif
}
#endif // PROF_GPU_ENABLED

void Profiler::EmitOverlay(TextManager* tm, int x, int y, int maxLines) {
    if (!tm || !GetEnabled()) { return; }
    CPU_SCOPE(kProfilerEmitOverlayKey);

    // Read the active read buffers WITHOUT locks
    const int readIdx = overlayReadBuf_.load(std::memory_order_acquire);
    const auto& rows = overlayRows_[readIdx];
#if PROF_GPU_ENABLED
    const int gpuReadIdx = gpuOverlayReadBuf_.load(std::memory_order_acquire);
    const auto& gpuRows = gpuOverlayRows_[gpuReadIdx];
#endif

    bool captureActive = false;
    bool capturePending = false;
    uint32_t captureRemaining = 0;
    uint32_t captureTotal = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        captureActive = traceCapturing_;
        capturePending = traceCaptureRequested_;
        captureRemaining = traceFramesRemaining_;
        captureTotal = traceRequestFrameCount_;
    }

    // Estimate the region width directly, without measuring individual lines
    // Row format: "%-35s  avg:%6.2f  max:%6.2f  p/u:%6.2f  usages:%u"
    const int namePad = 35;
    const int otherCols = 1 + 28;
    const int lineCols = namePad + otherCols + 16;
    const double charW = 0.60 * 21.0;
    const double needW = charW * (double)lineCols;
    overlayWidthPx_ = overlayWidthPx_ * 0.9 + needW * 0.1;
    const double boxW = std::max(overlayWidthPx_, 480.0);
#if PROF_GPU_ENABLED
    const bool hasGpuRows = !gpuRows.empty();
    double gpuBoxW = 0.0;
    if (hasGpuRows) {
        gpuOverlayWidthPx_ = gpuOverlayWidthPx_ * 0.9 + needW * 0.1;
        gpuBoxW = std::max(gpuOverlayWidthPx_, 480.0);
    }
#endif

    // CPU region
    auto reg = tm->CreateRegion(x, y, TextManager::Align::Left);
    tm->RegionSetPadding(reg, 8, 6);
    tm->RegionSetBackground(reg, float4(0.00f, 0.00f, 0.05f, 0.75f));
    tm->RegionSetFixedWidth(reg, (float)boxW);
    tm->RegionSetAutoMeasure(reg, false);
    tm->RegionSetKerning(reg, false);

    // Header
    tm->AddTextfShadow(reg, 18.0f, float4(1, 1, 0.6f, 0.95f), true,
        L"[CPU profiler] frame=%llu  (max reset: %.1fs, sort every: %.2fs)",
        (unsigned long long)frameNo_, GetMaxCooldownSeconds(), GetOverlayResortIntervalSeconds());

    if (captureActive || capturePending) {
        if (captureActive) {
            const uint32_t recorded = (captureTotal > captureRemaining) ? (captureTotal - captureRemaining) : 0u;
            tm->AddTextfShadow(reg, 16.0f, float4(1.0f, 0.85f, 0.25f, 0.95f), true,
                L"[Trace capture active] recorded:%u remaining:%u  (press F10 to stop)",
                recorded, captureRemaining);
        }
        else {
            tm->AddTextfShadow(reg, 16.0f, float4(0.7f, 0.85f, 1.0f, 0.95f), true,
                L"[Trace capture pending] frames:%u  (press F10 again to cancel)",
                captureTotal);
        }
    }

    tm->AddText(reg, 20.0f, float4(1, 1, 0.6f, 0.95f), L" ");

    // Rows
    int shown = 0;
    const float4 colOdd = { 1, 1, 1, 0.92f };
    const float4 colEven = { 0.5f, 0.5f, 0.5f, 0.92f };
    for (const auto& r : rows) {
        if (shown >= maxLines) { break; }
        tm->AddText(reg, 18.0f, (shown & 1) ? colOdd : colEven, r.formatted, true);
        shown++;
    }

#if PROF_GPU_ENABLED
    if (hasGpuRows) {
        // GPU region to the right of CPU
        const int gpuX = x + (int)boxW + 16;
        auto regGpu = tm->CreateRegion(gpuX, y, TextManager::Align::Left);
        tm->RegionSetPadding(regGpu, 8, 6);
        tm->RegionSetBackground(regGpu, float4(0.00f, 0.05f, 0.00f, 0.75f));
        tm->RegionSetFixedWidth(regGpu, (float)gpuBoxW);
        tm->RegionSetAutoMeasure(regGpu, false);
        tm->RegionSetKerning(regGpu, false);

        tm->AddTextfShadow(regGpu, 18.0f, float4(0.6f, 1, 0.6f, 0.95f), true,
            L"[GPU profiler] frame=%llu  (max reset: %.1fs, sort every: %.2fs)",
            (unsigned long long)frameNo_, GetMaxCooldownSeconds(), GetOverlayResortIntervalSeconds());
        tm->AddText(regGpu, 20.0f, float4(0.6f, 1, 0.6f, 0.95f), L" ");

        int gshown = 0;
        for (const auto& r : gpuRows) {
            if (gshown >= maxLines) { break; }
            tm->AddText(regGpu, 18.0f, (gshown & 1) ? colOdd : colEven, r.formatted, true);
            gshown++;
        }
    }
#endif
}

void Profiler::SetMaxCooldownSeconds(double sec) {
    if (sec < 0.1) { sec = 0.1; }
    std::lock_guard<std::mutex> lk(mtx_);
    maxResetIntervalSec_ = sec;
}

double Profiler::GetMaxCooldownSeconds() const {
    return maxResetIntervalSec_;
}

void Profiler::ResetMaxNow() {
    std::lock_guard<std::mutex> lk(mtx_);
    ResetMax_Unsafe();
    lastMaxReset_ = CoolClock::now();
}

void Profiler::ResetMax_Unsafe() {
    for (auto& kv : stats_) {
        kv.second.stats.maxMs = kv.second.stats.avgMs;
    }
    #if PROF_GPU_ENABLED
    for (auto& kv : gpuStats_) {
        kv.second.stats.maxMs = kv.second.stats.avgMs;
    }
    #endif
}

void Profiler::RequestTraceCapture(uint32_t frameCount) {
    if (frameCount == 0) { return; }
    std::lock_guard<std::mutex> lk(mtx_);
    if (traceCapturing_) {
        traceStopRequested_ = true;
        std::printf("Profiler trace capture stop requested\n");
        return;
    }
    if (traceCaptureRequested_) {
        traceCaptureRequested_ = false;
        traceRequestFrameCount_ = 0;
        std::printf("Profiler trace capture canceled\n");
        return;
    }
    traceCaptureRequested_ = true;
    traceRequestFrameCount_ = frameCount;
    traceStopRequested_ = false;
    std::printf("Profiler trace capture requested: %u frames\n", frameCount);
}

void Profiler::Tick()
{
#if PROF_ENABLED
    auto& input = Systems::GetInput();
    if (input.WasActionPressed("CaptureTrace")) {
        constexpr uint32_t kTraceFrames = 120;
        RequestTraceCapture(kTraceFrames);
    }
#endif
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

void Profiler::WriteTraceJson(const std::vector<TraceEvent>& events) {
    if (events.empty()) { return; }
    const uint32_t fileIdx = traceFileCounter_.fetch_add(1u, std::memory_order_relaxed);
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    std::ostringstream fname;
#if defined(NDEBUG)
    constexpr const char* kTraceBuildSuffix = "_release";
#else
    constexpr const char* kTraceBuildSuffix = "_debug";
#endif
    fname << "traces/trace_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << kTraceBuildSuffix
          << "_" << std::setw(3) << std::setfill('0') << fileIdx << ".json";
    const std::string fileName = fname.str();
    std::filesystem::create_directory("traces");
    std::ofstream out(fileName, std::ios::binary);
    if (!out) {
        std::printf("Failed to write profiler trace to %s\n", fileName.c_str());
        return;
    }

    out << "{\n\"traceEvents\":[\n";
    bool first = true;
    auto writeEntry = [&](const std::string& line) {
        if (!first) { out << ",\n"; }
        first = false;
        out << line;
    };

    static const std::wstring kUnknownWide = L"unknown";
    static const std::string  kUnknownNarrow = "unknown";
    robin_hood::unordered_flat_map<const std::wstring*, std::string> nameLookup;
    nameLookup.reserve(events.size());

#if PROF_GPU_ENABLED
    {
        std::ostringstream line;
        line << "  {\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":0,\"tid\":" << kGpuTraceThreadIndex
             << ",\"args\":{\"name\":\"" << EscapeJson(kGpuTraceThreadName) << "\"}}";
        writeEntry(line.str());
    }
#endif

    for (size_t tid = 0; tid < threadIndexToName_.size(); ++tid) {
        if (threadIndexToName_[tid].empty()) { continue; }
        std::ostringstream line;
        line << "  {\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":0,\"tid\":" << (tid + 1u)
             << ",\"args\":{\"name\":\"" << EscapeJson(threadIndexToName_[tid]) << "\"}}";
        writeEntry(line.str());
    }

    for (const auto& ev : events) {
        const std::wstring* nameWidePtr = ev.key.name;
        if (!nameWidePtr || nameWidePtr->empty()) {
            nameWidePtr = &kUnknownWide;
        }
        auto lookupIt = nameLookup.find(nameWidePtr);
        if (lookupIt == nameLookup.end()) {
            std::string narrow = WideToUtf8(*nameWidePtr);
            if (narrow.empty()) {
                narrow = kUnknownNarrow;
            }
            lookupIt = nameLookup.emplace(nameWidePtr, std::move(narrow)).first;
        }
        const std::string& name = lookupIt->second;
        const char* category = (ev.category == TraceEvent::Category::Gpu) ? "GPU" : "CPU";
        const uint64_t outputTid = TraceOutputThreadIndex(ev);
        std::ostringstream line;
        line << "  {\"name\":\"" << EscapeJson(name) << "\",\"cat\":\"" << category << "\",\"ph\":\"X\",\"ts\":"
             << ev.tsUs << ",\"dur\":" << ev.durUs << ",\"pid\":0,\"tid\":" << outputTid;
        if (ev.hasFrameNumber) {
            line << ",\"args\":{\"frame\":" << ev.frameNumber << "}";
        }
        line << "}";
        writeEntry(line.str());
    }

    out << "\n],\n\"displayTimeUnit\":\"us\"\n}\n";
    out.close();
    std::printf("Profiler trace saved to %s (%zu events)\n", fileName.c_str(), events.size());
}

#endif // PROF_ENABLED
