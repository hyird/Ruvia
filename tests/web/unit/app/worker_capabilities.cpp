#include "test_harness.h"

#include <concepts>
#include <memory>
#include <type_traits>

#include <asio/io_context.hpp>

#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/detail/io/ConnectionScanner.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/web/detail/integration/WorkerCapabilities.h"

static_assert(!std::is_copy_constructible_v<ruvia::detail::WorkerCapabilities>);
static_assert(!std::is_move_constructible_v<ruvia::detail::WorkerCapabilities>);
static_assert(std::is_trivially_copyable_v<ruvia::detail::WorkerClientRegistryView>);
static_assert(!std::is_default_constructible_v<ruvia::detail::WorkerClientRegistryView>);
static_assert(!std::constructible_from<ruvia::detail::WorkerClientRegistryView, ruvia::detail::DbRegistry*, ruvia::detail::RedisRegistry*, ruvia::detail::HttpClientRegistry*>);
static_assert(std::constructible_from<ruvia::detail::WorkerClientRegistryView, ruvia::detail::DbRegistry&, ruvia::detail::RedisRegistry&, ruvia::detail::HttpClientRegistry&>);
static_assert(ruvia::detail::WorkerCapabilityOptions{}.rateLimitCapacity == ruvia::kDefaultRateLimitCapacityPerWorker);
static_assert(!std::constructible_from<ruvia::detail::WorkerCapabilities, asio::io_context&, ruvia::WorkerHandle&&, std::pmr::memory_resource*, ruvia::detail::WorkerCapabilityDefinitions, ruvia::detail::WorkerCapabilityOptions, ruvia::detail::ConnectionScanner&>);

RUVIA_TEST(worker_capabilities_exposes_one_address_stable_capability_graph) {
    asio::io_context ioContext;
    const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 64);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    ruvia::WorkerMemory memory;
    ruvia::detail::ConnectionScanner scanner(worker, {});
    ruvia::detail::WorkerCapabilities capabilities(ioContext, worker, memory.resource(), {}, {}, scanner);
    const ruvia::StopToken stopToken;

    capabilities.initializeWorkerState();
    const auto services = capabilities.contextServices(stopToken);
    const auto ownedClients = capabilities.clientRegistries();
    const auto requestClients = services.clientRegistries();

    RUVIA_CHECK(requestClients.attached());
    RUVIA_CHECK(requestClients == ownedClients);
    RUVIA_CHECK(!ruvia::detail::WorkerClientRegistryView::detached().attached());
    RUVIA_CHECK(services.workerStates() == &capabilities.workerStates());
    RUVIA_CHECK(services.rateLimiter() == &capabilities.rateLimiter());
    RUVIA_CHECK(&services.worker() == &worker);
    RUVIA_CHECK(&services.stopToken() == &stopToken);
    RUVIA_CHECK_EQ(services.maxDecodedBodyBytes(), ruvia::kDefaultMaxBufferedBodyBytes);
    RUVIA_CHECK(services.blockingPool() == nullptr);

    capabilities.closeNow();
    capabilities.shutdownWorkerState();
}
