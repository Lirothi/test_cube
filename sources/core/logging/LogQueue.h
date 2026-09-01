#pragma once

#include "LogRecord.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace logging
{
    // Bounded multi-producer / single-consumer ring of LogRecord slots (Vyukov's bounded queue
    // used with one consumer). Producers never block and never allocate: a full ring fails the
    // push and the caller counts the drop. Slots are published with a release store on their
    // sequence, so a producer that stalls between claiming a slot and publishing it only delays
    // the consumer; it can never corrupt or reorder another producer's record.
    //
    // Only the bytes a record actually uses are copied in and out (header + message + terminator),
    // so a typical 80-byte message costs ~130 bytes of copying, not the full 1 KiB slot.
    class LogQueue
    {
    public:
        LogQueue() noexcept = default;
        ~LogQueue();

        LogQueue(const LogQueue&) = delete;
        LogQueue& operator=(const LogQueue&) = delete;

        // Rounds the capacity up to a power of two (minimum 16) and allocates the slots once.
        // Returns false when the allocation fails; the queue is then unusable and Push fails.
        [[nodiscard]] bool Initialize(std::size_t capacity) noexcept;
        void Destroy() noexcept;

        // Producers. Never blocks; false = ring full or queue not initialized.
        [[nodiscard]] bool TryPush(const LogRecord& record) noexcept;

        // Single consumer. False = nothing published yet.
        [[nodiscard]] bool TryPop(LogRecord& record) noexcept;

        [[nodiscard]] std::size_t Capacity() const noexcept { return mask_ + 1; }

        // Claimed-but-not-necessarily-published count; used by the consumer to decide whether it
        // may sleep. Exact enough for that purpose, not for accounting.
        [[nodiscard]] std::size_t ApproximateSize() const noexcept;

    private:
        struct Slot
        {
            std::atomic<std::size_t> sequence{ 0 };
            LogRecord record;
        };

        Slot* slots_ = nullptr;
        std::size_t mask_ = 0;
        alignas(64) std::atomic<std::size_t> enqueuePos_{ 0 };
        alignas(64) std::atomic<std::size_t> dequeuePos_{ 0 };
    };
}
