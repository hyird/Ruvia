#include "ruvia/web/db/DbClient.h"

#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#include <asio/bind_executor.hpp>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/detail/db/DbClientState.h"

namespace ruvia::detail {

EventLoop DbClientState::requireLoop(EventLoop loop) {
    if (!loop.valid()) {
        throw std::invalid_argument("database client requires a valid event loop");
    }
    return loop;
}

DbClientState::DbClientState(EventLoop loop, const DbConfig& config)
    : loop_(requireLoop(std::move(loop))),
      worker_(loop_.handle()),
      memory_(),
      databases_(loop_.ioContext(), worker_, memory_.resource(), config),
      closeState_(worker_) {}

DbClientState::~DbClientState() {
    const auto phase = phase_.load(std::memory_order_acquire);
    if (phase != Phase::kClosed || !closeState_.complete() ||
        operationScope_.hasPendingOperations()) {
        std::terminate();
    }
}

void DbClientState::bindStop() {
    try {
        std::weak_ptr<DbClientState> weak = shared_from_this();
        stopRegistration_ = loop_.onStop([weak = std::move(weak)] {
            if (const auto state = weak.lock()) {
                state->startCloseOnWorker();
            }
        });
    } catch (...) {
        databases_.closeNow();
        phase_.store(Phase::kClosed, std::memory_order_release);
        closeState_.completeBeforeWorkerStart();
        throw;
    }
}

Task<void> DbClientState::connect() {
    return connectOwned(shared_from_this());
}

Task<void> DbClientState::connectOwned(std::shared_ptr<DbClientState> state) {
    co_await state->connectOnWorker();
}

Task<void> DbClientState::connectOnWorker() {
    if (!worker_.isCurrent()) {
        throw std::logic_error("database client must connect on its bound event loop");
    }

    try {
        auto expected = Phase::kFresh;
        if (!phase_.compare_exchange_strong(expected, Phase::kConnecting, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            if (expected == Phase::kClosing || expected == Phase::kClosed) {
                throw std::runtime_error("database client closed before connecting");
            }
            throw std::logic_error("database client can only connect once");
        }
        connectInFlight_ = true;
        if (stopSource_.stopRequested() || !worker_.accepting()) {
            throw std::runtime_error("database client closed before connecting");
        }
        co_await databases_.connect();
        expected = Phase::kConnecting;
        if (!phase_.compare_exchange_strong(expected, Phase::kConnected, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            throw std::runtime_error("worker stopped while database client was connecting");
        }
        connectInFlight_ = false;
        closeState_.notifyProgress();
    } catch (...) {
        databases_.closeNow();
        stopSource_.requestStop();
        phase_.store(Phase::kClosed, std::memory_order_release);
        connectInFlight_ = false;
        if (closeState_.taskStarted()) {
            closeState_.notifyProgress();
        } else if (!closeState_.complete()) {
            closeState_.completeNow();
        }
        throw;
    }
}

DbHandle DbClientState::handle(OperationOptions options) {
    requireConnectedOnWorker();
    options = mergeOperationOptions(
        OperationOptions{.timeout = std::nullopt, .stopToken = stopSource_.token()},
        std::move(options));
    return databases_.get(memory_.resource(), operationScope_).withOptions(std::move(options));
}

void DbClientState::requireConnectedOnWorker() const {
    if (!worker_.isCurrent()) {
        throw std::logic_error("database client must be used on its bound event loop");
    }
    if (phase_.load(std::memory_order_acquire) != Phase::kConnected) {
        throw std::logic_error("database client is not connected");
    }
}

void DbClientState::requestClose() noexcept {
    stopSource_.requestStop();
    auto phase = phase_.load(std::memory_order_acquire);
    for (;;) {
        if (phase == Phase::kClosed || phase == Phase::kClosing) {
            return;
        }
        if (phase == Phase::kFresh) {
            // A fresh client has not published any backend operation or
            // worker-owned connection yet. Closing it here preserves the
            // no-loop-needed destruction path while the atomic transition
            // excludes a concurrent connect() from entering the backend.
            if (phase_.compare_exchange_weak(
                    phase, Phase::kClosed, std::memory_order_acq_rel, std::memory_order_acquire)) {
                closeState_.completeBeforeWorkerStart();
                return;
            }
            continue;
        }
        if (phase_.compare_exchange_weak(
                phase, Phase::kClosing, std::memory_order_acq_rel, std::memory_order_acquire)) {
            break;
        }
    }

    if (worker_.isCurrent()) {
        startCloseOnWorker();
        return;
    }
    try {
        if (!WorkerHandleAccess::deferIfAttached(
                worker_, [state = shared_from_this()] { state->startCloseOnWorker(); })) {
            if (phase_.load(std::memory_order_acquire) != Phase::kClosed) {
                std::terminate();
            }
        }
    } catch (...) {
        if (phase_.load(std::memory_order_acquire) != Phase::kClosed) {
            std::terminate();
        }
    }
}

Task<void> DbClientState::shutdown() {
    return shutdownOwned(shared_from_this());
}

Task<void> DbClientState::shutdownOwned(std::shared_ptr<DbClientState> state) {
    if (!state->worker_.isCurrent()) {
        throw std::logic_error("database client shutdown must run on its bound event loop");
    }
    state->startCloseOnWorker();
    while (!state->closeState_.complete()) {
        co_await state->closeState_.wait();
    }
    state->closeState_.rethrowFailure();
}

void DbClientState::startCloseOnWorker() noexcept {
    if (!worker_.isCurrent()) {
        std::terminate();
    }
    auto phase = phase_.load(std::memory_order_acquire);
    while (phase != Phase::kClosing && phase != Phase::kClosed) {
        if (phase_.compare_exchange_weak(
                phase, Phase::kClosing, std::memory_order_acq_rel, std::memory_order_acquire)) {
            break;
        }
    }
    stopSource_.requestStop();
    databases_.closeNow();
    if (!closeState_.startTask()) {
        return;
    }
    try {
        auto state = shared_from_this();
        asyncStartTask(closeOnWorker(),
            asio::bind_executor(loop_.executor(),
                [state](const TaskCompletionResult<void>& result) { state->finishClose(result); }));
    } catch (...) {
        phase_.store(Phase::kClosed, std::memory_order_release);
        std::terminate();
    }
}

Task<void> DbClientState::closeOnWorker() {
    while (connectInFlight_) {
        co_await closeState_.wait();
    }
    co_await operationScope_.closeAndJoin();
}

void DbClientState::finishClose(const TaskCompletionResult<void>& result) {
    phase_.store(Phase::kClosed, std::memory_order_release);
    closeState_.finish(result);
}

}  // namespace ruvia::detail

namespace ruvia {

DbClient::DbClient(EventLoop loop, const DbConfig& config)
    : state_(std::make_shared<detail::DbClientState>(std::move(loop), config)) {
    state_->bindStop();
}

DbClient::~DbClient() {
    state_->requestClose();
}

Task<void> DbClient::connect() & {
    return state_->connect();
}

DbHandle DbClient::withOptions(OperationOptions options) const& {
    return state_->handle(std::move(options));
}

ScopedOperation<DbRows> DbClient::query(
    std::string_view sql, std::span<const DbValue> params) const& {
    return withOptions({}).query(sql, params);
}

ScopedOperation<DbExecResult> DbClient::execute(
    std::string_view sql, std::span<const DbValue> params) const& {
    return withOptions({}).execute(sql, params);
}

ScopedOperation<DbStreamResult> DbClient::queryStream(
    std::string_view sql, std::span<const DbValue> params) const& {
    return withOptions({}).queryStream(sql, params);
}

ScopedOperation<DbTransaction> DbClient::beginTransaction() const& {
    return withOptions({}).beginTransaction();
}

void DbClient::close() noexcept {
    state_->requestClose();
}

Task<void> DbClient::shutdown() & {
    return state_->shutdown();
}

const WorkerHandle& DbClient::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia
