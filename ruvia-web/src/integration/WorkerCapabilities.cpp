#include "integration/WorkerCapabilities.h"

#include <memory>
#include <stdexcept>
#include <utility>

#include "context/ContextServices.h"

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
      redis_(ioContext, resource, definitions.redis, worker_),
      databases_(ioContext, worker_, resource, definitions.databases, &redis_),
      httpClientResultBudgetDomain_(
          std::make_shared<HttpClientResultBudgetDomain>(options.http_client_result_budget)),
      httpClients_(ioContext, worker_, resource, definitions.httpClients,
          httpClientResultBudgetDomain_),
      workerStates_(resource, definitions.workerStates),
      rateLimiter_(
          options.defaultRateLimit, options.routeRateLimits, options.rateLimitCapacity, resource),
      options_(std::move(options)) {}

Task<void> WorkerCapabilities::connect() {
    try {
        if (!redis_.empty()) {
            co_await redis_.connect();
        }
        if (!databases_.empty()) {
            co_await databases_.connect();
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
    databases_.closeNow();
    redis_.closeNow();
}

void WorkerCapabilities::initializeWorkerState() {
    workerStates_.initialize();
}

void WorkerCapabilities::shutdownWorkerState() noexcept {
    workerStates_.shutdown();
}

ContextServices WorkerCapabilities::contextServices(const StopToken& stopToken) {
    return ContextServices(context_worker_services{
                               .worker = worker_,
                               .clients = clientRegistries(),
                               .rate_limiter = &rateLimiter_,
                               .env = options_.env,
                               .max_decoded_body_bytes = options_.maxDecodedBodyBytes,
                               .states = &workerStates_,
                               .blocking_pool = options_.blockingPool,
                               .precompressed_static_files = options_.precompressedStaticFiles,
                               .trusted_proxies = options_.trustedProxies,
                           },
        stopToken);
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
