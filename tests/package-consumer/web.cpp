#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include <asio/io_context.hpp>

#include <ruvia/web/App.h>
#include <ruvia/web/AppHook.h>
#include <ruvia/web/ConnInfo.h>
#include <ruvia/web/Controller.h>
#include <ruvia/web/Error.h>
#include <ruvia/web/HttpServerOptions.h>
#include <ruvia/web/MiddlewareRuntime.h>
#include <ruvia/web/Model.h>
#include <ruvia/web/RouteModes.h>
#include <ruvia/web/Streaming.h>
#include <ruvia/web/WebSocket.h>
#include <ruvia/web/detail/ContextValues.h>
#include <ruvia/web/detail/ValidatedValues.h>
#include <ruvia/web/detail/http2/Http2SansIoStreamRuntime.h>
#include <ruvia/web/detail/model/Parser.h>
#include <ruvia/web/detail/router/RouteTable.h>
#include <ruvia/web/detail/server/Http2SansIoSession.h>

#ifdef RUVIA_ENABLE_JWT
#include <ruvia/web/auth/Jwt.h>
#endif
#ifdef RUVIA_ENABLE_MARIADB
#include <ruvia/web/db/Db.h>
#endif
#ifdef RUVIA_ENABLE_REDIS
#include <ruvia/web/redis/Redis.h>
#endif

template <typename Runtime, typename Executor>
concept HasDirectHttp2BeginDispatch = requires(
    Runtime& runtime,
    Executor executor) {
    runtime.beginDispatch(executor);
};

template <typename Body>
concept HasDirectHttp2BodyModeSelection = requires(Body& body) {
    body.selectMode(ruvia::detail::RequestBodyMode::kBuffered);
};

template <typename Resolution>
concept HasLooseRouteResolutionAccessors = requires(
    const Resolution& resolution) {
    resolution.found();
    resolution.route();
    resolution.match();
    resolution.allowedMethods();
};

static_assert(std::is_same_v<
    decltype(std::declval<ruvia::ResponseStreamWriter&>().end(
        std::declval<std::span<const ruvia::HttpHeaderView>>())),
    ruvia::Task<void>>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::ContextRequest&>().method()),
    std::string_view>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::ContextRequest&>().knownMethod()),
    ruvia::HttpKnownMethod>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::AccessLogRecord&>().method()),
    std::string_view>);
static_assert(std::is_same_v<
    decltype(std::declval<ruvia::detail::Http2RequestBodyRuntime&>().store(
        std::declval<std::string_view>(),
        std::size_t{},
        std::size_t{})),
    ruvia::detail::Http2RequestBodyStoreResult>);
static_assert(!HasDirectHttp2BeginDispatch<
    ruvia::detail::Http2SansIoStreamRuntime,
    asio::io_context::executor_type>);
static_assert(!HasDirectHttp2BodyModeSelection<
    ruvia::detail::Http2RequestBodyRuntime>);
static_assert(!HasLooseRouteResolutionAccessors<
    ruvia::detail::RouteResolution>);
static_assert(!std::is_default_constructible_v<
    ruvia::detail::RouteEndpoint>);
static_assert(!std::is_polymorphic_v<ruvia::detail::RouteTable>);
static_assert(!std::is_default_constructible_v<
    ruvia::detail::Http2SansIoSessionContext>);
static_assert(std::is_nothrow_constructible_v<
    ruvia::detail::Http2SansIoSessionContext,
    ruvia::detail::ContextServices,
    const ruvia::HttpServerOptions&,
    ruvia::detail::ConnectionScanner::Entry&,
    const std::atomic_bool&,
    std::string_view,
    std::string_view>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::detail::RouteResolution&>().resolved()),
    const ruvia::detail::ResolvedRoute*>);

std::string_view peerAddress(const ruvia::Context& context) {
    return ruvia::getConnInfo(context).remote().address();
}

int main() {
    const ruvia::HttpErrorInfo error(500);
    if (error.status() != 500) {
        return 2;
    }
    const ruvia::WebSocketRouteOptions webSocketOptions;
    if (webSocketOptions.lifecycle.closeHandshakeTimeout != std::chrono::seconds(5)) {
        return 3;
    }
    ruvia::detail::Http2SansIoStreamRuntime standaloneRuntime(
        3, std::pmr::get_default_resource());
    if (!standaloneRuntime.selectRoute(
            ruvia::detail::RouteResolution{},
            ruvia::detail::RequestBodyMode::kStream)) {
        return 4;
    }
    auto& body = standaloneRuntime.body();
    if (body.selectedMode() == nullptr ||
        *body.selectedMode() != ruvia::detail::RequestBodyMode::kStream ||
        body.store("web-owned", 0, 1024) !=
            ruvia::detail::Http2RequestBodyStoreResult::kAccepted ||
        body.queue().pop() != "web-owned") {
        return 4;
    }
    asio::io_context io;
    ruvia::detail::Http2SansIoStreamRuntimeTable runtimes(
        std::pmr::get_default_resource());
    auto* runtime = runtimes.ensure(1);
    if (runtime == nullptr ||
        !runtime->selectRoute(
            ruvia::detail::RouteResolution{},
            ruvia::detail::RequestBodyMode::kBuffered)) {
        return 5;
    }
    auto* signal = runtimes.beginDispatch(1, io.get_executor());
    if (signal == nullptr || runtimes.dispatchedCount() != 1) {
        return 6;
    }
    signal->end();
    if (!signal->ended() || !runtimes.remove(1) ||
        runtimes.dispatchedCount() != 0) {
        return 7;
    }
    ruvia::app().setHttpListenPort(8080);
    return 0;
}
