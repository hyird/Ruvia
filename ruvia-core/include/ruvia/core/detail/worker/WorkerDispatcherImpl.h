#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

#include <ruvia/core/detail/worker/WorkerDispatcher.h>
#include <ruvia/core/memory/PmrResource.h>

// The dispatcher's state, declared here because two translation units own parts
// of it: WorkerDispatcher.cpp runs the mailbox and the worker's lifecycle, while
// WorkerTimers.cpp runs the timer heap that shares its mutex.

namespace ruvia::detail {

inline constexpr std::size_t kNoTimerSlot = static_cast<std::size_t>(-1);

// One pending deadline in the heap. `slot` and `generation` identify the
// registration it belongs to, so a cancelled entry is recognised as stale
// instead of being searched for and removed.
struct TimerEntry final {
    std::chrono::steady_clock::time_point deadline;
    std::uint64_t sequence{0};
    std::size_t slot{kNoTimerSlot};
    std::uint64_t generation{0};
};

// A registration slot, reused through a free list; `generation` invalidates every
// heap entry that referred to a previous occupant.
struct TimerSlot final {
    std::uint64_t generation{0};
    bool active{false};
    std::size_t nextFree{kNoTimerSlot};
    MoveOnlyFunction<void(WorkerTimerOutcome)> completion;
};

// Heap order: earliest deadline first, ties broken by registration order.
struct TimerEntryLater final {
    bool operator()(const TimerEntry& left, const TimerEntry& right) const noexcept {
        return left.deadline > right.deadline || (left.deadline == right.deadline && left.sequence > right.sequence);
    }
};

// The worker whose run loop is executing on this thread, or nullptr. A plain
// thread-local read: the timer path uses it to take the on-worker shortcut
// before it would otherwise lock.
[[nodiscard]] const WorkerDispatcher* currentWorkerDispatcher() noexcept;

// The process-wide worker id source.
[[nodiscard]] WorkerId nextWorkerDispatcherId() noexcept;

struct WorkerDispatcher::Impl {
    explicit Impl(asio::io_context& context, std::size_t requestedCapacity)
        : ioContext(context),
          timer(std::make_unique<asio::steady_timer>(context)),
          slots(requestedCapacity),
          workerId(nextWorkerDispatcherId()),
          timers(detail::processResource()),
          timerSlots(detail::processResource()) {
        if (requestedCapacity == 0) {
            throw std::invalid_argument("worker mailbox capacity must be greater than zero");
        }
        timers.reserve(requestedCapacity);
        timerSlots.reserve(requestedCapacity);
    }

    asio::io_context& ioContext;
    std::unique_ptr<asio::steady_timer> timer;
    std::vector<std::optional<MoveOnlyFunction<void()>>> slots;
    std::mutex mutex;
    std::size_t head{0};
    std::size_t tail{0};
    std::size_t size{0};
    WorkerId workerId{0};
    bool accepting{true};
    bool contextAttached{true};
    bool drainScheduled{false};
    bool abandonDrain{false};
    std::vector<std::weak_ptr<WorkerShutdownListener>> shutdownListeners;
    std::pmr::vector<TimerEntry> timers;
    std::pmr::vector<TimerSlot> timerSlots;
    std::size_t freeTimerSlot{kNoTimerSlot};
    std::uint64_t nextTimerSequence{0};
    std::uint64_t timerGeneration{0};
    bool timerArmed{false};
    bool dispatchingTimers{false};
    bool timersStopping{false};
    std::size_t staleTimerCount{0};
};

}  // namespace ruvia::detail
