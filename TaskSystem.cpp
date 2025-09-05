#include "TaskSystem.h"

TaskSystem& TaskSystem::Get() {
    static TaskSystem g;
    return g;
}

TaskSystem::~TaskSystem() {
    Stop();
}

void TaskSystem::Start(unsigned threadCount) {
    if (threadCount == 0) {
        scheduler_.Initialize();
    } else {
        scheduler_.Initialize(threadCount + 1); // include main thread
    }
}

void TaskSystem::Stop() {
    scheduler_.WaitforAllAndShutdown();
}

namespace {
struct LambdaTaskSet : enki::ITaskSet {
    TaskSystem::Task fn;
    LambdaTaskSet(TaskSystem::Task&& f)
        : enki::ITaskSet(1), fn(std::move(f)) {}
    void ExecuteRange(enki::TaskSetPartition, uint32_t) override {
        if (fn) {
            fn();
        }
    }
};

struct RangeTaskSet : enki::ITaskSet {
    std::function<void(std::size_t)> fn;
    RangeTaskSet(std::size_t count, std::function<void(std::size_t)> f)
        : enki::ITaskSet(static_cast<uint32_t>(count)), fn(std::move(f)) {}
    void ExecuteRange(enki::TaskSetPartition range, uint32_t) override {
        for (uint32_t i = range.start; i < range.end; ++i) {
            fn(i);
        }
    }
};

// Helper which deletes the associated task once it completes.
struct AutoDelete : enki::ICompletable {
    enki::ITaskSet* task;
    enki::Dependency dep;
    explicit AutoDelete(enki::ITaskSet* t) : task(t) {
        SetDependency(dep, task);
    }
    void OnDependenciesComplete(enki::TaskScheduler*, uint32_t) override {
        delete task;
        delete this;
    }
};
}

void TaskSystem::Submit(const Task& t, TaskGroup* group) { Submit(Task(t), group); }

void TaskSystem::Submit(Task&& t, TaskGroup* group) {
    if (group) {
        auto taskPtr = std::make_unique<LambdaTaskSet>(std::move(t));
        scheduler_.AddTaskSetToPipe(taskPtr.get());
        group->tasks.push_back(std::move(taskPtr));
    } else {
        auto* taskPtr = new LambdaTaskSet(std::move(t));
        new AutoDelete(taskPtr);
        scheduler_.AddTaskSetToPipe(taskPtr);
    }
}

void TaskSystem::Dispatch(std::size_t jobCount,
                          std::function<void(std::size_t)> fn,
                          std::size_t batchSize,
                          TaskGroup* group) {
    if (jobCount == 0 || !fn) { return; }
    if (batchSize == 0) { batchSize = 1; }
    if (group) {
        auto taskPtr = std::make_unique<RangeTaskSet>(jobCount, std::move(fn));
        taskPtr->m_MinRange = static_cast<uint32_t>(batchSize);
        scheduler_.AddTaskSetToPipe(taskPtr.get());
        group->tasks.push_back(std::move(taskPtr));
    } else {
        auto* taskPtr = new RangeTaskSet(jobCount, std::move(fn));
        taskPtr->m_MinRange = static_cast<uint32_t>(batchSize);
        new AutoDelete(taskPtr);
        scheduler_.AddTaskSetToPipe(taskPtr);
    }
}

void TaskSystem::WaitGroup(TaskGroup* group) {
    if (!group) { return; }
    for (auto& t : group->tasks) {
        scheduler_.WaitforTask(t.get());
    }
    group->tasks.clear();
}

void TaskSystem::WaitForAll() {
    scheduler_.WaitforAll();
}

std::size_t TaskSystem::ThreadIndex() const {
    return scheduler_.GetThreadNum();
}

