#include "ruvia/web/detail/integration/WorkerCapabilities.h"

#include <stdexcept>
#include <utility>

#include "ruvia/web/detail/http/context/ContextServices.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] const WorkerHandle& requireWorkerCapabilitiesWorker(const WorkerHandle& worker) {
    if (!worker.valid()) {
        throw std::invalid_argument("worker capabilities require a valid worker");
    }
    return worker;
}

}  // namespace

WorkerCapabilities::WorkerCapabilities(asio::io_context& ioContext, const WorkerHandle& worker,
    std::pmr::memory_resource* resource, WorkerCapabilityDefinitions definitions,
    WorkerCapabilityOptions options)
    : worker_(requireWorkerCapabilitiesWorker(worker)),
      databases_(ioContext, worker_, resource, definitions.databases),
      redis_(ioContext, resource, definitions.redis, worker_),
      httpClients_(ioContext, worker_, resource, definitions.httpClients),
      workerStates_(resource, definitions.workerStates),
      rateLimiter_(
          options.defaultRateLimit, options.routeRateLimits, options.rateLimitCapacity, resource),
      options_(std::move(options)) {}

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

ContextServices WorkerCapabilities::contextServices(const StopToken& stopToken) {
    ContextServices services(
        worker_, stopToken, clientRegistries(), &rateLimiter_, options_.maxDecodedBodyBytes);
    services = services.withWorkerStates(workerStates_)
                   .withPrecompressedStaticFiles(options_.precompressedStaticFiles);
    if (options_.blockingPool != nullptr) {
        services = services.withBlockingPool(*options_.blockingPool);
    }
    if (options_.trustedProxies != nullptr) {
        services = services.withTrustedProxies(*options_.trustedProxies);
    }
    if (options_.env != nullptr) {
        services = services.withEnv(*options_.env);
    }
    return services;
}

WorkerClientRegistryView WorkerCapabilities::clientRegistries() noexcept {
    return WorkerClientRegistryView(databases_, redis_, httpClients_);
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
