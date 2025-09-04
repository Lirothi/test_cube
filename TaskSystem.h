#pragma once
#include <functional>
#include <vector>
#include <thread>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstddef>

struct TaskGroup {
    struct State {
        std::atomic<std::size_t> pending{ 0 };
        std::mutex               m;
        std::condition_variable  cv;
    };

    // разделяемое ядро группы — переживёт сам TaskGroup
    std::shared_ptr<State> state;

    TaskGroup() : state(std::make_shared<State>()) {}

    void Wait() {
        std::unique_lock<std::mutex> lk(state->m);
        state->cv.wait(lk, [this]() {
            return state->pending.load(std::memory_order_acquire) == 0;
            });
    }

    bool IsDone() const {
        return state->pending.load(std::memory_order_acquire) == 0;
    }
};

class TaskSystem {
public:
    using Task = std::function<void()>;

    // Глобальный доступ
    static TaskSystem& Get();

    // Запуск/остановка пула
    void Start(unsigned threadCount = 0);
    void Stop();

    void Submit(const Task& t, TaskGroup* group);
    void Submit(Task&& t, TaskGroup* group);

    void Dispatch(std::size_t jobCount,
        std::function<void(std::size_t)> fn,
        std::size_t batchSize,
        TaskGroup* group);

    void WaitGroup(struct TaskGroup* group);

	template<class F>
    static void ParallelFor(std::size_t jobCount, F&& fn, std::size_t batchSize)
    {
        TaskGroup g;
        Get().Dispatch(jobCount, std::forward<F>(fn), batchSize, &g);
        Get().WaitGroup(&g);
    }

    template<class F>
    static void ParallelForNoHelp(std::size_t jobCount, F&& fn, std::size_t batchSize)
    {
        TaskGroup g;
        Get().Dispatch(jobCount, std::forward<F>(fn), batchSize, &g);
        g.Wait();
    }

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
    std::vector<std::deque<Task>>   queues_;
    std::deque<Task>                globalQueue_;
    mutable std::mutex              mtx_;
    std::condition_variable         cvWork_;
    std::condition_variable         cvIdle_;
    std::atomic<bool>               running_{ false };
    std::atomic<std::size_t>        inFlight_{ 0 };

    static thread_local std::size_t tlsIndex_;

    bool HasTasksLocked_() const;
};