#pragma once

#include <functional>
#include <vector>
#include <cstddef>
#include <memory>

#include "third_party/enkiTS/src/TaskScheduler.h"

class TaskSystem;

struct TaskGroup {
    std::vector<std::unique_ptr<enki::ITaskSet>> tasks;
    void Wait();
};

class TaskSystem {
public:
    using Task = std::function<void()>;

    static TaskSystem& Get();

    void Start(unsigned threadCount = 0);
    void Stop();

    void Submit(const Task& t, TaskGroup* group = nullptr);
    void Submit(Task&& t, TaskGroup* group = nullptr);

    void Dispatch(std::size_t jobCount,
                  std::function<void(std::size_t)> fn,
                  std::size_t batchSize = 1,
                  TaskGroup* group = nullptr);

    void WaitGroup(TaskGroup* group);

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
        Get().WaitGroup(&g);
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

inline void TaskGroup::Wait() { TaskSystem::Get().WaitGroup(this); }

