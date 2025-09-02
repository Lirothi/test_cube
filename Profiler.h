#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <thread>
#include <atomic>
#include <algorithm>

#ifndef PROF_ENABLED
#define PROF_ENABLED 1
#endif

class TextManager; // forward

// Профайлер CPU: скоупы, EMA-среднее, кулдаун сброса максимумов,
// темпоральная сортировка (сглаженный ранг) и дешёвый оверлей.
class Profiler {
public:
    struct ScopeSample {
        const char* name = nullptr; // ожидается литерал/статическая строка
        uint64_t    nameId = 0;     // резерв под быстрый id (не обязателен)
        double      ms = 0.0;
    };

    struct ScopeStats {
        // Инклюзивные накопители за кадр
        double   frameMsSum = 0.0;
        uint32_t frameCount = 0;

        // Показатели
        double   avgMs = 0.0;  // EMA
        double   maxMs = 0.0;  // сбрасывается по кулдауну
        uint32_t lastCount = 0; // сколько раз встретился скоуп в прошлом кадре (для usages)

        void Accumulate(double ms) {
            frameMsSum += ms;
            frameCount += 1u;
            if (ms > maxMs) { maxMs = ms; }
        }
        void CommitFrame(double emaAlpha) {
            if (frameCount == 0) { lastCount = 0; return; }
            const double cur = frameMsSum;
            if (avgMs <= 0.0) { avgMs = cur; }
            else { avgMs = avgMs * emaAlpha + cur * (1.0 - emaAlpha); }
            lastCount = frameCount;
            frameMsSum = 0.0;
            frameCount = 0;
        }
    };

public:
    static Profiler& Get();

    // Границы кадра
    void BeginFrame(uint64_t frameNo);
    void EndFrame();

    // Скоповая отметка (CPU)
    class ScopedCpu {
    public:
        ScopedCpu(const char* name, uint64_t nameId = 0) : name_(name), id_(nameId) {
#if PROF_ENABLED
            start_ = Clock::now();
#endif
        }
        ~ScopedCpu() {
#if PROF_ENABLED
            const auto end = Clock::now();
            const double ms = std::chrono::duration<double, std::milli>(end - start_).count();
            Profiler::Get().PushSample(name_, id_, ms);
#endif
        }
    private:
#if PROF_ENABLED
        using Clock = std::chrono::high_resolution_clock;
        Clock::time_point start_{};
#endif
        const char* name_;
        uint64_t    id_;
    };

#if PROF_ENABLED
#define CPU_SCOPE(nameLiteral)       Profiler::ScopedCpu _prof_scope_##__LINE__(nameLiteral, 0)
#define CPU_SCOPE_N(nameLiteral, id) Profiler::ScopedCpu _prof_scope_##__LINE__(nameLiteral, (id))
#else
#define CPU_SCOPE(nameLiteral)       do { } while (0)
#define CPU_SCOPE_N(nameLiteral, id) do { } while (0)
#endif

    // Оверлей с табличкой
    void EmitOverlay(TextManager* tm, int x = 8, int y = 48, int maxLines = 16);

    // Управление
    void SetEnabled(bool v) { enabled_.store(v, std::memory_order_relaxed); }
    bool GetEnabled() const { return enabled_.load(std::memory_order_relaxed); }

    // Кулдаун сброса максимумов
    void   SetMaxCooldownSeconds(double sec);
    double GetMaxCooldownSeconds() const;
    void   ResetMaxNow();

    // Темпоральная сортировка (0..1): чем больше — тем инерционнее порядок
    void   SetRankSmoothing(double alpha) {
#if PROF_ENABLED
        if (alpha < 0.0) { alpha = 0.0; }
        if (alpha > 0.99) { alpha = 0.99; }
        std::lock_guard<std::mutex> lk(mtx_);
        rankAlpha_ = alpha;
#else
        (void)alpha;
#endif
    }
    double GetRankSmoothing() const {
#if PROF_ENABLED
        return rankAlpha_;
#else
        return 0.0;
#endif
    }

    // Необязательно: имя потока (на будущее)
    void SetThreadName(const std::string& n) {
#if PROF_ENABLED
        std::lock_guard<std::mutex> lk(mtx_);
        threadNames_[std::this_thread::get_id()] = n;
#else
        (void)n;
#endif
    }

private:
    Profiler() = default;
    void PushSample(const char* name, uint64_t id, double ms);

#if PROF_ENABLED
    void ResetMax_Unsafe(); // вызывать под mtx_
    void UpdateSmoothRanks_Unsafe(const std::vector<std::pair<std::string, double>>& ranked); // под mtx_
#endif

private:
#if PROF_ENABLED
    std::mutex mtx_;
    std::unordered_map<std::string, ScopeStats> stats_;
    std::unordered_map<std::thread::id, std::string> threadNames_;

    std::vector<ScopeSample> frameSamples_;
    uint64_t  frameNo_ = 0;
    bool      frameOpen_ = false;

    // EMA для avgMs
    double emaAlpha_ = 0.99;

    std::atomic<bool> enabled_{ true };

    // Кулдаун сброса максимумов
    using CoolClock = std::chrono::steady_clock;
    CoolClock::time_point lastMaxReset_{};
    double maxResetIntervalSec_ = 3.0;

    // Темпоральная сортировка
    std::unordered_map<std::string, double> rankSmooth_; // сглаженный ранг (0 — верх)
    double rankAlpha_ = 0.99;

    // Ширина оверлей-региона (фикс) — оцениваем и сглаживаем, чтобы не мерить строки
    double overlayWidthPx_ = 640.0;
#endif
};

#if !PROF_ENABLED
inline Profiler& Profiler::Get() { static Profiler p; return p; }
inline void Profiler::BeginFrame(uint64_t) {}
inline void Profiler::EndFrame() {}
inline void Profiler::EmitOverlay(TextManager*, int, int, int) {}
inline void Profiler::SetMaxCooldownSeconds(double) {}
inline double Profiler::GetMaxCooldownSeconds() const { return 0.0; }
inline void Profiler::ResetMaxNow() {}
#endif