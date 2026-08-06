#include "ruvia/web/detail/integration/DataAccessServiceState.h"

#include <optional>
#include <stdexcept>

#include <asio/bind_executor.hpp>

#include "ruvia/web/detail/integration/DataAccessDefinitions.h"

namespace ruvia::detail {

DataAccessServiceState::DataAccessServiceState(EventLoop loop, DataAccessOptions options)
    : loop_(requireEventLoop(std::move(loop))),
      worker_(loop_.handle()),
      memory_(),
      databaseDefinitions_(makeDatabaseDefinitions(options, memory_.resource())),
      redisDefinitions_(makeRedisDefinitions(options, memory_.resource())),
      scanner_(worker_, ConnectionScannerOptions{.scanInterval = options.maintenanceInterval, .idleTimeout = std::nullopt, .initialReadTimeout = std::nullopt, .payloadReadTimeout = std::nullopt, .writeTimeout = std::nullopt}),
      access_(loop_.ioContext(), worker_, memory_.resource(), databaseDefinitions_, redisDefinitions_, scanner_),
      failureHandler_(std::move(options.failureHandler)) {}

DataAccessServiceState::~DataAccessServiceState() {
    const auto phase = phase_.load(std::memory_order_acquire);
    if (phase == Phase::kConnectScheduled || phase == Phase::kConnecting || phase == Phase::kConnected || outstanding_.load(std::memory_order_acquire) != 0) {
        std::terminate();
    }
}

void DataAccessServiceState::bindStop() {
    std::weak_ptr<DataAccessServiceState> weak = shared_from_this();
    stopRegistration_ = loop_.onStop([weak = std::move(weak)] {
        if (const auto state = weak.lock()) {
            state->closeOnWorker();
        }
    });
}

std::future<void> DataAccessServiceState::scheduleConnect() {
    auto completion = std::make_shared<std::promise<void>>();
    auto future = completion->get_future();

    auto expected = Phase::kFresh;
    if (!phase_.compare_exchange_strong(expected, Phase::kConnectScheduled, std::memory_order_acq_rel, std::memory_order_acquire)) {
        completion->set_exception(std::make_exception_ptr(std::logic_error("data access service can only connect once")));
        return future;
    }
    if (!worker_.accepting()) {
        closeSubmissions();
        phase_.store(Phase::kClosed, std::memory_order_release);
        stopSource_.requestStop();
        completion->set_exception(std::make_exception_ptr(std::runtime_error("worker stopped before data integrations connected")));
        return future;
    }

    try {
        WorkerHandleAccess::defer(worker_, [state = shared_from_this(), completion] {
            try {
                asyncStartTask(state->connectOnWorker(), asio::bind_executor(state->loop_.executor(), [state, completion](TaskCompletionResult<void> result) mutable {
                    if (const auto* failure = result.failure()) {
                        completion->set_exception(failure->exception());
                    } else {
                        completion->set_value();
                    }
                }));
            } catch (...) {
                // Coroutine-frame or completion-state allocation can
                // fail after connect() has already returned its future.
                // Publish that exact terminal before the exception is
                // allowed to fail the event-loop runner; otherwise the
                // caller can only observe a broken promise and this
                // state remains stuck at kConnectScheduled.
                const auto failure = std::current_exception();
                state->closeOnWorker();
                completion->set_exception(failure);
                std::rethrow_exception(failure);
            }
        });
    } catch (...) {
        closeSubmissions();
        phase_.store(Phase::kClosed, std::memory_order_release);
        stopSource_.requestStop();
        completion->set_exception(std::current_exception());
    }
    return future;
}

Task<void> DataAccessServiceState::connectOnWorker() {
    if (!worker_.isCurrent()) {
        throw std::logic_error("data access service must connect on its bound event loop");
    }
    auto expected = Phase::kConnectScheduled;
    if (!phase_.compare_exchange_strong(expected, Phase::kConnecting, std::memory_order_acq_rel, std::memory_order_acquire)) {
        throw std::runtime_error("worker stopped before data integrations connected");
    }

    try {
        if (access_.hasMaintenance()) {
            scanner_.start();
        }
        co_await access_.connect();
        expected = Phase::kConnecting;
        if (!phase_.compare_exchange_strong(expected, Phase::kConnected, std::memory_order_acq_rel, std::memory_order_acquire)) {
            throw std::runtime_error("worker stopped while data integrations were connecting");
        }
    } catch (...) {
        scanner_.stop();
        access_.closeNow();
        stopSource_.requestStop();
        phase_.store(Phase::kClosed, std::memory_order_release);
        throw;
    }
}

DataAccessPostResult DataAccessServiceState::post(Job task) {
    std::lock_guard lock(submitMutex_);
    if (phase_.load(std::memory_order_acquire) != Phase::kConnected) {
        if (!accepting_) {
            postCounters_.recordWorkerStopping();
            return DataAccessPostResult::reject(PostStatus::kWorkerStopping, std::move(task));
        }
        throw std::logic_error("data access service must finish connecting before jobs are posted");
    }
    if (!accepting_) {
        postCounters_.recordWorkerStopping();
        return DataAccessPostResult::reject(PostStatus::kWorkerStopping, std::move(task));
    }

    const auto status = WorkerHandleAccess::postFactory(worker_, [this, &task]() mutable -> MoveOnlyFunction<void()> {
        outstanding_.fetch_add(1, std::memory_order_acq_rel);
        JobReservation reservation(shared_from_this());
        return [task = std::move(task), reservation = std::move(reservation)]() mutable {
            auto state = reservation.release();
            state->startJob(std::move(task));
        };
    });
    postCounters_.record(status);
    return status == PostStatus::kAccepted ? DataAccessPostResult::accept() : DataAccessPostResult::reject(status, std::move(task));
}

void DataAccessServiceState::requestClose() noexcept {
    stopSource_.requestStop();
    if (phase_.load(std::memory_order_acquire) == Phase::kClosed) {
        closeSubmissions();
        return;
    }
    if (worker_.isCurrent()) {
        closeOnWorker();
        return;
    }
    closeSubmissions();

    try {
        WorkerHandleAccess::defer(worker_, [state = shared_from_this()] { state->closeOnWorker(); });
    } catch (...) {
        // A detached worker must already have delivered its stop hook and
        // closed every connected integration on the worker thread.
        if (phase_.load(std::memory_order_acquire) != Phase::kClosed && phase_.load(std::memory_order_acquire) != Phase::kFresh) {
            std::terminate();
        }
    }
}

void DataAccessServiceState::closeOnWorker() noexcept {
    if (!worker_.isCurrent()) {
        std::terminate();
    }
    closeSubmissions();
    if (phase_.exchange(Phase::kClosed, std::memory_order_acq_rel) == Phase::kClosed) {
        return;
    }
    stopSource_.requestStop();
    scanner_.stop();
    access_.closeNow();
}

DataAccessStats DataAccessServiceState::stats() const noexcept {
    return DataAccessStats{
        .accepted = postCounters_.accepted(),
        .queueFull = postCounters_.queueFull(),
        .workerStopping = postCounters_.workerStopping(),
        .completed = completed_.load(std::memory_order_relaxed),
        .failed = failed_.load(std::memory_order_relaxed),
        .outstanding = outstanding_.load(std::memory_order_acquire),
    };
}

void DataAccessServiceState::requireConnectedOnWorker() const {
    if (!worker_.isCurrent()) {
        throw std::logic_error("data access context must be created and used on its bound event loop");
    }
    if (phase_.load(std::memory_order_acquire) != Phase::kConnected) {
        throw std::logic_error("data access service is not connected");
    }
}

void DataAccessServiceState::closeSubmissions() noexcept {
    std::lock_guard lock(submitMutex_);
    accepting_ = false;
}

void DataAccessServiceState::startJob(Job task) {
    auto state = shared_from_this();
    try {
        asyncStartTask(runJob(std::move(task), state), asio::bind_executor(loop_.executor(), [state](TaskCompletionResult<void> result) { state->completeJob(std::move(result)); }));
    } catch (...) {
        abandonJob();
        throw;
    }
}

Task<void> DataAccessServiceState::runJob(Job task, std::shared_ptr<DataAccessServiceState> state) {
    DataAccessContext context(std::move(state));
    co_await task(context);
}

void DataAccessServiceState::completeJob(TaskCompletionResult<void> result) {
    std::exception_ptr failure;
    if (const auto* failed = result.failure()) {
        failure = failed->exception();
        failed_.fetch_add(1, std::memory_order_relaxed);
    }
    completed_.fetch_add(1, std::memory_order_relaxed);
    outstanding_.fetch_sub(1, std::memory_order_acq_rel);
    if (failure != nullptr) {
        if (failureHandler_) {
            try {
                failureHandler_(std::move(failure));
            } catch (...) {
                // An exception is about to leave io_context::run(). Close
                // while worker affinity is still provable; a stop hook
                // deferred after run() unwinds cannot drain.
                closeOnWorker();
                throw;
            }
        } else {
            closeOnWorker();
            std::rethrow_exception(failure);
        }
    }
}

void DataAccessServiceState::abandonJob() noexcept {
    outstanding_.fetch_sub(1, std::memory_order_acq_rel);
}

}  // namespace ruvia::detail
