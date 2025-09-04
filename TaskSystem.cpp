#include "TaskSystem.h"
#include <algorithm>

thread_local std::size_t TaskSystem::tlsIndex_ = static_cast<std::size_t>(-1);

TaskSystem& TaskSystem::Get() {
    static TaskSystem g;
    return g;
}

TaskSystem::~TaskSystem() {
    Stop();
}

void TaskSystem::Start(unsigned threadCount) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (running_) {
        return;
    }

    running_ = true;

    queues_.clear();
    globalQueue_.clear();

    if (threadCount == 0) {
        unsigned hc = std::thread::hardware_concurrency();
        if (hc == 0u) {
            hc = 4u;
        }
        threadCount = std::max(1u, hc - 1u); // один поток оставим главному
    }

    workers_.reserve(threadCount);
    queues_.resize(threadCount);
    for (unsigned i = 0; i < threadCount; ++i) {
        workers_.emplace_back([this, i]() {
            tlsIndex_ = i;
            WorkerLoop_(i);
        });
    }
}

void TaskSystem::Stop() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!running_) {
            return;
        }
        running_ = false;
    }
    cvWork_.notify_all();

    for (auto& th : workers_) {
        if (th.joinable()) {
            th.join();
        }
    }
    workers_.clear();

    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& q : queues_) { q.clear(); }
        queues_.clear();
        globalQueue_.clear();
    }
}

void TaskSystem::Submit(const Task& t) {
    std::size_t idx = ThreadIndex();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!running_) {
            return;
        }
        if (idx < queues_.size()) {
            queues_[idx].push_back(t);
        }
        else {
            globalQueue_.push_back(t);
        }
        inFlight_.fetch_add(1, std::memory_order_relaxed);
    }
    cvWork_.notify_one();
}

void TaskSystem::Submit(Task&& t) {
    std::size_t idx = ThreadIndex();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!running_) {
            return;
        }
        if (idx < queues_.size()) {
            queues_[idx].push_back(std::move(t));
        }
        else {
            globalQueue_.push_back(std::move(t));
        }
        inFlight_.fetch_add(1, std::memory_order_relaxed);
    }
    cvWork_.notify_one();
}

void TaskSystem::Dispatch(std::size_t jobCount,
                          std::function<void(std::size_t)> fn,
                          std::size_t batchSize) {
    if (jobCount == 0 || !fn) {
        return;
    }
    if (batchSize == 0) {
        batchSize = 1;
    }

    // Держим fn живой даже после выхода из Dispatch
    auto fnShared = std::make_shared<std::function<void(std::size_t)>>(std::move(fn));

    const std::size_t batches = (jobCount + batchSize - 1) / batchSize;
    for (std::size_t b = 0; b < batches; ++b) {
        const std::size_t begin = b * batchSize;
        const std::size_t end = std::min(jobCount, begin + batchSize);

        Submit([begin, end, fnShared]() {
            for (std::size_t i = begin; i < end; ++i) {
                (*fnShared)(i);
            }
            });
    }
}

void TaskSystem::Submit(const Task& t, TaskGroup* group) {
    std::shared_ptr<TaskGroup::State> st = group ? group->state : nullptr;

    std::size_t idx = ThreadIndex();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!running_) { return; }
        if (st) { st->pending.fetch_add(1, std::memory_order_relaxed); }

        Task wrapped = [self = this, t, st]() {
            if (t) { t(); }

            if (st) {
                st->pending.fetch_sub(1, std::memory_order_acq_rel);
                // Будим ЖДУЩИХ ИМЕННО ЭТУ ГРУППУ — сигнал не потеряется
                {
                    std::lock_guard<std::mutex> lg(st->m);
                    st->cv.notify_all();
                }
            }
            // Параллельно будим «помогающих» (WaitGroup, воркеров)
            self->cvWork_.notify_all();
        };

        if (idx < queues_.size()) {
            queues_[idx].push_back(std::move(wrapped));
        }
        else {
            globalQueue_.push_back(std::move(wrapped));
        }

        inFlight_.fetch_add(1, std::memory_order_relaxed);
    }
    cvWork_.notify_one();
}

void TaskSystem::Submit(Task&& t, TaskGroup* group) {
    std::shared_ptr<TaskGroup::State> st = group ? group->state : nullptr;

    std::size_t idx = ThreadIndex();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!running_) { return; }
        if (st) { st->pending.fetch_add(1, std::memory_order_relaxed); }

        Task wrapped = [self = this, tt = std::move(t), st]() mutable {
            if (tt) { tt(); }

            if (st) {
                st->pending.fetch_sub(1, std::memory_order_acq_rel);
                {
                    std::lock_guard<std::mutex> lg(st->m);
                    st->cv.notify_all();
                }
            }
            self->cvWork_.notify_all();
        };

        if (idx < queues_.size()) {
            queues_[idx].push_back(std::move(wrapped));
        }
        else {
            globalQueue_.push_back(std::move(wrapped));
        }

        inFlight_.fetch_add(1, std::memory_order_relaxed);
    }
    cvWork_.notify_one();
}

void TaskSystem::Dispatch(std::size_t jobCount,
    std::function<void(std::size_t)> fn,
    std::size_t batchSize,
    TaskGroup* group)
{
    if (jobCount == 0 || !fn) {
        return;
    }
    if (batchSize == 0) {
        batchSize = 1;
    }

    auto fnShared = std::make_shared<std::function<void(std::size_t)>>(std::move(fn));
    const std::size_t batches = (jobCount + batchSize - 1) / batchSize;

    for (std::size_t b = 0; b < batches; ++b) {
        const std::size_t begin = b * batchSize;
        const std::size_t end = std::min(jobCount, begin + batchSize);

        Submit([begin, end, fnShared]() {
            for (std::size_t i = begin; i < end; ++i) {
                (*fnShared)(i);
            }
            }, group);
    }
}

void TaskSystem::WaitGroup(TaskGroup* group) {
    if (!group) { return; }
    std::shared_ptr<TaskGroup::State> st = group->state;

    for (;;) {
        if (st->pending.load(std::memory_order_acquire) == 0) {
            return;
        }

        Task task;
        {   // попробуем украсть работу из очередей
            std::lock_guard<std::mutex> lk(mtx_);
            if (!globalQueue_.empty()) {
                task = std::move(globalQueue_.front());
                globalQueue_.pop_front();
            }
            else {
                for (auto& q : queues_) {
                    if (!q.empty()) {
                        task = std::move(q.front());
                        q.pop_front();
                        break;
                    }
                }
            }
        }

        if (task) {
            task(); // выполняем вне лока

            const std::size_t left =
                inFlight_.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (left == 0) {
                std::lock_guard<std::mutex> lk(mtx_);
                if (!HasTasksLocked_()) {
                    cvIdle_.notify_all();
                }
            }
            continue; // снова проверим pending
        }

        // работы нет — ждём прогресса ИМЕННО по этой группе
        std::unique_lock<std::mutex> glk(st->m);
        st->cv.wait(glk, [&] {
            return st->pending.load(std::memory_order_acquire) == 0 || !running_;
            });
        if (!running_) { return; }
    }
}

void TaskSystem::WaitForAll() {
    std::unique_lock<std::mutex> lk(mtx_);
    cvIdle_.wait(lk, [this]() {
        return (inFlight_.load(std::memory_order_acquire) == 0) && !HasTasksLocked_();
    });
}

std::size_t TaskSystem::ThreadIndex() const {
    return tlsIndex_;
}

void TaskSystem::WorkerLoop_(std::size_t index) {
    for (;;) {
        Task task;

        {
            std::unique_lock<std::mutex> lk(mtx_);
            auto get_task = [&]() -> bool {
                if (!queues_[index].empty()) {
                    task = std::move(queues_[index].back());
                    queues_[index].pop_back();
                    return true;
                }
                if (!globalQueue_.empty()) {
                    task = std::move(globalQueue_.front());
                    globalQueue_.pop_front();
                    return true;
                }
                for (std::size_t i = 0; i < queues_.size(); ++i) {
                    if (i == index) { continue; }
                    if (!queues_[i].empty()) {
                        task = std::move(queues_[i].front());
                        queues_[i].pop_front();
                        return true;
                    }
                }
                return false;
            };

            while (!get_task()) {
                cvWork_.wait(lk, [this]() {
                    return !running_ || HasTasksLocked_();
                });
                if (!running_ && !HasTasksLocked_()) {
                    return;
                }
            }
        }

        if (task) {
            task();
        }

        const std::size_t left = inFlight_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (left == 0) {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!HasTasksLocked_()) {
                cvIdle_.notify_all();
            }
        }
    }
}

bool TaskSystem::HasTasksLocked_() const {
    if (!globalQueue_.empty()) {
        return true;
    }
    for (auto const& q : queues_) {
        if (!q.empty()) {
            return true;
        }
    }
    return false;
}