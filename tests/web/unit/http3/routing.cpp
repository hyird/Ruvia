#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http3_client_request_head.h"
#include "ruvia/http/http3_message_head.h"
#include "ruvia/http/http3_server_request.h"
#include "ruvia/web/context.h"

#include "context_services_fixture.h"
#include "router/router.h"
#include "router/router_impl.h"
#include "routing_fixture.h"
#include "server/document_root_binding.h"
#include "test_harness.h"

namespace {

ruvia::task<ruvia::http_response> route_http3_request(void*, ruvia::context& context_value) {
    const auto body = co_await context_value.req().text();
    const bool valid = context_value.req().method() == "POST" && context_value.req().path() == "/items" &&
                       context_value.req().header("Host") == "example.test" &&
                       context_value.req().cookie("session") == "one" &&
                       context_value.req().cookie("other") == "two" && body == "payload";
    co_return context_value.text(valid ? std::string_view("h3-route-ok")
                                       : std::string_view("h3-route-mismatch"));
}

}  // namespace

RUVIA_TEST(http3_request_resolves_at_headers_and_dispatches_after_incremental_body_completion) {
    ruvia::detail::router router;
    auto& implementation = ruvia::detail::router_impl::from(router);
    implementation.register_route(ruvia::http_known_method::post, routing_test::path("/items"),
        ruvia::detail::route_handler_type(nullptr, &route_http3_request),
        ruvia::detail::request_body_mode::buffered, {}, {});
    implementation.finalize();
    const auto& routes_value = implementation.route_table();

    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    std::optional<ruvia::http3_server_request> owner;
    const std::array fields_value{
        ruvia::http3_field_section_field_view{"cookie", "session=one"},
        ruvia::http3_field_section_field_view{"cookie", "other=two"}};
    const auto wire = ruvia::encode_http3_client_request_head({.method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .path_ = "/items", .fields_ = fields_value, .body_length_ = 7});
    RUVIA_CHECK((wire.index() == 0));
    if ((wire.index() != 0)) {
        return;
    }
    {
        auto decoded = ruvia::decode_http3_message_head(std::get<0>(wire).field_section_,
            ruvia::http3_message_head_kind::request, worker.resource());
        RUVIA_CHECK((decoded.index() == 0));
        if ((decoded.index() != 0)) {
            return;
        }
        owner.emplace(std::get<0>(decoded), memory.resource(), memory.upstream_resource());
    }
    const auto& request = owner->request();
    RUVIA_CHECK(!owner->body_complete());
    RUVIA_CHECK(request.body_bytes().empty());
    const auto resolution = routes_value.resolve(request);
    RUVIA_CHECK(resolution.resolved() != nullptr);
    if (resolution.resolved() == nullptr) {
        return;
    }

    std::array part_one{std::byte{'p'}, std::byte{'a'}, std::byte{'y'}};
    std::array part_two{std::byte{'l'}, std::byte{'o'}, std::byte{'a'}, std::byte{'d'}};
    owner->append_body(part_one);
    owner->append_body(part_two);
    RUVIA_CHECK(request.body_bytes().empty());
    owner->finish_body();
    part_one.fill(std::byte{'x'});
    part_two.fill(std::byte{'x'});
    RUVIA_CHECK(owner->body_complete());

    asio::io_context io(1);
    auto future = asio::co_spawn(io, ruvia::as_awaitable(routes_value.dispatch_buffered_response(request, resolution, memory, ruvia::detail::document_root_binding::none(), ruvia::test::test_context_services())), asio::use_future);
    io.run();
    const auto response = future.get();
    RUVIA_CHECK(response.status() == ruvia::http_status::ok);
    RUVIA_CHECK_EQ(response.body_bytes(), std::string_view("h3-route-ok"));
}

RUVIA_TEST(http3_request_uses_normal_web_buffered_route_and_owns_body_through_dispatch) {
    ruvia::detail::router router;
    auto& implementation = ruvia::detail::router_impl::from(router);
    implementation.register_route(ruvia::http_known_method::post, routing_test::path("/items"),
        ruvia::detail::route_handler_type(nullptr, &route_http3_request),
        ruvia::detail::request_body_mode::buffered, {}, {});
    implementation.finalize();
    const auto& routes_value = implementation.route_table();

    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    const std::array fields_value{
        ruvia::http3_field_section_field_view{"cookie", "session=one"},
        ruvia::http3_field_section_field_view{"cookie", "other=two"}};
    const auto wire = ruvia::encode_http3_client_request_head({.method_ = "POST", .scheme_ = "https", .authority_ = "example.test", .path_ = "/items", .fields_ = fields_value, .body_length_ = 7});
    RUVIA_CHECK((wire.index() == 0));
    if ((wire.index() != 0)) {
        return;
    }
    auto decoded = ruvia::decode_http3_message_head(std::get<0>(wire).field_section_,
        ruvia::http3_message_head_kind::request, memory.resource());
    RUVIA_CHECK((decoded.index() == 0));
    if ((decoded.index() != 0)) {
        return;
    }

    std::array body{std::byte{'p'}, std::byte{'a'}, std::byte{'y'}, std::byte{'l'},
        std::byte{'o'}, std::byte{'a'}, std::byte{'d'}};
    ruvia::http3_server_request owner_value(std::get<0>(decoded), memory.resource(), memory.upstream_resource());
    owner_value.append_body(body);
    owner_value.finish_body();
    body.fill(std::byte{'x'});  // The handler must not borrow the caller's transport buffer.
    const auto& request = owner_value.request();
    RUVIA_CHECK(request.protocol_version() == ruvia::http_protocol_version::http3);
    RUVIA_CHECK(request.target_form() == ruvia::http_request_target_form::http3);
    const auto resolution = routes_value.resolve(request);
    RUVIA_CHECK(resolution.resolved() != nullptr);
    if (resolution.resolved() == nullptr) {
        return;
    }

    asio::io_context io(1);
    auto future = asio::co_spawn(io, ruvia::as_awaitable(routes_value.dispatch_buffered_response(request, resolution, memory, ruvia::detail::document_root_binding::none(), ruvia::test::test_context_services())), asio::use_future);
    io.run();
    const auto response = future.get();
    RUVIA_CHECK(response.status() == ruvia::http_status::ok);
    RUVIA_CHECK_EQ(response.body_bytes(), std::string_view("h3-route-ok"));
}
