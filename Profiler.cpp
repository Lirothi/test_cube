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

    // Инициализация «старта» кулдауна при первом кадре
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
    // Обновить EMA и подготовиться к следующему кадру
    for (auto& kv : stats_) {
        kv.second.CommitFrame(emaAlpha_);
    }

    // Кулдаун: периодически сбрасываем maxMs (чтобы «шип» не висел вечность)
    const auto now = CoolClock::now();
    const double secSince = std::chrono::duration<double>(now - lastMaxReset_).count();
    if (secSince >= maxResetIntervalSec_) {
        ResetMax_Unsafe();            // сбросить maxMs у всех скоупов
        lastMaxReset_ = now;          // начать новый цикл
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

    std::vector<std::pair<std::string, ScopeStats>> tmp;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        tmp.reserve(stats_.size());
        for (const auto& kv : stats_) {
            tmp.push_back(kv);
        }
    }

    std::sort(tmp.begin(), tmp.end(),
        [](const auto& a, const auto& b) { return a.second.avgMs > b.second.avgMs; });

    const auto headerCol = float4(1, 1, 0.6f, 0.90f);
    const auto lineCol = float4(1, 1, 1, 0.85f);

    tm->AddTextf(x, y, headerCol, 18.0f, "[CPU profiler] frame=%llu",
        (unsigned long long)frameNo_, GetMaxCooldownSeconds());
    y += 20;

    int shown = 0;
    for (const auto& kv : tmp) {
        if (shown >= maxLines) { break; }
        const auto& name = kv.first;
        const auto& st = kv.second;
        tm->AddTextf(x, y, lineCol, 16.0f, "%-24s  avg:%4.2f   max:%4.2f", name.c_str(), st.avgMs, st.maxMs);
        y += 18;
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
        // Можно сбрасывать в 0 или в текущее EMA. Выберем EMA — так линия max не «падает» в ноль.
        kv.second.maxMs = kv.second.avgMs;
    }
}

#endif // PROF_ENABLED