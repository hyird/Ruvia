#include "ruvia/web/db/DbClient.h"

#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#include <asio/bind_executor.hpp>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/detail/db/DbClientState.h"
#include "ruvia/web/detail/db/DbConfigStorage.h"

namespace ruvia::detail {

EventLoop DbClientState::requireLoop(EventLoop loop) {
    if (!loop.valid()) {
        throw std::invalid_argument("database client requires a valid event loop");
    }
    return loop;
}

std::pmr::vector<DbDefinition> DbClientState::makeDefinitions(const DbConfig& config, std::pmr::memory_resource* resource) {
    std::pmr::vector<DbDefinition> definitions(resource);
    definitions.push_back(DbDefinition{
        std::pmr::string("default", resource),
        DbConfigStorage(config, resource),
    });
    return definitions;
}

ConnectionScannerOptions DbClientState::scannerOptions() {
    return ConnectionScannerOptions{
        .scanInterval = std::chrono::seconds(1),
        .idleTimeout = std::nullopt,
        .initialReadTimeout = std::nullopt,
        .payloadReadTimeout = std::nullopt,
        .writeTimeout = std::nullopt,
    };
}

DbClientState::DbClientState(EventLoop loop, DbConfig config)
    : loop_(requireLoop(std::move(loop))),
      worker_(loop_.handle()),
      memory_(),
      definitions_(makeDefinitions(config, memory_.resource())),
      scanner_(worker_, scannerOptions()),
      databases_(loop_.ioContext(), memory_.resource(), definitions_, &worker_) {}

DbClientState::~DbClientState() {
    const auto phase = phase_.load(std::memory_order_acquire);
    if ((phase != Phase::kFresh && phase != Phase::kClosed) || operationScope_.hasPendingOperations()) {
        std::terminate();
    }
}

void DbClientState::bindStop() {
    std::weak_ptr<DbClientState> weak = shared_from_this();
    stopRegistration_ = loop_.onStop([weak = std::move(weak)] {
        if (const auto state = weak.lock()) {
            state->closeOnWorker();
        }
    });
}

std::future<void> DbClientState::scheduleConnect() {
    auto completion = std::make_shared<std::promise<void>>();
    auto future = completion->get_future();
    auto expected = Phase::kFresh;
    if (!phase_.compare_exchange_strong(expected, Phase::kConnectScheduled, std::memory_order_acq_rel, std::memory_order_acquire)) {
        completion->set_exception(std::make_exception_ptr(std::logic_error("database client can only connect once")));
        return future;
    }
    if (!worker_.accepting()) {
        phase_.store(Phase::kClosed, std::memory_order_release);
        stopSource_.requestStop();
        completion->set_exception(std::make_exception_ptr(std::runtime_error("worker stopped before database client connected")));
        return future;
    }

    try {
        WorkerHandleAccess::defer(worker_, [state = shared_from_this(), completion] {
            try {
                asyncStartTask(state->connectOnWorker(), asio::bind_executor(state->loop_.executor(), [state, completion](TaskCompletionResult<void> result) mutable {
                    if (const auto* failed = result.failure()) {
                        completion->set_exception(failed->exception());
                    } else {
                        completion->set_value();
                    }
                }));
            } catch (...) {
                const auto failure = std::current_exception();
                state->closeOnWorker();
                completion->set_exception(failure);
                std::rethrow_exception(failure);
            }
        });
    } catch (...) {
        phase_.store(Phase::kClosed, std::memory_order_release);
        stopSource_.requestStop();
        completion->set_exception(std::current_exception());
    }
    return future;
}

Task<void> DbClientState::connectOnWorker() {
    if (!worker_.isCurrent()) {
        throw std::logic_error("database client must connect on its bound event loop");
    }

    try {
        if (stopSource_.stopRequested()) {
            throw std::runtime_error("database client closed before connecting");
        }
        auto expected = Phase::kConnectScheduled;
        if (!phase_.compare_exchange_strong(expected, Phase::kConnecting, std::memory_order_acq_rel, std::memory_order_acquire)) {
            throw std::runtime_error("worker stopped before database client connected");
        }
        if (databases_.needsDeadlineScan()) {
            scanner_.start();
        }
        co_await databases_.connect();
        expected = Phase::kConnecting;
        if (!phase_.compare_exchange_strong(expected, Phase::kConnected, std::memory_order_acq_rel, std::memory_order_acquire)) {
            throw std::runtime_error("worker stopped while database client was connecting");
        }
    } catch (...) {
        scanner_.stop();
        databases_.closeNow();
        stopSource_.requestStop();
        phase_.store(Phase::kClosed, std::memory_order_release);
        throw;
    }
}

DbHandle DbClientState::handle(OperationOptions options) {
    requireConnectedOnWorker();
    options = mergeOperationOptions(OperationOptions{.timeout = std::nullopt, .stopToken = stopSource_.token()}, std::move(options));
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
            if (phase_.compare_exchange_weak(phase, Phase::kClosed, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return;
            }
            continue;
        }
        if (phase_.compare_exchange_weak(phase, Phase::kClosing, std::memory_order_acq_rel, std::memory_order_acquire)) {
            break;
        }
    }

    if (worker_.isCurrent()) {
        closeOnWorker();
        return;
    }
    try {
        if (!WorkerHandleAccess::deferIfAttached(worker_, [state = shared_from_this()] { state->closeOnWorker(); })) {
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

void DbClientState::closeOnWorker() noexcept {
    if (!worker_.isCurrent()) {
        std::terminate();
    }
    if (phase_.exchange(Phase::kClosed, std::memory_order_acq_rel) == Phase::kClosed) {
        return;
    }
    stopSource_.requestStop();
    scanner_.stop();
    databases_.closeNow();
}

}  // namespace ruvia::detail

namespace ruvia {

DbClient::DbClient(EventLoop loop, DbConfig config)
    : state_(std::make_shared<detail::DbClientState>(std::move(loop), std::move(config))) {
    state_->bindStop();
}

DbClient::~DbClient() {
    state_->requestClose();
}

std::future<void> DbClient::connect() {
    return state_->scheduleConnect();
}

DbHandle DbClient::withOptions(OperationOptions options) const {
    return state_->handle(std::move(options));
}

ScopedOperation<DbRows> DbClient::query(std::string_view sql, std::span<const DbValue> params) const {
    return withOptions({}).query(sql, params);
}

ScopedOperation<DbExecResult> DbClient::execute(std::string_view sql, std::span<const DbValue> params) const {
    return withOptions({}).execute(sql, params);
}

ScopedOperation<DbStreamResult> DbClient::queryStream(std::string_view sql, std::span<const DbValue> params) const {
    return withOptions({}).queryStream(sql, params);
}

ScopedOperation<DbTransaction> DbClient::beginTransaction() const {
    return withOptions({}).beginTransaction();
}

void DbClient::close() noexcept {
    state_->requestClose();
}

const WorkerHandle& DbClient::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia
