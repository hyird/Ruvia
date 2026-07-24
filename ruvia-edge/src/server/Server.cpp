#include "ruvia/edge/detail/server/ServerImpl.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/co_spawn.hpp>
#include <asio/error.hpp>
#include <asio/multiple_exceptions.hpp>
#include <asio/post.hpp>

#include "ruvia/core/detail/util/FailureReport.h"
#include "ruvia/edge/detail/proxy/HeaderRules.h"
#include "ruvia/edge/detail/server/TlsContext.h"

namespace ruvia::edge {

namespace {

[[nodiscard]] asio::ip::tcp::endpoint toAsioEndpoint(const EdgeEndpoint& endpoint) {
    return {asio::ip::make_address(endpoint.address), endpoint.port};
}

[[nodiscard]] std::string_view taskKindName(EdgeTaskKind kind) noexcept {
    // A new kind must name itself here; -Werror catches an unhandled one.
    switch (kind) {
        case EdgeTaskKind::kAcceptLoop:
            return "accept";
        case EdgeTaskKind::kSession:
            return "session";
        case EdgeTaskKind::kBackgroundRefresh:
            return "background-refresh";
        case EdgeTaskKind::kWorker:
            return "worker";
        case EdgeTaskKind::kAccessLog:
            return "access-log";
        case EdgeTaskKind::kDiskCache:
            return "disk-cache";
        case EdgeTaskKind::kControl:
            return "control";
    }
    return "unknown";
}

// The last resort when there is no taskFailure callback, or when that callback
// itself threw: the shared reporter every layer ends an unowned failure at.
void writeFailureLine(EdgeTaskKind kind, std::exception_ptr exception) noexcept {
    // "edge session", "edge disk-cache", ... -- one buffer, no allocation on a
    // path that may be reporting bad_alloc.
    std::array<char, 48> context{};
    const auto name = taskKindName(kind);
    static constexpr std::string_view kPrefix = "edge ";
    const auto length = std::min(name.size(), context.size() - kPrefix.size());
    std::memcpy(context.data(), kPrefix.data(), kPrefix.size());
    std::memcpy(context.data() + kPrefix.size(), name.data(), length);
    ruvia::detail::reportUnhandledFailure(std::string_view(context.data(), kPrefix.size() + length), exception);
}

}  // namespace

EdgeServer::Impl::Impl(EdgeEndpoint endpoint, EdgeServerOptions options)
    : acceptor_(ioContext_, toAsioEndpoint(endpoint)),
      shutdownSignal_(ioContext_),
      activeOperations_(memory_.resource()),
      config_(memory_.resource()),
      cache_(options.cache, memory_.resource()),
      disk_(options.cacheDirectory, options.maxDiskCacheBytes, [this](std::exception_ptr exception) { reportFailure(EdgeTaskKind::kDiskCache, std::move(exception)); }),
      fetcher_(options.fetch),
      maxCacheableBytes_(options.maxCacheableBytes),
      maxConnections_(options.maxConnections),
      accessLog_(std::move(options.accessLog)),
      taskFailure_(std::move(options.taskFailure)) {
    const auto bound = acceptor_.local_endpoint();
    localEndpoint_ = EdgeEndpoint{bound.address().to_string(), bound.port()};
    shutdownSignal_.expires_at((std::chrono::steady_clock::time_point::max)());
    if (options.tls) {
        tlsEnabled_ = true;
        storeTlsContext(makeTlsContext(*options.tls));
    }
}

EdgeServer::Impl::~Impl() {
    {
        const std::lock_guard lock(lifecycleMutex_);
        if (workerThreadId_ == std::this_thread::get_id()) {
            std::terminate();
        }
    }
    stop();
}

void EdgeServer::Impl::start() {
    std::unique_lock lock(lifecycleMutex_);
    if (lifecycle_ != Lifecycle::kReady) {
        throw std::logic_error("EdgeServer::start() may be called only once");
    }

    std::promise<std::thread::id> identityPromise;
    auto identity = identityPromise.get_future();
    std::promise<void> runGatePromise;
    auto runGate = runGatePromise.get_future();

    worker_ = std::thread([this, identityPromise = std::move(identityPromise), runGate = std::move(runGate)]() mutable {
        identityPromise.set_value(std::this_thread::get_id());
        runGate.wait();
        // A handler that is not a tracked coroutine's completion (a posted
        // control operation, a timer callback) can throw straight out of
        // run(). Letting it leave this thread function would terminate the
        // process; report it and resume the loop instead, since the
        // io_context stays runnable and the listener is still open.
        for (;;) {
            try {
                ioContext_.run();
                break;
            } catch (...) {
                reportFailure(EdgeTaskKind::kWorker, std::current_exception());
            }
        }
        const std::lock_guard finishedLock(lifecycleMutex_);
        workerThreadId_ = {};
        if (lifecycle_ != Lifecycle::kReady) {
            lifecycle_ = Lifecycle::kStopped;
        }
        lifecycleChanged_.notify_all();
    });
    workerThreadId_ = identity.get();

    try {
        spawnTracked(acceptLoop(), EdgeTaskKind::kAcceptLoop);
        lifecycle_ = Lifecycle::kRunning;
        runGatePromise.set_value();
    } catch (...) {
        ioContext_.stop();
        runGatePromise.set_value();
        std::thread failed = std::move(worker_);
        lock.unlock();
        failed.join();
        ioContext_.restart();
        throw;
    }
}

void EdgeServer::Impl::requestStopOnWorker() noexcept {
    shutdownRequestedOnWorker_ = true;
    asio::error_code ignore;
    acceptor_.close(ignore);
    shutdownSignal_.cancel(ignore);
    // Moving the registry is allocation-free. Completion callbacks erase from
    // the now-empty live registry, so even inline cancellation cannot invalidate
    // this traversal; each callback itself retains its signal until completion.
    auto operations = std::move(activeOperations_);
    for (const auto& operation : operations) {
        operation->emit(asio::cancellation_type::terminal);
    }
    // Do not call io_context::stop(): it abandons ready completions and keeps
    // their coroutine frames until io_context destruction, after several Impl
    // members captured by those frames have already been destroyed. Closing and
    // cancelling all roots lets run() return only after structured teardown.
}

void EdgeServer::Impl::stop() {
    std::thread worker;
    {
        std::unique_lock lock(lifecycleMutex_);
        for (;;) {
            const bool onWorker = workerThreadId_ == std::this_thread::get_id();

            if (lifecycle_ == Lifecycle::kReady) {
                asio::error_code ignore;
                acceptor_.close(ignore);
                lifecycle_ = Lifecycle::kStopped;
                lifecycleChanged_.notify_all();
                break;
            }
            if (lifecycle_ == Lifecycle::kRunning) {
                lifecycle_ = Lifecycle::kStopping;
                if (onWorker) {
                    lock.unlock();
                    requestStopOnWorker();
                    return;
                }
                lifecycleChanged_.wait(lock, [this] { return pendingControls_ == 0; });
                try {
                    asio::post(ioContext_, [this] { requestStopOnWorker(); });
                } catch (...) {
                    // No stop handler was published. Restore the only state in
                    // which another caller can retry, and wake stop/control
                    // waiters so none remains parked behind a phantom owner.
                    lifecycle_ = Lifecycle::kRunning;
                    lifecycleChanged_.notify_all();
                    throw;
                }
                break;
            }
            if (lifecycle_ == Lifecycle::kStopping) {
                if (onWorker) {
                    return;
                }
                lifecycleChanged_.wait(lock, [this] { return lifecycle_ != Lifecycle::kStopping; });
                // A failed publication rolls back to Running. Compete to issue
                // the stop request again; a successful one reaches Stopped.
                continue;
            }
            break;  // kStopped
        }

        if (worker_.joinable()) {
            worker = std::move(worker_);
        }
    }
    if (worker.joinable()) {
        worker.join();
    }
    disk_.stop();
}

void EdgeServer::Impl::join() {
    std::thread worker;
    {
        std::unique_lock lock(lifecycleMutex_);
        if (workerThreadId_ == std::this_thread::get_id()) {
            throw std::logic_error("EdgeServer::join() cannot join its worker thread");
        }
        if (lifecycle_ == Lifecycle::kReady) {
            // Joining before start is a no-op. In particular, do not permanently
            // stop the optional disk executor that a later start() will use.
            return;
        }
        lifecycleChanged_.wait(lock, [this] { return lifecycle_ == Lifecycle::kStopped; });
        if (worker_.joinable()) {
            worker = std::move(worker_);
        }
    }
    if (worker.joinable()) {
        worker.join();
    }
    disk_.stop();
}

EdgeEndpoint EdgeServer::Impl::localEndpoint() const {
    return localEndpoint_;
}

EdgeStats EdgeServer::Impl::stats() const {
    const auto count = [this](EdgeTaskKind kind) { return failureCounts_[static_cast<std::size_t>(kind)].load(std::memory_order_relaxed); };
    EdgeStats stats;
    stats.activeConnections = activeConnections_.load(std::memory_order_relaxed);
    stats.connectionsRefused = connectionsRefused_.load(std::memory_order_relaxed);
    stats.originCircuitRejections = fetcher_.circuitRejectionCount();
    stats.acceptFailures = count(EdgeTaskKind::kAcceptLoop);
    stats.sessionFailures = count(EdgeTaskKind::kSession);
    stats.backgroundRefreshFailures = count(EdgeTaskKind::kBackgroundRefresh);
    stats.workerFailures = count(EdgeTaskKind::kWorker);
    stats.accessLogFailures = count(EdgeTaskKind::kAccessLog);
    stats.diskCacheFailures = count(EdgeTaskKind::kDiskCache);
    stats.controlFailures = count(EdgeTaskKind::kControl);
    return stats;
}

EdgeServer::Impl::TlsContextPtr EdgeServer::Impl::loadTlsContext() const noexcept {
    return tlsContext_;
}

void EdgeServer::Impl::storeTlsContext(TlsContextPtr context) noexcept {
    tlsContext_ = std::move(context);
}

// Asio unwinds a terminally cancelled coroutine by resuming it with
// operation_aborted, so shutdown ends every tracked task with that exception.
// It is how a task stops, not a failure, and reporting it would bury the real
// failures under one line per live connection at every stop().
bool EdgeServer::Impl::isCancellationUnwind(std::exception_ptr exception) noexcept {
    try {
        std::rethrow_exception(exception);
    } catch (const asio::multiple_exceptions& group) {
        // Cancelling an awaitable group (the HTTP/2 session's reader && writer)
        // unwinds both sides, and asio reports that as one wrapper.
        return isCancellationUnwind(group.first_exception());
    } catch (const std::system_error& error) {
        return error.code() == asio::error::operation_aborted;
    } catch (...) {
        // Classification only: returning false sends the caller straight to
        // reportFailure, which still owns the exception.
    }
    return false;
}

void EdgeServer::Impl::reportFailure(EdgeTaskKind kind, std::exception_ptr exception) noexcept {
    if (exception == nullptr) {
        return;
    }
    // Counted before anything that could fail, so a node whose callback throws
    // (or that has none) still has an accurate failure count.
    failureCounts_[static_cast<std::size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
    if (!taskFailure_) {
        writeFailureLine(kind, exception);
        return;
    }
    try {
        // kDiskCache arrives from the disk thread while the worker may be
        // reporting its own failure; the callback sees one at a time.
        const std::lock_guard guard(failureMutex_);
        taskFailure_(EdgeTaskFailure{kind, exception});
    } catch (...) {
        // The reporting callback is the application's; it must not decide
        // whether the original failure is observable. Fall back to the line
        // that needs nothing from the application.
        writeFailureLine(kind, exception);
    }
}

void EdgeServer::Impl::spawnTracked(asio::awaitable<void> operation, EdgeTaskKind kind) {
    auto cancellation = std::make_shared<asio::cancellation_signal>();
    activeOperations_.push_back(cancellation);
    try {
        asio::co_spawn(ioContext_, std::move(operation), asio::bind_cancellation_slot(cancellation->slot(), [this, cancellation, kind](std::exception_ptr exception) noexcept {
            // A detached coroutine has no caller to rethrow into: this
            // completion is the only place its failure can surface.
            // Everything except a shutdown unwind is a failure.
            if (exception != nullptr && !(shutdownRequestedOnWorker_ && isCancellationUnwind(exception))) {
                reportFailure(kind, std::move(exception));
            }
            std::erase(activeOperations_, cancellation);
        }));
        if (shutdownRequestedOnWorker_) {
            cancellation->emit(asio::cancellation_type::terminal);
        }
    } catch (...) {
        std::erase(activeOperations_, cancellation);
        throw;
    }
}

void EdgeServer::Impl::dispatchControl(std::function<void()> operation) {
    std::unique_lock lock(lifecycleMutex_);
    for (;;) {
        if (workerThreadId_ == std::this_thread::get_id()) {
            lock.unlock();
            operation();
            return;
        }

        if (lifecycle_ == Lifecycle::kReady || lifecycle_ == Lifecycle::kStopped) {
            // There is no owner thread in these states, so the lifecycle mutex
            // is the temporary owner and serializes embedding threads.
            operation();
            return;
        }

        if (lifecycle_ == Lifecycle::kStopping) {
            lifecycleChanged_.wait(lock, [this] { return lifecycle_ != Lifecycle::kStopping; });
            // Stop publication may have rolled back. Re-evaluate ownership and
            // either post to the live worker or run after it has stopped.
            continue;
        }
        break;  // kRunning
    }

    ++pendingControls_;
    try {
        asio::post(ioContext_, [this, operation = std::move(operation)]() mutable {
            operation();
            const std::lock_guard completedLock(lifecycleMutex_);
            --pendingControls_;
            lifecycleChanged_.notify_all();
        });
    } catch (...) {
        --pendingControls_;
        lifecycleChanged_.notify_all();
        throw;
    }
}

bool EdgeServer::Impl::addOrigin(std::string frontHost, OriginSettings settings) {
    auto task = std::make_shared<std::packaged_task<bool()>>([this, frontHost = std::move(frontHost), settings = std::move(settings)]() mutable { return config_.addOrigin(std::move(frontHost), std::move(settings)); });
    auto result = task->get_future();
    dispatchControl([task = std::move(task)] { (*task)(); });
    return result.get();
}

bool EdgeServer::Impl::removeOrigin(std::string_view frontHost) {
    auto task = std::make_shared<std::packaged_task<bool()>>([this, frontHost = std::string(frontHost)] { return config_.removeOrigin(frontHost); });
    auto result = task->get_future();
    dispatchControl([task = std::move(task)] { (*task)(); });
    return result.get();
}

bool EdgeServer::Impl::setTlsCertificate(const EdgeTlsConfig& tls) {
    if (!tlsEnabled_) {
        return false;  // TLS was not enabled at startup; the listener is plaintext
    }
    try {
        auto context = makeTlsContext(tls);
        auto task = std::make_shared<std::packaged_task<void()>>([this, context = std::move(context)]() mutable { storeTlsContext(std::move(context)); });
        auto result = task->get_future();
        dispatchControl([task = std::move(task)] { (*task)(); });
        result.get();
    } catch (...) {
        // The caller learns that the rotation failed from the return value, but
        // only the exception says why this PEM was rejected. Report it so that
        // reason is not lost with the exception.
        reportFailure(EdgeTaskKind::kControl, std::current_exception());
        return false;
    }
    return true;
}

bool EdgeServer::Impl::purge(std::string_view frontHost, std::string_view target) {
    // Remove every cached variant of the URL, not just one encoding.
    const std::string prefix = cacheVariantPrefix("GET", frontHost, target);
    auto task = std::make_shared<std::packaged_task<bool()>>([this, prefix] { return cache_.purgePrefix(prefix) > 0; });
    auto memoryResult = task->get_future();
    dispatchControl([task = std::move(task)] { (*task)(); });
    const bool removed = memoryResult.get();
    if (disk_.enabled()) {
        const auto diskResult = disk_.purgePrefixSync(prefix);
        return diskResult.complete && (diskResult.removed > 0 || removed);
    }
    return removed;
}

bool EdgeServer::Impl::clearCache() {
    auto task = std::make_shared<std::packaged_task<void()>>([this] { cache_.clear(); });
    auto memoryResult = task->get_future();
    dispatchControl([task = std::move(task)] { (*task)(); });
    memoryResult.get();
    if (disk_.enabled()) {
        return disk_.clearSync();
    }
    return true;
}

void EdgeServer::Impl::recordRequest(const AccessLogEntry& entry) noexcept {
    if (accessLog_) {
        try {
            accessLog_(entry);
        } catch (...) {
            // Observability is not part of response correctness. In particular,
            // RequestRecord invokes this from its destructor, whose implicit
            // noexcept contract must never turn a user callback into process
            // termination. The exception is not dropped: it is reported like
            // any other task failure.
            reportFailure(EdgeTaskKind::kAccessLog, std::current_exception());
        }
    }
}

void EdgeServer::Impl::wakeInFlight(const std::string& key) {
    const auto it = inFlight_.find(key);
    if (it == inFlight_.end()) {
        return;
    }
    for (auto* waiter : it->second.waiters) {
        waiter->cancel();
    }
    inFlight_.erase(it);
}

EdgeServer::EdgeServer(EdgeEndpoint endpoint, EdgeServerOptions options)
    : impl_(std::make_unique<Impl>(std::move(endpoint), std::move(options))) {}

EdgeServer::~EdgeServer() = default;

void EdgeServer::start() {
    impl_->start();
}

void EdgeServer::stop() {
    impl_->stop();
}

void EdgeServer::join() {
    impl_->join();
}

EdgeEndpoint EdgeServer::localEndpoint() const {
    return impl_->localEndpoint();
}

EdgeStats EdgeServer::stats() const {
    return impl_->stats();
}

bool EdgeServer::addOrigin(std::string frontHost, OriginSettings settings) {
    return impl_->addOrigin(std::move(frontHost), std::move(settings));
}

bool EdgeServer::removeOrigin(std::string_view frontHost) {
    return impl_->removeOrigin(frontHost);
}

bool EdgeServer::setTlsCertificate(const EdgeTlsConfig& tls) {
    return impl_->setTlsCertificate(tls);
}

bool EdgeServer::purge(std::string_view frontHost, std::string_view target) {
    return impl_->purge(frontHost, target);
}

bool EdgeServer::clearCache() {
    return impl_->clearCache();
}

}  // namespace ruvia::edge
