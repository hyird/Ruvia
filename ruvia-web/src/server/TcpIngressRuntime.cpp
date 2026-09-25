#include "ruvia/web/detail/server/TcpIngressRuntime.h"

#include <chrono>
#include <stdexcept>
#include <system_error>

#include <asio/post.hpp>
#include <asio/socket_base.hpp>

namespace ruvia::detail {
namespace {

bool recoverableAcceptError(const asio::error_code& error) noexcept {
    return error == asio::error::connection_aborted || error == asio::error::connection_reset ||
           error == asio::error::eof || error == asio::error::try_again ||
           error == asio::error::would_block || error == asio::error::no_descriptors ||
           error == asio::error::no_buffer_space;
}

}  // namespace

TcpIngressRuntime::TcpIngressRuntime(std::span<const HttpServerListenerDefinition> listeners,
    std::span<const Target> targets, void* failureTarget, FailureCallback failureCallback)
    : workGuard_(asio::make_work_guard(ioContext_)),
      listeners_(processResource()),
      targets_(targets.begin(), targets.end(), processResource()),
      failureTarget_(failureTarget),
      failureCallback_(failureCallback) {
    if (listeners.empty()) {
        throw std::invalid_argument("TCP ingress requires at least one listener");
    }
    for (const auto& target : targets_) {
        if (target.worker == nullptr || target.object == nullptr || target.available == nullptr ||
            target.accept == nullptr) {
            throw std::invalid_argument("TCP ingress target callbacks and object must be set");
        }
    }
    listeners_.reserve(listeners.size());
    for (const auto& definition : listeners) {
        listeners_.push_back(makePmrObject<Listener>(processResource(), ioContext_, definition.endpoint));
    }
}

TcpIngressRuntime::~TcpIngressRuntime() {
    stop();
    {
        std::lock_guard lock(mutex_);
        if (ownerThreadId_ == std::this_thread::get_id()) {
            // Destroying the runtime on its owner thread would invalidate handlers
            // still using it; there is no safe detach-based recovery.
            std::terminate();
        }
    }
    join();
    if (!prepared_ || joined_) {
        // Before launch there is no ingress owner thread yet.
        closeAcceptors();
    }
}

void TcpIngressRuntime::prepare() {
    if (prepared_ || lifecycle_.state() != ruvia::RuntimeLifecycle::State::kReady) {
        throw std::logic_error("TCP ingress runtime can only be prepared once before launch");
    }
    for (auto& listener : listeners_) {
        auto& acceptor = listener->acceptor;
        asio::error_code error;
        acceptor.open(listener->endpoint.protocol(), error);
        if (!error) {
            acceptor.set_option(asio::socket_base::reuse_address(true), error);
        }
        if (!error) {
            acceptor.bind(listener->endpoint, error);
        }
        if (!error) {
            acceptor.listen(asio::socket_base::max_listen_connections, error);
        }
        if (!error) {
            listener->endpoint = acceptor.local_endpoint(error);
        }
        if (error) {
            closeAcceptors();
            throw std::system_error(error, "failed to prepare TCP ingress listener");
        }
    }
    prepared_ = true;
}

void TcpIngressRuntime::launch() {
    std::lock_guard lock(mutex_);
    if (!prepared_) {
        throw std::logic_error("TCP ingress runtime must be prepared before launch");
    }
    if (!lifecycle_.start()) {
        throw std::logic_error("TCP ingress runtime can only be launched once");
    }
    try {
        // Publish thread state under the same lock used by stop() and readiness.
        thread_ = std::thread([this] { run(); });
        launched_ = true;
    } catch (...) {
        (void)lifecycle_.requestStop();
        lifecycle_.completeStop();
        condition_.notify_all();
        throw;
    }
}

void TcpIngressRuntime::waitUntilReady() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] {
        const auto current = lifecycle_.state();
        return ready_ || current == ruvia::RuntimeLifecycle::State::kStopping ||
               current == ruvia::RuntimeLifecycle::State::kStopped;
    });
}

void TcpIngressRuntime::requestServe() {
    {
        std::lock_guard lock(mutex_);
        if (lifecycle_.state() != ruvia::RuntimeLifecycle::State::kRunning || serveRequested_) {
            return;
        }
        serveRequested_ = true;
    }
    try {
        asio::post(ioContext_, [this] {
            {
                std::lock_guard lock(mutex_);
                if (lifecycle_.state() != ruvia::RuntimeLifecycle::State::kRunning) {
                    condition_.notify_all();
                    return;
                }
                serving_ = true;
                condition_.notify_all();
            }
            for (std::size_t i = 0; i < listeners_.size(); ++i) {
                beginAccept(i);
            }
        });
    } catch (...) {
        {
            std::lock_guard lock(mutex_);
            if (failure_ == nullptr) {
                failure_ = std::current_exception();
            }
            (void)lifecycle_.requestStop();
            condition_.notify_all();
        }
        if (failureCallback_ != nullptr) {
            failureCallback_(failureTarget_);
        }
        try {
            ioContext_.stop();  // run() performs owner-thread cleanup on exit.
        } catch (...) {
            // Neither the cleanup post nor the fallback wakeup succeeded;
            // continuing would strand the owner thread and hang join().
            std::terminate();
        }
    }
}

bool TcpIngressRuntime::waitUntilServing() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] {
        const auto current = lifecycle_.state();
        return serving_ || failure_ != nullptr || current == ruvia::RuntimeLifecycle::State::kStopping ||
               current == ruvia::RuntimeLifecycle::State::kStopped;
    });
    return serving_ && failure_ == nullptr;
}

void TcpIngressRuntime::stop() noexcept {
    bool hasThread = false;
    {
        std::lock_guard lock(mutex_);
        if (!lifecycle_.requestStop()) {
            return;
        }
        hasThread = launched_;
        if (!hasThread) {
            workGuard_.reset();
            lifecycle_.completeStop();
            condition_.notify_all();
            return;  // No owner thread was launched; destruction owns cleanup.
        }
        condition_.notify_all();
    }
    try {
        asio::post(ioContext_, [this] {
            closeAcceptors();
            workGuard_.reset();
            std::lock_guard lock(mutex_);
            condition_.notify_all();
        });
    } catch (...) {
        // Do not close acceptors or reset the guard from the caller thread.
        // The owner performs cleanup after io_context::run() returns.
        try {
            ioContext_.stop();
        } catch (...) {
            // The owner cannot be woken and owns live acceptors/timers.
            // Fail fast instead of waiting forever or closing them off-thread.
            std::terminate();
        }
    }
}

void TcpIngressRuntime::join() {
    std::thread joiningThread;
    {
        std::unique_lock lock(mutex_);
        if (thread_.joinable() && thread_.get_id() == std::this_thread::get_id()) {
            throw std::logic_error("cannot join TCP ingress runtime from its own thread");
        }
        condition_.wait(lock, [this] { return !joining_; });
        if (joined_) {
            return;
        }
        if (thread_.joinable()) {
            joining_ = true;
            joiningThread = std::move(thread_);
        } else {
            joined_ = true;
        }
    }
    if (joiningThread.joinable()) {
        joiningThread.join();
    }
    {
        std::lock_guard lock(mutex_);
        joining_ = false;
        joined_ = true;
        if (lifecycle_.state() == ruvia::RuntimeLifecycle::State::kStopping) {
            lifecycle_.completeStop();
        }
        condition_.notify_all();
    }
}

asio::ip::tcp::endpoint TcpIngressRuntime::localEndpoint(std::size_t index) const {
    return listeners_.at(index)->endpoint;
}

std::exception_ptr TcpIngressRuntime::failure() const noexcept {
    std::lock_guard lock(mutex_);
    return failure_;
}

void TcpIngressRuntime::rethrowFailure() const {
    if (auto exception = failure()) {
        std::rethrow_exception(exception);
    }
}

void TcpIngressRuntime::run() noexcept {
    {
        std::lock_guard lock(mutex_);
        ownerThreadId_ = std::this_thread::get_id();
        ready_ = true;
        condition_.notify_all();
    }
    try {
        ioContext_.run();
    } catch (...) {
        fail(std::current_exception());
    }
    closeAcceptors();  // The ingress owner thread is the only post-launch closer.
    workGuard_.reset();
    {
        std::lock_guard lock(mutex_);
        (void)lifecycle_.requestStop();
        lifecycle_.completeStop();
        condition_.notify_all();
    }
}

void TcpIngressRuntime::beginAccept(std::size_t listenerIndex) noexcept {
    if (lifecycle_.state() != ruvia::RuntimeLifecycle::State::kRunning || targets_.empty()) {
        return;
    }
    auto& acceptor = listeners_[listenerIndex]->acceptor;
    if (!acceptor.is_open()) {
        return;
    }
    try {
        // The accepted socket is always constructed on the ingress context. In
        // particular, UNSAFE_IO worker contexts must never own ingress accepts.
        acceptor.async_accept([this, listenerIndex](const asio::error_code& error,
                                  TcpIngressSocket socket) mutable {
            accepted(listenerIndex, error, std::move(socket));
        });
    } catch (...) {
        fail(std::current_exception());
    }
}

void TcpIngressRuntime::accepted(std::size_t listenerIndex,
    const asio::error_code& error, TcpIngressSocket socket) noexcept {
    if (error) {
        if (error == asio::error::operation_aborted &&
            lifecycle_.state() != ruvia::RuntimeLifecycle::State::kRunning) {
            return;
        }
        if (!recoverableAcceptError(error)) {
            try {
                throw std::system_error(error, "fatal TCP ingress accept error");
            } catch (...) {
                fail(std::current_exception());
            }
            return;
        }
        scheduleRetry(listenerIndex);
        return;
    }
    // Detach while still on the ingress owner. On platforms where Asio cannot
    // release native ownership, this connection is rejected (never retried).
    asio::error_code releaseError;
    const auto native = socket.release(releaseError);
    if (releaseError) {
        try {
            throw std::system_error(releaseError, "failed to detach accepted TCP socket");
        } catch (...) {
            fail(std::current_exception());
        }
        return;
    }
    NativeAcceptedSocketTicket ticket(listeners_[listenerIndex]->endpoint.protocol(), listenerIndex,
        native);
    if (lifecycle_.state() != ruvia::RuntimeLifecycle::State::kRunning || targets_.empty()) {
        return;
    }
    std::size_t selected = targets_.size();
    for (std::size_t offset = 0; offset < targets_.size(); ++offset) {
        const auto index = (nextTarget_ + offset) % targets_.size();
        if (targets_[index].available(targets_[index].object) &&
            targets_[index].worker->accepting()) {
            selected = index;
            nextTarget_ = (index + 1) % targets_.size();
            break;
        }
    }
    if (selected == targets_.size()) {
        scheduleRetry(listenerIndex);
        return;
    }
    const auto target = targets_[selected];
    try {
        auto posted = target.worker->post([target, ticket = std::move(ticket)]() mutable noexcept {
            if (!target.available(target.object)) {
                return;
            }
            target.accept(target.object, std::move(ticket));
        });
        if (!posted.accepted()) {
            auto rejected = std::move(posted).takeRejected();
            rejected = {};
        }
    } catch (...) {
        // ticket's destructor closes the native socket as this frame unwinds.
        fail(std::current_exception());
        return;
    }
    if (lifecycle_.state() == ruvia::RuntimeLifecycle::State::kRunning) {
        beginAccept(listenerIndex);
    }
}

void TcpIngressRuntime::scheduleRetry(std::size_t listenerIndex) noexcept {
    if (lifecycle_.state() != ruvia::RuntimeLifecycle::State::kRunning) {
        return;
    }
    try {
        auto& timer = listeners_[listenerIndex]->retry;
        timer.expires_after(std::chrono::milliseconds(25));
        timer.async_wait([this, listenerIndex](const asio::error_code& error) {
            if (!error) {
                beginAccept(listenerIndex);
            }
        });
    } catch (...) {
        fail(std::current_exception());
    }
}

void TcpIngressRuntime::fail(std::exception_ptr failure) noexcept {
    {
        std::lock_guard lock(mutex_);
        if (failure_ == nullptr) {
            failure_ = std::move(failure);
        }
        (void)lifecycle_.requestStop();
        condition_.notify_all();
    }
    // fail() is called only from ingress io_context handlers; keep descriptor
    // teardown on its owner thread and prevent another accept from being armed.
    closeAcceptors();
    workGuard_.reset();
    if (failureCallback_ != nullptr) {
        failureCallback_(failureTarget_);
    }
}

void TcpIngressRuntime::closeAcceptors() noexcept {
    for (auto& listener : listeners_) {
        asio::error_code ignored;
        listener->retry.cancel(ignored);
        listener->acceptor.close(ignored);
    }
}

}  // namespace ruvia::detail
