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
    TaskHandle Submit(Task t, const std::vector<TaskHandle>& deps = {});
    TaskHandle Dispatch(std::size_t jobCount,
                        std::function<void(std::size_t)> fn,
                        std::size_t batchSize = 1,
                        const std::vector<TaskHandle>& deps = {});

    // fire-and-forget versions (auto delete)
    void SubmitDetach(Task t, const std::vector<TaskHandle>& deps = {});
    void DispatchDetach(std::size_t jobCount,
                        std::function<void(std::size_t)> fn,
                        std::size_t batchSize = 1,
                        const std::vector<TaskHandle>& deps = {});

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

