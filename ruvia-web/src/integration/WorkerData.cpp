#include "ruvia/web/WorkerData.h"

#include <atomic>
#include <exception>
#include <future>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <asio/bind_executor.hpp>

#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/detail/ConnectionScanner.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/detail/WorkerDataState.h"
#include "ruvia/web/detail/db/DbInternal.h"
#include "ruvia/web/detail/redis/RedisInternal.h"

namespace ruvia::detail {

class WorkerDataState::Impl final {
public:
    Impl(
        asio::io_context& ioContext,
        std::pmr::memory_resource* resource,
        std::span<const DbDefinition> databaseDefinitions,
        std::span<const RedisDefinition> redisDefinitions,
        ConnectionScanner& scanner)
        : databases(ioContext, resource, databaseDefinitions),
          redis(ioContext, resource, redisDefinitions) {
        if (databases.hasAnyTimeout()) {
            scanner.registerWorkerMaintenance(
                databaseDeadlineCheck,
                &databases,
                [](void* target) noexcept {
                    static_cast<DbRegistry*>(target)->scanDeadlines();
                });
        }
        if (redis.hasAnyTimeout()) {
            scanner.registerWorkerMaintenance(
                redisDeadlineCheck,
                &redis,
                [](void* target) noexcept {
                    static_cast<RedisRegistry*>(target)->scanDeadlines();
                });
        }
    }

    DbRegistry databases;
    RedisRegistry redis;
    ConnectionScanner::WorkerMaintenanceRegistration databaseDeadlineCheck;
    ConnectionScanner::WorkerMaintenanceRegistration redisDeadlineCheck;
};

WorkerDataState::WorkerDataState(
    asio::io_context& ioContext,
    std::pmr::memory_resource* resource,
    std::span<const DbDefinition> databases,
    std::span<const RedisDefinition> redis,
    ConnectionScanner& scanner)
    : impl_(std::make_unique<Impl>(
          ioContext, resource, databases, redis, scanner)) {}

WorkerDataState::~WorkerDataState() = default;

Task<void> WorkerDataState::connect() {
    try {
        if (!impl_->databases.empty()) {
            co_await impl_->databases.connect();
        }
        if (!impl_->redis.empty()) {
            co_await impl_->redis.connect();
        }
    } catch (...) {
        closeNow();
        throw;
    }
}

void WorkerDataState::closeNow() noexcept {
    impl_->redis.closeNow();
    impl_->databases.closeNow();
}

bool WorkerDataState::hasMaintenance() const noexcept {
    return impl_->databases.hasAnyTimeout() || impl_->redis.hasAnyTimeout();
}

DbRegistry& WorkerDataState::databases() noexcept {
    return impl_->databases;
}

const DbRegistry& WorkerDataState::databases() const noexcept {
    return impl_->databases;
}

RedisRegistry& WorkerDataState::redis() noexcept {
    return impl_->redis;
}

const RedisRegistry& WorkerDataState::redis() const noexcept {
    return impl_->redis;
}

namespace {

[[nodiscard]] EventLoop requireEventLoop(EventLoop loop) {
    if (!loop.valid()) {
        throw std::invalid_argument(
            "worker data runtime requires a valid event loop");
    }
    return loop;
}

#ifdef RUVIA_ENABLE_DATABASE
[[nodiscard]] DbConfig cloneDbConfig(
    const DbConfig& source,
    std::pmr::memory_resource* resource) {
    return DbConfig{
        .driver = source.driver,
        .host = std::pmr::string(source.host, resource),
        .port = source.port,
        .username = std::pmr::string(source.username, resource),
        .password = std::pmr::string(source.password, resource),
        .database = std::pmr::string(source.database, resource),
        .connectTimeout = source.connectTimeout,
        .readTimeout = source.readTimeout,
        .writeTimeout = source.writeTimeout,
        .queryTimeout = source.queryTimeout,
        .acquireTimeout = source.acquireTimeout,
    };
}
#endif

[[nodiscard]] std::pmr::vector<DbDefinition> makeDatabaseDefinitions(
    const WorkerDataOptions& options,
    std::pmr::memory_resource* resource) {
    std::pmr::vector<DbDefinition> definitions(resource);
#ifdef RUVIA_ENABLE_DATABASE
    definitions.reserve(options.databases.size());
    for (const auto& database : options.databases) {
        definitions.push_back(DbDefinition{
            std::pmr::string(database.alias, resource),
            cloneDbConfig(database.config, resource)});
    }
#else
    (void)options;
#endif
    return definitions;
}

#ifdef RUVIA_ENABLE_REDIS
[[nodiscard]] RedisConfig cloneRedisConfig(
    const RedisConfig& source,
    std::pmr::memory_resource* resource) {
    return RedisConfig{
        .host = std::pmr::string(source.host, resource),
        .port = source.port,
        .username = std::pmr::string(source.username, resource),
        .password = std::pmr::string(source.password, resource),
        .database = source.database,
        .poolSizePerWorker = source.poolSizePerWorker,
        .connectTimeout = source.connectTimeout,
        .commandTimeout = source.commandTimeout,
        .acquireTimeout = source.acquireTimeout,
        .maxReplyBytes = source.maxReplyBytes,
        .maxArrayDepth = source.maxArrayDepth,
        .tcpNoDelay = source.tcpNoDelay,
        .keepAlive = source.keepAlive,
    };
}
#endif

[[nodiscard]] std::pmr::vector<RedisDefinition> makeRedisDefinitions(
    const WorkerDataOptions& options,
    std::pmr::memory_resource* resource) {
    std::pmr::vector<RedisDefinition> definitions(resource);
#ifdef RUVIA_ENABLE_REDIS
    definitions.reserve(options.redis.size());
    for (const auto& redis : options.redis) {
        definitions.push_back(RedisDefinition{
            std::pmr::string(redis.alias, resource),
            cloneRedisConfig(redis.config, resource)});
    }
#else
    (void)options;
#endif
    return definitions;
}

}  // namespace

class WorkerDataRuntimeState final
    : public std::enable_shared_from_this<WorkerDataRuntimeState> {
public:
    using Job = MoveOnlyFunction<Task<void>(WorkerDataContext&)>;

    WorkerDataRuntimeState(EventLoop loop, WorkerDataOptions options)
        : loop_(requireEventLoop(std::move(loop))),
          worker_(loop_.handle()),
          memory_(),
          databaseDefinitions_(
              makeDatabaseDefinitions(options, memory_.resource())),
          redisDefinitions_(
              makeRedisDefinitions(options, memory_.resource())),
          scanner_(
              worker_,
              ConnectionScannerOptions{
                  .scanInterval = options.maintenanceInterval,
                  .idleTimeout = std::nullopt,
                  .initialReadTimeout = std::nullopt,
                  .payloadReadTimeout = std::nullopt,
                  .writeTimeout = std::nullopt}),
          data_(
              loop_.ioContext(),
              memory_.resource(),
              databaseDefinitions_,
              redisDefinitions_,
              scanner_),
          failureHandler_(std::move(options.failureHandler)) {}

    ~WorkerDataRuntimeState() {
        const auto phase = phase_.load(std::memory_order_acquire);
        if (phase == Phase::kConnectScheduled ||
            phase == Phase::kConnecting ||
            phase == Phase::kConnected ||
            outstanding_.load(std::memory_order_acquire) != 0) {
            std::terminate();
        }
    }

    void bindStop() {
        std::weak_ptr<WorkerDataRuntimeState> weak = shared_from_this();
        stopRegistration_ = loop_.onStop([weak = std::move(weak)] {
            if (const auto state = weak.lock()) {
                state->closeOnWorker();
            }
        });
    }

    [[nodiscard]] std::future<void> scheduleConnect() {
        auto completion = std::make_shared<std::promise<void>>();
        auto future = completion->get_future();

        auto expected = Phase::kFresh;
        if (!phase_.compare_exchange_strong(
                expected,
                Phase::kConnectScheduled,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            completion->set_exception(std::make_exception_ptr(
                std::logic_error(
                    "worker data runtime can only connect once")));
            return future;
        }
        if (!worker_.accepting()) {
            closeSubmissions();
            phase_.store(Phase::kClosed, std::memory_order_release);
            stopSource_.requestStop();
            completion->set_exception(std::make_exception_ptr(
                std::runtime_error(
                    "worker stopped before data integrations connected")));
            return future;
        }

        try {
            WorkerHandleAccess::defer(
                worker_,
                [state = shared_from_this(),
                 completion] {
                    try {
                        asyncStartTask(
                            state->connectOnWorker(),
                            asio::bind_executor(
                                state->loop_.executor(),
                                [state,
                                 completion](
                                    TaskCompletionResult<void> result) mutable {
                                    if (const auto* failure = result.failure()) {
                                        completion->set_exception(
                                            failure->exception());
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

    [[nodiscard]] Task<void> connectOnWorker() {
        if (!worker_.isCurrent()) {
            throw std::logic_error(
                "worker data runtime must connect on its bound event loop");
        }
        auto expected = Phase::kConnectScheduled;
        if (!phase_.compare_exchange_strong(
                expected,
                Phase::kConnecting,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            throw std::runtime_error(
                "worker stopped before data integrations connected");
        }

        try {
            if (data_.hasMaintenance()) {
                scanner_.start();
            }
            co_await data_.connect();
            expected = Phase::kConnecting;
            if (!phase_.compare_exchange_strong(
                    expected,
                    Phase::kConnected,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                throw std::runtime_error(
                    "worker stopped while data integrations were connecting");
            }
        } catch (...) {
            scanner_.stop();
            data_.closeNow();
            stopSource_.requestStop();
            phase_.store(Phase::kClosed, std::memory_order_release);
            throw;
        }
    }

    [[nodiscard]] WorkerDataPostResult post(Job task) {
        std::lock_guard lock(submitMutex_);
        if (phase_.load(std::memory_order_acquire) != Phase::kConnected) {
            if (!accepting_) {
                workerStopping_.fetch_add(1, std::memory_order_relaxed);
                return WorkerDataPostResult::reject(
                    PostStatus::kWorkerStopping, std::move(task));
            }
            throw std::logic_error(
                "worker data runtime must finish connecting before jobs are posted");
        }
        if (!accepting_) {
            workerStopping_.fetch_add(1, std::memory_order_relaxed);
            return WorkerDataPostResult::reject(
                PostStatus::kWorkerStopping, std::move(task));
        }

        const auto status = WorkerHandleAccess::postFactory(
            worker_, [this, &task]() mutable -> MoveOnlyFunction<void()> {
                outstanding_.fetch_add(1, std::memory_order_acq_rel);
                JobReservation reservation(shared_from_this());
                return [task = std::move(task),
                        reservation = std::move(reservation)]() mutable {
                    auto state = reservation.release();
                    state->startJob(std::move(task));
                };
            });
        switch (status) {
        case PostStatus::kAccepted:
            accepted_.fetch_add(1, std::memory_order_relaxed);
            break;
        case PostStatus::kQueueFull:
            queueFull_.fetch_add(1, std::memory_order_relaxed);
            break;
        case PostStatus::kWorkerStopping:
            workerStopping_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        return status == PostStatus::kAccepted
            ? WorkerDataPostResult::accept()
            : WorkerDataPostResult::reject(status, std::move(task));
    }

    void requestClose() noexcept {
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
            WorkerHandleAccess::defer(
                worker_,
                [state = shared_from_this()] { state->closeOnWorker(); });
        } catch (...) {
            // A detached worker must already have delivered its stop hook and
            // closed every connected integration on the worker thread.
            if (phase_.load(std::memory_order_acquire) != Phase::kClosed &&
                phase_.load(std::memory_order_acquire) != Phase::kFresh) {
                std::terminate();
            }
        }
    }

    void closeOnWorker() noexcept {
        if (!worker_.isCurrent()) {
            std::terminate();
        }
        closeSubmissions();
        if (phase_.exchange(
                Phase::kClosed,
                std::memory_order_acq_rel) == Phase::kClosed) {
            return;
        }
        stopSource_.requestStop();
        scanner_.stop();
        data_.closeNow();
    }

    [[nodiscard]] WorkerDataStats stats() const noexcept {
        return WorkerDataStats{
            .accepted = accepted_.load(std::memory_order_relaxed),
            .queueFull = queueFull_.load(std::memory_order_relaxed),
            .workerStopping = workerStopping_.load(std::memory_order_relaxed),
            .completed = completed_.load(std::memory_order_relaxed),
            .failed = failed_.load(std::memory_order_relaxed),
            .outstanding = outstanding_.load(std::memory_order_acquire),
        };
    }

    void requireConnectedOnWorker() const {
        if (!worker_.isCurrent()) {
            throw std::logic_error(
                "worker data context must be created and used on its bound event loop");
        }
        if (phase_.load(std::memory_order_acquire) != Phase::kConnected) {
            throw std::logic_error("worker data runtime is not connected");
        }
    }

    [[nodiscard]] const WorkerHandle& worker() const noexcept {
        return worker_;
    }

    [[nodiscard]] std::pmr::memory_resource* resource() noexcept {
        return memory_.resource();
    }

    [[nodiscard]] StopToken stopToken() const noexcept {
        return stopSource_.token();
    }

    [[nodiscard]] WorkerDataState& data() noexcept { return data_; }

private:
    class JobReservation final {
    public:
        explicit JobReservation(
            std::shared_ptr<WorkerDataRuntimeState> state) noexcept
            : state_(std::move(state)) {}

        ~JobReservation() {
            if (state_ != nullptr) {
                state_->abandonJob();
            }
        }

        JobReservation(const JobReservation&) = delete;
        JobReservation& operator=(const JobReservation&) = delete;
        JobReservation(JobReservation&& other) noexcept
            : state_(std::move(other.state_)) {}
        JobReservation& operator=(JobReservation&&) = delete;

        [[nodiscard]] std::shared_ptr<WorkerDataRuntimeState> release() noexcept {
            return std::exchange(state_, nullptr);
        }

    private:
        std::shared_ptr<WorkerDataRuntimeState> state_;
    };

    void closeSubmissions() noexcept {
        std::lock_guard lock(submitMutex_);
        accepting_ = false;
    }

    void startJob(Job task) {
        auto state = shared_from_this();
        try {
            asyncStartTask(
                runJob(std::move(task), state),
                asio::bind_executor(
                    loop_.executor(),
                    [state](
                        TaskCompletionResult<void> result) {
                        state->completeJob(std::move(result));
                    }));
        } catch (...) {
            abandonJob();
            throw;
        }
    }

    [[nodiscard]] static Task<void> runJob(
        Job task,
        std::shared_ptr<WorkerDataRuntimeState> state) {
        WorkerDataContext context(std::move(state));
        co_await task(context);
    }

    void completeJob(TaskCompletionResult<void> result) {
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

    void abandonJob() noexcept {
        outstanding_.fetch_sub(1, std::memory_order_acq_rel);
    }

    enum class Phase : unsigned char {
        kFresh,
        kConnectScheduled,
        kConnecting,
        kConnected,
        kClosed,
    };

    EventLoop loop_;
    WorkerHandle worker_;
    WorkerMemory memory_;
    std::pmr::vector<DbDefinition> databaseDefinitions_;
    std::pmr::vector<RedisDefinition> redisDefinitions_;
    ConnectionScanner scanner_;
    WorkerDataState data_;
    StopSource stopSource_;
    // Published once by bindStop() and retained until state destruction. The
    // listener only weakly references this state, so retaining it forms no
    // cycle; avoiding callback-side reset also closes the register-vs-stop
    // publication race during WorkerDataRuntime construction.
    EventLoopStopRegistration stopRegistration_;
    MoveOnlyFunction<void(std::exception_ptr)> failureHandler_;
    mutable std::mutex submitMutex_;
    bool accepting_{true};
    std::atomic<Phase> phase_{Phase::kFresh};
    std::atomic_size_t outstanding_{0};
    std::atomic_uint64_t accepted_{0};
    std::atomic_uint64_t queueFull_{0};
    std::atomic_uint64_t workerStopping_{0};
    std::atomic_uint64_t completed_{0};
    std::atomic_uint64_t failed_{0};
};

}  // namespace ruvia::detail

namespace ruvia {

WorkerDataContext::WorkerDataContext(
    std::shared_ptr<detail::WorkerDataRuntimeState> state) noexcept
    : state_(std::move(state)) {}

const WorkerHandle& WorkerDataContext::worker() const & noexcept {
    return state_->worker();
}

std::pmr::memory_resource* WorkerDataContext::resource() const noexcept {
    return state_->resource();
}

StopToken WorkerDataContext::stopToken() const noexcept {
    return state_->stopToken();
}

#ifdef RUVIA_ENABLE_DATABASE
DbHandle WorkerDataContext::db() const {
    state_->requireConnectedOnWorker();
    return state_->data().databases().get(resource(), operationScope_);
}

DbHandle WorkerDataContext::db(std::string_view alias) const {
    state_->requireConnectedOnWorker();
    return state_->data().databases().get(
        alias, resource(), operationScope_);
}
#endif

#ifdef RUVIA_ENABLE_REDIS
RedisHandle WorkerDataContext::redis() const {
    state_->requireConnectedOnWorker();
    return state_->data().redis().get(resource(), operationScope_);
}

RedisHandle WorkerDataContext::redis(std::string_view alias) const {
    state_->requireConnectedOnWorker();
    return state_->data().redis().get(
        alias, resource(), operationScope_);
}
#endif

WorkerDataRuntime::WorkerDataRuntime(
    EventLoop loop,
    WorkerDataOptions options)
    : state_(std::make_shared<detail::WorkerDataRuntimeState>(
          std::move(loop), std::move(options))) {
    state_->bindStop();
}

WorkerDataRuntime::~WorkerDataRuntime() {
    state_->requestClose();
}

std::future<void> WorkerDataRuntime::connect() {
    return state_->scheduleConnect();
}

WorkerDataPostResult WorkerDataRuntime::postTask(
    MoveOnlyFunction<Task<void>(WorkerDataContext&)> task) {
    return state_->post(std::move(task));
}

void WorkerDataRuntime::close() {
    if (!state_->worker().isCurrent()) {
        throw std::logic_error(
            "worker data runtime must close on its bound event loop");
    }
    state_->closeOnWorker();
}

WorkerDataStats WorkerDataRuntime::stats() const noexcept {
    return state_->stats();
}

const WorkerHandle& WorkerDataRuntime::worker() const & noexcept {
    return state_->worker();
}

}  // namespace ruvia
