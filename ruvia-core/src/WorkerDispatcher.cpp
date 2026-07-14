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
std::atomic<WorkerId> gNextWorkerId{1};

struct TimerEntry final {
    std::chrono::steady_clock::time_point deadline;
    std::uint64_t sequence{0};
    std::shared_ptr<WorkerTimerState> state;
};

struct TimerEntryLater final {
    bool operator()(const TimerEntry& left, const TimerEntry& right) const noexcept {
        return left.deadline > right.deadline ||
               (left.deadline == right.deadline && left.sequence > right.sequence);
    }
};
}

struct WorkerTimerState final {
    explicit WorkerTimerState(std::move_only_function<void(bool)> value)
        : completion(std::move(value)) {}

    std::atomic_bool active{true};
    std::move_only_function<void(bool)> completion;
};

struct WorkerDispatcher::Impl {
    explicit Impl(asio::io_context& context, std::size_t requestedCapacity)
        : ioContext(context),
          timer(context),
          slots(requestedCapacity),
          workerId(gNextWorkerId.fetch_add(1, std::memory_order_relaxed)),
          timers(detail::processResource()) {
        if (requestedCapacity == 0) {
            throw std::invalid_argument("worker mailbox capacity must be greater than zero");
        }
    }

    asio::io_context& ioContext;
    asio::steady_timer timer;
    std::vector<std::optional<std::move_only_function<void()>>> slots;
    std::mutex mutex;
    std::size_t head{0};
    std::size_t tail{0};
    std::size_t size{0};
    WorkerId workerId{0};
    bool accepting{true};
    bool drainScheduled{false};
    std::vector<std::weak_ptr<WorkerShutdownListener>> shutdownListeners;
    std::pmr::vector<TimerEntry> timers;
    std::uint64_t nextTimerSequence{0};
    std::uint64_t timerGeneration{0};
    bool timerArmed{false};
    bool dispatchingTimers{false};
    bool timersStopping{false};
    std::size_t cancelledTimerCount{0};
};

WorkerTimerRegistration::WorkerTimerRegistration(
    std::weak_ptr<WorkerDispatcher> dispatcher,
    std::shared_ptr<WorkerTimerState> state) noexcept
    : dispatcher_(std::move(dispatcher)), state_(std::move(state)) {}

WorkerTimerRegistration::~WorkerTimerRegistration() {
    cancel();
}

WorkerTimerRegistration::WorkerTimerRegistration(WorkerTimerRegistration&& other) noexcept
    : dispatcher_(std::move(other.dispatcher_)), state_(std::move(other.state_)) {}

WorkerTimerRegistration& WorkerTimerRegistration::operator=(
    WorkerTimerRegistration&& other) noexcept {
    if (this != &other) {
        cancel();
        dispatcher_ = std::move(other.dispatcher_);
        state_ = std::move(other.state_);
    }
    return *this;
}

void WorkerTimerRegistration::cancel() noexcept {
    auto state = std::exchange(state_, nullptr);
    const auto dispatcher = dispatcher_.lock();
    dispatcher_.reset();
    if (!state || !dispatcher || !state->active.load(std::memory_order_acquire)) {
        return;
    }
    if (dispatcher->isCurrent()) {
        dispatcher->cancelTimer(state);
        return;
    }
    try {
        dispatcher->defer(
            [dispatcher, state = std::move(state)] { dispatcher->cancelTimer(state); });
    } catch (...) {
    }
}

bool WorkerTimerRegistration::valid() const noexcept {
    return state_ && state_->active.load(std::memory_order_acquire);
}

WorkerDispatcher::WorkerDispatcher(asio::io_context& ioContext, std::size_t capacity)
    : impl_(std::make_unique<Impl>(ioContext, capacity)) {}

WorkerDispatcher::~WorkerDispatcher() = default;

PostResult WorkerDispatcher::post(std::move_only_function<void()> task) {
    bool scheduleDrain = false;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->accepting) {
            return PostResult::kWorkerStopping;
        }
        if (impl_->size == impl_->slots.size()) {
            return PostResult::kQueueFull;
        }
        impl_->slots[impl_->tail].emplace(std::move(task));
        impl_->tail = (impl_->tail + 1) % impl_->slots.size();
        ++impl_->size;
        if (!impl_->drainScheduled) {
            impl_->drainScheduled = true;
            scheduleDrain = true;
        }
    }
    if (scheduleDrain) {
        asio::post(impl_->ioContext, [self = shared_from_this()] { self->drain(); });
    }
    return PostResult::kAccepted;
}

void WorkerDispatcher::defer(std::move_only_function<void()> task) {
    asio::post(impl_->ioContext,
               [self = shared_from_this(), task = std::move(task)]() mutable { task(); });
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

WorkerTimerRegistration WorkerDispatcher::scheduleTimer(
    std::chrono::steady_clock::time_point deadline,
    std::move_only_function<void(bool)> completion) {
    if (!isCurrent()) {
        throw std::logic_error("worker timers must be scheduled on their worker");
    }
    if (impl_->timersStopping) {
        throw std::runtime_error("worker timer queue is stopping");
    }

    std::pmr::polymorphic_allocator<WorkerTimerState> allocator(processResource());
    auto state = std::allocate_shared<WorkerTimerState>(allocator, std::move(completion));
    impl_->timers.push_back(TimerEntry{
        .deadline = deadline,
        .sequence = impl_->nextTimerSequence++,
        .state = state,
    });
    std::push_heap(impl_->timers.begin(), impl_->timers.end(), TimerEntryLater{});
    if (!impl_->dispatchingTimers) {
        armTimer();
    }
    return WorkerTimerRegistration(weak_from_this(), std::move(state));
}

void WorkerDispatcher::cancelTimer(const std::shared_ptr<WorkerTimerState>& state) noexcept {
    if (!state || !state->active.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    ++impl_->cancelledTimerCount;
    auto completion = std::move(state->completion);
    if (impl_->cancelledTimerCount >= 64 &&
        impl_->cancelledTimerCount * 2 >= impl_->timers.size()) {
        std::erase_if(impl_->timers, [](const TimerEntry& entry) {
            return !entry.state->active.load(std::memory_order_acquire);
        });
        std::make_heap(impl_->timers.begin(), impl_->timers.end(), TimerEntryLater{});
        impl_->cancelledTimerCount = 0;
    }
    if (!impl_->dispatchingTimers) {
        armTimer();
    }
    try {
        if (completion) {
            defer([completion = std::move(completion)]() mutable { completion(true); });
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
    impl_->timer.cancel(ignored);

    while (!impl_->timers.empty()) {
        std::pop_heap(impl_->timers.begin(), impl_->timers.end(), TimerEntryLater{});
        auto entry = std::move(impl_->timers.back());
        impl_->timers.pop_back();
        if (!entry.state->active.exchange(false, std::memory_order_acq_rel)) {
            if (impl_->cancelledTimerCount != 0) {
                --impl_->cancelledTimerCount;
            }
            continue;
        }
        try {
            if (entry.state->completion) {
                auto completion = std::move(entry.state->completion);
                completion(true);
            }
        } catch (...) {
            std::terminate();
        }
    }
}

void WorkerDispatcher::close() noexcept {
    notifyStopping(beginStopping(false));
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
    return impl_->ioContext.get_executor().running_in_this_thread();
}

bool WorkerDispatcher::accepting() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->accepting;
}

WorkerId WorkerDispatcher::id() const noexcept {
    return impl_->workerId;
}

void WorkerDispatcher::drain() {
    for (;;) {
        std::move_only_function<void()> task;
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
    impl_->timer.cancel(ignored);
    impl_->timerArmed = false;
    while (!impl_->timers.empty() &&
           !impl_->timers.front().state->active.load(std::memory_order_acquire)) {
        std::pop_heap(impl_->timers.begin(), impl_->timers.end(), TimerEntryLater{});
        impl_->timers.pop_back();
        if (impl_->cancelledTimerCount != 0) {
            --impl_->cancelledTimerCount;
        }
    }
    if (impl_->timersStopping || impl_->timers.empty()) {
        return;
    }
    impl_->timer.expires_at(impl_->timers.front().deadline);
    impl_->timerArmed = true;
    impl_->timer.async_wait([self = shared_from_this(), generation](const std::error_code& error) {
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
        std::pop_heap(impl_->timers.begin(), impl_->timers.end(), TimerEntryLater{});
        auto entry = std::move(impl_->timers.back());
        impl_->timers.pop_back();
        if (!entry.state->active.exchange(false, std::memory_order_acq_rel)) {
            if (impl_->cancelledTimerCount != 0) {
                --impl_->cancelledTimerCount;
            }
            continue;
        }
        if (entry.state->completion) {
            auto completion = std::move(entry.state->completion);
            completion(false);
        }
    }
    impl_->dispatchingTimers = false;
    armTimer();
}

}
