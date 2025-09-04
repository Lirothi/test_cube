#include "Profiler.h"
#include<unordered_set>
#include"third_party/robin_hood.h"
#include "TextManager.h"


#if PROF_ENABLED

Profiler& Profiler::Get() {
    static Profiler g;
    return g;
}

void Profiler::BeginFrame(uint64_t frameNo) {
    if (!GetEnabled()) { return; }
    std::lock_guard<std::mutex> lk(mtx_);
    frameSamples_.clear();
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
    TaskSystem::Get().WaitGroup(&overlayGroup_);

    // 1) заберём сэмплы текущего кадра и закроем кадр (быстро)
    std::vector<ScopeSample> samples;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!frameOpen_) { return; }
        samples = std::move(frameSamples_);
        frameSamples_.clear();
        frameOpen_ = false;
    }

    // 2) асинхронно свернём статистику и обновим дабл-буфер оверлея
    TaskSystem::Get().Submit([this, samples = std::move(samples)]() mutable {
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

        // B) формируем текущий набор строк (без лока)
        robin_hood::unordered_flat_map<std::wstring, OverlayRow> current;
        current.reserve(stats_.size() * 2u);
        for (const auto& kv : stats_) {
            OverlayRow row;
            row.name.assign(kv.first.begin(), kv.first.end());
            row.avgMs = kv.second.avgMs;
            row.maxMs = kv.second.maxMs;
            row.usages = kv.second.lastCount;
            current.emplace(row.name, std::move(row));
        }

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
            lastOverlaySort_ = now;
        }
        else {
            std::unordered_set<std::wstring> used; used.reserve(current.size());
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
            writeRows.insert(writeRows.begin(), std::move(self));
        }

        // F) флипним read-буфер
        overlayReadBuf_.store(writeIdx, std::memory_order_release);
        }, &overlayGroup_);
}

void Profiler::PushSample(const char* name, uint64_t /*id*/, double ms) {
    if (!GetEnabled()) { return; }
    std::lock_guard<std::mutex> lk(mtx_);
    if (!frameOpen_) { return; }
    frameSamples_.push_back({ name, 0u, ms });
}

void Profiler::EmitOverlay(TextManager* tm, int x, int y, int maxLines) {
    if (!tm || !GetEnabled()) { return; }
    CPU_SCOPE("Profiler::EmitOverlay");

    // читаем актуальный read-буфер БЕЗ локов
    const int readIdx = overlayReadBuf_.load(std::memory_order_acquire);
    const auto& rows = overlayRows_[readIdx];

    // оценка ширины региона «в лоб», без измерения строк
    // Формат строки: "%-40s  avg:%6.2f  max:%6.2f  p/u:%6.2f  usages:%u"
    const int namePad = 40;
    const int otherCols = 1 + 28;
    const int lineCols = namePad + otherCols + 16;
    const double charW = 0.60 * 16.0;
    const double needW = charW * (double)lineCols;
    overlayWidthPx_ = overlayWidthPx_ * 0.9 + needW * 0.1;
    const double boxW = std::max(overlayWidthPx_, 480.0);

    // рисуем через TextManager::Region (фикс ширины, без измерений)
    auto reg = tm->CreateRegion(x, y, TextManager::Align::Left);
    tm->RegionSetPadding(reg, 8, 6);
    tm->RegionSetBackground(reg, float4(0.00f, 0.00f, 0.05f, 0.55f));
    tm->RegionSetFixedWidth(reg, (float)boxW);
    tm->RegionSetAutoMeasure(reg, false);

    // заголовок
    tm->AddTextf(reg, 18.0f, float4(1, 1, 0.6f, 0.95f),
        L"[CPU profiler] frame=%llu  (max reset: %.1fs, sort every: %.2fs)",
        (unsigned long long)frameNo_, GetMaxCooldownSeconds(), GetOverlayResortIntervalSeconds());
    tm->AddTextf(reg, 18.0f, float4(1, 1, 0.6f, 0.95f), L" ");

    // строки
    int shown = 0;
    const float4 colOdd = { 1, 1, 1,   0.92f };
    const float4 colEven = { 0.5f, 0.5f, 0.5f, 0.92f };
    for (const auto& r : rows) {
        if (shown >= maxLines) { break; }
        const double perUse = (r.usages ? (r.avgMs / (double)r.usages) : 0.0);
        tm->AddTextf(reg, 16.0f, (shown & 1) ? colOdd : colEven,
            L"%-40s  avg:%6.2f  max:%6.2f  p/u:%6.2f  usages:%u",
            r.name.c_str(), r.avgMs, r.maxMs, perUse, r.usages);
        shown++;
    }
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
}

#endif // PROF_ENABLED