#include "ruvia/web/detail/integration/WorkerCapabilities.h"

#include <utility>

#include "ruvia/web/detail/http/context/ContextServices.h"

namespace ruvia::detail {

WorkerCapabilities::WorkerCapabilities(
    asio::io_context& ioContext,
    const WorkerHandle& worker,
    std::pmr::memory_resource* resource,
    WorkerCapabilityDefinitions definitions,
    WorkerCapabilityOptions options,
    ConnectionScanner& scanner)
    : data_(ioContext, worker, resource, definitions.databases, definitions.redis, scanner),
      httpClients_(ioContext, worker, resource, definitions.httpClients, options.maxHttpClientOrigins),
      workerStates_(resource, definitions.workerStates),
      rateLimiter_(options.defaultRateLimit, options.routeRateLimits, options.rateLimitCapacity, resource),
      options_(std::move(options)) {}

Task<void> WorkerCapabilities::connect() {
    co_await data_.connect();
}

Task<void> WorkerCapabilities::join() {
    co_await httpClients_.join();
}

void WorkerCapabilities::closeNow() noexcept {
    data_.closeNow();
    httpClients_.closeNow();
}

void WorkerCapabilities::initializeWorkerState() {
    workerStates_.initialize();
}

void WorkerCapabilities::shutdownWorkerState() noexcept {
    workerStates_.shutdown();
}

ContextServices WorkerCapabilities::contextServices() noexcept {
    ContextServices services(
        &data_.databases(),
        &data_.redis(),
        &rateLimiter_,
        options_.maxDecodedBodyBytes,
        nullptr,
        &httpClients_);
    services = services
        .withWorkerStates(workerStates_)
        .withBlockingPool(options_.blockingPool)
        .withPrecompressedStaticFiles(options_.precompressedStaticFiles)
        .withTrustedProxies(options_.trustedProxies);
    if (options_.env != nullptr) {
        services = services.withEnv(*options_.env);
    }
    return services;
}

DbRegistry& WorkerCapabilities::databases() noexcept {
    return data_.databases();
}

RedisRegistry& WorkerCapabilities::redis() noexcept {
    return data_.redis();
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
