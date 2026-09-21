#include "ruvia/web/redis/RedisClient.h"

#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#include <asio/bind_executor.hpp>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/detail/redis/RedisClientState.h"

namespace ruvia::detail {

EventLoop RedisClientState::requireLoop(EventLoop loop) {
    if (!loop.valid()) {
        throw std::invalid_argument("redis client requires a valid event loop");
    }
    return loop;
}

RedisClientState::RedisClientState(EventLoop loop, const RedisConfig& config)
    : loop_(requireLoop(std::move(loop))),
      worker_(loop_.handle()),
      memory_(),
      runtime_(loop_.ioContext(), worker_, RedisConfigStorage(config, memory_.resource()), memory_.resource()),
      closeState_(worker_) {}

RedisClientState::~RedisClientState() {
    const auto phase = phase_.load(std::memory_order_acquire);
    if (phase != Phase::kClosed || !closeState_.complete() ||
        operationScope_.hasPendingOperations()) {
        std::terminate();
    }
}

void RedisClientState::bindStop() {
    try {
        std::weak_ptr<RedisClientState> weak = shared_from_this();
        stopRegistration_ = loop_.onStop([weak = std::move(weak)] {
            if (const auto state = weak.lock()) {
                state->startCloseOnWorker();
            }
        });
    } catch (...) {
        runtime_.closeNow();
        phase_.store(Phase::kClosed, std::memory_order_release);
        closeState_.completeBeforePublication();
        throw;
    }
}

Task<void> RedisClientState::connect() {
    return connectOwned(shared_from_this());
}

Task<void> RedisClientState::connectOwned(std::shared_ptr<RedisClientState> state) {
    co_await state->connectOnWorker();
}

Task<void> RedisClientState::connectOnWorker() {
    if (!worker_.isCurrent()) {
        throw std::logic_error("redis client must connect on its bound event loop");
    }

    auto expected = Phase::kFresh;
    if (!phase_.compare_exchange_strong(expected, Phase::kConnecting, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        if (expected == Phase::kClosing || expected == Phase::kClosed) {
            throw std::runtime_error("redis client closed before connecting");
        }
        throw std::logic_error("redis client can only connect once");
    }
    try {
        connectInFlight_ = true;
        if (stopSource_.stopRequested() || !worker_.accepting()) {
            throw std::runtime_error("redis client closed before connecting");
        }
        co_await runtime_.connect();
        expected = Phase::kConnecting;
        if (!phase_.compare_exchange_strong(expected, Phase::kConnected, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            throw std::runtime_error("worker stopped while redis client was connecting");
        }
        connectInFlight_ = false;
        closeState_.notifyProgress();
    } catch (...) {
        runtime_.closeNow();
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

RedisHandle RedisClientState::handle(OperationOptions options) {
    requireConnectedOnWorker();
    options = mergeOperationOptions(
        OperationOptions{.timeout = std::nullopt, .stopToken = stopSource_.token()},
        std::move(options));
    return runtime_.handle(operationScope_).withOptions(std::move(options));
}

void RedisClientState::requireConnectedOnWorker() const {
    if (!worker_.isCurrent()) {
        throw std::logic_error("redis client must be used on its bound event loop");
    }
    if (phase_.load(std::memory_order_acquire) != Phase::kConnected) {
        throw std::logic_error("redis client is not connected");
    }
}

void RedisClientState::requestClose() noexcept {
    stopSource_.requestStop();
    auto phase = phase_.load(std::memory_order_acquire);
    for (;;) {
        if (phase == Phase::kClosed || phase == Phase::kClosing) {
            return;
        }
        if (phase_.compare_exchange_weak(
                phase, Phase::kClosing, std::memory_order_acq_rel, std::memory_order_acquire)) {
            break;
        }
    }

    // Completion is worker-affine even before connect(): a fresh client's
    // close request may race the loop's stop callback.
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

Task<void> RedisClientState::shutdown() {
    return shutdownOwned(shared_from_this());
}

Task<void> RedisClientState::shutdownOwned(std::shared_ptr<RedisClientState> state) {
    if (!state->worker_.isCurrent()) {
        throw std::logic_error("redis client shutdown must run on its bound event loop");
    }
    state->startCloseOnWorker();
    while (!state->closeState_.complete()) {
        co_await state->closeState_.wait();
    }
    state->closeState_.rethrowFailure();
}

void RedisClientState::startCloseOnWorker() noexcept {
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
    runtime_.closeNow();
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

Task<void> RedisClientState::closeOnWorker() {
    while (connectInFlight_) {
        co_await closeState_.wait();
    }
    co_await operationScope_.closeAndJoin();
}

void RedisClientState::finishClose(const TaskCompletionResult<void>& result) {
    phase_.store(Phase::kClosed, std::memory_order_release);
    closeState_.finish(result);
}

}  // namespace ruvia::detail

namespace ruvia {

RedisClient::RedisClient(EventLoop loop, const RedisConfig& config)
    : state_(std::make_shared<detail::RedisClientState>(std::move(loop), config)) {
    state_->bindStop();
}

RedisClient::~RedisClient() {
    state_->requestClose();
}

Task<void> RedisClient::connect() & {
    return state_->connect();
}

RedisHandle RedisClient::withOptions(OperationOptions options) const& {
    return state_->handle(std::move(options));
}

void RedisClient::close() noexcept {
    state_->requestClose();
}

Task<void> RedisClient::shutdown() & {
    return state_->shutdown();
}

const WorkerHandle& RedisClient::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia
