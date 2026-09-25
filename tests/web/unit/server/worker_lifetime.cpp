#include <array>
#include <chrono>
#include <semaphore>
#include <span>
#include <thread>
#include <utility>

#include <asio/ip/address.hpp>

#include "ruvia/web/HttpClientTypes.h"
#include "ruvia/web/detail/client/HttpClientConfigStorage.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/server/HttpServerOptionsValidation.h"
#include "ruvia/web/detail/server/WebWorkerRuntime.h"

#include "test_harness.h"

namespace {

ruvia::Task<void> completePost() {
    co_return;
}

}  // namespace

RUVIA_TEST(session_only_worker_keeps_capability_clients_until_stop) {
    using namespace std::chrono_literals;

    ruvia::detail::HttpServerOptions options;
    const ruvia::detail::HttpServerListenerDefinition listener(
        {asio::ip::address_v4::loopback(), 0});
    auto configuration = ruvia::detail::validateHttpServerConfiguration(
        std::span<const ruvia::detail::HttpServerListenerDefinition>(&listener, 1),
        std::move(options));
    ruvia::detail::RouteTable routes(std::pmr::get_default_resource());

    ruvia::HttpClientConfig clientConfig;
    clientConfig.host = "127.0.0.1";
    clientConfig.port = 1;
    const ruvia::detail::HttpClientDefinition clientDefinition{
        std::pmr::string("probe"),
        ruvia::detail::HttpClientConfigStorage(clientConfig, std::pmr::get_default_resource())};
    const ruvia::detail::WorkerCapabilityDefinitions capabilities{
        .httpClients = std::span<const ruvia::detail::HttpClientDefinition>(&clientDefinition, 1)};
    ruvia::detail::WebWorkerRuntime runtime(configuration, routes, capabilities);
    runtime.prepare();
    runtime.launch();
    runtime.waitUntilReady();
    runtime.requestServe();
    RUVIA_CHECK(runtime.waitUntilServing());

    std::binary_semaphore inspected(0);
    bool clientUsable = false;
    const auto posted = runtime.webWorker().post([&](ruvia::WebWorkerContext& context) {
        const auto client = context.httpClient("probe");
        clientUsable = client.host() == "127.0.0.1" && client.port() == 1;
        inspected.release();
        return completePost();
    });
    RUVIA_CHECK(posted.accepted());
    RUVIA_CHECK(inspected.try_acquire_for(2s));
    RUVIA_CHECK(clientUsable);

    runtime.stop();
    runtime.join();
    RUVIA_CHECK(!runtime.webWorker().accepting());
}
