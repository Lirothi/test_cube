#include "Profiler.h"
#include <cwchar>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <cstdio>
#include <ctime>
#include <codecvt>
#include <filesystem>
#include "third_party/robin_hood.h"
#include "TextManager.h"

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

struct TraceDumpData {
    std::vector<Profiler::TraceEvent> events;
    std::vector<std::string> threadNames;
};

std::wstring BuildWideName(const Profiler::ScopeNameKey& key) {
    if (!key.namePtr) {
        return L"unknown";
    }
    if (key.isWide) {
        const wchar_t* ws = static_cast<const wchar_t*>(key.namePtr);
        return ws ? std::wstring(ws) : std::wstring(L"unknown");
    }
    const char* cs = static_cast<const char*>(key.namePtr);
    if (!cs) {
        return L"unknown";
    }
    std::wstring result;
    while (*cs) {
        result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*cs)));
        ++cs;
    }
    if (result.empty()) {
        result = L"unknown";
    }
    return result;
}

std::string WideToUtf8(const std::wstring& input) {
    if (input.empty()) { return std::string(); }
    std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
    return conv.to_bytes(input);
}

void FormatOverlayRow(Profiler::OverlayRow& row, const std::wstring* name) {
    if (!name) {
        static const std::wstring kUnknown = L"unknown";
        row.namePtr = &kUnknown;
    }
    else {
        row.namePtr = name;
    }
    const std::wstring& n = *row.namePtr;
    const double perUse = (row.usages ? (row.avgMs / static_cast<double>(row.usages)) : 0.0);
    wchar_t buf[192];
    std::swprintf(buf, sizeof(buf) / sizeof(wchar_t),
        L"%-40s  avg:%6.2f  max:%6.2f  p/u:%6.3f  usages:%u",
        n.c_str(), row.avgMs, row.maxMs, perUse, row.usages);
    row.formatted.assign(buf);
}

} // namespace


#if PROF_ENABLED

Profiler& Profiler::Get() {
    static Profiler g;
    return g;
}

#if PROF_GPU_ENABLED
Profiler::ScopedGpu::ScopedGpu(ID3D12GraphicsCommandList* cl, const char* name, uint64_t id)
    : cl_(cl)
{
    idx_ = Profiler::Get().BeginGpuSample(cl, ScopeNameKey::FromNarrow(name, id));
}

Profiler::ScopedGpu::ScopedGpu(ID3D12GraphicsCommandList* cl, const wchar_t* name, uint64_t id)
    : cl_(cl)
{
    idx_ = Profiler::Get().BeginGpuSample(cl, ScopeNameKey::FromWide(name, id));
}

Profiler::ScopedGpu::~ScopedGpu() {
    Profiler::Get().EndGpuSample(cl_, idx_);
}
#endif

void Profiler::BeginFrame(uint64_t frameNo) {
    if (!GetEnabled()) { return; }
    std::lock_guard<std::mutex> lk(mtx_);
    frameSamples_.clear();
#if PROF_GPU_ENABLED
    gpuFrameSamples_.clear();
    gpuFrameSampleIdx_.store(SIZE_MAX, std::memory_order_relaxed);
#endif
    frameNo_ = frameNo;
    frameOpen_ = true;
    frameCpuStart_ = CpuClock::now();

    if (traceCaptureRequested_ && !traceCapturing_ && traceRequestFrameCount_ > 0) {
        traceCaptureRequested_ = false;
        traceCapturing_ = true;
        traceFramesRemaining_ = traceRequestFrameCount_;
        traceEvents_.clear();
        traceStartUs_ = 0;
        traceStartSet_ = false;
        traceStopRequested_ = false;
    }

    if (traceCapturing_ && !traceStartSet_) {
        traceStartUs_ = ToMicroseconds(frameCpuStart_);
        traceStartSet_ = true;
    }

    if (lastMaxReset_.time_since_epoch().count() == 0) {
        lastMaxReset_ = CoolClock::now();
    }
    if (lastOverlaySort_.time_since_epoch().count() == 0) {
        lastOverlaySort_ = CoolClock::now();
    }
}

void Profiler::EndFrame() {
    if (!GetEnabled()) { return; }

    // 0) ждём прошлую асинхронную сборку (минимум работы в главном потоке)
    TaskSystem::Get().Wait(overlayTask_);
    overlayTask_ = nullptr;

    // 1) заберём сэмплы текущего кадра и закроем кадр (быстро)
    std::vector<ScopeSample> samples;
#if PROF_GPU_ENABLED
    std::vector<ScopeSample> gpuSamples;
#endif
    TraceDumpData traceDump;
    bool haveTraceDump = false;
    const auto frameEnd = CpuClock::now();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!frameOpen_) { return; }
        samples = std::move(frameSamples_);
        frameSamples_.clear();
#if PROF_GPU_ENABLED
        gpuSamples = std::move(gpuFrameSamples_);
        gpuFrameSamples_.clear();
#endif
        if (traceCapturing_) {
            const uint64_t startUs = ToMicroseconds(frameCpuStart_);
            const uint64_t endUs = ToMicroseconds(frameEnd);
            const uint64_t durUs = (endUs > startUs) ? (endUs - startUs) : 0;
            const uint32_t threadIdx = GetThreadIndex_Locked(std::this_thread::get_id());
            TraceEvent fev;
            fev.name = "Frame " + std::to_string(frameNo_);
            fev.tsUs = (startUs >= traceStartUs_) ? (startUs - traceStartUs_) : 0;
            fev.durUs = durUs;
            fev.threadIndex = threadIdx;
            traceEvents_.push_back(std::move(fev));

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
                traceCapturing_ = false;
                traceStartUs_ = 0;
                traceStartSet_ = false;
                traceDump.events = std::move(traceEvents_);
                traceDump.threadNames = threadIndexToName_;
                haveTraceDump = true;
                traceRequestFrameCount_ = 0;
            }
        }
        frameOpen_ = false;
    }

#if PROF_GPU_ENABLED
    // 2) prepare gpu samples for next frame
    {
        std::lock_guard<std::mutex> lk(gpuMtx_);
        gpuResolved_ = std::move(gpuPending_);
        gpuPending_.clear();
        lastGpuQueryCount_ = nextGpuQuery_;
        nextGpuQuery_ = 0;
    }
#endif

#if PROF_GPU_ENABLED
    overlayTask_ = TaskSystem::Get().Submit([this, samples = std::move(samples), gpuSamples = std::move(gpuSamples), traceDump = std::move(traceDump), haveTraceDump]() mutable {
#else
    overlayTask_ = TaskSystem::Get().Submit([this, samples = std::move(samples), traceDump = std::move(traceDump), haveTraceDump]() mutable {
#endif
        const auto t0 = CoolClock::now();

        // A) свёртка сэмплов в stats_ (без локов) + EMA/lastCount
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

        accumulateSamples(samples, stats_, nextOverlayId_);
        samples.clear();

#if PROF_GPU_ENABLED
        accumulateSamples(gpuSamples, gpuStats_, nextGpuOverlayId_);
        gpuSamples.clear();
#endif

        // B) формируем текущий набор строк, переиспользуя выделенные буферы
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

        // C) редкая сортировка или стабильное обновление порядка
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
                    const std::wstring& na = *ra.namePtr;
                    const std::wstring& nb = *rb.namePtr;
                    if (na == nb) { return ra.entryId < rb.entryId; }
                    return na < nb;
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
                    const std::wstring& na = *ra.namePtr;
                    const std::wstring& nb = *rb.namePtr;
                    if (na == nb) { return ra.entryId < rb.entryId; }
                    return na < nb;
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

        // D) кулдаун сброса максимумов (под локаом для stats_)
        {
            const double secSinceMax = std::chrono::duration<double>(now - lastMaxReset_).count();
            if (secSinceMax >= maxResetIntervalSec_) {
                std::lock_guard<std::mutex> lk(mtx_);
                ResetMax_Unsafe();
                lastMaxReset_ = now;
                endFrameAsyncMaxMs_ = 0.0;
            }
        }

        // E) измерим саму асинхронную фазу и вставим первым пунктом
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
        // F) флипним read-буферы
        overlayReadBuf_.store(writeIdx, std::memory_order_release);
        gpuOverlayReadBuf_.store(gpuWriteIdx, std::memory_order_release);
#else
        overlayReadBuf_.store(writeIdx, std::memory_order_release);
#endif

        if (haveTraceDump && !traceDump.events.empty()) {
            WriteTraceJson(traceDump.events, traceDump.threadNames);
        }
    });
}

void Profiler::PushSample(const ScopeNameKey& key, CpuClock::time_point start, CpuClock::time_point end) {
    if (!GetEnabled()) { return; }
    std::lock_guard<std::mutex> lk(mtx_);
    if (!frameOpen_) { return; }
    const uint64_t startUs = ToMicroseconds(start);
    const uint64_t endUs = ToMicroseconds(end);
    const uint64_t durUs = (endUs > startUs) ? (endUs - startUs) : 0;
    const uint32_t threadIdx = GetThreadIndex_Locked(std::this_thread::get_id());
    const double ms = std::chrono::duration<double, std::milli>(end - start).count();
    frameSamples_.push_back({ key, ms });
    if (traceCapturing_) {
        if (!traceStartSet_) {
            traceStartUs_ = startUs;
            traceStartSet_ = true;
        }
        TraceEvent ev;
        ev.isWide = key.isWide;
        if (key.isWide) {
            const wchar_t* ws = static_cast<const wchar_t*>(key.namePtr);
            if (ws) {
                ev.wideName.assign(ws);
            }
            else {
                ev.wideName = L"unknown";
            }
            ev.narrowName = WideToUtf8(ev.wideName);
        }
        else {
            const char* cs = static_cast<const char*>(key.namePtr);
            ev.narrowName = cs ? std::string(cs) : std::string("unknown");
        }
        ev.tsUs = (startUs >= traceStartUs_) ? (startUs - traceStartUs_) : 0;
        ev.durUs = durUs;
        ev.threadIndex = threadIdx;
        traceEvents_.push_back(std::move(ev));
    }
}
#if PROF_GPU_ENABLED
void Profiler::PushGpuSample(const ScopeNameKey& key, double ms) {
    if (!GetEnabled()) { return; }
    std::lock_guard<std::mutex> lk(mtx_);
    if (!frameOpen_) { return; }
    gpuFrameSamples_.push_back({ key, ms });
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

    UINT64 bufSize = (UINT64)maxQueries * sizeof(UINT64);
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
    gpuInitialized_ = true;
#else
    (void)device; (void)queue; (void)maxQueries;
#endif
}

void Profiler::CollectGpuResults() {
#if PROF_ENABLED
    if (!gpuInitialized_) { return; }

    std::vector<GpuSampleRange> resolved;
    UINT queryCount = 0;
    {
        std::lock_guard<std::mutex> lk(gpuMtx_);
        if (gpuResolved_.empty()) { return; }
        resolved = std::move(gpuResolved_);
        gpuResolved_.clear();
        queryCount = lastGpuQueryCount_;
    }

    UINT64* data = nullptr;
    D3D12_RANGE range{ 0, (SIZE_T)queryCount * sizeof(UINT64) };
    if (SUCCEEDED(gpuReadback_->Map(0, &range, reinterpret_cast<void**>(&data)))) {
        for (const auto& s : resolved) {
            if (s.completed && s.start < queryCount && s.end < queryCount) {
                UINT64 a = data[s.start];
                UINT64 b = data[s.end];
                double ms = (double)(b - a) * 1000.0 / (double)gpuFreq_;
                PushGpuSample(s.key, ms);
            }
        }
        gpuReadback_->Unmap(0, nullptr);
    }
#endif
}

void Profiler::BeginGpuFrame(ID3D12GraphicsCommandList* cl) {
#if PROF_ENABLED
    if (!GetEnabled()) {
        gpuFrameSampleIdx_.store(SIZE_MAX, std::memory_order_relaxed);
        return;
    }
    const size_t idx = BeginGpuSample(cl, ScopeNameKey::FromNarrow("GPU.Frame", 0));
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
        gpuPending_.push_back({ key, start, end, false });
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
    cl->ResolveQueryData(gpuQueryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
        start, 2, gpuReadback_.Get(), start * sizeof(UINT64));
#else
    (void)cl; (void)idx;
#endif
}
#endif // PROF_GPU_ENABLED

void Profiler::EmitOverlay(TextManager* tm, int x, int y, int maxLines) {
    if (!tm || !GetEnabled()) { return; }
    CPU_SCOPE("Profiler::EmitOverlay");

    // читаем актуальные read-буферы БЕЗ локов
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

    // оценка ширины региона «в лоб», без измерения строк
    // Формат строки: "%-40s  avg:%6.2f  max:%6.2f  p/u:%6.2f  usages:%u"
    const int namePad = 40;
    const int otherCols = 1 + 28;
    const int lineCols = namePad + otherCols + 16;
    const double charW = 0.60 * 16.0;
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
    tm->RegionSetBackground(reg, float4(0.00f, 0.00f, 0.05f, 0.55f));
    tm->RegionSetFixedWidth(reg, (float)boxW);
    tm->RegionSetAutoMeasure(reg, false);

    // заголовок
    tm->AddTextf(reg, 18.0f, float4(1, 1, 0.6f, 0.95f),
        L"[CPU profiler] frame=%llu  (max reset: %.1fs, sort every: %.2fs)",
        (unsigned long long)frameNo_, GetMaxCooldownSeconds(), GetOverlayResortIntervalSeconds());

    if (captureActive || capturePending) {
        if (captureActive) {
            const uint32_t recorded = (captureTotal > captureRemaining) ? (captureTotal - captureRemaining) : 0u;
            tm->AddTextf(reg, 16.0f, float4(1.0f, 0.85f, 0.25f, 0.95f),
                L"[Trace capture active] recorded:%u remaining:%u  (press F10 to stop)",
                recorded, captureRemaining);
        }
        else {
            tm->AddTextf(reg, 16.0f, float4(0.7f, 0.85f, 1.0f, 0.95f),
                L"[Trace capture pending] frames:%u  (press F10 again to cancel)",
                captureTotal);
        }
    }

    tm->AddText(reg, 18.0f, float4(1, 1, 0.6f, 0.95f), L" ");

    // строки
    int shown = 0;
    const float4 colOdd = { 1, 1, 1,   0.92f };
    const float4 colEven = { 0.5f, 0.5f, 0.5f, 0.92f };
    for (const auto& r : rows) {
        if (shown >= maxLines) { break; }
        tm->AddText(reg, 16.0f, (shown & 1) ? colOdd : colEven, r.formatted);
        shown++;
    }

#if PROF_GPU_ENABLED
    if (hasGpuRows) {
        // GPU region to the right of CPU
        const int gpuX = x + (int)boxW + 16;
        auto regGpu = tm->CreateRegion(gpuX, y, TextManager::Align::Left);
        tm->RegionSetPadding(regGpu, 8, 6);
        tm->RegionSetBackground(regGpu, float4(0.00f, 0.05f, 0.00f, 0.55f));
        tm->RegionSetFixedWidth(regGpu, (float)gpuBoxW);
        tm->RegionSetAutoMeasure(regGpu, false);

        tm->AddTextf(regGpu, 18.0f, float4(0.6f, 1, 0.6f, 0.95f),
            L"[GPU profiler] frame=%llu  (max reset: %.1fs, sort every: %.2fs)",
            (unsigned long long)frameNo_, GetMaxCooldownSeconds(), GetOverlayResortIntervalSeconds());
        tm->AddText(regGpu, 18.0f, float4(0.6f, 1, 0.6f, 0.95f), L" ");

        int gshown = 0;
        for (const auto& r : gpuRows) {
            if (gshown >= maxLines) { break; }
            tm->AddText(regGpu, 16.0f, (gshown & 1) ? colOdd : colEven, r.formatted);
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

void Profiler::WriteTraceJson(const std::vector<TraceEvent>& events, const std::vector<std::string>& threadNames) {
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
    fname << "traces/trace_" << std::put_time(&tm, "%Y%m%d_%H%M%S")
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

    for (size_t tid = 0; tid < threadNames.size(); ++tid) {
        if (threadNames[tid].empty()) { continue; }
        std::ostringstream line;
        line << "  {\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":0,\"tid\":" << tid
             << ",\"args\":{\"name\":\"" << EscapeJson(threadNames[tid]) << "\"}}";
        writeEntry(line.str());
    }

    for (const auto& ev : events) {
        std::string name = ev.narrowName;
        if (name.empty() && !ev.wideName.empty()) {
            name = WideToUtf8(ev.wideName);
        }
        if (name.empty()) { name = "unknown"; }
        std::ostringstream line;
        line << "  {\"name\":\"" << EscapeJson(name) << "\",\"cat\":\"CPU\",\"ph\":\"X\",\"ts\":"
             << ev.tsUs << ",\"dur\":" << ev.durUs << ",\"pid\":0,\"tid\":" << ev.threadIndex << "}";
        writeEntry(line.str());
    }

    out << "\n],\n\"displayTimeUnit\":\"us\"\n}\n";
    out.close();
    std::printf("Profiler trace saved to %s (%zu events)\n", fileName.c_str(), events.size());
}

#endif // PROF_ENABLED
