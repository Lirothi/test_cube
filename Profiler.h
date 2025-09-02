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

// === Публичный API ============================================================
//
// 1) В начале/конце кадра:
//      Profiler::Get().BeginFrame(frameNumber);
//      Profiler::Get().EndFrame();
//
// 2) Скоповые метки в коде:
//      CPU_SCOPE("GBuffer");
//      CPU_SCOPE_N("MyScope", 123);
//
// 3) Оверлей:
//      Profiler::Get().EmitOverlay(tm, 8, 48, 16);
//
// 4) Настройка кулдауна сброса максимумов (секунды):
//      Profiler::Get().SetMaxCooldownSeconds(5.0);
//      Profiler::Get().ResetMaxNow();
//
// 5) Необязательно: имя потока (для будущего расширения)
//      Profiler::Get().SetThreadName("Worker-0");
//
// Релиз можно собрать с PROF_ENABLED 0 — всё отключится компилятором.
//

class TextManager; // forward

class Profiler {
public:
    struct ScopeSample {
        const char* name = nullptr;  // ожидается литерал/статическая строка
        uint64_t    nameId = 0;      // резерв под быстрый id (не обязателен)
        double      ms = 0.0;
    };

    struct ScopeStats {
        // агрегаты за текущий кадр (инклюзивные)
        double   frameMsSum = 0.0;
        uint32_t frameCount = 0;

        // сглаженное среднее и максимум
        double   avgMs = 0.0;  // EMA
        double   maxMs = 0.0;

        void Accumulate(double ms) {
            frameMsSum += ms;
            frameCount += 1u;
            if (ms > maxMs) { maxMs = ms; }
        }
        void CommitFrame(double emaAlpha) {
            if (frameCount == 0) { return; }
            const double cur = frameMsSum;
            if (avgMs <= 0.0) { avgMs = cur; }
            else { avgMs = avgMs * emaAlpha + cur * (1.0 - emaAlpha); }
            frameMsSum = 0.0;
            frameCount = 0;
        }
    };

public:
    static Profiler& Get();

    // Кадровые границы
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

    // Макросы-обёртки
#if PROF_ENABLED
#define CPU_SCOPE(nameLiteral)       Profiler::ScopedCpu _prof_scope_##__LINE__(nameLiteral, 0)
#define CPU_SCOPE_N(nameLiteral, id) Profiler::ScopedCpu _prof_scope_##__LINE__(nameLiteral, (id))
#else
#define CPU_SCOPE(nameLiteral)       do { } while (0)
#define CPU_SCOPE_N(nameLiteral, id) do { } while (0)
#endif

    // Оверлей
    void EmitOverlay(TextManager* tm, int x = 8, int y = 48, int maxLines = 16);

    // Необязательно: имя потока
    void SetThreadName(const std::string& n) {
#if PROF_ENABLED
        std::lock_guard<std::mutex> lk(mtx_);
        threadNames_[std::this_thread::get_id()] = n;
#else
        (void)n;
#endif
    }

    // Рантайм-переключатель
    void SetEnabled(bool v) { enabled_.store(v, std::memory_order_relaxed); }
    bool GetEnabled() const { return enabled_.load(std::memory_order_relaxed); }

    // === Настройки/управление кулдауном сброса максимумов ===
    void   SetMaxCooldownSeconds(double sec);
    double GetMaxCooldownSeconds() const;
    void   ResetMaxNow(); // принудительный сброс maxMs

private:
    Profiler() = default;
    void PushSample(const char* name, uint64_t id, double ms);

#if PROF_ENABLED
    void ResetMax_Unsafe(); // без лока, вызывать под mtx_
#endif

private:
#if PROF_ENABLED
    std::mutex mtx_;
    std::unordered_map<std::string, ScopeStats> stats_;
    std::unordered_map<std::thread::id, std::string> threadNames_;

    std::vector<ScopeSample> frameSamples_;
    uint64_t  frameNo_ = 0;
    bool      frameOpen_ = false;

    // EMA коэффициент (0..1): чем больше, тем инерционнее среднее
    double emaAlpha_ = 0.90;

    // Включено/выключено
    std::atomic<bool> enabled_{ true };

    // Кулдаун сброса максимумов
    using CoolClock = std::chrono::steady_clock;
    CoolClock::time_point lastMaxReset_{};
    double maxResetIntervalSec_ = 3.0; // дефолт: раз в 8 секунд
#endif
};

#if !PROF_ENABLED
// Заглушки в выключенной сборке
inline Profiler& Profiler::Get() { static Profiler p; return p; }
inline void Profiler::BeginFrame(uint64_t) {}
inline void Profiler::EndFrame() {}
inline void Profiler::EmitOverlay(TextManager*, int, int, int) {}
inline void Profiler::SetMaxCooldownSeconds(double) {}
inline double Profiler::GetMaxCooldownSeconds() const { return 0.0; }
inline void Profiler::ResetMaxNow() {}
#endif