#include <array>
#include <chrono>
#include <semaphore>
#include <span>
#include <thread>
#include <utility>

#include <asio/ip/address.hpp>

#include "ruvia/web/http_client_types.h"

#include "client/http_client_config_storage.h"
#include "router/route_table.h"
#include "server/http_server_options_validation.h"
#include "server/web_worker_runtime.h"
#include "test_harness.h"

namespace {

ruvia::task<void> complete_post() {
    co_return;
}

}  // namespace

RUVIA_TEST(unstarted_worker_stop_and_join_are_safe) {
    ruvia::detail::route_table routes(std::pmr::get_default_resource());
    ruvia::detail::web_worker_runtime runtime(
        asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0), routes);

    runtime.stop();
    runtime.join();
    RUVIA_CHECK(!runtime.worker().accepting());
}

RUVIA_TEST(session_only_worker_aborts_before_serve_and_finalizes) {
    ruvia::detail::http_server_options options;
    const ruvia::detail::http_server_listener_definition listener_value(
        {asio::ip::address_v4::loopback(), 0});
    auto configuration = ruvia::detail::validate_http_server_configuration(
        std::span<const ruvia::detail::http_server_listener_definition>(&listener_value, 1),
        std::move(options));
    ruvia::detail::route_table routes(std::pmr::get_default_resource());
    ruvia::detail::web_worker_runtime runtime(configuration, routes, {});
    runtime.prepare();
    runtime.launch();
    runtime.wait_until_ready();

    runtime.stop_admission();
    runtime.finalize_after_network_quiesced();
    runtime.join();
    RUVIA_CHECK(!runtime.worker().accepting());
}

RUVIA_TEST(standalone_listener_worker_stops_without_app_coordination) {
    ruvia::detail::route_table routes(std::pmr::get_default_resource());
    ruvia::detail::web_worker_runtime runtime(
        asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0), routes);
    runtime.prepare();
    runtime.launch();
    runtime.wait_until_ready();
    runtime.request_serve();
    RUVIA_CHECK(runtime.wait_until_serving());
    RUVIA_CHECK(runtime.local_endpoint().port() != 0);

    runtime.stop();
    runtime.join();
    RUVIA_CHECK(!runtime.worker().accepting());
}

RUVIA_TEST(session_only_worker_keeps_capability_clients_until_stop) {
    using namespace std::chrono_literals;

    ruvia::detail::http_server_options options;
    const ruvia::detail::http_server_listener_definition listener_value(
        {asio::ip::address_v4::loopback(), 0});
    auto configuration = ruvia::detail::validate_http_server_configuration(
        std::span<const ruvia::detail::http_server_listener_definition>(&listener_value, 1),
        std::move(options));
    ruvia::detail::route_table routes(std::pmr::get_default_resource());

    ruvia::http_client_config client_config;
    client_config.host_ = "127.0.0.1";
    client_config.port_ = 1;
    const ruvia::detail::http_client_definition_type client_definition{
        std::pmr::string("probe"),
        ruvia::detail::http_client_config_storage(client_config, std::pmr::get_default_resource())};
    const ruvia::detail::worker_capability_definitions capabilities{
        .http_clients_ = std::span<const ruvia::detail::http_client_definition_type>(&client_definition, 1)};
    ruvia::detail::web_worker_runtime runtime(configuration, routes, capabilities);
    runtime.prepare();
    runtime.launch();
    runtime.wait_until_ready();
    runtime.request_serve();
    RUVIA_CHECK(runtime.wait_until_serving());

    std::binary_semaphore inspected(0);
    bool client_usable = false;
    const auto posted = runtime.web_worker().post([&](ruvia::web_worker_context& context_value) {
        const auto client = context_value.get_http_client("probe");
        client_usable = client.host() == "127.0.0.1" && client.port() == 1;
        inspected.release();
        return complete_post();
    });
    RUVIA_CHECK(posted.accepted());
    RUVIA_CHECK(inspected.try_acquire_for(2s));
    RUVIA_CHECK(client_usable);

    runtime.stop();
    runtime.join();
    RUVIA_CHECK(!runtime.web_worker().accepting());
}
