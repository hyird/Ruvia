#include "router/router_impl.h"
#include "routing_fixture.h"

namespace {
ruvia::task<void> connect_handler(void*, ruvia::context&) {
    co_return;
}
void register_connect_routes(ruvia::detail::router_impl& router_value) {
    const ruvia::detail::route_stream_handler_type handler(nullptr, &connect_handler);
    router_value.register_tunnel_route({}, std::pmr::string("proxy.test:443"), handler, {}, {});
    router_value.register_tunnel_route({}, std::pmr::string("*"), handler, {}, {});
    router_value.register_tunnel_route("connect-udp", std::pmr::string("/udp/:host/:port"), handler, {}, {});
    router_value.register_tunnel_route("test-protocol", std::pmr::string("/udp/:host/:port"), handler, {}, {});
    router_value.register_tunnel_route("connect-udp", std::pmr::string("/udp/static/443"), handler, {}, {});
}
}  // namespace
RUVIA_TEST(connect_routes_match_authorities_and_separate_protocol_paths_with_shared_worker_plan) {
    ruvia::detail::router first_router;
    auto& first = ruvia::detail::router_impl::from(first_router);
    register_connect_routes(first);
    first.finalize();
    auto plan = first.release_compiled_plan();
    ruvia::detail::router second_router;
    auto& second = ruvia::detail::router_impl::from(second_router);
    register_connect_routes(second);
    second.finalize(plan.get());
    const auto& table_value = second.route_table();
    auto exact = table_value.resolve(ruvia::http_known_method::connect, "PROXY.TEST:0443");
    RUVIA_CHECK(exact.resolved() != nullptr);
    RUVIA_CHECK(exact.resolved()->route().path() == "proxy.test:443");
    auto fallback = table_value.resolve(ruvia::detail::route_request_view{ruvia::http_known_method::connect, "CONNECT", {}, "other.test:80", {}});
    RUVIA_CHECK(fallback.resolved() != nullptr);
    RUVIA_CHECK(fallback.resolved()->route().path() == "*");
    auto udp = table_value.resolve(ruvia::detail::route_request_view{ruvia::http_known_method::connect, "CONNECT", "/udp/target.test/443", "ignored.test:443", "connect-udp"});
    RUVIA_CHECK(udp.resolved() != nullptr);
    RUVIA_CHECK(udp.resolved()->route().endpoint().tunnel()->protocol() == "connect-udp");
    RUVIA_CHECK_EQ(udp.resolved()->match().values().size(), std::size_t{2});
    RUVIA_CHECK(udp.resolved()->match().values()[0] == "target.test");
    RUVIA_CHECK(udp.resolved()->match().values()[1] == "443");
    auto static_route = table_value.resolve(ruvia::detail::route_request_view{ruvia::http_known_method::connect, "CONNECT", "/udp/static/443", "ignored.test:443", "connect-udp"});
    RUVIA_CHECK(static_route.resolved() != nullptr);
    RUVIA_CHECK(static_route.resolved()->match().size() == 0);
    const auto unknown = table_value.resolve(ruvia::detail::route_request_view{ruvia::http_known_method::connect, "CONNECT", "/udp/target.test/443", "proxy.test:443", "unregistered"});
    RUVIA_CHECK(unknown.not_found() != nullptr);
    const auto ordinary = table_value.resolve(ruvia::http_known_method::get, "/udp/target.test/443");
    RUVIA_CHECK(ordinary.not_found() != nullptr);
}
RUVIA_TEST(connect_routes_reject_invalid_authorities_and_equivalent_duplicate_targets) {
    const ruvia::detail::route_stream_handler_type handler(nullptr, &connect_handler);
    for (const auto authority : {"proxy.test", "proxy.test:", "proxy.test:65536", "/proxy", "user@proxy.test:443"}) {
        ruvia::detail::router router;
        auto& routes_value = ruvia::detail::router_impl::from(router);
        bool rejected = false;
        try {
            routes_value.register_tunnel_route({}, std::pmr::string(authority), handler, {}, {});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
    }
    ruvia::detail::router router;
    auto& routes_value = ruvia::detail::router_impl::from(router);
    routes_value.register_tunnel_route({}, std::pmr::string("proxy.test:443"), handler, {}, {});
    bool rejected = false;
    try {
        routes_value.register_tunnel_route({}, std::pmr::string("PROXY.TEST:0443"), handler, {}, {});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(route_facts_keep_exact_methods_and_extended_connect_targets_separate) {
    ruvia::detail::router router;
    auto& routes_value = ruvia::detail::router_impl::from(router);
    register_connect_routes(routes_value);
    routes_value.register_extension_method_route("MiXeD", std::pmr::string("/extension"),
        ruvia::detail::route_handler_type(nullptr, &routing_test::dummy_handler),
        ruvia::detail::request_body_mode::stream, {}, {});
    routes_value.register_websocket_route(ruvia::http_known_method::get, std::pmr::string("/socket"),
        ruvia::detail::route_stream_handler_type(nullptr, &connect_handler), {}, {}, {});
    routes_value.register_tunnel_route("WebSocket", std::pmr::string("/socket"),
        ruvia::detail::route_stream_handler_type(nullptr, &connect_handler), {}, {});
    routes_value.finalize();
    const auto& table_value = routes_value.route_table();
    using facts = ruvia::detail::route_request_view;
    const auto extension = table_value.resolve(facts{ruvia::http_known_method::unknown, "MiXeD", "/extension", {}, {}});
    RUVIA_CHECK(extension.resolved() != nullptr);
    RUVIA_CHECK(extension.resolved()->route().endpoint().request_body_mode() == ruvia::detail::request_body_mode::stream);
    const auto different_case = table_value.resolve(facts{ruvia::http_known_method::unknown, "mixed", "/extension", {}, {}});
    RUVIA_CHECK(different_case.not_found() != nullptr);
    const auto socket = table_value.resolve(facts{ruvia::http_known_method::connect, "CONNECT", "/socket", "proxy.test:443", "websocket"});
    RUVIA_CHECK(socket.resolved() != nullptr);
    RUVIA_CHECK(socket.resolved()->route().endpoint().get_websocket() != nullptr);
    const auto raw_protocol = table_value.resolve(facts{ruvia::http_known_method::connect, "CONNECT", "/socket", "proxy.test:443", "WeBsOcKeT"});
    RUVIA_CHECK(raw_protocol.not_found() != nullptr);
    const auto exact_protocol = table_value.resolve(facts{ruvia::http_known_method::connect, "CONNECT", "/socket", "proxy.test:443", "WebSocket"});
    RUVIA_CHECK(exact_protocol.resolved() != nullptr);
    RUVIA_CHECK(exact_protocol.resolved()->route().endpoint().tunnel() != nullptr);
    RUVIA_CHECK(exact_protocol.resolved()->route().endpoint().get_websocket() == nullptr);
    const auto authority = table_value.resolve(facts{ruvia::http_known_method::connect, "CONNECT", "/socket", "PROXY.TEST:0443", {}});
    RUVIA_CHECK(authority.resolved() != nullptr);
    RUVIA_CHECK_EQ(authority.resolved()->route().path(), std::string_view("proxy.test:443"));
    const auto server_options = table_value.resolve(facts{ruvia::http_known_method::options, "OPTIONS", "*", {}, {}});
    RUVIA_CHECK(server_options.resolved() == nullptr);
}
