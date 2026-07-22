#include <ruvia/core/detail/worker/WorkerDispatcherImpl.h>

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
namespace ruvia::detail {
namespace {

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

}  // namespace

const WorkerDispatcher* currentWorkerDispatcher() noexcept {
    return gCurrentWorker;
}

WorkerId nextWorkerDispatcherId() noexcept {
    return gNextWorkerId.fetch_add(1, std::memory_order_relaxed);
}

WorkerDispatcher::WorkerDispatcher(asio::io_context& ioContext, std::size_t capacity)
    : impl_(std::make_unique<Impl>(ioContext, capacity)) {}

WorkerDispatcher::~WorkerDispatcher() = default;

PostResult WorkerDispatcher::post(MoveOnlyFunction<void()> task) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->contextAttached || !impl_->accepting) {
        return PostResult::reject(
            PostStatus::kWorkerStopping, std::move(task));
    }
    if (impl_->size == impl_->slots.size()) {
        return PostResult::reject(PostStatus::kQueueFull, std::move(task));
    }
    if (!impl_->drainScheduled) {
        asio::post(
            impl_->ioContext,
            [self = shared_from_this()] { self->drain(); });
        impl_->drainScheduled = true;
    }
    impl_->slots[impl_->tail].emplace(std::move(task));
    impl_->tail = (impl_->tail + 1) % impl_->slots.size();
    ++impl_->size;
    return PostResult::accept();
}

PostStatus WorkerDispatcher::postFactory(
    MoveOnlyFunction<MoveOnlyFunction<void()>()> factory) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->contextAttached || !impl_->accepting) {
        return PostStatus::kWorkerStopping;
    }
    if (impl_->size == impl_->slots.size()) {
        return PostStatus::kQueueFull;
    }
    if (!impl_->drainScheduled) {
        asio::post(
            impl_->ioContext,
            [self = shared_from_this()] { self->drain(); });
        impl_->drainScheduled = true;
    }
    impl_->slots[impl_->tail].emplace(factory());
    impl_->tail = (impl_->tail + 1) % impl_->slots.size();
    ++impl_->size;
    return PostStatus::kAccepted;
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

void WorkerDispatcher::runContext() {
    std::exception_ptr failure;
    runContext([&failure](std::exception_ptr value) noexcept {
        failure = std::move(value);
    });
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

void WorkerDispatcher::runContext(
    MoveOnlyFunction<void(std::exception_ptr)> failureHandler) {
    runContext({}, std::move(failureHandler), {});
}

void WorkerDispatcher::runContext(
    MoveOnlyFunction<void()> startupHandler,
    MoveOnlyFunction<void(std::exception_ptr)> failureHandler,
    MoveOnlyFunction<void()> shutdownHandler) {
    CurrentWorkerGuard current(*this);
    bool failureDelivered = false;
    std::exception_ptr deferredFailure;
    const auto handleFailure = [this, &failureDelivered, &deferredFailure,
                                &failureHandler](std::exception_ptr failure) {
        notifyStopping(beginStopping(true));
        abandonQueued();
        if (!failureDelivered) {
            failureDelivered = true;
            try {
                if (failureHandler) {
                    failureHandler(failure);
                } else {
                    deferredFailure = std::move(failure);
                }
            } catch (...) {
                deferredFailure = std::current_exception();
            }
        }
        stopTimers();
    };

    try {
        if (startupHandler) {
            startupHandler();
        }
    } catch (...) {
        handleFailure(std::current_exception());
    }
    for (;;) {
        try {
            impl_->ioContext.run();
            break;
        } catch (...) {
            handleFailure(std::current_exception());
        }
    }
    if (shutdownHandler) {
        try {
            shutdownHandler();
        } catch (...) {
            std::terminate();
        }
    }
    if (deferredFailure != nullptr) {
        std::rethrow_exception(deferredFailure);
    }
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
        impl_->abandonDrain = true;
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
            impl_->abandonDrain = true;
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

void WorkerDispatcher::abandonQueued() noexcept {
    for (;;) {
        MoveOnlyFunction<void()> abandoned;
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->size == 0) {
                impl_->head = 0;
                impl_->tail = 0;
                return;
            }
            abandoned = std::move(*impl_->slots[impl_->head]);
            impl_->slots[impl_->head].reset();
            impl_->head = (impl_->head + 1) % impl_->slots.size();
            --impl_->size;
        }
        // Destroy user closures outside the dispatcher mutex. Their destructors
        // may reconcile higher-level outstanding-work reservations.
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
            if (impl_->abandonDrain || impl_->size == 0) {
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
            abandonQueued();
            throw;
        }
    }
}

}  // namespace ruvia::detail
