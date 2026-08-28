#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>

#include "ruvia/core/Task.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/detail/io/ConnectionScanner.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/web/detail/client/HttpClientRegistry.h"
#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/web/detail/integration/WorkerState.h"
#include "ruvia/web/detail/ratelimit/RateLimiter.h"
#include "ruvia/web/detail/redis/RedisRegistry.h"

namespace asio {
class io_context;
}

namespace ruvia {
class BlockingPool;
class Env;
}  // namespace ruvia

namespace ruvia::detail {

class ContextServices;
class TrustedProxySet;

struct WorkerCapabilityDefinitions final {
    std::span<const DbDefinition> databases{};
    std::span<const RedisDefinition> redis{};
    std::span<const WorkerStateDefinition> workerStates{};
    std::span<const HttpClientDefinition> httpClients{};
};

struct WorkerCapabilityOptions final {
    std::optional<RateLimitRule> defaultRateLimit{};
    RouteRateLimitPresence routeRateLimits{RouteRateLimitPresence::kAbsent};
    std::size_t rateLimitCapacity{kDefaultRateLimitCapacityPerWorker};
    std::size_t maxDecodedBodyBytes{kDefaultMaxBufferedBodyBytes};
    BlockingPool* blockingPool{nullptr};
    const Env* env{nullptr};
    const TrustedProxySet* trustedProxies{nullptr};
    bool precompressedStaticFiles{false};
};

// Owns every application capability attached to one worker. Construction,
// startup, request exposure, and shutdown all follow this single ownership
// boundary; none of these registries is process-global or shared by workers.
class WorkerCapabilities final {
public:
    WorkerCapabilities(asio::io_context& ioContext, const WorkerHandle& worker,
        std::pmr::memory_resource* resource, WorkerCapabilityDefinitions definitions,
        WorkerCapabilityOptions options, ConnectionScanner& scanner);

    WorkerCapabilities(const WorkerCapabilities&) = delete;
    WorkerCapabilities& operator=(const WorkerCapabilities&) = delete;

    [[nodiscard]] Task<void> connect();
    [[nodiscard]] Task<void> join();
    void closeNow() noexcept;
    void initializeWorkerState();
    void shutdownWorkerState() noexcept;

    [[nodiscard]] ContextServices contextServices() noexcept;
    [[nodiscard]] DbRegistry& databases() noexcept;
    [[nodiscard]] RedisRegistry& redis() noexcept;
    [[nodiscard]] HttpClientRegistry& httpClients() noexcept;
    [[nodiscard]] const WorkerStateRegistry& workerStates() const noexcept;
    [[nodiscard]] RateLimiter& rateLimiter() noexcept;
    [[nodiscard]] BlockingPool* blockingPool() const noexcept;

private:
    DbRegistry databases_;
    RedisRegistry redis_;
    ConnectionScanner::WorkerMaintenanceRegistration databaseDeadlineMaintenance_;
    ConnectionScanner::WorkerMaintenanceRegistration redisDeadlineMaintenance_;
    HttpClientRegistry httpClients_;
    WorkerStateRegistry workerStates_;
    RateLimiter rateLimiter_;
    WorkerCapabilityOptions options_;
};

}  // namespace ruvia::detail
