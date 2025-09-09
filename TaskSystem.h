#pragma once

#include <functional>
#include <vector>
#include <cstddef>
#include <memory>
#include <initializer_list>

#include "third_party/enkiTS/src/TaskScheduler.h"

class TaskSystem {
public:
    using Task = std::function<void()>;
    using TaskHandle = enki::ICompletable*;

    static TaskSystem& Get();

    void Start(unsigned threadCount = 0);
    void Stop();

    // manual handle-returning versions
    TaskHandle Submit(Task t);
    // staged creation to allow setting dependencies before firing
    TaskHandle CreateTask(Task t, std::size_t depCount = 0);
    TaskHandle CreateRangeTask(std::size_t jobCount,
                               std::function<void(std::size_t)> fn,
                               std::size_t batchSize = 1,
                               std::size_t depCount = 0);
    void SetDependencies(TaskHandle handle, const std::vector<TaskHandle>& deps);
    void Submit(TaskHandle handle);
    TaskHandle Dispatch(std::size_t jobCount,
                        std::function<void(std::size_t)> fn,
                        std::size_t batchSize = 1);

    // fire-and-forget versions (auto delete)
    void SubmitDetach(Task t);
    void DispatchDetach(std::size_t jobCount,
                        std::function<void(std::size_t)> fn,
                        std::size_t batchSize = 1);

    void Wait(TaskHandle handle);

    template<class F>
    static void ParallelFor(std::size_t jobCount, F&& fn, std::size_t batchSize)
    {
        auto h = Get().Dispatch(jobCount, std::forward<F>(fn), batchSize);
        Get().Wait(h);
    }

    template<class F>
    static void ParallelForNoHelp(std::size_t jobCount, F&& fn, std::size_t batchSize)
    {
        auto h = Get().Dispatch(jobCount, std::forward<F>(fn), batchSize);
        Get().Wait(h);
    }

    void WaitForAll();

    std::size_t ThreadIndex() const;

private:
    TaskSystem() = default;
    ~TaskSystem();

    TaskSystem(const TaskSystem&) = delete;
    TaskSystem& operator=(const TaskSystem&) = delete;

private:
    enki::TaskScheduler scheduler_;
};

