#include "ruvia/web/detail/DataAccessState.h"

#include <memory>
#include <utility>

#include "ruvia/core/detail/ConnectionScanner.h"
#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/web/detail/redis/RedisRegistry.h"

// The worker-local owner of the database and Redis registries: one per worker,
// connected and closed with that worker, and reachable from a Context without
// any HTTP or App dependency.

namespace ruvia::detail {

class DataAccessState::Impl final {
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

DataAccessState::DataAccessState(
    asio::io_context& ioContext,
    std::pmr::memory_resource* resource,
    std::span<const DbDefinition> databases,
    std::span<const RedisDefinition> redis,
    ConnectionScanner& scanner)
    : impl_(std::make_unique<Impl>(
          ioContext, resource, databases, redis, scanner)) {}

DataAccessState::~DataAccessState() = default;

Task<void> DataAccessState::connect() {
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

void DataAccessState::closeNow() noexcept {
    impl_->redis.closeNow();
    impl_->databases.closeNow();
}

bool DataAccessState::hasMaintenance() const noexcept {
    return impl_->databases.hasAnyTimeout() || impl_->redis.hasAnyTimeout();
}

DbRegistry& DataAccessState::databases() noexcept {
    return impl_->databases;
}

const DbRegistry& DataAccessState::databases() const noexcept {
    return impl_->databases;
}

RedisRegistry& DataAccessState::redis() noexcept {
    return impl_->redis;
}

const RedisRegistry& DataAccessState::redis() const noexcept {
    return impl_->redis;
}

}  // namespace ruvia::detail
