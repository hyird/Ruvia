#include "ruvia/edge/detail/server/ServerImpl.h"

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/co_spawn.hpp>
#include <asio/post.hpp>

#include "ruvia/edge/detail/proxy/HeaderRules.h"
#include "ruvia/edge/detail/server/TlsContext.h"

namespace ruvia::edge {

namespace {

[[nodiscard]] asio::ip::tcp::endpoint toAsioEndpoint(const EdgeEndpoint& endpoint) {
    return {asio::ip::make_address(endpoint.address), endpoint.port};
}

}  // namespace

EdgeServer::Impl::Impl(EdgeEndpoint endpoint, EdgeServerOptions options)
    : acceptor_(ioContext_, toAsioEndpoint(endpoint)),
      shutdownSignal_(ioContext_),
      activeOperations_(memory_.resource()),
      config_(memory_.resource()),
      cache_(options.cache, memory_.resource()),
      disk_(options.cacheDirectory, options.maxDiskCacheBytes),
      fetcher_(options.fetch),
      maxCacheableBytes_(options.maxCacheableBytes),
      accessLog_(std::move(options.accessLog)) {
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

    worker_ = std::thread(
        [this, identityPromise = std::move(identityPromise),
         runGate = std::move(runGate)]() mutable {
            identityPromise.set_value(std::this_thread::get_id());
            runGate.wait();
            ioContext_.run();
            const std::lock_guard finishedLock(lifecycleMutex_);
            workerThreadId_ = {};
            if (lifecycle_ != Lifecycle::kReady) {
                lifecycle_ = Lifecycle::kStopped;
            }
            lifecycleChanged_.notify_all();
        });
    workerThreadId_ = identity.get();

    try {
        spawnTracked(acceptLoop());
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
            const bool onWorker =
                workerThreadId_ == std::this_thread::get_id();

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
                lifecycleChanged_.wait(
                    lock, [this] { return pendingControls_ == 0; });
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
                lifecycleChanged_.wait(lock, [this] {
                    return lifecycle_ != Lifecycle::kStopping;
                });
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
        lifecycleChanged_.wait(
            lock, [this] { return lifecycle_ == Lifecycle::kStopped; });
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

EdgeServer::Impl::TlsContextPtr EdgeServer::Impl::loadTlsContext() const noexcept {
    return tlsContext_;
}

void EdgeServer::Impl::storeTlsContext(TlsContextPtr context) noexcept {
    tlsContext_ = std::move(context);
}

void EdgeServer::Impl::spawnTracked(asio::awaitable<void> operation) {
    auto cancellation = std::make_shared<asio::cancellation_signal>();
    activeOperations_.push_back(cancellation);
    try {
        asio::co_spawn(
            ioContext_,
            std::move(operation),
            asio::bind_cancellation_slot(
                cancellation->slot(),
                [this, cancellation](std::exception_ptr) noexcept {
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

        if (lifecycle_ == Lifecycle::kReady ||
            lifecycle_ == Lifecycle::kStopped) {
            // There is no owner thread in these states, so the lifecycle mutex
            // is the temporary owner and serializes embedding threads.
            operation();
            return;
        }

        if (lifecycle_ == Lifecycle::kStopping) {
            lifecycleChanged_.wait(lock, [this] {
                return lifecycle_ != Lifecycle::kStopping;
            });
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
    auto task = std::make_shared<std::packaged_task<bool()>>(
        [this, frontHost = std::move(frontHost), settings = std::move(settings)]() mutable {
            return config_.addOrigin(std::move(frontHost), std::move(settings));
        });
    auto result = task->get_future();
    dispatchControl([task = std::move(task)] { (*task)(); });
    return result.get();
}

bool EdgeServer::Impl::removeOrigin(std::string_view frontHost) {
    auto task = std::make_shared<std::packaged_task<bool()>>(
        [this, frontHost = std::string(frontHost)] {
            return config_.removeOrigin(frontHost);
        });
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
        auto task = std::make_shared<std::packaged_task<void()>>(
            [this, context = std::move(context)]() mutable {
                storeTlsContext(std::move(context));
            });
        auto result = task->get_future();
        dispatchControl([task = std::move(task)] { (*task)(); });
        result.get();
    } catch (...) {
        return false;  // invalid PEM
    }
    return true;
}

bool EdgeServer::Impl::purge(std::string_view frontHost, std::string_view target) {
    // Remove every cached variant of the URL, not just one encoding.
    const std::string prefix = cacheVariantPrefix("GET", frontHost, target);
    auto task = std::make_shared<std::packaged_task<bool()>>(
        [this, prefix] { return cache_.purgePrefix(prefix) > 0; });
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
    auto task = std::make_shared<std::packaged_task<void()>>(
        [this] { cache_.clear(); });
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
            // termination.
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
