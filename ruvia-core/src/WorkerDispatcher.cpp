#include <ruvia/core/detail/WorkerDispatcher.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

#include <ruvia/core/memory/PmrResource.h>

namespace ruvia::detail {
namespace {
constexpr std::size_t kNoTimerSlot = static_cast<std::size_t>(-1);

std::atomic<WorkerId> gNextWorkerId{1};
thread_local const WorkerDispatcher* gCurrentWorker = nullptr;

class CurrentWorkerGuard final {
public:
    explicit CurrentWorkerGuard(const WorkerDispatcher& worker)
        : previous_(std::exchange(gCurrentWorker, &worker)) {
        if (previous_ != nullptr && previous_ != &worker) {
            gCurrentWorker = previous_;
            throw std::logic_error(
                "one thread cannot run multiple Ruvia workers concurrently");
        }
    }

    ~CurrentWorkerGuard() {
        gCurrentWorker = previous_;
    }

    CurrentWorkerGuard(const CurrentWorkerGuard&) = delete;
    CurrentWorkerGuard& operator=(const CurrentWorkerGuard&) = delete;

private:
    const WorkerDispatcher* previous_;
};

struct TimerEntry final {
    std::chrono::steady_clock::time_point deadline;
    std::uint64_t sequence{0};
    std::size_t slot{kNoTimerSlot};
    std::uint64_t generation{0};
};

struct TimerSlot final {
    std::uint64_t generation{0};
    bool active{false};
    std::size_t nextFree{kNoTimerSlot};
    MoveOnlyFunction<void(WorkerTimerOutcome)> completion;
};

struct TimerEntryLater final {
    bool operator()(const TimerEntry& left, const TimerEntry& right) const noexcept {
        return left.deadline > right.deadline ||
               (left.deadline == right.deadline && left.sequence > right.sequence);
    }
};
}

struct WorkerDispatcher::Impl {
    explicit Impl(asio::io_context& context, std::size_t requestedCapacity)
        : ioContext(context),
          timer(std::make_unique<asio::steady_timer>(context)),
          slots(requestedCapacity),
          workerId(gNextWorkerId.fetch_add(1, std::memory_order_relaxed)),
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

WorkerTimerRegistration::~WorkerTimerRegistration() {
    cancel();
}

void WorkerTimerRegistration::cancel() noexcept {
    auto* dispatcher = std::exchange(dispatcher_, nullptr);
    const auto slot = std::exchange(slot_, 0);
    const auto generation = std::exchange(generation_, 0);
    if (dispatcher == nullptr || generation == 0) {
        return;
    }
    dispatcher->requestTimerCancellation(slot, generation);
}

bool WorkerTimerRegistration::registered() const noexcept {
    return dispatcher_ != nullptr;
}

void WorkerTimerRegistration::bind(
    WorkerDispatcher& dispatcher,
    std::size_t slot,
    std::uint64_t generation) noexcept {
    dispatcher_ = &dispatcher;
    slot_ = slot;
    generation_ = generation;
}

void WorkerTimerRegistration::release() noexcept {
    dispatcher_ = nullptr;
    slot_ = 0;
    generation_ = 0;
}

WorkerDispatcher::WorkerDispatcher(asio::io_context& ioContext, std::size_t capacity)
    : impl_(std::make_unique<Impl>(ioContext, capacity)) {}

WorkerDispatcher::~WorkerDispatcher() = default;

PostResult WorkerDispatcher::post(MoveOnlyFunction<void()> task) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->contextAttached || !impl_->accepting) {
        return PostResult::kWorkerStopping;
    }
    if (impl_->size == impl_->slots.size()) {
        return PostResult::kQueueFull;
    }
    const auto insertedIndex = impl_->tail;
    impl_->slots[insertedIndex].emplace(std::move(task));
    impl_->tail = (impl_->tail + 1) % impl_->slots.size();
    ++impl_->size;
    if (impl_->drainScheduled) {
        return PostResult::kAccepted;
    }
    impl_->drainScheduled = true;
    try {
        asio::post(
            impl_->ioContext,
            [self = shared_from_this()] { self->drain(); });
    } catch (...) {
        impl_->slots[insertedIndex].reset();
        impl_->tail = insertedIndex;
        --impl_->size;
        impl_->drainScheduled = false;
        throw;
    }
    return PostResult::kAccepted;
}

void WorkerDispatcher::defer(MoveOnlyFunction<void()> task) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->contextAttached) {
        throw std::runtime_error("worker execution context is detached");
    }
    asio::post(impl_->ioContext,
               [self = shared_from_this(), task = std::move(task)]() mutable { task(); });
}

void WorkerDispatcher::deferOrTerminate(
    MoveOnlyFunction<void()> task) noexcept {
    try {
        defer(std::move(task));
    } catch (...) {
        std::terminate();
    }
}

void WorkerDispatcher::registerShutdownListener(
    const std::shared_ptr<WorkerShutdownListener>& listener) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->accepting) {
        throw std::runtime_error("cannot register state on a stopping worker");
    }
    std::erase_if(impl_->shutdownListeners, [](const auto& entry) { return entry.expired(); });
    impl_->shutdownListeners.emplace_back(listener);
}

void WorkerDispatcher::scheduleTimer(
    WorkerTimerRegistration& registration,
    std::chrono::steady_clock::time_point deadline,
    MoveOnlyFunction<void(WorkerTimerOutcome)> completion) {
    if (!isCurrent()) {
        throw std::logic_error("worker timers must be scheduled on their worker");
    }
    if (!attached()) {
        throw std::runtime_error("worker execution context is detached");
    }
    if (impl_->timersStopping) {
        throw std::runtime_error("worker timer queue is stopping");
    }

    if (registration.dispatcher_ != nullptr) {
        if (registration.dispatcher_ != this ||
            hasTimer(registration.slot_, registration.generation_)) {
            throw std::logic_error("worker timer registration is already active");
        }
        registration.release();
    }

    std::size_t slotIndex = impl_->freeTimerSlot;
    if (slotIndex == kNoTimerSlot) {
        slotIndex = impl_->timerSlots.size();
        impl_->timerSlots.emplace_back();
    } else {
        impl_->freeTimerSlot = impl_->timerSlots[slotIndex].nextFree;
    }
    auto& slot = impl_->timerSlots[slotIndex];
    if (++slot.generation == 0) {
        ++slot.generation;
    }
    slot.active = true;
    slot.nextFree = kNoTimerSlot;
    slot.completion = std::move(completion);
    try {
        impl_->timers.push_back(TimerEntry{
            .deadline = deadline,
            .sequence = impl_->nextTimerSequence++,
            .slot = slotIndex,
            .generation = slot.generation,
        });
    } catch (...) {
        slot.active = false;
        slot.completion = nullptr;
        slot.nextFree = impl_->freeTimerSlot;
        impl_->freeTimerSlot = slotIndex;
        throw;
    }
    std::ranges::push_heap(impl_->timers, TimerEntryLater{});
    registration.bind(*this, slotIndex, slot.generation);
    if (!impl_->dispatchingTimers) {
        armTimer();
    }
}

void WorkerDispatcher::requestTimerCancellation(
    std::size_t slot, std::uint64_t generation) noexcept {
    if (generation == 0) {
        return;
    }
    if (gCurrentWorker == this) {
        cancelTimer(slot, generation);
        return;
    }
    bool current = false;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->contextAttached || impl_->ioContext.stopped()) {
            return;
        }
        current = impl_->ioContext.get_executor().running_in_this_thread();
        if (!current) {
            try {
                asio::post(
                    impl_->ioContext,
                    [self = shared_from_this(), slot, generation] {
                        self->cancelTimer(slot, generation);
                    });
            } catch (...) {
                std::terminate();
            }
            return;
        }
    }
    cancelTimer(slot, generation);
}

void WorkerDispatcher::cancelTimer(
    std::size_t slotIndex, std::uint64_t generation) noexcept {
    if (slotIndex >= impl_->timerSlots.size()) {
        return;
    }
    auto& slot = impl_->timerSlots[slotIndex];
    if (!slot.active || slot.generation != generation) {
        return;
    }
    slot.active = false;
    auto completion = std::move(slot.completion);
    slot.nextFree = impl_->freeTimerSlot;
    impl_->freeTimerSlot = slotIndex;
    ++impl_->staleTimerCount;
    if (impl_->staleTimerCount >= 64 &&
        impl_->staleTimerCount * 2 >= impl_->timers.size()) {
        std::erase_if(impl_->timers, [this](const TimerEntry& entry) {
            return !hasTimer(entry.slot, entry.generation);
        });
        std::ranges::make_heap(impl_->timers, TimerEntryLater{});
        impl_->staleTimerCount = 0;
    }
    if (!impl_->dispatchingTimers) {
        armTimer();
    }
    try {
        if (completion) {
            defer([completion = std::move(completion)]() mutable {
                completion(WorkerTimerOutcome::kCancelled);
            });
        }
    } catch (...) {
        std::terminate();
    }
}

void WorkerDispatcher::stopTimers() noexcept {
    if (impl_->timersStopping) {
        return;
    }
    impl_->timersStopping = true;
    ++impl_->timerGeneration;
    impl_->timerArmed = false;
    std::error_code ignored;
    if (impl_->timer) {
        impl_->timer->cancel(ignored);
    }

    impl_->timers.clear();
    impl_->staleTimerCount = 0;
    for (std::size_t index = 0; index < impl_->timerSlots.size(); ++index) {
        auto& slot = impl_->timerSlots[index];
        if (!slot.active) {
            continue;
        }
        slot.active = false;
        auto completion = std::move(slot.completion);
        slot.nextFree = impl_->freeTimerSlot;
        impl_->freeTimerSlot = index;
        try {
            if (completion) {
                completion(WorkerTimerOutcome::kCancelled);
            }
        } catch (...) {
            std::terminate();
        }
    }
}

void WorkerDispatcher::runContext() {
    CurrentWorkerGuard current(*this);
    impl_->ioContext.run();
}

void WorkerDispatcher::close() noexcept {
    notifyStopping(beginStopping(false));
}

void WorkerDispatcher::detachContext() noexcept {
    std::vector<std::optional<MoveOnlyFunction<void()>>> abandonedSlots;
    ShutdownListeners abandonedListeners;
    std::pmr::vector<TimerEntry> abandonedTimers(impl_->timers.get_allocator());
    std::pmr::vector<TimerSlot> abandonedTimerSlots(
        impl_->timerSlots.get_allocator());
    std::unique_ptr<asio::steady_timer> detachedTimer;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->contextAttached) {
            return;
        }
        impl_->accepting = false;
        impl_->contextAttached = false;
        impl_->drainScheduled = false;
        impl_->head = 0;
        impl_->tail = 0;
        impl_->size = 0;
        abandonedSlots.swap(impl_->slots);
        abandonedListeners.swap(impl_->shutdownListeners);
        detachedTimer = std::move(impl_->timer);
    }

    // The caller guarantees no worker-thread timer activity runs concurrently
    // with detachContext: EventLoopPool joins its threads first, and the
    // attachEventLoop teardown contract requires run() to have returned before
    // the attachment is destroyed. The timer heap is therefore exclusively owned
    // here. All user-owned closures are destroyed outside the mutex so a
    // destructor that releases another worker primitive cannot deadlock.
    abandonedTimers.swap(impl_->timers);
    abandonedTimerSlots.swap(impl_->timerSlots);
    impl_->freeTimerSlot = kNoTimerSlot;
    impl_->staleTimerCount = 0;
    impl_->timerArmed = false;
    impl_->timersStopping = true;
    detachedTimer.reset();
}

bool WorkerDispatcher::attached() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->contextAttached;
}

WorkerDispatcher::ShutdownListeners WorkerDispatcher::beginStopping(
    bool abandonDrain) noexcept {
    ShutdownListeners listeners;
    {
        std::lock_guard lock(impl_->mutex);
        if (abandonDrain) {
            impl_->drainScheduled = false;
        }
        if (!impl_->accepting) {
            return listeners;
        }
        impl_->accepting = false;
        listeners.swap(impl_->shutdownListeners);
    }
    return listeners;
}

void WorkerDispatcher::notifyStopping(
    const ShutdownListeners& listeners) noexcept {
    for (const auto& entry : listeners) {
        if (const auto listener = entry.lock()) {
            listener->workerStopping();
        }
    }
}

bool WorkerDispatcher::isCurrent() const noexcept {
    if (gCurrentWorker == this) {
        return true;
    }
    std::lock_guard lock(impl_->mutex);
    return impl_->contextAttached &&
           impl_->ioContext.get_executor().running_in_this_thread();
}

bool WorkerDispatcher::accepting() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->accepting;
}

WorkerId WorkerDispatcher::id() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->contextAttached ? impl_->workerId : 0;
}

void WorkerDispatcher::drain() {
    for (;;) {
        MoveOnlyFunction<void()> task;
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->size == 0) {
                impl_->drainScheduled = false;
                return;
            }
            task = std::move(*impl_->slots[impl_->head]);
            impl_->slots[impl_->head].reset();
            impl_->head = (impl_->head + 1) % impl_->slots.size();
            --impl_->size;
        }
        try {
            task();
        } catch (...) {
            notifyStopping(beginStopping(true));
            throw;
        }
    }
}

void WorkerDispatcher::armTimer() {
    ++impl_->timerGeneration;
    const auto generation = impl_->timerGeneration;
    std::error_code ignored;
    if (!impl_->timer) {
        return;
    }
    impl_->timer->cancel(ignored);
    impl_->timerArmed = false;
    while (!impl_->timers.empty() &&
           !hasTimer(impl_->timers.front().slot,
                     impl_->timers.front().generation)) {
        std::ranges::pop_heap(impl_->timers, TimerEntryLater{});
        impl_->timers.pop_back();
        if (impl_->staleTimerCount != 0) {
            --impl_->staleTimerCount;
        }
    }
    if (impl_->timersStopping || impl_->timers.empty()) {
        return;
    }
    impl_->timer->expires_at(impl_->timers.front().deadline);
    impl_->timerArmed = true;
    impl_->timer->async_wait([self = shared_from_this(), generation](const std::error_code& error) {
        if (error || generation != self->impl_->timerGeneration) {
            return;
        }
        self->impl_->timerArmed = false;
        self->fireTimers();
    });
}

void WorkerDispatcher::fireTimers() {
    impl_->dispatchingTimers = true;
    while (!impl_->timers.empty() &&
           impl_->timers.front().deadline <= std::chrono::steady_clock::now()) {
        std::ranges::pop_heap(impl_->timers, TimerEntryLater{});
        auto entry = std::move(impl_->timers.back());
        impl_->timers.pop_back();
        if (!hasTimer(entry.slot, entry.generation)) {
            if (impl_->staleTimerCount != 0) {
                --impl_->staleTimerCount;
            }
            continue;
        }
        auto& slot = impl_->timerSlots[entry.slot];
        slot.active = false;
        auto completion = std::move(slot.completion);
        slot.nextFree = impl_->freeTimerSlot;
        impl_->freeTimerSlot = entry.slot;
        if (completion) {
            completion(WorkerTimerOutcome::kExpired);
        }
    }
    impl_->dispatchingTimers = false;
    armTimer();
}

bool WorkerDispatcher::hasTimer(
    std::size_t slot, std::uint64_t generation) const noexcept {
    return slot < impl_->timerSlots.size() &&
        impl_->timerSlots[slot].active &&
        impl_->timerSlots[slot].generation == generation;
}

}
