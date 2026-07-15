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

#include <ruvia/core/detail/WorkerDispatcher.h>
#include <ruvia/core/detail/WorkerSelection.h>

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

class EventLoopStopListener final : public detail::WorkerShutdownListener {
public:
    EventLoopStopListener(
        WorkerHandle worker,
        std::move_only_function<void()> callback)
        : worker_(std::move(worker)), callback_(std::move(callback)) {}

    void workerStopping() noexcept override {
        if (!callback_) {
            return;
        }
        auto callback = std::move(callback_);
        if (worker_.isCurrent()) {
            try {
                callback();
            } catch (...) {
            }
            return;
        }
        try {
            detail::WorkerHandleAccess::defer(
                worker_,
                [callback = std::move(callback)]() mutable noexcept {
                    try {
                        callback();
                    } catch (...) {
                    }
                });
        } catch (...) {
        }
    }

private:
    WorkerHandle worker_;
    std::move_only_function<void()> callback_;
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
          dispatcher(std::make_shared<WorkerDispatcher>(ioContext, mailboxCapacity)),
          handle(WorkerHandleAccess::make(dispatcher)) {
        if (mailboxCapacity == 0) {
            throw std::invalid_argument(
                "event loop mailbox capacity must be greater than zero");
        }
    }

    ~EventLoopState() {
        // Dispatcher handles may escape EventLoop/EventLoopPool. Retire their
        // endpoint while the owned or attached io_context is still alive.
        dispatcher->detachContext();
    }

    void stop() noexcept {
        if (stopping.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        dispatcher->close();
        if (dispatcher->isCurrent()) {
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
    std::move_only_function<void()> callback) const {
    if (!state_) {
        throw std::logic_error("cannot register a stop callback on an invalid event loop");
    }
    auto listener = std::make_shared<EventLoopStopListener>(state_->handle, std::move(callback));
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

EventLoopAttachment& EventLoopAttachment::operator=(
    EventLoopAttachment&& other) noexcept {
    if (this != &other) {
        stop();
        state_ = std::move(other.state_);
    }
    return *this;
}

bool EventLoopAttachment::valid() const noexcept {
    return state_ != nullptr;
}

EventLoop EventLoopAttachment::loop() const noexcept {
    return EventLoop(state_);
}

void EventLoopAttachment::stop() noexcept {
    if (state_) {
        state_->stop();
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
        }
    }

    void recordFailure(std::exception_ptr failure) noexcept {
        std::lock_guard lock(failureMutex);
        if (!firstFailure) {
            firstFailure = std::move(failure);
        }
    }

    void stop() noexcept {
        if (!lifecycle.requestStop()) {
            return;
        }
        for (const auto& loop : loops) {
            loop->stop();
        }
    }

    std::vector<std::shared_ptr<detail::EventLoopState>> loops;
    detail::RuntimeLifecycle lifecycle;
    std::atomic<std::size_t> nextIndex{0};
    std::mutex failureMutex;
    std::exception_ptr firstFailure;
};

EventLoopPool::EventLoopPool(EventLoopPoolOptions options)
    : impl_(std::make_unique<Impl>(options)) {}

EventLoopPool::~EventLoopPool() {
    stop();
    try {
        join();
    } catch (...) {
    }
}

void EventLoopPool::start() {
    if (!impl_->lifecycle.start()) {
        throw std::logic_error("event loop pool can only be started once");
    }
    try {
        for (const auto& loop : impl_->loops) {
            loop->thread = std::thread([this, loop] {
                try {
                    loop->ioContext.run();
                } catch (...) {
                    impl_->recordFailure(std::current_exception());
                    impl_->stop();
                }
            });
        }
    } catch (...) {
        impl_->stop();
        for (const auto& loop : impl_->loops) {
            if (loop->thread.joinable()) {
                loop->thread.join();
            }
        }
        impl_->lifecycle.completeStop();
        throw;
    }
}

void EventLoopPool::stop() noexcept {
    impl_->stop();
}

void EventLoopPool::join() {
    if (impl_->lifecycle.state() == detail::RuntimeLifecycle::State::kReady) {
        impl_->stop();
    }
    for (const auto& loop : impl_->loops) {
        if (loop->thread.joinable()) {
            loop->thread.join();
        }
    }
    impl_->lifecycle.completeStop();

    std::exception_ptr failure;
    {
        std::lock_guard lock(impl_->failureMutex);
        failure = std::exchange(impl_->firstFailure, nullptr);
    }
    if (failure) {
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
