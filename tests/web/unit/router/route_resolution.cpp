#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_response_stream.h"

#include "router/route_table.h"
#include "test_harness.h"

namespace {

using ruvia::detail::max_route_params;
using ruvia::detail::route_endpoint;
using ruvia::detail::route_entry;
using ruvia::detail::route_handler_type;
using ruvia::detail::route_match;
using ruvia::detail::route_resolution;
using ruvia::detail::route_stream_handler_type;
using ruvia::detail::route_table;

ruvia::task<ruvia::http_response> route_handler(void*, ruvia::context& context_value) {
    co_return ruvia::http_response({.resource_ = context_value.arena()});
}

ruvia::task<void> stream_route_handler(void*, ruvia::context&) {
    co_return;
}

const route_entry& fake_route() {
    static route_entry route(std::pmr::get_default_resource(),
        route_entry::init_type{.method_ = ruvia::http_known_method::get,
            .path_ = "/route",
            .endpoint_ = ruvia::detail::route_endpoint::buffered(
                ruvia::detail::route_handler_type(nullptr, &route_handler),
                ruvia::detail::request_body_mode::buffered)});
    return route;
}

RUVIA_TEST(route_endpoint_binds_handler_shape_and_only_relevant_metadata) {
    const auto buffered = route_endpoint::buffered(
        route_handler_type(nullptr, &route_handler), ruvia::detail::request_body_mode::stream);
    RUVIA_CHECK(buffered.buffered() != nullptr);
    RUVIA_CHECK(buffered.response_stream() == nullptr);
    RUVIA_CHECK(buffered.get_websocket() == nullptr);
    RUVIA_CHECK(buffered.request_body_mode() == ruvia::detail::request_body_mode::stream);

    const auto stream = route_endpoint::response_stream(
        route_stream_handler_type(nullptr, &stream_route_handler), ruvia::http_response_stream_kind::sse);
    RUVIA_CHECK(stream.buffered() == nullptr);
    RUVIA_CHECK(stream.response_stream() != nullptr);
    RUVIA_CHECK(stream.get_websocket() == nullptr);
    RUVIA_CHECK(stream.response_stream()->kind() == ruvia::http_response_stream_kind::sse);
    RUVIA_CHECK(stream.request_body_mode() == ruvia::detail::request_body_mode::buffered);

    std::vector<std::string> source_protocols{"chat", "superchat"};
    ruvia::websocket_route_config options;
    options.subprotocols_ = source_protocols;
    options.lifecycle_.heartbeat_ = {
        .ping_interval_ = std::chrono::milliseconds(25),
    };
    const auto websocket_value = route_endpoint::get_websocket(std::pmr::get_default_resource(),
        route_stream_handler_type(nullptr, &stream_route_handler), options);
    source_protocols.front().assign("mutated");
    RUVIA_CHECK(websocket_value.buffered() == nullptr);
    RUVIA_CHECK(websocket_value.response_stream() == nullptr);
    RUVIA_CHECK(websocket_value.get_websocket() != nullptr);
    RUVIA_CHECK_EQ(websocket_value.get_websocket()->subprotocols().size(), std::size_t{2});
    RUVIA_CHECK_EQ(websocket_value.get_websocket()->subprotocols()[0], std::string_view("chat"));
    RUVIA_CHECK_EQ(websocket_value.get_websocket()->subprotocols()[1], std::string_view("superchat"));
    RUVIA_CHECK_EQ(
        websocket_value.get_websocket()->lifecycle().heartbeat_.ping_interval_->count(), std::int64_t{25});
    RUVIA_CHECK_EQ(
        websocket_value.get_websocket()->lifecycle().heartbeat_.pong_timeout_->count(), std::int64_t{25});
}

RUVIA_TEST(route_endpoint_rejects_empty_handlers_and_invalid_discriminants) {
    bool rejected = false;
    try {
        (void)route_endpoint::buffered(route_handler_type{}, ruvia::detail::request_body_mode::buffered);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);

    rejected = false;
    try {
        (void)route_endpoint::buffered(
            route_handler_type(nullptr, &route_handler), static_cast<ruvia::detail::request_body_mode>(99));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);

    rejected = false;
    try {
        (void)route_endpoint::response_stream(route_stream_handler_type(nullptr, &stream_route_handler),
            static_cast<ruvia::http_response_stream_kind>(99));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

}  // namespace

RUVIA_TEST(route_match_add_and_values) {
    route_match match;
    RUVIA_CHECK_EQ(match.size(), std::size_t{0});
    RUVIA_CHECK(match.add("alpha"));
    RUVIA_CHECK(match.add("beta"));
    RUVIA_CHECK_EQ(match.size(), std::size_t{2});
    RUVIA_CHECK_EQ(match.values().size(), std::size_t{2});
    RUVIA_CHECK_EQ(match.values()[0], std::string_view("alpha"));
    RUVIA_CHECK_EQ(match.values()[1], std::string_view("beta"));
}

RUVIA_TEST(route_match_truncate_and_clear) {
    route_match match;
    RUVIA_CHECK(match.add("a"));
    RUVIA_CHECK(match.add("b"));
    RUVIA_CHECK(match.add("c"));
    match.truncate(2);
    RUVIA_CHECK_EQ(match.size(), std::size_t{2});
    // A count larger than the current size is clamped (no growth).
    match.truncate(10);
    RUVIA_CHECK_EQ(match.size(), std::size_t{2});
    match.clear();
    RUVIA_CHECK_EQ(match.size(), std::size_t{0});
}

RUVIA_TEST(route_match_add_rejects_when_full) {
    route_match match;
    for (std::size_t i = 0; i < max_route_params; ++i) {
        RUVIA_CHECK(match.add("x"));
    }
    RUVIA_CHECK_EQ(match.size(), max_route_params);
    RUVIA_CHECK(!match.add("overflow"));  // capacity reached
    RUVIA_CHECK_EQ(match.size(), max_route_params);
}

RUVIA_TEST(route_resolution_found_static) {
    const auto resolution = route_resolution::resolved(fake_route());
    const auto* resolved = resolution.resolved();
    RUVIA_CHECK(resolved != nullptr);
    RUVIA_CHECK(resolution.method_not_allowed() == nullptr);
    RUVIA_CHECK(resolution.not_found() == nullptr);
    RUVIA_CHECK(resolved->match().values().empty());
}

RUVIA_TEST(route_resolution_found_dynamic) {
    route_match match;
    RUVIA_CHECK(match.add("id"));
    const auto resolution = route_resolution::resolved(fake_route(), match);
    const auto* resolved = resolution.resolved();
    RUVIA_CHECK(resolved != nullptr);
    RUVIA_CHECK(&resolved->match() != &match);
    RUVIA_CHECK_EQ(resolved->match().size(), std::size_t{1});
    RUVIA_CHECK_EQ(resolved->match().values()[0], std::string_view("id"));
}

RUVIA_TEST(route_resolution_method_not_allowed_vs_not_found) {
    // 405: no route, but a non-zero allowed-methods mask drives the Allow header.
    const auto not_allowed = route_resolution::method_not_allowed(0x5);
    RUVIA_CHECK(not_allowed.resolved() == nullptr);
    RUVIA_CHECK(not_allowed.not_found() == nullptr);
    RUVIA_CHECK(not_allowed.method_not_allowed() != nullptr);
    RUVIA_CHECK_EQ(not_allowed.method_not_allowed()->allowed_methods(), std::uint32_t{0x5});

    // 404 is its own payload-free alternative.
    const route_resolution not_found;
    RUVIA_CHECK(not_found.resolved() == nullptr);
    RUVIA_CHECK(not_found.method_not_allowed() == nullptr);
    RUVIA_CHECK(not_found.not_found() != nullptr);

    // A zero Allow mask cannot materialize a fake 405 state.
    const auto zero_mask = route_resolution::method_not_allowed(0);
    RUVIA_CHECK(zero_mask.not_found() != nullptr);
}
