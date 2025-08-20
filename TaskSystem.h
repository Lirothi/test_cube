#pragma once
#include <functional>
#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstddef>

class TaskSystem {
public:
    using Task = std::function<void()>;

    // Глобальный доступ
    static TaskSystem& Get();

    // Запуск/остановка пула
    void Start(unsigned threadCount = 0);
    void Stop();

    // Постановка задач
    void Submit(const Task& t);
    void Submit(Task&& t);

    // Распараллеливание "N одинаковых работ" батчами (по умолчанию по 1)
    void Dispatch(std::size_t jobCount,
        std::function<void(std::size_t)> fn,
        std::size_t batchSize = 1);

    void WaitForAll();

    // Индекс воркера (0..threads-1) или SIZE_MAX, если внешний поток
    std::size_t ThreadIndex() const;

private:
    TaskSystem() = default;
    ~TaskSystem();

    TaskSystem(const TaskSystem&) = delete;
    TaskSystem& operator=(const TaskSystem&) = delete;

    void WorkerLoop_(std::size_t index);

private:
    std::vector<std::thread>        workers_;
    std::queue<Task>                queue_;
    mutable std::mutex              mtx_;
    std::condition_variable         cvWork_;
    std::condition_variable         cvIdle_;
    std::atomic<bool>               running_{ false };
    std::atomic<std::size_t>        inFlight_{ 0 };

    static thread_local std::size_t tlsIndex_;
};