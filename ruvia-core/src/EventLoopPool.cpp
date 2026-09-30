#include "ruvia/core/EventLoopPool.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <memory_resource>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio/execution_context.hpp>
#include <asio/executor_work_guard.hpp>

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/WorkerRuntimeContext.h"
#include "ruvia/core/detail/RuntimeLifecycle.h"
#include "ruvia/core/detail/util/FailureReport.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/core/detail/worker/WorkerSelection.h"
#include "ruvia/core/memory/ProcessResource.h"

namespace ruvia {
namespace {

std::size_t defaultLoopCount() noexcept {
    return std::max<std::size_t>(1, std::thread::hardware_concurrency());
}

class ExternalContextAttachmentService final : public asio::execution_context::service {
public:
    static asio::execution_context::id id;

    explicit ExternalContextAttachmentService(asio::execution_context& context)
        : asio::execution_context::service(context) {}

    [[nodiscard]] bool claim() noexcept {
        const std::lock_guard lock(mutex_);
        if (claimed_) {
            return false;
        }
        claimed_ = true;
        return true;
    }

    void releaseClaim() noexcept;
    void retain(std::shared_ptr<detail::EventLoopState> state) noexcept;
    void releaseState(detail::EventLoopState* state) noexcept;

private:
    void shutdown() override;

    std::mutex mutex_;
    bool claimed_{false};
    std::shared_ptr<detail::EventLoopState> state_;
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
        if (!retained_) {
            service_->releaseClaim();
        }
    }

    void retain(std::shared_ptr<detail::EventLoopState> state) noexcept {
        service_->retain(std::move(state));
        retained_ = true;
    }

    [[nodiscard]] ExternalContextAttachmentService* service() const noexcept {
        return service_;
    }

    ExternalContextClaim(const ExternalContextClaim&) = delete;
    ExternalContextClaim& operator=(const ExternalContextClaim&) = delete;

private:
    ExternalContextAttachmentService* service_;
    bool retained_{false};
};

class EventLoopRetirement final {
public:
    EventLoopRetirement() = default;

    void setFinalize(MoveOnlyFunction<void()> finalize) noexcept {
        const std::lock_guard lock(mutex_);
        finalize_ = std::move(finalize);
    }

    [[nodiscard]] bool tryBegin() noexcept {
        const std::lock_guard lock(mutex_);
        if (sealed_ || finalized_) {
            return false;
        }
        ++pending_;
        return true;
    }

    void begin() noexcept {
        if (!tryBegin()) {
            std::terminate();
        }
    }

    [[nodiscard]] std::shared_ptr<void> tryAcquire() {
        {
            const std::lock_guard lock(mutex_);
            if (rootAdmissionClosed_ || sealed_ || finalized_) {
                return {};
            }
            ++pending_;
        }
        try {
            auto token = std::make_shared<LeaseToken>(this);
            return std::shared_ptr<void>(std::move(token), this);
        } catch (...) {
            finish();
            throw;
        }
    }

    void finish() noexcept {
        MoveOnlyFunction<void()> finalize;
        {
            const std::lock_guard lock(mutex_);
            if (pending_ == 0) {
                std::terminate();
            }
            --pending_;
            if (sealed_ && pending_ == 0 && !finalized_) {
                finalized_ = true;
                finalize = std::move(finalize_);
            }
        }
        if (finalize) {
            finalize();
        }
    }

    [[nodiscard]] std::size_t pending() const noexcept {
        const std::lock_guard lock(mutex_);
        return pending_;
    }

    void closeRootAdmission() noexcept {
        const std::lock_guard lock(mutex_);
        rootAdmissionClosed_ = true;
    }

    void closeAdmission() noexcept {
        const std::lock_guard lock(mutex_);
        rootAdmissionClosed_ = true;
        sealed_ = true;
    }

    void seal() noexcept {
        MoveOnlyFunction<void()> finalize;
        {
            const std::lock_guard lock(mutex_);
            sealed_ = true;
            if (pending_ == 0 && !finalized_) {
                finalized_ = true;
                finalize = std::move(finalize_);
            }
        }
        if (finalize) {
            finalize();
        }
    }

private:
    struct LeaseToken final {
        explicit LeaseToken(EventLoopRetirement* owner) noexcept
            : owner_(owner) {}
        ~LeaseToken() {
            owner_->finish();
        }
        EventLoopRetirement* owner_;
    };

    mutable std::mutex mutex_;
    std::size_t pending_{0};
    bool rootAdmissionClosed_{false};
    bool sealed_{false};
    bool finalized_{false};
    MoveOnlyFunction<void()> finalize_;
};

// The callback owner is kept alive by the completion handler until both the
// returned Task frame and its completion chain have retired.
class EventLoopStopListener final : public detail::WorkerShutdownListener,
                                    public std::enable_shared_from_this<EventLoopStopListener> {
public:
    EventLoopStopListener(WorkerHandle worker, asio::io_context::executor_type executor,
        MoveOnlyFunction<Task<void>()> callback, detail::EventLoopFailureSink failureSink,
        std::shared_ptr<EventLoopRetirement> retirement)
        : worker_(std::move(worker)),
          executor_(std::move(executor)),
          callback_(std::move(callback)),
          failureSink_(std::move(failureSink)),
          retirement_(std::move(retirement)) {}

    void workerStopping() noexcept override {
        if (!callback_) {
            return;
        }
        retirement_->begin();
        auto self = shared_from_this();
        if (worker_.isCurrent()) {
            startCallback(std::move(self));
            return;
        }
        try {
            detail::WorkerHandleAccess::defer(worker_,
                [self = std::move(self)]() mutable noexcept { self->startCallback(std::move(self)); });
        } catch (...) {
            report(failureSink_, std::current_exception());
            std::terminate();
        }
    }

private:
    static void report(
        const detail::EventLoopFailureSink& sink, std::exception_ptr failure) noexcept {
        if (sink) {
            sink(std::move(failure));
            return;
        }
        // An attached loop has no pool to hold the failure for join().
        detail::reportUnhandledFailure("event loop stop callback", failure);
    }

    void startCallback(std::shared_ptr<EventLoopStopListener> self) noexcept {
        std::optional<Task<void>> task;
        try {
            task.emplace(callback_());
        } catch (const std::bad_alloc&) {
            report(failureSink_, std::current_exception());
            std::terminate();
        } catch (...) {
            report(failureSink_, std::current_exception());
            retirement_->finish();
            return;
        }

        try {
            detail::asyncStartTask(std::move(*task), asio::bind_executor(executor_,
                                                         [self = std::move(self)](detail::TaskCompletionResult<void> result) mutable {
                                                             if (const auto* failure = result.failure()) {
                                                                 report(self->failureSink_, failure->exception());
                                                             }
                                                             self->retirement_->finish();
                                                         }));
        } catch (...) {
            report(failureSink_, std::current_exception());
            std::terminate();
        }
    }

    WorkerHandle worker_;
    asio::io_context::executor_type executor_;
    MoveOnlyFunction<Task<void>()> callback_;
    detail::EventLoopFailureSink failureSink_;
    std::shared_ptr<EventLoopRetirement> retirement_;
};

}  // namespace

namespace detail {

struct EventLoopState final : WorkerShutdownListener,
                              std::enable_shared_from_this<EventLoopState> {
    using ContextOwnership = std::variant<std::unique_ptr<asio::io_context>, ExternalContextClaim>;

    explicit EventLoopState(std::size_t mailboxCapacity)
        : contextOwnership(std::in_place_type<std::unique_ptr<asio::io_context>>,
              std::make_unique<asio::io_context>()),
          ioContext(
              std::addressof(**std::get_if<std::unique_ptr<asio::io_context>>(&contextOwnership))),
          executor(ioContext->get_executor()),
          work(asio::make_work_guard(*ioContext)),
          runtime(*ioContext, mailboxCapacity) {}

    EventLoopState(asio::io_context& externalContext, std::size_t mailboxCapacity)
        : contextOwnership(std::in_place_type<ExternalContextClaim>, externalContext),
          ioContext(std::addressof(externalContext)),
          executor(externalContext.get_executor()),
          work(asio::make_work_guard(*ioContext)),
          // WorkerDispatcher::Impl is the single authority that validates the
          // mailbox capacity: it throws std::invalid_argument for a zero
          // capacity while this member is constructed, before any body check
          // here could run. The owned-context constructor relies on the same.
          runtime(*ioContext, mailboxCapacity) {}

    void retainExternalContext(const std::shared_ptr<EventLoopState>& self) noexcept {
        if (auto* claim = std::get_if<ExternalContextClaim>(&contextOwnership)) {
            claim->retain(self);
        }
    }

    void installLifecycleListener() {
        WorkerHandleAccess::registerShutdownListener(runtime.handle(), shared_from_this());
    }

    void workerStopping() noexcept override {}

    void workerStoppingComplete() noexcept override {
        stop(true, shared_from_this());
    }

    void reportFailure(std::exception_ptr failure) noexcept {
        if (!failure) {
            return;
        }
        if (failureSink) {
            failureSink(std::move(failure));
            return;
        }
        reportUnhandledFailure("attached event loop failure", failure);
        stop(true, shared_from_this());
    }

    void reportAttachedFailure(std::exception_ptr failure) noexcept {
        if (!failure) {
            return;
        }
        reportUnhandledFailure("attached event loop failure", failure);
        stop(true, shared_from_this());
    }

    [[nodiscard]] std::shared_ptr<void> acquireRootLease() {
        return retirement->tryAcquire();
    }

    [[noreturn]] void failTeardownStart(std::exception_ptr failure) noexcept {
        if (failureSink) {
            failureSink(std::move(failure));
        } else {
            reportUnhandledFailure("event loop teardown startup", std::move(failure));
        }
        std::terminate();
    }

    void stop(bool runtimeStarted, std::shared_ptr<EventLoopState> keepAlive) noexcept {
        if (stopping.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        retirement->closeRootAdmission();
        if (!retirement->tryBegin()) {
            return;
        }
        std::weak_ptr<EventLoopState> weak = keepAlive;
        retirement->setFinalize([weak] {
            auto state = weak.lock();
            if (!state) {
                return;
            }
            try {
                detail::WorkerHandleAccess::whenIdle(state->runtime.handle(), [weak] {
                    auto idleState = weak.lock();
                    if (!idleState) {
                        return;
                    }
                    auto failureOwner = idleState;
                    const auto worker = idleState->runtime.handle();
                    auto finalizationKeepAlive = std::move(idleState);
                    try {
                        detail::WorkerHandleAccess::defer(worker,
                            [state = std::move(finalizationKeepAlive)] { state->finalizeStop(); });
                    } catch (...) {
                        failureOwner->failTeardownStart(std::current_exception());
                    }
                });
            } catch (...) {
                state->failTeardownStart(std::current_exception());
            }
        });
        runtime.close();
        if (!runtimeStarted || runtime.handle().isCurrent()) {
            runtime.stopTimers();
        } else {
            try {
                detail::WorkerHandleAccess::defer(runtime.handle(),
                    [keepAlive = std::move(keepAlive)] { keepAlive->runtime.stopTimers(); });
            } catch (...) {
                failTeardownStart(std::current_exception());
            }
        }
        // close() can lose the race to a dispatcher that already set
        // accepting=false but has not yet called every shutdown listener. Do
        // not seal the retirement gate until that first notification batch is
        // fully complete, so every listener can acquire its cleanup lease.
        try {
            detail::WorkerHandleAccess::whenShutdownNotificationsComplete(runtime.handle(),
                [retirement = retirement] {
                    retirement->seal();
                    retirement->finish();
                });
        } catch (...) {
            failTeardownStart(std::current_exception());
        }
    }

    void finalizeStop() noexcept {
        work.reset();
        if (std::holds_alternative<ExternalContextClaim>(contextOwnership)) {
            runtime.detach();
            std::get<ExternalContextClaim>(contextOwnership).service()->releaseState(this);
        }
    }

    void shutdownExternalContext() noexcept {
        if (!std::holds_alternative<ExternalContextClaim>(contextOwnership)) {
            return;
        }
        retirement->closeAdmission();
        if (retirement->pending() != 0) {
            std::terminate();
        }
        stopping.store(true, std::memory_order_release);
        runtime.stopTimers();
        work.reset();
        runtime.detach();
        detail::WorkerHandleAccess::waitForReservations(runtime.handle());
    }

    ContextOwnership contextOwnership;
    asio::io_context* const ioContext;
    asio::io_context::executor_type executor;
    asio::executor_work_guard<asio::io_context::executor_type> work;
    WorkerRuntimeContext runtime;
    std::thread thread;
    std::atomic_bool stopping{false};
    // Bound once before publication to the stable pool owner or weak attachment
    // owner. Failure reporting and stop registration only read this channel.
    EventLoopFailureSink failureSink;
    std::shared_ptr<EventLoopRetirement> retirement{std::make_shared<EventLoopRetirement>()};
};

}  // namespace detail

namespace {

void ExternalContextAttachmentService::releaseClaim() noexcept {
    const std::lock_guard lock(mutex_);
    claimed_ = false;
}

void ExternalContextAttachmentService::retain(
    std::shared_ptr<detail::EventLoopState> state) noexcept {
    const std::lock_guard lock(mutex_);
    state_ = std::move(state);
}

void ExternalContextAttachmentService::releaseState(detail::EventLoopState* state) noexcept {
    std::shared_ptr<detail::EventLoopState> abandoned;
    {
        const std::lock_guard lock(mutex_);
        if (state_.get() != state) {
            return;
        }
        abandoned = std::move(state_);
        claimed_ = false;
    }
    abandoned.reset();
}

void ExternalContextAttachmentService::shutdown() {
    std::shared_ptr<detail::EventLoopState> state;
    {
        const std::lock_guard lock(mutex_);
        state = std::move(state_);
        claimed_ = false;
    }
    if (state) {
        // The context is entering Asio's service shutdown, so no new handler
        // may be queued here. Retire the worker before the context destroys
        // its scheduler. The state may still be held by EventLoop/Attachment;
        // those handles become terminal and no longer expose a dangling
        // io_context reference.
        state->shutdownExternalContext();
    }
}

}  // namespace

class EventLoopPoolOwner final : public std::enable_shared_from_this<EventLoopPoolOwner> {
public:
    struct FailureRecord final {
        std::mutex mutex;
        std::exception_ptr first;
        bool observed{true};

        void record(std::exception_ptr failure) noexcept {
            if (!failure) {
                return;
            }
            {
                const std::lock_guard lock(mutex);
                if (observed) {
                    if (!first) {
                        first = std::move(failure);
                    }
                    return;
                }
            }
            detail::reportUnhandledFailure("event loop failure after pool retirement", failure);
        }

        void detachObserver() noexcept {
            std::exception_ptr remaining;
            {
                const std::lock_guard lock(mutex);
                observed = false;
                remaining = std::exchange(first, nullptr);
            }
            if (remaining) {
                detail::reportUnhandledFailure("event loop failure during pool retirement", remaining);
            }
        }

        [[nodiscard]] std::exception_ptr take() noexcept {
            const std::lock_guard lock(mutex);
            return std::exchange(first, nullptr);
        }
    };

    explicit EventLoopPoolOwner(std::size_t loopCount) {
        loops_.reserve(loopCount);
    }

    void addLoop(const std::shared_ptr<detail::EventLoopState>& loop) {
        loops_.emplace_back(loop);
    }

    void recordFailure(std::exception_ptr failure) noexcept {
        if (!failure) {
            return;
        }
        failureRecord_.record(std::move(failure));
    }

    void reportFailure(std::exception_ptr failure) noexcept {
        if (!failure) {
            return;
        }
        recordFailure(failure);
        stop();
    }

    void stop() noexcept {
        const bool runtimeStarted = lifecycle_.state() != detail::RuntimeLifecycle::State::kReady;
        if (!lifecycle_.requestStop()) {
            return;
        }
        for (const auto& weakLoop : loops_) {
            if (auto loop = weakLoop.lock()) {
                loop->stop(runtimeStarted, std::move(loop));
            }
        }
    }

    void run(const std::shared_ptr<detail::EventLoopState>& loop) noexcept {
        const auto self = shared_from_this();
        try {
            loop->runtime.run([self](std::exception_ptr failure) noexcept {
                self->reportFailure(std::move(failure));
            });
        } catch (...) {
            reportFailure(std::current_exception());
        }
        loop->runtime.detach();
    }

    [[nodiscard]] bool start() noexcept {
        return lifecycle_.start();
    }

    [[nodiscard]] detail::RuntimeLifecycle::State state() const noexcept {
        return lifecycle_.state();
    }

    void completeStop() noexcept {
        lifecycle_.completeStop();
    }

    [[nodiscard]] std::exception_ptr takeFailure() noexcept {
        return failureRecord_.take();
    }

    void detachPoolObserver() noexcept {
        failureRecord_.detachObserver();
    }

private:
    std::pmr::vector<std::weak_ptr<detail::EventLoopState>> loops_{detail::processResource()};
    detail::RuntimeLifecycle lifecycle_;
    FailureRecord failureRecord_;
};

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
    return state_ && state_->runtime.handle().valid();
}

bool EventLoop::accepting() const noexcept {
    return state_ && state_->runtime.handle().accepting();
}

bool EventLoop::isCurrent() const noexcept {
    return state_ && state_->runtime.handle().isCurrent();
}

WorkerId EventLoop::id() const noexcept {
    return state_ ? state_->runtime.handle().id() : 0;
}

asio::io_context& EventLoop::ioContext() const& {
    if (!state_ || !state_->runtime.handle().valid()) {
        if (state_) {
            throw std::logic_error("event loop execution context is detached");
        }
        throw std::logic_error("cannot access a default-constructed event loop");
    }
    return *state_->ioContext;
}

asio::io_context::executor_type EventLoop::executor() const {
    static_cast<void>(ioContext());
    return state_->executor;
}

const WorkerHandle& EventLoop::dispatchHandle() const noexcept {
    if (state_) {
        return state_->runtime.handle();
    }
    static const WorkerHandle emptyHandle;
    return emptyHandle;
}

WorkerHandle EventLoop::handle() const noexcept {
    return dispatchHandle();
}

void EventLoop::reportFailure(std::exception_ptr failure) const noexcept {
    if (!failure) {
        return;
    }
    if (state_) {
        state_->reportFailure(std::move(failure));
    } else {
        detail::reportUnhandledFailure("invalid event loop failure", std::move(failure));
    }
}

detail::EventLoopFailureSink EventLoop::failureSink() const {
    if (!state_) {
        throw std::logic_error("cannot start a task on an invalid event loop");
    }
    return state_->failureSink;
}

std::shared_ptr<void> EventLoop::acquireRootLease() const {
    if (!state_) {
        return {};
    }
    return state_->acquireRootLease();
}

EventLoopStopRegistration EventLoop::registerStopCallback(
    MoveOnlyFunction<Task<void>()> callback) const {
    if (!state_) {
        throw std::logic_error("cannot register a stop callback on an invalid event loop");
    }
    if (!callback) {
        throw std::invalid_argument("event loop stop callback must be callable");
    }
    if (!state_->runtime.handle().accepting()) {
        throw std::runtime_error("cannot register a stop callback on a stopping event loop");
    }
    auto listener = std::make_shared<EventLoopStopListener>(state_->runtime.handle(),
        state_->executor, std::move(callback), state_->failureSink,
        state_->retirement);
    detail::WorkerHandleAccess::registerShutdownListener(state_->runtime.handle(), listener);
    return EventLoopStopRegistration(std::move(listener));
}

EventLoopAttachment::EventLoopAttachment(std::shared_ptr<detail::EventLoopState> state) noexcept
    : state_(std::move(state)) {}

EventLoopAttachment::~EventLoopAttachment() {
    stop();
}

EventLoopAttachment::EventLoopAttachment(EventLoopAttachment&& other) noexcept
    : state_(std::move(other.state_)) {}

bool EventLoopAttachment::valid() const noexcept {
    return state_ != nullptr && state_->runtime.handle().valid();
}

EventLoop EventLoopAttachment::loop() const noexcept {
    return EventLoop(state_);
}

void EventLoopAttachment::run() {
    auto state = state_;
    if (!state) {
        throw std::logic_error("cannot run an invalid event loop attachment");
    }
    if (!state->runtime.handle().valid()) {
        throw std::logic_error("event loop execution context is detached");
    }
    state->runtime.run();
}

void EventLoopAttachment::stop() noexcept {
    if (state_) {
        state_->stop(true, state_);
    }
}

EventLoopAttachment attachEventLoop(
    asio::io_context& ioContext, EventLoopAttachmentOptions options) {
    auto state = std::make_shared<detail::EventLoopState>(ioContext, options.mailboxCapacity);
    const std::weak_ptr<detail::EventLoopState> weakState = state;
    // RootTask uses this sink directly; it must not call EventLoopState::reportFailure,
    // which would route back through this same sink.
    state->failureSink = [weakState](std::exception_ptr failure) {
        if (const auto lockedState = weakState.lock()) {
            lockedState->reportAttachedFailure(std::move(failure));
        } else {
            detail::reportUnhandledFailure(
                "attached event loop failure after retirement", std::move(failure));
        }
    };
    state->installLifecycleListener();
    state->retainExternalContext(state);
    return EventLoopAttachment(std::move(state));
}

struct EventLoopPool::Impl {
    explicit Impl(EventLoopPoolOptions options) {
        const auto count = options.loopCount == 0 ? defaultLoopCount() : options.loopCount;
        if (options.mailboxCapacity == 0) {
            throw std::invalid_argument("event loop mailbox capacity must be greater than zero");
        }
        owner = std::make_shared<EventLoopPoolOwner>(count);
        loops.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            loops.push_back(std::make_shared<detail::EventLoopState>(options.mailboxCapacity));
            loops.back()->installLifecycleListener();
            owner->addLoop(loops.back());
            loops.back()->failureSink = [stableOwner = owner](std::exception_ptr failure) {
                stableOwner->reportFailure(std::move(failure));
            };
        }
    }

    void stop() noexcept {
        owner->stop();
    }

    void launch(const std::shared_ptr<detail::EventLoopState>& loop) {
        auto stableOwner = owner;
        loop->thread = std::thread(
            [stableOwner = std::move(stableOwner), loop] { stableOwner->run(loop); });
    }

    void drainUnlaunched() noexcept {
        // A partial thread-launch failure must not strand accepted mailbox work
        // or owner-affine cleanup; missing workers are drained synchronously.
        for (const auto& loop : loops) {
            if (!loop->thread.joinable()) {
                owner->run(loop);
            }
        }
    }

    std::pmr::vector<std::shared_ptr<detail::EventLoopState>> loops{detail::processResource()};
    std::shared_ptr<EventLoopPoolOwner> owner;
    std::atomic<std::size_t> nextIndex{0};
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
    impl_->owner->detachPoolObserver();
}

void EventLoopPool::start() {
    if (!impl_->owner->start()) {
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
        impl_->owner->completeStop();
        std::rethrow_exception(launchFailure);
    }
}

void EventLoopPool::stop() noexcept {
    impl_->stop();
}

void EventLoopPool::join() {
    if (std::ranges::any_of(
            impl_->loops, [](const auto& loop) { return loop->runtime.handle().isCurrent(); })) {
        throw std::logic_error("cannot join an event loop pool from one of its workers");
    }
    impl_->stop();
    if (impl_->owner->state() == detail::RuntimeLifecycle::State::kStopping) {
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
            impl_->owner->recordFailure(std::current_exception());
            impl_->drainUnlaunched();
        }
    }
    for (const auto& loop : impl_->loops) {
        if (loop->thread.joinable()) {
            loop->thread.join();
        }
        detail::WorkerHandleAccess::waitForReservations(loop->runtime.handle());
        loop->runtime.detach();
    }
    impl_->owner->completeStop();

    if (const auto failure = impl_->owner->takeFailure()) {
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

}  // namespace ruvia
