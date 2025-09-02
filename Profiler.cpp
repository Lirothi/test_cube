#include "Profiler.h"
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
}

void Profiler::EndFrame() {
    if (!GetEnabled()) { return; }
    std::lock_guard<std::mutex> lk(mtx_);
    if (!frameOpen_) { return; }

    // Свернуть сэмплы текущего кадра
    for (const auto& s : frameSamples_) {
        auto& st = stats_[s.name ? s.name : "unknown"];
        st.Accumulate(s.ms);
    }
    // Обновить EMA и usages
    for (auto& kv : stats_) {
        kv.second.CommitFrame(emaAlpha_);
    }

    // --- Сформируем упорядоченный список по avgMs для обновления сглаженных рангов ---
    std::vector<std::pair<std::string, double>> ranked;
    ranked.reserve(stats_.size());
    for (const auto& kv : stats_) {
        ranked.emplace_back(kv.first, kv.second.avgMs);
    }
    std::sort(ranked.begin(), ranked.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });
    UpdateSmoothRanks_Unsafe(ranked);

    // Кулдаун сброса максимумов
    const auto now = CoolClock::now();
    const double secSince = std::chrono::duration<double>(now - lastMaxReset_).count();
    if (secSince >= maxResetIntervalSec_) {
        ResetMax_Unsafe();
        lastMaxReset_ = now;
    }

    frameOpen_ = false;
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

    // Снимок под локом (имена, stats, сглаженные ранги)
    struct Row { std::string name; double avgMs = 0.0; double maxMs = 0.0; uint32_t usages = 0; double r = 0.0; };
    std::vector<Row> rows;
    rows.reserve(32);
    size_t maxNameLen = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        rows.reserve(stats_.size());
        for (const auto& kv : stats_) {
            Row r;
            r.name = kv.first;
            r.avgMs = kv.second.avgMs;
            r.maxMs = kv.second.maxMs;
            r.usages = kv.second.lastCount;
            auto it = rankSmooth_.find(kv.first);
            r.r = (it != rankSmooth_.end() ? it->second : 1e9);
            maxNameLen = std::max(maxNameLen, r.name.size());
            rows.push_back(std::move(r));
        }
    }

    // Темпоральная сортировка: по сглаженному рангу, затем по avgMs
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.r == b.r) { return a.avgMs > b.avgMs; }
        return a.r < b.r;
        });

    // --- оценка ширины региона «в лоб», без измерения строк ---
    // Формат строки: "%-40s  avg:%4.2f  max:%4.2f  usages:%d"
    // Имя мы всё равно кладём с паддингом 40, так что ширина почти константа.
    const int namePad = 40;
    const int otherCols = 1 /*двойной пробел и метки*/ + 24; // довольно грубо — но стабильно
    const int lineCols = namePad + otherCols + 16;          // небольшой запас под числа
    const double charW = 0.60 * 16.0;                       // моно-приближение для 16px шрифта
    const double needW = charW * (double)lineCols;
    // сглаживаем оценку, чтобы не дёргать фон
    overlayWidthPx_ = overlayWidthPx_ * 0.9 + needW * 0.1;
    const double boxW = std::max(overlayWidthPx_, 480.0); // нижняя граница

    // Рисуем через текст-регион (ЛЕВОЕ выравнивание, фиксированная ширина и БЕЗ измерений)
    auto reg = tm->CreateRegion(x, y, TextManager::Align::Left);
    tm->RegionSetPadding(reg, 8, 6);
    tm->RegionSetBackground(reg, float4(0.00f, 0.00f, 0.05f, 0.55f));
    tm->RegionSetFixedWidth(reg, (float)boxW);    // <— фиксируем ширину
    tm->RegionSetAutoMeasure(reg, false);         // <— и запрещаем измерять строки

    // заголовок
    tm->AddTextf(reg, 18.0f, float4(1, 1, 0.6f, 0.95f),
        "[CPU profiler] frame=%llu  (max reset: %.1fs, rankSmooth=%.2f)",
        (unsigned long long)frameNo_, GetMaxCooldownSeconds(), GetRankSmoothing());

    // строки
    int shown = 0;
    const float4 colOdd = { 1, 1, 1,   0.92f };
    const float4 colEven = { 0.5f, 0.5f, 0.5f, 0.92f };
    for (const auto& r : rows) {
        if (shown >= maxLines) { break; }
        tm->AddTextf(reg, 16.0f, (shown & 1) ? colOdd : colEven,
            "%-40s  avg:%4.2f  max:%4.2f  usages:%u",
            r.name.c_str(), r.avgMs, r.maxMs, r.usages);
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

void Profiler::UpdateSmoothRanks_Unsafe(const std::vector<std::pair<std::string, double>>& ranked) {
    const double oldW = rankAlpha_;
    const double newW = 1.0 - rankAlpha_;
    for (size_t i = 0; i < ranked.size(); ++i) {
        const std::string& name = ranked[i].first;
        const double rNow = (double)i; // текущий жёсткий ранг
        auto it = rankSmooth_.find(name);
        if (it == rankSmooth_.end()) {
            rankSmooth_.emplace(name, rNow);
        }
        else {
            it->second = it->second * oldW + rNow * newW;
        }
    }
}

#endif // PROF_ENABLED