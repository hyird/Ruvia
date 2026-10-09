#include <chrono>
#include <span>
#include <thread>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "router/route_table.h"
#include "server/http_server_options_validation.h"
#include "server/web_worker_runtime.h"
#include "test_harness.h"

RUVIA_TEST(validated_web_worker_runs_without_binding_listeners) {
    using listener_definition = ruvia::detail::http_server_listener_definition;
    asio::io_context reservation_context;
    asio::ip::tcp::acceptor reservation(reservation_context, asio::ip::tcp::v4());
    reservation.bind({asio::ip::address_v4::loopback(), 0});
    const auto endpoint = reservation.local_endpoint();
    reservation.close();

    listener_definition listener(endpoint);
    ruvia::detail::http_server_options options;
    auto configuration = ruvia::detail::validate_http_server_configuration(
        std::span<const listener_definition>(&listener, 1), std::move(options));
    ruvia::detail::route_table routes(std::pmr::get_default_resource());
    ruvia::detail::web_worker_runtime runtime(configuration, routes, {});
    runtime.prepare();

    asio::ip::tcp::acceptor probe_value(runtime.worker_executor());
    probe_value.open(endpoint.protocol());
    probe_value.bind(endpoint);

    runtime.launch();
    runtime.wait_until_ready();
    runtime.request_serve();
    RUVIA_CHECK(runtime.wait_until_serving());
    runtime.stop();
    runtime.join();
}

RUVIA_TEST(web_worker_records_transferred_socket_assignment_failure) {
    using namespace std::chrono_literals;

    ruvia::detail::route_table routes(std::pmr::get_default_resource());
    ruvia::detail::web_worker_runtime runtime(
        asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0), routes, {});
    runtime.start();

    auto ticket = ruvia::detail::native_accepted_socket_ticket(asio::ip::tcp::v4(), 0,
        ruvia::detail::native_accepted_socket_ticket::invalid_native());
    auto post = runtime.network_submission().post([&runtime, ticket = std::move(ticket)]() mutable {
        runtime.accept_transferred_connection(std::move(ticket));
    });
    RUVIA_CHECK(post.accepted());

    const auto deadline_value = std::chrono::steady_clock::now() + 2s;
    while (runtime.stats().accept_failures_ == 0 &&
           std::chrono::steady_clock::now() < deadline_value) {
        std::this_thread::sleep_for(1ms);
    }
    RUVIA_CHECK(runtime.stats().accept_failures_ == 1U);
    RUVIA_CHECK(runtime.stats().worker_failures_ == 0U);
    runtime.stop();
    runtime.join();
}

RUVIA_TEST(web_worker_accepts_transferred_connection_on_its_worker) {
    using namespace std::chrono_literals;

    ruvia::detail::route_table routes(std::pmr::get_default_resource());
    ruvia::detail::http_server_options options;
    options.max_connections_ = 1;
    ruvia::detail::web_worker_runtime runtime(
        asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0), routes, {}, options);
    runtime.start();
    RUVIA_CHECK(runtime.available_for_network_dispatch());

    asio::ip::tcp::acceptor source_value(runtime.worker_executor(),
        asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    asio::ip::tcp::socket first_client(runtime.worker_executor());
    first_client.connect(source_value.local_endpoint());
    asio::ip::tcp::socket first(runtime.worker_executor());
    source_value.accept(first);
    asio::error_code release_error;
    auto first_native = first.release(release_error);
    RUVIA_CHECK(!release_error);
    auto first_ticket = ruvia::detail::native_accepted_socket_ticket(
        asio::ip::tcp::v4(), 0, first_native);
    auto first_post = runtime.network_submission().post([&runtime, first_ticket = std::move(first_ticket)]() mutable {
        runtime.accept_transferred_connection(std::move(first_ticket));
    });
    RUVIA_CHECK(first_post.accepted());

    const auto deadline_value = std::chrono::steady_clock::now() + 2s;
    while (runtime.stats().active_connections_ == 0 &&
           std::chrono::steady_clock::now() < deadline_value) {
        std::this_thread::sleep_for(1ms);
    }
    RUVIA_CHECK(runtime.stats().active_connections_ == 1U);
    RUVIA_CHECK(!runtime.available_for_network_dispatch());

    asio::ip::tcp::socket second_client(runtime.worker_executor());
    second_client.connect(source_value.local_endpoint());
    asio::ip::tcp::socket second(runtime.worker_executor());
    source_value.accept(second);
    auto second_native = second.release(release_error);
    RUVIA_CHECK(!release_error);
    auto second_ticket = ruvia::detail::native_accepted_socket_ticket(
        asio::ip::tcp::v4(), 0, second_native);
    auto second_post = runtime.network_submission().post([&runtime, second_ticket = std::move(second_ticket)]() mutable {
        runtime.accept_transferred_connection(std::move(second_ticket));
    });
    RUVIA_CHECK(second_post.accepted());
    while (runtime.stats().connections_refused_ == 0 &&
           std::chrono::steady_clock::now() < deadline_value) {
        std::this_thread::sleep_for(1ms);
    }
    RUVIA_CHECK(runtime.stats().connections_refused_ >= 1U);
    runtime.stop();
    runtime.join();

    RUVIA_CHECK(runtime.network_submission().post([] {}).status() ==
                ruvia::post_status::worker_stopping);
}
