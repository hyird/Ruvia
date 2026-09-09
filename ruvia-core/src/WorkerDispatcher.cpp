#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/detail/worker/WorkerDispatcherImpl.h"
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
            throw std::logic_error("one thread cannot run multiple Ruvia workers concurrently");
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
    if (!task) {
        throw std::invalid_argument("worker post requires a callable task");
    }
    std::size_t index = kNoTimerSlot;
    PostStatus rejection = PostStatus::kAccepted;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->contextAttached || !impl_->accepting) {
            rejection = PostStatus::kWorkerStopping;
        } else if (impl_->pendingCount == impl_->nodes.size() - 1 ||
                   impl_->freeHead == kNoTimerSlot) {
            rejection = PostStatus::kQueueFull;
        } else {
            index = impl_->freeHead;
            impl_->freeHead = impl_->nodes[index].next;
            impl_->nodes[index].state = Impl::NodeState::kReserved;
            ++impl_->pendingCount;
        }
    }
    if (rejection != PostStatus::kAccepted) {
        return PostResult::reject(rejection, std::move(task));
    }
    impl_->nodes[index].task = std::move(task);
    try {
        publish(index);
    } catch (...) {
        rollbackReserved(index);
        throw;
    }
    return PostResult::accept();
}

PostStatus WorkerDispatcher::postFactory(MoveOnlyFunction<MoveOnlyFunction<void()>()> factory) {
    if (!factory) {
        throw std::invalid_argument("worker post factory requires a callable");
    }
    std::size_t index = kNoTimerSlot;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->contextAttached || !impl_->accepting) {
            return PostStatus::kWorkerStopping;
        }
        if (impl_->pendingCount == impl_->nodes.size() - 1 ||
            impl_->freeHead == kNoTimerSlot) {
            return PostStatus::kQueueFull;
        }
        index = impl_->freeHead;
        impl_->freeHead = impl_->nodes[index].next;
        impl_->nodes[index].state = Impl::NodeState::kReserved;
        ++impl_->pendingCount;
    }
    try {
        auto task = factory();
        if (!task) {
            throw std::invalid_argument("worker post factory produced an empty task");
        }
        impl_->nodes[index].task = std::move(task);
        publish(index);
    } catch (...) {
        rollbackReserved(index);
        throw;
    }
    return PostStatus::kAccepted;
}

void WorkerDispatcher::publish(std::size_t index) {
    bool abandon = false;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->abandonDrain || !impl_->contextAttached) {
            impl_->nodes[index].state = Impl::NodeState::kReleasing;
            abandon = true;
        } else {
            if (!impl_->drainScheduled) {
                asio::post(impl_->ioContext, [self = shared_from_this()] { self->drain(); });
                impl_->drainScheduled = true;
            }
            impl_->nodes[index].state = Impl::NodeState::kReady;
            impl_->nodes[index].next = kNoTimerSlot;
            if (impl_->readyTail == kNoTimerSlot) {
                impl_->readyHead = index;
            } else {
                impl_->nodes[impl_->readyTail].next = index;
            }
            impl_->readyTail = index;
        }
    }
    if (abandon) {
        auto abandoned = std::move(impl_->nodes[index].task);
        {
            std::lock_guard lock(impl_->mutex);
            impl_->nodes[index].state = Impl::NodeState::kFree;
            impl_->nodes[index].next = impl_->freeHead;
            impl_->freeHead = index;
            --impl_->pendingCount;
        }
    }
}

void WorkerDispatcher::rollbackReserved(std::size_t index) noexcept {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->nodes[index].state != Impl::NodeState::kReserved) {
            return;
        }
        impl_->nodes[index].state = Impl::NodeState::kReleasing;
    }
    auto abandoned = std::move(impl_->nodes[index].task);
    {
        std::lock_guard lock(impl_->mutex);
        impl_->nodes[index].state = Impl::NodeState::kFree;
        impl_->nodes[index].next = impl_->freeHead;
        impl_->freeHead = index;
        --impl_->pendingCount;
    }
}

void WorkerDispatcher::defer(MoveOnlyFunction<void()> task) {
    if (!task) {
        throw std::invalid_argument("worker defer requires a callable task");
    }
    std::lock_guard lock(impl_->mutex);
    if (!impl_->contextAttached) {
        throw std::runtime_error("worker execution context is detached");
    }
    asio::post(impl_->ioContext,
        [self = shared_from_this(), task = std::move(task)]() mutable { task(); });
}

bool WorkerDispatcher::deferIfAttached(MoveOnlyFunction<void()> task) {
    if (!task) {
        throw std::invalid_argument("worker defer requires a callable task");
    }
    std::lock_guard lock(impl_->mutex);
    if (!impl_->contextAttached) {
        return false;
    }
    asio::post(impl_->ioContext,
        [self = shared_from_this(), task = std::move(task)]() mutable { task(); });
    return true;
}

void WorkerDispatcher::deferOrTerminate(MoveOnlyFunction<void()> task) noexcept {
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
    runContext([&failure](std::exception_ptr value) noexcept { failure = std::move(value); });
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

void WorkerDispatcher::runContext(MoveOnlyFunction<void(std::exception_ptr)> failureHandler) {
    runContext({}, std::move(failureHandler), {});
}

void WorkerDispatcher::runContext(MoveOnlyFunction<void()> startupHandler,
    MoveOnlyFunction<void(std::exception_ptr)> failureHandler,
    MoveOnlyFunction<void()> shutdownHandler) {
    CurrentWorkerGuard current(*this);
    bool failureDelivered = false;
    std::exception_ptr deferredFailure;
    const auto handleFailure = [this, &failureDelivered, &deferredFailure, &failureHandler](
                                   std::exception_ptr failure) {
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
    ShutdownListeners abandonedListeners;
    std::pmr::vector<TimerEntry> abandonedTimers(impl_->timers.get_allocator());
    std::pmr::vector<TimerSlot> abandonedTimerSlots(impl_->timerSlots.get_allocator());
    std::unique_ptr<asio::steady_timer> detachedTimer;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->contextAttached) {
            return;
        }
        impl_->accepting = false;
        impl_->contextAttached = false;
        impl_->abandonDrain = true;
        abandonedListeners.swap(impl_->shutdownListeners);
        detachedTimer = std::move(impl_->timer);
    }

    // The caller guarantees no worker-thread timer activity runs concurrently
    // with detachContext: EventLoopPool joins its threads first, an attached
    // loop invokes this from its terminal context handler, and the external
    // context service invokes it while the context is shutting down. The timer
    // heap is therefore exclusively owned here. All user-owned closures are
    // destroyed outside the mutex so a destructor that releases another worker
    // primitive cannot deadlock.
    abandonedTimers.swap(impl_->timers);
    abandonedTimerSlots.swap(impl_->timerSlots);
    impl_->freeTimerSlot = kNoTimerSlot;
    impl_->staleTimerCount = 0;
    impl_->timerArmed = false;
    impl_->timersStopping.store(true, std::memory_order_release);
    detachedTimer.reset();
    abandonQueued();
}

bool WorkerDispatcher::attached() const noexcept {
    return impl_->contextAttached.load(std::memory_order_acquire);
}

WorkerDispatcher::ShutdownListeners WorkerDispatcher::beginStopping(bool abandonDrain) noexcept {
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

void WorkerDispatcher::notifyStopping(const ShutdownListeners& listeners) noexcept {
    for (const auto& entry : listeners) {
        if (const auto listener = entry.lock()) {
            listener->workerStopping();
        }
    }
}

void WorkerDispatcher::abandonQueued() noexcept {
    for (;;) {
        std::size_t index = kNoTimerSlot;
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->readyHead == kNoTimerSlot) {
                return;
            }
            index = impl_->readyHead;
            impl_->readyHead = impl_->nodes[index].next;
            if (impl_->readyHead == kNoTimerSlot) {
                impl_->readyTail = kNoTimerSlot;
            }
            impl_->nodes[index].state = Impl::NodeState::kReleasing;
        }
        auto abandoned = std::move(impl_->nodes[index].task);
        {
            std::lock_guard lock(impl_->mutex);
            impl_->nodes[index].state = Impl::NodeState::kFree;
            impl_->nodes[index].next = impl_->freeHead;
            impl_->freeHead = index;
            --impl_->pendingCount;
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
    return impl_->contextAttached && impl_->ioContext.get_executor().running_in_this_thread();
}

bool WorkerDispatcher::accepting() const noexcept {
    return impl_->accepting.load(std::memory_order_acquire);
}

WorkerId WorkerDispatcher::id() const noexcept {
    return impl_->contextAttached.load(std::memory_order_acquire) ? impl_->workerId : 0;
}

void WorkerDispatcher::drain() {
    for (;;) {
        MoveOnlyFunction<void()> task;
        std::size_t index = kNoTimerSlot;
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->abandonDrain) {
                impl_->drainScheduled = false;
                break;
            }
            if (impl_->readyHead == kNoTimerSlot) {
                impl_->drainScheduled = false;
                return;
            }
            index = impl_->readyHead;
            impl_->readyHead = impl_->nodes[index].next;
            if (impl_->readyHead == kNoTimerSlot) {
                impl_->readyTail = kNoTimerSlot;
            }
            impl_->nodes[index].state = Impl::NodeState::kActive;
            --impl_->pendingCount;
        }
        task = std::move(impl_->nodes[index].task);
        try {
            task();
        } catch (...) {
            {
                std::lock_guard lock(impl_->mutex);
                impl_->nodes[index].state = Impl::NodeState::kFree;
                impl_->nodes[index].next = impl_->freeHead;
                impl_->freeHead = index;
            }
            notifyStopping(beginStopping(true));
            abandonQueued();
            throw;
        }
        task = nullptr;
        {
            std::lock_guard lock(impl_->mutex);
            impl_->nodes[index].state = Impl::NodeState::kFree;
            impl_->nodes[index].next = impl_->freeHead;
            impl_->freeHead = index;
        }
    }
    abandonQueued();
}

}  // namespace ruvia::detail
