#include "ruvia/web/detail/integration/WorkerCapabilities.h"

#include <utility>

#include "ruvia/web/detail/http/context/ContextServices.h"

namespace ruvia::detail {
namespace {

template <typename Registry>
void registerDeadlineScan(ConnectionScanner& scanner,
    ConnectionScanner::WorkerMaintenanceRegistration& registration, Registry& registry) noexcept {
    if (!registry.needsDeadlineScan()) {
        return;
    }
    scanner.registerWorkerMaintenance(registration, &registry,
        [](void* target) noexcept { static_cast<Registry*>(target)->scanDeadlines(); });
}

}  // namespace

WorkerCapabilities::WorkerCapabilities(asio::io_context& ioContext, const WorkerHandle& worker,
    std::pmr::memory_resource* resource, WorkerCapabilityDefinitions definitions,
    WorkerCapabilityOptions options, ConnectionScanner& scanner)
    : databases_(ioContext, resource, definitions.databases, &worker),
      redis_(ioContext, resource, definitions.redis, &worker),
      httpClients_(ioContext, worker, resource, definitions.httpClients),
      workerStates_(resource, definitions.workerStates),
      rateLimiter_(
          options.defaultRateLimit, options.routeRateLimits, options.rateLimitCapacity, resource),
      options_(std::move(options)) {
    registerDeadlineScan(scanner, databaseDeadlineMaintenance_, databases_);
    registerDeadlineScan(scanner, redisDeadlineMaintenance_, redis_);
}

Task<void> WorkerCapabilities::connect() {
    try {
        if (!databases_.empty()) {
            co_await databases_.connect();
        }
        if (!redis_.empty()) {
            co_await redis_.connect();
        }
    } catch (...) {
        closeNow();
        throw;
    }
}

Task<void> WorkerCapabilities::join() {
    co_await httpClients_.join();
}

void WorkerCapabilities::closeNow() noexcept {
    httpClients_.closeNow();
    redis_.closeNow();
    databases_.closeNow();
}

void WorkerCapabilities::initializeWorkerState() {
    workerStates_.initialize();
}

void WorkerCapabilities::shutdownWorkerState() noexcept {
    workerStates_.shutdown();
}

ContextServices WorkerCapabilities::contextServices() noexcept {
    ContextServices services(
        &databases_, &redis_, &rateLimiter_, options_.maxDecodedBodyBytes, nullptr, &httpClients_);
    services = services.withWorkerStates(workerStates_)
                   .withBlockingPool(options_.blockingPool)
                   .withPrecompressedStaticFiles(options_.precompressedStaticFiles)
                   .withTrustedProxies(options_.trustedProxies);
    if (options_.env != nullptr) {
        services = services.withEnv(*options_.env);
    }
    return services;
}

DbRegistry& WorkerCapabilities::databases() noexcept {
    return databases_;
}

RedisRegistry& WorkerCapabilities::redis() noexcept {
    return redis_;
}

HttpClientRegistry& WorkerCapabilities::httpClients() noexcept {
    return httpClients_;
}

const WorkerStateRegistry& WorkerCapabilities::workerStates() const noexcept {
    return workerStates_;
}

RateLimiter& WorkerCapabilities::rateLimiter() noexcept {
    return rateLimiter_;
}

BlockingPool* WorkerCapabilities::blockingPool() const noexcept {
    return options_.blockingPool;
}

}  // namespace ruvia::detail
