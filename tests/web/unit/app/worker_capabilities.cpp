#include <concepts>
#include <memory>
#include <type_traits>

#include <asio/io_context.hpp>

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/web/detail/integration/WorkerCapabilities.h"

#include "test_harness.h"
#include "test_io_context.h"

RUVIA_TEST(worker_capabilities_exposes_one_address_stable_capability_graph) {
    auto& ioContext = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(ioContext, {.mailboxCapacity = 64});
    const auto worker = attachment.loop().handle();
    ruvia::WorkerMemory memory;
    ruvia::detail::WorkerCapabilities capabilities(ioContext, worker, memory.resource(), {}, {});
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
