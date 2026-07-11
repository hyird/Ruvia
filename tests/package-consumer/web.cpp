#include <chrono>
#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

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

#ifdef RUVIA_ENABLE_JWT
#include <ruvia/web/auth/Jwt.h>
#endif
#ifdef RUVIA_ENABLE_MARIADB
#include <ruvia/web/db/Db.h>
#endif
#ifdef RUVIA_ENABLE_REDIS
#include <ruvia/web/redis/Redis.h>
#endif

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
    ruvia::detail::Http2RequestBodyRuntime body;
    if (!body.selectMode(ruvia::detail::RequestBodyMode::kStream) ||
        body.store("web-owned", 0, 1024) !=
            ruvia::detail::Http2RequestBodyStoreResult::kAccepted ||
        body.queue().pop() != "web-owned") {
        return 4;
    }
    ruvia::app().setHttpListenPort(8080);
    return 0;
}
