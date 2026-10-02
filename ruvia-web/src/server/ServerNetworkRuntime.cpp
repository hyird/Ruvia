#include "ruvia/web/detail/server/ServerNetworkRuntime.h"

#include <algorithm>
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

ServerNetworkRuntime::ServerNetworkRuntime(std::span<const HttpServerListenerDefinition> listeners,
    std::span<const Target> targets, void* failureTarget, FailureCallback failureCallback)
    : workerRuntime_(ioContext_, 128),
      workGuard_(asio::make_work_guard(ioContext_)),
      listenerDefinitions_(listeners),
      listeners_(processResource()),
      targets_(targets.begin(), targets.end(), processResource()),
      http3Servers_(processResource()),
      failureTarget_(failureTarget),
      failureCallback_(failureCallback) {
    if (listeners.empty()) {
        throw std::invalid_argument("server network requires at least one TCP listener");
    }
    const auto http3Count = std::ranges::count_if(listeners,
        [](const HttpServerListenerDefinition& listener) { return listener.http3.has_value(); });
    if (http3Count > 1) {
        throw std::invalid_argument("server network runtime supports one HTTP/3 listener");
    }
    if (http3Count != 0) {
        if (targets_.empty()) {
            throw std::invalid_argument("HTTP/3 server network requires worker targets");
        }
        for (const auto& target : targets_) {
            if (target.http3Server == nullptr || target.http3MaxConnections == 0 ||
                target.http3MailboxCapacity == 0 ||
                target.http3MaxRequestsPerConnection == 0) {
                throw std::invalid_argument("HTTP/3 server network worker target is incomplete");
            }
        }
    }
    for (const auto& target : targets_) {
        if (!target.submission.valid() || target.object == nullptr || target.available == nullptr ||
            target.accept == nullptr) {
            throw std::invalid_argument("server network TCP target callbacks and object must be set");
        }
    }
    listeners_.reserve(listeners.size());
    http3Servers_.reserve(static_cast<std::size_t>(http3Count));
    for (const auto& definition : listeners) {
        listeners_.push_back(makePmrObject<Listener>(processResource(), ioContext_, definition.endpoint));
    }
}

void ServerNetworkRuntime::prepareHttp3Owners() {
    std::pmr::vector<Http3NetworkRuntime::WorkerTarget> configuredTargets(processResource());
    const auto listener = std::ranges::find_if(listenerDefinitions_,
        [](const HttpServerListenerDefinition& definition) {
            return definition.http3.has_value();
        });
    if (listener == listenerDefinitions_.end()) {
        return;
    }
    configuredTargets.reserve(targets_.size());
    for (const auto& target : targets_) {
        configuredTargets.push_back({
            .server = target.http3Server,
            .maxConnections = target.http3MaxConnections,
            .mailboxCapacity = target.http3MailboxCapacity,
            .maxRequestsPerConnection = target.http3MaxRequestsPerConnection,
            .idleTimeout = target.http3IdleTimeout,
            .requestHeaderTimeout = target.http3RequestHeaderTimeout,
            .requestBodyTimeout = target.http3RequestBodyTimeout,
            .writeTimeout = target.http3WriteTimeout,
        });
    }
    auto configuredListener = *listener;
    const auto listenerIndex = static_cast<std::size_t>(std::distance(listenerDefinitions_.begin(), listener));
    configuredListener.endpoint = listeners_[listenerIndex]->endpoint;
    http3Servers_.push_back(makePmrObject<Http3NetworkRuntime>(processResource(),
        workerRuntime_, configuredListener, configuredTargets,
        Http3NetworkRuntime::FailureNotification{this, &http3Failed}));
    http3Servers_.back()->stageWorkerLinks();
}

void ServerNetworkRuntime::http3Failed(void* context, std::exception_ptr failure) noexcept {
    static_cast<ServerNetworkRuntime*>(context)->fail(std::move(failure));
}

ServerNetworkRuntime::~ServerNetworkRuntime() {
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
        // Before launch there is no server network thread yet.
        closeAcceptors();
    }
}

void ServerNetworkRuntime::prepare() {
    if (prepared_ || lifecycle_.state() != ruvia::RuntimeLifecycle::State::kReady) {
        throw std::logic_error("server network runtime can only be prepared once before launch");
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
            throw std::system_error(error, "failed to prepare TCP listener on server network runtime");
        }
    }
    prepared_ = true;
}

void ServerNetworkRuntime::launch() {
    std::lock_guard lock(mutex_);
    if (!prepared_) {
        throw std::logic_error("server network runtime must be prepared before launch");
    }
    if (!lifecycle_.start()) {
        throw std::logic_error("server network runtime can only be launched once");
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

void ServerNetworkRuntime::waitUntilReady() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] {
        const auto current = lifecycle_.state();
        return ready_ || current == ruvia::RuntimeLifecycle::State::kStopping ||
               current == ruvia::RuntimeLifecycle::State::kStopped;
    });
}

void ServerNetworkRuntime::requestServe() {
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
            }
            try {
                for (auto& server : http3Servers_) {
                    server->start();
                }
                for (std::size_t i = 0; i < listeners_.size(); ++i) {
                    beginAccept(i);
                }
                std::lock_guard lock(mutex_);
                if (lifecycle_.state() == ruvia::RuntimeLifecycle::State::kRunning &&
                    failure_ == nullptr) {
                    serving_ = true;
                }
                condition_.notify_all();
            } catch (...) {
                fail(std::current_exception());
            }
        });
    } catch (...) {
        const auto failure = std::current_exception();
        {
            std::lock_guard lock(mutex_);
            if (failure_ == nullptr) {
                failure_ = failure;
            }
            (void)lifecycle_.requestStop();
            condition_.notify_all();
        }
        if (failureCallback_ != nullptr) {
            failureCallback_(failureTarget_);
        }
        workerRuntime_.deferOrTerminate([this, failure] { fail(failure); });
    }
}

bool ServerNetworkRuntime::waitUntilServing() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] {
        const auto current = lifecycle_.state();
        return serving_ || failure_ != nullptr || current == ruvia::RuntimeLifecycle::State::kStopping ||
               current == ruvia::RuntimeLifecycle::State::kStopped;
    });
    return serving_ && failure_ == nullptr;
}

void ServerNetworkRuntime::stop() noexcept {
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
            for (auto& server : http3Servers_) {
                server->stop();
            }
            workGuard_.reset();
            std::lock_guard lock(mutex_);
            condition_.notify_all();
        });
    } catch (...) {
        // Preserve owner-thread cleanup even if the public Asio post cannot
        // allocate. The dispatcher's internal control lane is reliable.
        workerRuntime_.deferOrTerminate([this] {
            closeAcceptors();
            for (auto& server : http3Servers_) {
                server->stop();
            }
            workGuard_.reset();
            std::lock_guard lock(mutex_);
            condition_.notify_all();
        });
    }
}

void ServerNetworkRuntime::join() {
    std::thread joiningThread;
    {
        std::unique_lock lock(mutex_);
        if (thread_.joinable() && thread_.get_id() == std::this_thread::get_id()) {
            throw std::logic_error("cannot join server network runtime from its own thread");
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

asio::ip::tcp::endpoint ServerNetworkRuntime::localEndpoint(std::size_t index) const {
    return listeners_.at(index)->endpoint;
}

std::exception_ptr ServerNetworkRuntime::failure() const noexcept {
    std::lock_guard lock(mutex_);
    return failure_;
}

void ServerNetworkRuntime::rethrowFailure() const {
    if (auto exception = failure()) {
        std::rethrow_exception(exception);
    }
}

void ServerNetworkRuntime::run() noexcept {
    {
        std::lock_guard lock(mutex_);
        ownerThreadId_ = std::this_thread::get_id();
    }
    try {
        prepareHttp3Owners();
        std::lock_guard lock(mutex_);
        if (failure_ == nullptr &&
            lifecycle_.state() == ruvia::RuntimeLifecycle::State::kRunning) {
            ready_ = true;
        }
        condition_.notify_all();
    } catch (...) {
        fail(std::current_exception());
    }

    try {
        // The dispatcher resumes io_context after a thrown handler. Report
        // failure immediately so fail() drops the work guard before that retry;
        // otherwise an idle server network runtime could wait forever with its guard held.
        workerRuntime_.run([this](std::exception_ptr failure) noexcept { fail(failure); });
    } catch (...) {
        fail(std::current_exception());
    }

    closeAcceptors();  // The server network thread is the only post-launch closer.
    for (const auto& server : http3Servers_) {
        if (!server->drained()) {
            std::terminate();
        }
    }
    http3Servers_.clear();
    workGuard_.reset();
    workerRuntime_.close();
    {
        std::lock_guard lock(mutex_);
        (void)lifecycle_.requestStop();
        lifecycle_.completeStop();
        condition_.notify_all();
    }
}

void ServerNetworkRuntime::beginAccept(std::size_t listenerIndex) noexcept {
    if (lifecycle_.state() != ruvia::RuntimeLifecycle::State::kRunning || targets_.empty()) {
        return;
    }
    auto& acceptor = listeners_[listenerIndex]->acceptor;
    if (!acceptor.is_open()) {
        return;
    }
    try {
        // The accepted socket is always constructed on the server network context. In
        // particular, UNSAFE_IO worker contexts must never own network accepts.
        acceptor.async_accept([this, listenerIndex](const asio::error_code& error,
                                  TcpAcceptedSocket socket) mutable {
            accepted(listenerIndex, error, std::move(socket));
        });
    } catch (...) {
        fail(std::current_exception());
    }
}

void ServerNetworkRuntime::accepted(std::size_t listenerIndex,
    const asio::error_code& error, TcpAcceptedSocket socket) noexcept {
    if (error) {
        if (error == asio::error::operation_aborted &&
            lifecycle_.state() != ruvia::RuntimeLifecycle::State::kRunning) {
            return;
        }
        if (!recoverableAcceptError(error)) {
            try {
                throw std::system_error(error, "fatal TCP accept error on server network runtime");
            } catch (...) {
                fail(std::current_exception());
            }
            return;
        }
        scheduleRetry(listenerIndex);
        return;
    }
    // Detach while still on the server network owner. On platforms where Asio cannot
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
            targets_[index].submission.accepting()) {
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
        auto posted = target.submission.post([target, ticket = std::move(ticket)]() mutable noexcept {
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

void ServerNetworkRuntime::scheduleRetry(std::size_t listenerIndex) noexcept {
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

void ServerNetworkRuntime::fail(std::exception_ptr failure) noexcept {
    {
        std::lock_guard lock(mutex_);
        if (failure_ == nullptr) {
            failure_ = std::move(failure);
        }
        (void)lifecycle_.requestStop();
        condition_.notify_all();
    }
    // fail() runs on the server network owner. Stop both TCP accepts and HTTP/3 so
    // their owner-thread resources drain before the context can exit.
    closeAcceptors();
    for (auto& server : http3Servers_) {
        server->stop();
    }
    workGuard_.reset();
    if (failureCallback_ != nullptr) {
        failureCallback_(failureTarget_);
    }
}

void ServerNetworkRuntime::closeAcceptors() noexcept {
    for (auto& listener : listeners_) {
        asio::error_code ignored;
        listener->retry.cancel(ignored);
        listener->acceptor.close(ignored);
    }
}

}  // namespace ruvia::detail
