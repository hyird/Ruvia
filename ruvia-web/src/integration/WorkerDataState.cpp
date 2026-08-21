#include "ruvia/web/detail/integration/WorkerDataState.h"

#include <memory>

#include "ruvia/core/detail/io/ConnectionScanner.h"
#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/web/detail/redis/RedisRegistry.h"

namespace ruvia::detail {

class WorkerDataState::Impl final {
public:
    Impl(asio::io_context& ioContext, const WorkerHandle& worker, std::pmr::memory_resource* resource, std::span<const DbDefinition> databaseDefinitions, std::span<const RedisDefinition> redisDefinitions, ConnectionScanner& scanner)
        : databases(ioContext, resource, databaseDefinitions, &worker),
          redis(ioContext, resource, redisDefinitions, &worker) {
        if (databases.needsDeadlineScan()) {
            scanner.registerWorkerMaintenance(databaseDeadlineCheck, &databases, [](void* target) noexcept { static_cast<DbRegistry*>(target)->scanDeadlines(); });
        }
        if (redis.needsDeadlineScan()) {
            scanner.registerWorkerMaintenance(redisDeadlineCheck, &redis, [](void* target) noexcept { static_cast<RedisRegistry*>(target)->scanDeadlines(); });
        }
    }

    DbRegistry databases;
    RedisRegistry redis;
    ConnectionScanner::WorkerMaintenanceRegistration databaseDeadlineCheck;
    ConnectionScanner::WorkerMaintenanceRegistration redisDeadlineCheck;
};

WorkerDataState::WorkerDataState(asio::io_context& ioContext, const WorkerHandle& worker, std::pmr::memory_resource* resource, std::span<const DbDefinition> databases, std::span<const RedisDefinition> redis, ConnectionScanner& scanner)
    : impl_(std::make_unique<Impl>(ioContext, worker, resource, databases, redis, scanner)) {}

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
    return impl_->databases.needsDeadlineScan() || impl_->redis.needsDeadlineScan();
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

}  // namespace ruvia::detail
