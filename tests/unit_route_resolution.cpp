#include "test_harness.h"

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/http/HttpKnownMethod.h"

namespace {

using ruvia::detail::kMaxRouteParams;
using ruvia::detail::RouteEntry;
using ruvia::detail::RouteEndpoint;
using ruvia::detail::RouteHandler;
using ruvia::detail::RouteMatch;
using ruvia::detail::RouteResolution;
using ruvia::detail::RouteStreamHandler;
using ruvia::detail::RouteTable;

template <typename T>
concept HasLooseRouteResolutionAccessors = requires(const T& value) {
    value.found();
    value.route();
    value.match();
    value.allowedMethods();
};

template <typename T>
concept HasAnyRvalueRouteResolutionBorrow =
    requires(T&& value) { std::move(value).values(); } ||
    requires(T&& value) { std::move(value).match(); } ||
    requires(T&& value) { std::move(value).resolved(); } ||
    requires(T&& value) { std::move(value).methodNotAllowed(); } ||
    requires(T&& value) { std::move(value).notFound(); };

template <typename T>
concept HasAnyRvalueStreamDispatchBorrow =
    requires(T&& value) { std::move(value).handled(); } ||
    requires(T&& value) { std::move(value).buffered(); };

static_assert(!HasLooseRouteResolutionAccessors<RouteResolution>);
static_assert(!HasAnyRvalueRouteResolutionBorrow<RouteMatch>);
static_assert(!HasAnyRvalueRouteResolutionBorrow<ruvia::detail::ResolvedRoute>);
static_assert(!HasAnyRvalueRouteResolutionBorrow<RouteResolution>);
static_assert(!HasAnyRvalueStreamDispatchBorrow<
    ruvia::detail::StreamDispatchResult>);
static_assert(!std::default_initializable<RouteEndpoint>);
static_assert(!std::copy_constructible<RouteEndpoint>);
static_assert(std::move_constructible<RouteEndpoint>);
static_assert(!std::is_move_assignable_v<RouteEndpoint>);
static_assert(std::move_constructible<RouteEntry>);
static_assert(!std::is_move_assignable_v<RouteEntry>);
static_assert(!std::is_polymorphic_v<RouteTable>);
static_assert(!std::is_move_constructible_v<RouteTable>);
static_assert(!std::is_move_assignable_v<RouteTable>);

ruvia::Task<ruvia::HttpResponse> routeHandler(void*, ruvia::Context& context) {
    co_return ruvia::HttpResponse(context.resource());
}

ruvia::Task<void> streamRouteHandler(void*, ruvia::Context&) {
    co_return;
}

const RouteEntry& fakeRoute() {
    static RouteEntry route(
        std::pmr::get_default_resource(),
        RouteEntry::Init{
            .method = ruvia::HttpKnownMethod::kGet,
            .path = "/route",
            .endpoint = ruvia::detail::RouteEndpoint::buffered(
                ruvia::detail::RouteHandler(nullptr, &routeHandler),
                ruvia::detail::RequestBodyMode::kBuffered)});
    return route;
}

RUVIA_TEST(route_endpoint_binds_handler_shape_and_only_relevant_metadata) {
    const auto buffered = RouteEndpoint::buffered(
        RouteHandler(nullptr, &routeHandler),
        ruvia::detail::RequestBodyMode::kStream);
    RUVIA_CHECK(buffered.buffered() != nullptr);
    RUVIA_CHECK(buffered.responseStream() == nullptr);
    RUVIA_CHECK(buffered.webSocket() == nullptr);
    RUVIA_CHECK(buffered.requestBodyMode() ==
        ruvia::detail::RequestBodyMode::kStream);

    const auto stream = RouteEndpoint::responseStream(
        RouteStreamHandler(nullptr, &streamRouteHandler),
        ruvia::detail::ResponseStreamKind::kSse);
    RUVIA_CHECK(stream.buffered() == nullptr);
    RUVIA_CHECK(stream.responseStream() != nullptr);
    RUVIA_CHECK(stream.webSocket() == nullptr);
    RUVIA_CHECK(stream.responseStream()->kind() ==
        ruvia::detail::ResponseStreamKind::kSse);
    RUVIA_CHECK(stream.requestBodyMode() ==
        ruvia::detail::RequestBodyMode::kBuffered);

    std::pmr::string sourceProtocols(
        "chat, superchat", std::pmr::get_default_resource());
    ruvia::WebSocketRouteOptions options;
    options.subprotocols = sourceProtocols;
    options.lifecycle.heartbeat = ruvia::WebSocketHeartbeatPolicy::periodic(
        std::chrono::milliseconds(25));
    const auto webSocket = RouteEndpoint::webSocket(
        std::pmr::get_default_resource(),
        RouteStreamHandler(nullptr, &streamRouteHandler),
        options);
    sourceProtocols.assign("mutated");
    RUVIA_CHECK(webSocket.buffered() == nullptr);
    RUVIA_CHECK(webSocket.responseStream() == nullptr);
    RUVIA_CHECK(webSocket.webSocket() != nullptr);
    RUVIA_CHECK(webSocket.webSocket()->subprotocols() == "chat, superchat");
    RUVIA_CHECK_EQ(
        webSocket.webSocket()->lifecycle().heartbeat->pingInterval().count(),
        std::int64_t{25});
}

RUVIA_TEST(route_endpoint_rejects_empty_handlers_and_invalid_discriminants) {
    bool rejected = false;
    try {
        (void)RouteEndpoint::buffered(
            RouteHandler{}, ruvia::detail::RequestBodyMode::kBuffered);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);

    rejected = false;
    try {
        (void)RouteEndpoint::buffered(
            RouteHandler(nullptr, &routeHandler),
            static_cast<ruvia::detail::RequestBodyMode>(99));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);

    rejected = false;
    try {
        (void)RouteEndpoint::responseStream(
            RouteStreamHandler(nullptr, &streamRouteHandler),
            static_cast<ruvia::detail::ResponseStreamKind>(99));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

}  // namespace

RUVIA_TEST(route_match_add_and_values) {
    RouteMatch match;
    RUVIA_CHECK_EQ(match.size(), std::size_t{0});
    RUVIA_CHECK(match.add("alpha"));
    RUVIA_CHECK(match.add("beta"));
    RUVIA_CHECK_EQ(match.size(), std::size_t{2});
    RUVIA_CHECK_EQ(match.values().size(), std::size_t{2});
    RUVIA_CHECK_EQ(match.values()[0], std::string_view("alpha"));
    RUVIA_CHECK_EQ(match.values()[1], std::string_view("beta"));
}

RUVIA_TEST(route_match_truncate_and_clear) {
    RouteMatch match;
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
    RouteMatch match;
    for (std::size_t i = 0; i < kMaxRouteParams; ++i) {
        RUVIA_CHECK(match.add("x"));
    }
    RUVIA_CHECK_EQ(match.size(), kMaxRouteParams);
    RUVIA_CHECK(!match.add("overflow"));  // capacity reached
    RUVIA_CHECK_EQ(match.size(), kMaxRouteParams);
}

RUVIA_TEST(route_resolution_found_static) {
    const auto resolution = RouteResolution::resolved(fakeRoute());
    const auto* resolved = resolution.resolved();
    RUVIA_CHECK(resolved != nullptr);
    RUVIA_CHECK(resolution.methodNotAllowed() == nullptr);
    RUVIA_CHECK(resolution.notFound() == nullptr);
    RUVIA_CHECK(resolved->match().values().empty());
}

RUVIA_TEST(route_resolution_found_dynamic) {
    RouteMatch match;
    RUVIA_CHECK(match.add("id"));
    const auto resolution = RouteResolution::resolved(fakeRoute(), match);
    const auto* resolved = resolution.resolved();
    RUVIA_CHECK(resolved != nullptr);
    RUVIA_CHECK(&resolved->match() != &match);
    RUVIA_CHECK_EQ(resolved->match().size(), std::size_t{1});
    RUVIA_CHECK_EQ(
        resolved->match().values()[0], std::string_view("id"));
}

RUVIA_TEST(route_resolution_method_not_allowed_vs_not_found) {
    // 405: no route, but a non-zero allowed-methods mask drives the Allow header.
    const auto notAllowed = RouteResolution::methodNotAllowed(0x5);
    RUVIA_CHECK(notAllowed.resolved() == nullptr);
    RUVIA_CHECK(notAllowed.notFound() == nullptr);
    RUVIA_CHECK(notAllowed.methodNotAllowed() != nullptr);
    RUVIA_CHECK_EQ(
        notAllowed.methodNotAllowed()->allowedMethods(),
        std::uint32_t{0x5});

    // 404 is its own payload-free alternative.
    const RouteResolution notFound;
    RUVIA_CHECK(notFound.resolved() == nullptr);
    RUVIA_CHECK(notFound.methodNotAllowed() == nullptr);
    RUVIA_CHECK(notFound.notFound() != nullptr);

    // A zero Allow mask cannot materialize a fake 405 state.
    const auto zeroMask = RouteResolution::methodNotAllowed(0);
    RUVIA_CHECK(zeroMask.notFound() != nullptr);
}
