#include "Profiler.h"
#include <cwchar>
#include "third_party/robin_hood.h"
#include "TextManager.h"


#if PROF_ENABLED

Profiler& Profiler::Get() {
    static Profiler g;
    return g;
}

#if PROF_GPU_ENABLED
Profiler::ScopedGpu::ScopedGpu(ID3D12GraphicsCommandList* cl, const char* name, uint64_t id)
    : cl_(cl)
{
    idx_ = Profiler::Get().BeginGpuSample(cl, name, id);
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
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!frameOpen_) { return; }
        samples = std::move(frameSamples_);
        frameSamples_.clear();
#if PROF_GPU_ENABLED
        gpuSamples = std::move(gpuFrameSamples_);
        gpuFrameSamples_.clear();
#endif
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
    overlayTask_ = TaskSystem::Get().Submit([this, samples = std::move(samples), gpuSamples = std::move(gpuSamples)]() mutable {
#else
    overlayTask_ = TaskSystem::Get().Submit([this, samples = std::move(samples)]() mutable {
#endif
        const auto t0 = CoolClock::now();

        // A) свёртка сэмплов в stats_ (под мьютексом) + EMA/lastCount
        for (const auto& s : samples) {
            auto& st = stats_[s.name ? s.name : "unknown"];
            st.Accumulate(s.ms);
        }
        for (auto& kv : stats_) {
            kv.second.CommitFrame(emaAlpha_);
        }
        samples.clear();

#if PROF_GPU_ENABLED
        for (const auto& s : gpuSamples) {
            auto& st = gpuStats_[s.name ? s.name : "unknown"];
            st.Accumulate(s.ms);
        }
        for (auto& kv : gpuStats_) {
            kv.second.CommitFrame(emaAlpha_);
        }
        gpuSamples.clear();
#endif
        // B) формируем текущий набор строк (без лока)
        robin_hood::unordered_flat_map<std::wstring, OverlayRow> current;
        current.reserve(stats_.size() * 2u);
        for (const auto& kv : stats_) {
            OverlayRow row;
            row.name.assign(kv.first.begin(), kv.first.end());
            row.avgMs = kv.second.avgMs;
            row.maxMs = kv.second.maxMs;
            row.usages = kv.second.lastCount;
            const double perUse = (row.usages ? (row.avgMs / (double)row.usages) : 0.0);
            wchar_t buf[128];
            std::swprintf(buf, sizeof(buf) / sizeof(wchar_t),
                L"%-40s  avg:%6.2f  max:%6.2f  p/u:%6.3f  usages:%u",
                row.name.c_str(), row.avgMs, row.maxMs, perUse, row.usages);
            row.formatted = buf;
            current.emplace(row.name, std::move(row));
        }

#if PROF_GPU_ENABLED
        robin_hood::unordered_flat_map<std::wstring, OverlayRow> gpuCurrent;
        gpuCurrent.reserve(gpuStats_.size());
        for (const auto& kv : gpuStats_) {
            OverlayRow row;
            row.name.assign(kv.first.begin(), kv.first.end());
            row.avgMs = kv.second.avgMs;
            row.maxMs = kv.second.maxMs;
            row.usages = kv.second.lastCount;
            const double perUse = (row.usages ? (row.avgMs / (double)row.usages) : 0.0);
            wchar_t buf[128];
            std::swprintf(buf, sizeof(buf) / sizeof(wchar_t),
                L"%-40s  avg:%6.2f  max:%6.2f  p/u:%6.3f  usages:%u",
                row.name.c_str(), row.avgMs, row.maxMs, perUse, row.usages);
            row.formatted = buf;
            gpuCurrent.emplace(row.name, std::move(row));
        }
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
        writeRows.reserve(current.size() + 1u); // +1 под строку EndFrame.Async

#if PROF_GPU_ENABLED
        const int gpuReadIdx = gpuOverlayReadBuf_.load(std::memory_order_acquire);
        const int gpuWriteIdx = gpuReadIdx ^ 1;
        auto& gpuReadRows = gpuOverlayRows_[gpuReadIdx];
        auto& gpuWriteRows = gpuOverlayRows_[gpuWriteIdx];
        gpuWriteRows.clear();
        gpuWriteRows.reserve(gpuCurrent.size());
#endif

        if (needResort) {
            std::vector<OverlayRow> tmp; tmp.reserve(current.size());
            for (auto& kv : current) {
                tmp.push_back(std::move(kv.second));
            }
            std::sort(tmp.begin(), tmp.end(), [](const OverlayRow& a, const OverlayRow& b) {
                if (a.avgMs == b.avgMs) { return a.name < b.name; }
                return a.avgMs > b.avgMs;
            });
            writeRows = std::move(tmp);

#if PROF_GPU_ENABLED
            std::vector<OverlayRow> gtmp; gtmp.reserve(gpuCurrent.size());
            for (auto& kv : gpuCurrent) { gtmp.push_back(std::move(kv.second)); }
            std::sort(gtmp.begin(), gtmp.end(), [](const OverlayRow& a, const OverlayRow& b) {
                if (a.avgMs == b.avgMs) { return a.name < b.name; }
                return a.avgMs > b.avgMs;
            });
            gpuWriteRows = std::move(gtmp);
#endif
            lastOverlaySort_ = now;
        }
        else {
            robin_hood::unordered_flat_set<std::wstring> used; used.reserve(current.size());
            for (const OverlayRow& prev : readRows) {
                const std::wstring& key = prev.name;
                auto it = current.find(key);
                if (it != current.end()) {
                    writeRows.push_back(std::move(it->second));
                    used.insert(key);
                }
            }
            for (auto& kv : current) {
                if (used.find(kv.first) == used.end()) {
                    writeRows.push_back(std::move(kv.second));
                }
            }

#if PROF_GPU_ENABLED
            robin_hood::unordered_flat_set<std::wstring> gused; gused.reserve(gpuCurrent.size());
            for (const OverlayRow& prev : gpuReadRows) {
                const std::wstring& key = prev.name;
                auto it = gpuCurrent.find(key);
                if (it != gpuCurrent.end()) {
                    gpuWriteRows.push_back(std::move(it->second));
                    gused.insert(key);
                }
            }
            for (auto& kv : gpuCurrent) {
                if (gused.find(kv.first) == gused.end()) {
                    gpuWriteRows.push_back(std::move(kv.second));
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

            OverlayRow self;
            self.name = L"Profiler::EndFrame.Async";
            self.avgMs = endFrameAsyncAvgMs_;
            self.maxMs = endFrameAsyncMaxMs_;
            self.usages = 1u;
            wchar_t buf[128];
            std::swprintf(buf, sizeof(buf) / sizeof(wchar_t),
                L"%-40s  avg:%6.2f  max:%6.2f  p/u:%6.2f  usages:%u",
                self.name.c_str(), self.avgMs, self.maxMs, self.avgMs, self.usages);
            self.formatted = buf;
            writeRows.insert(writeRows.begin(), std::move(self));
        }

#if PROF_GPU_ENABLED
        // F) флипним read-буферы
        overlayReadBuf_.store(writeIdx, std::memory_order_release);
        gpuOverlayReadBuf_.store(gpuWriteIdx, std::memory_order_release);
#else
        overlayReadBuf_.store(writeIdx, std::memory_order_release);
#endif
    });
}

void Profiler::PushSample(const char* name, uint64_t /*id*/, double ms) {
    if (!GetEnabled()) { return; }
    std::lock_guard<std::mutex> lk(mtx_);
    if (!frameOpen_) { return; }
    frameSamples_.push_back({ name, 0u, ms });
}
#if PROF_GPU_ENABLED
void Profiler::PushGpuSample(const char* name, uint64_t /*id*/, double ms) {
    if (!GetEnabled()) { return; }
    std::lock_guard<std::mutex> lk(mtx_);
    if (!frameOpen_) { return; }
    gpuFrameSamples_.push_back({ name, 0u, ms });
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
                PushGpuSample(s.name, 0, ms);
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
    const size_t idx = BeginGpuSample(cl, "GPU.Frame", 0);
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

size_t Profiler::BeginGpuSample(ID3D12GraphicsCommandList* cl, const char* name, uint64_t id) {
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
        gpuPending_.push_back({ name, start, end, false });
    }
    cl->EndQuery(gpuQueryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, start);
    return idx;
#else
    (void)cl; (void)name; (void)id; return SIZE_MAX;
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
        kv.second.maxMs = kv.second.avgMs;
    }
    #if PROF_GPU_ENABLED
    for (auto& kv : gpuStats_) {
        kv.second.maxMs = kv.second.avgMs;
    }
    #endif
}

#endif // PROF_ENABLED
