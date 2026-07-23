#include <ruvia/core/EventLoopPool.h>
#include <ruvia/core/detail/RuntimeLifecycle.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio/execution_context.hpp>
#include <asio/executor_work_guard.hpp>

#include <ruvia/core/detail/util/FailureReport.h>
#include <ruvia/core/detail/worker/WorkerDispatcher.h>
#include <ruvia/core/detail/worker/WorkerSelection.h>

namespace ruvia {
namespace {

std::size_t defaultLoopCount() noexcept {
    return std::max<std::size_t>(1, std::thread::hardware_concurrency());
}

class ExternalContextAttachmentService final
    : public asio::execution_context::service {
public:
    static asio::execution_context::id id;

    explicit ExternalContextAttachmentService(asio::execution_context& context)
        : asio::execution_context::service(context) {}

    [[nodiscard]] bool claim() noexcept {
        bool expected = false;
        return claimed_.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    void release() noexcept {
        claimed_.store(false, std::memory_order_release);
    }

private:
    void shutdown() override {}

    std::atomic_bool claimed_{false};
};

asio::execution_context::id ExternalContextAttachmentService::id;

class ExternalContextClaim final {
public:
    explicit ExternalContextClaim(asio::io_context& ioContext)
        : service_(&asio::use_service<ExternalContextAttachmentService>(ioContext)) {
        if (!service_->claim()) {
            throw std::invalid_argument(
                "an io_context can have only one Ruvia event loop attachment");
        }
    }

    ~ExternalContextClaim() {
        service_->release();
    }

    ExternalContextClaim(const ExternalContextClaim&) = delete;
    ExternalContextClaim& operator=(const ExternalContextClaim&) = delete;

private:
    ExternalContextAttachmentService* service_;
};

// A stop callback runs during shutdown, after the last caller that could have
// received its exception is gone. Whatever it throws is routed to the loop's
// failure sink -- the pool's first-failure record, which join() rethrows -- so
// a failed cleanup is never invisible. Failing to even schedule the callback
// (the off-worker path) is reported the same way.
class EventLoopStopListener final : public detail::WorkerShutdownListener {
public:
    EventLoopStopListener(
        WorkerHandle worker,
        MoveOnlyFunction<void()> callback,
        detail::EventLoopFailureSink failureSink)
        : worker_(std::move(worker)),
          callback_(std::move(callback)),
          failureSink_(std::move(failureSink)) {}

    void workerStopping() noexcept override {
        if (!callback_) {
            return;
        }
        auto callback = std::move(callback_);
        if (worker_.isCurrent()) {
            runCallback(callback, failureSink_);
            return;
        }
        try {
            detail::WorkerHandleAccess::defer(
                worker_,
                [callback = std::move(callback),
                 failureSink = failureSink_]() mutable noexcept {
                    runCallback(callback, failureSink);
                });
        } catch (...) {
            report(failureSink_, std::current_exception());
        }
    }

private:
    static void report(
        const detail::EventLoopFailureSink& sink,
        std::exception_ptr failure) noexcept {
        if (sink) {
            sink(std::move(failure));
            return;
        }
        // An attached loop has no pool to hold the failure for join().
        detail::reportUnhandledFailure("event loop stop callback", failure);
    }

    static void runCallback(
        MoveOnlyFunction<void()>& callback,
        const detail::EventLoopFailureSink& sink) noexcept {
        try {
            callback();
        } catch (...) {
            report(sink, std::current_exception());
        }
    }

    WorkerHandle worker_;
    MoveOnlyFunction<void()> callback_;
    detail::EventLoopFailureSink failureSink_;
};

}

namespace detail {

struct EventLoopState final {
    using ContextOwnership =
        std::variant<std::unique_ptr<asio::io_context>, ExternalContextClaim>;

    explicit EventLoopState(std::size_t mailboxCapacity)
        : contextOwnership(
              std::in_place_type<std::unique_ptr<asio::io_context>>,
              std::make_unique<asio::io_context>()),
          ioContext(**std::get_if<std::unique_ptr<asio::io_context>>(
              &contextOwnership)),
          work(asio::make_work_guard(ioContext)),
          dispatcher(std::make_shared<WorkerDispatcher>(ioContext, mailboxCapacity)),
          handle(WorkerHandleAccess::make(dispatcher)) {}

    EventLoopState(asio::io_context& externalContext, std::size_t mailboxCapacity)
        : contextOwnership(
              std::in_place_type<ExternalContextClaim>, externalContext),
          ioContext(externalContext),
          work(asio::make_work_guard(ioContext)),
          // WorkerDispatcher::Impl is the single authority that validates the
          // mailbox capacity: it throws std::invalid_argument for a zero
          // capacity while this member is constructed, before any body check
          // here could run. The owned-context constructor relies on the same.
          dispatcher(std::make_shared<WorkerDispatcher>(ioContext, mailboxCapacity)),
          handle(WorkerHandleAccess::make(dispatcher)) {}

    ~EventLoopState() {
        // Dispatcher handles may escape EventLoop/EventLoopPool. Retire their
        // endpoint while the owned or attached io_context is still alive.
        dispatcher->detachContext();
    }

    void stop(bool runtimeStarted) noexcept {
        if (stopping.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        dispatcher->close();
        if (!runtimeStarted || dispatcher->isCurrent()) {
            dispatcher->stopTimers();
        } else {
            dispatcher->deferOrTerminate(
                [dispatcher = dispatcher] { dispatcher->stopTimers(); });
        }
        work.reset();
    }

    ContextOwnership contextOwnership;
    asio::io_context& ioContext;
    asio::executor_work_guard<asio::io_context::executor_type> work;
    std::shared_ptr<WorkerDispatcher> dispatcher;
    WorkerHandle handle;
    std::thread thread;
    std::atomic_bool stopping{false};
    // Set by the owning pool; empty for an attached loop. Read only while
    // registering a stop callback, which the owner does before start().
    EventLoopFailureSink failureSink;
};

}

EventLoopStopRegistration::EventLoopStopRegistration(
    std::shared_ptr<detail::WorkerShutdownListener> listener) noexcept
    : listener_(std::move(listener)) {}

bool EventLoopStopRegistration::valid() const noexcept {
    return listener_ != nullptr;
}

void EventLoopStopRegistration::reset() noexcept {
    listener_.reset();
}

EventLoop::EventLoop(std::shared_ptr<detail::EventLoopState> state) noexcept
    : state_(std::move(state)) {}

bool EventLoop::valid() const noexcept {
    return state_ != nullptr;
}

bool EventLoop::accepting() const noexcept {
    return state_ && state_->handle.accepting();
}

bool EventLoop::isCurrent() const noexcept {
    return state_ && state_->handle.isCurrent();
}

WorkerId EventLoop::id() const noexcept {
    return state_ ? state_->handle.id() : 0;
}

asio::io_context& EventLoop::ioContext() const {
    if (!state_) {
        throw std::logic_error("cannot access a default-constructed event loop");
    }
    return state_->ioContext;
}

asio::io_context::executor_type EventLoop::executor() const {
    return ioContext().get_executor();
}

WorkerHandle EventLoop::handle() const noexcept {
    return state_ ? state_->handle : WorkerHandle{};
}

EventLoopStopRegistration EventLoop::registerStopCallback(
    MoveOnlyFunction<void()> callback) const {
    if (!state_) {
        throw std::logic_error("cannot register a stop callback on an invalid event loop");
    }
    auto listener = std::make_shared<EventLoopStopListener>(
        state_->handle, std::move(callback), state_->failureSink);
    detail::WorkerHandleAccess::registerShutdownListener(state_->handle, listener);
    return EventLoopStopRegistration(std::move(listener));
}

EventLoopAttachment::EventLoopAttachment(
    std::shared_ptr<detail::EventLoopState> state) noexcept
    : state_(std::move(state)) {}

EventLoopAttachment::~EventLoopAttachment() {
    stop();
}

EventLoopAttachment::EventLoopAttachment(EventLoopAttachment&& other) noexcept
    : state_(std::move(other.state_)) {}

bool EventLoopAttachment::valid() const noexcept {
    return state_ != nullptr;
}

EventLoop EventLoopAttachment::loop() const noexcept {
    return EventLoop(state_);
}

void EventLoopAttachment::stop() noexcept {
    if (state_) {
        state_->stop(true);
    }
}

EventLoopAttachment attachEventLoop(
    asio::io_context& ioContext,
    EventLoopAttachmentOptions options) {
    return EventLoopAttachment(
        std::make_shared<detail::EventLoopState>(
            ioContext,
            options.mailboxCapacity));
}

struct EventLoopPool::Impl {
    explicit Impl(EventLoopPoolOptions options) {
        const auto count = options.loopCount == 0 ? defaultLoopCount() : options.loopCount;
        if (options.mailboxCapacity == 0) {
            throw std::invalid_argument("event loop mailbox capacity must be greater than zero");
        }
        loops.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            loops.push_back(std::make_shared<detail::EventLoopState>(options.mailboxCapacity));
            // Failures with no caller left (a stop callback that throws during
            // shutdown) become the pool's first failure, which join() rethrows.
            // The sink holds the record, not the pool: a loop handle may outlive
            // this Impl, and a sink capturing `this` would outlive it too.
            loops.back()->failureSink = [record = failure](std::exception_ptr failure) {
                record->record(std::move(failure));
            };
        }
    }

    void recordFailure(std::exception_ptr exception) noexcept {
        failure->record(std::move(exception));
    }

    void stop() noexcept {
        const bool runtimeStarted =
            lifecycle.state() != detail::RuntimeLifecycle::State::kReady;
        if (!lifecycle.requestStop()) {
            return;
        }
        for (const auto& loop : loops) {
            loop->stop(runtimeStarted);
        }
    }

    void run(const std::shared_ptr<detail::EventLoopState>& loop) noexcept {
        try {
            loop->dispatcher->runContext(
                [this](std::exception_ptr failure) noexcept {
                    recordFailure(std::move(failure));
                    stop();
                });
        } catch (...) {
            recordFailure(std::current_exception());
            stop();
        }
    }

    void launch(const std::shared_ptr<detail::EventLoopState>& loop) {
        loop->thread = std::thread([this, loop] { run(loop); });
    }

    void drainUnlaunched() noexcept {
        // A partial thread-launch failure must not strand work that was
        // accepted before start(), nor owner-affine stop callbacks queued by
        // stop(). The caller is not one of this pool's workers, so it can act
        // as a short-lived owner for every loop that never acquired a thread.
        for (const auto& loop : loops) {
            if (!loop->thread.joinable()) {
                run(loop);
            }
        }
    }

    // The pool's first failure, shared so a loop's failure sink can outlive the
    // pool without dangling. Only the first is kept: it is the one that caused
    // the shutdown, and the ones behind it are usually its consequences.
    struct FailureRecord final {
        std::mutex mutex;
        std::exception_ptr first;

        void record(std::exception_ptr failure) noexcept {
            const std::lock_guard lock(mutex);
            if (!first) {
                first = std::move(failure);
            }
        }

        [[nodiscard]] std::exception_ptr take() noexcept {
            const std::lock_guard lock(mutex);
            return std::exchange(first, nullptr);
        }
    };

    std::vector<std::shared_ptr<detail::EventLoopState>> loops;
    detail::RuntimeLifecycle lifecycle;
    std::atomic<std::size_t> nextIndex{0};
    std::shared_ptr<FailureRecord> failure{std::make_shared<FailureRecord>()};
};

EventLoopPool::EventLoopPool(EventLoopPoolOptions options)
    : impl_(std::make_unique<Impl>(options)) {}

EventLoopPool::~EventLoopPool() {
    stop();
    try {
        join();
    } catch (...) {
        // Destroying a pool that was never joined explicitly makes this the
        // only place its first failure is ever rethrown, and a destructor
        // cannot rethrow it further. Report rather than end here.
        detail::reportUnhandledFailure("event loop pool", std::current_exception());
    }
}

void EventLoopPool::start() {
    if (!impl_->lifecycle.start()) {
        throw std::logic_error("event loop pool can only be started once");
    }
    try {
        for (const auto& loop : impl_->loops) {
            impl_->launch(loop);
        }
    } catch (...) {
        const auto launchFailure = std::current_exception();
        impl_->stop();
        impl_->drainUnlaunched();
        for (const auto& loop : impl_->loops) {
            if (loop->thread.joinable()) {
                loop->thread.join();
            }
        }
        impl_->lifecycle.completeStop();
        std::rethrow_exception(launchFailure);
    }
}

void EventLoopPool::stop() noexcept {
    impl_->stop();
}

void EventLoopPool::join() {
    if (std::ranges::any_of(impl_->loops, [](const auto& loop) {
            return loop->dispatcher->isCurrent();
        })) {
        throw std::logic_error(
            "cannot join an event loop pool from one of its workers");
    }
    if (impl_->lifecycle.state() == detail::RuntimeLifecycle::State::kReady) {
        impl_->stop();
    }
    if (impl_->lifecycle.state() == detail::RuntimeLifecycle::State::kStopping) {
        try {
            // stop() before start() still owes accepted mailbox work and
            // owner-affine stop callbacks a real execution context. Launch
            // only the missing worker threads so join performs that drain.
            for (const auto& loop : impl_->loops) {
                if (!loop->thread.joinable()) {
                    impl_->launch(loop);
                }
            }
        } catch (...) {
            impl_->recordFailure(std::current_exception());
            impl_->drainUnlaunched();
        }
    }
    for (const auto& loop : impl_->loops) {
        if (loop->thread.joinable()) {
            loop->thread.join();
        }
    }
    impl_->lifecycle.completeStop();

    if (const auto failure = impl_->failure->take()) {
        std::rethrow_exception(failure);
    }
}

std::size_t EventLoopPool::loopCount() const noexcept {
    return impl_->loops.size();
}

EventLoop EventLoopPool::loop(std::size_t index) const {
    if (index >= impl_->loops.size()) {
        throw std::out_of_range("event loop index is out of range");
    }
    return EventLoop(impl_->loops[index]);
}

EventLoop EventLoopPool::nextLoop() noexcept {
    const auto index = impl_->nextIndex.fetch_add(1, std::memory_order_relaxed);
    return EventLoop(impl_->loops[index % impl_->loops.size()]);
}

EventLoop EventLoopPool::loopFor(std::uint64_t key) const noexcept {
    return EventLoop(impl_->loops[key % impl_->loops.size()]);
}

EventLoop EventLoopPool::loopFor(std::string_view key) const noexcept {
    return loopFor(detail::workerSelectionHash(key));
}

}
