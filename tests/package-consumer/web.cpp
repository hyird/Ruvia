#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <span>
#include <string>
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
#include <ruvia/web/detail/http/ContextCapabilities.h>
#include <ruvia/web/detail/http/ContextServices.h>
#include <ruvia/web/detail/http2/Http2SansIoStreamRuntime.h>
#include <ruvia/web/detail/model/Parser.h>
#include <ruvia/web/detail/router/RouteTable.h>
#include <ruvia/web/detail/server/Http2SansIoSession.h>
#include <ruvia/web/detail/server/HttpServerAccessLog.h>

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

template <typename Services>
concept HasSplitContextCapabilityAccessors = requires(
    const Services& services) {
    services.bodyReader();
    services.bodyLoader();
    services.webSocket();
    services.responseStream();
};

template <typename Services>
concept HasLegacyContextBodyRefinement = requires(
    const Services& services,
    ruvia::BodyReader& reader,
    ruvia::detail::RequestBodyLoader& loader) {
    services.withBodyReader(reader);
    services.withBodyLoader(loader);
};

template <typename Info>
concept HasLegacyConnInfoScalarAccessors = requires(const Info& info) {
    info.secure();
    info.clientCertificateSubject();
};

template <typename Services>
concept HasBooleanTransportRefinement = requires(
    const Services& services,
    std::string_view remoteAddress,
    std::string_view certificate,
    bool secure) {
    services.withTransport(remoteAddress, certificate, secure);
};

template <typename Services>
concept AcceptsRvaluePlainTransport = requires(const Services& services) {
    services.withPlainTransport(std::string("temporary"));
};

template <typename Services>
concept AcceptsRvalueTlsAddress = requires(const Services& services) {
    services.withTlsTransport(std::string("temporary"));
};

template <typename Services>
concept AcceptsRvalueTlsCertificate = requires(const Services& services) {
    services.withTlsTransport(
        std::string_view("stable"),
        std::string("temporary"));
};

template <typename Info>
concept ExposesRvalueTransportPointer = requires {
    std::declval<const Info&&>().plain();
    std::declval<const Info&&>().tls();
};

template <typename Record>
concept HasLegacyAccessLogHttp2Flag = requires(const Record& record) {
    record.http2();
};

using RecordHttpAccessFunction = void (*)(
    const ruvia::HttpServerOptions::AccessLog&,
    const ruvia::HttpRequest&,
    std::string_view,
    std::uint16_t,
    std::chrono::steady_clock::time_point) noexcept;

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
    decltype(std::declval<const ruvia::AccessLogRecord&>()
                 .protocolVersion()),
    ruvia::HttpProtocolVersion>);
static_assert(!HasLegacyAccessLogHttp2Flag<ruvia::AccessLogRecord>);
static_assert(std::is_same_v<
    decltype(&ruvia::detail::recordHttpAccess),
    RecordHttpAccessFunction>);
static_assert(std::is_nothrow_copy_constructible_v<
    ruvia::AccessLogRecord>);
static_assert(!std::is_copy_assignable_v<ruvia::AccessLogRecord>);
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
static_assert(!HasSplitContextCapabilityAccessors<
    ruvia::detail::ContextServices>);
static_assert(!HasLegacyContextBodyRefinement<
    ruvia::detail::ContextServices>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::detail::ContextServices&>()
                 .requestBodySource()),
    const ruvia::detail::ContextRequestBodySource&>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::detail::ContextServices&>()
                 .responseOutput()),
    const ruvia::detail::ContextResponseOutput&>);
static_assert(!std::is_default_constructible_v<
    ruvia::detail::ContextLazyRequestBodySource>);
static_assert(!std::is_default_constructible_v<
    ruvia::detail::ContextStreamingRequestBodySource>);
static_assert(!std::is_default_constructible_v<
    ruvia::detail::ContextResponseStreamOutput>);
static_assert(!std::is_default_constructible_v<
    ruvia::detail::ContextWebSocketOutput>);
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
    const std::atomic_bool&>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::ConnInfo&>().plain()),
    const ruvia::PlainConnectionTransport*>);
static_assert(std::is_same_v<
    decltype(std::declval<const ruvia::ConnInfo&>().tls()),
    const ruvia::TlsConnectionTransport*>);
static_assert(!HasLegacyConnInfoScalarAccessors<ruvia::ConnInfo>);
static_assert(!HasBooleanTransportRefinement<
    ruvia::detail::ContextServices>);
static_assert(!AcceptsRvaluePlainTransport<
    ruvia::detail::ContextServices>);
static_assert(!AcceptsRvalueTlsAddress<
    ruvia::detail::ContextServices>);
static_assert(!AcceptsRvalueTlsCertificate<
    ruvia::detail::ContextServices>);
static_assert(!ExposesRvalueTransportPointer<ruvia::ConnInfo>);
static_assert(!std::is_default_constructible_v<
    ruvia::PlainConnectionTransport>);
static_assert(!std::is_default_constructible_v<
    ruvia::TlsConnectionTransport>);
static_assert(!std::is_default_constructible_v<ruvia::ConnInfo>);
static_assert(std::is_nothrow_copy_constructible_v<ruvia::ConnInfo>);
static_assert(std::is_nothrow_move_constructible_v<ruvia::ConnInfo>);
static_assert(std::is_nothrow_copy_assignable_v<ruvia::ConnInfo>);
static_assert(std::is_nothrow_move_assignable_v<ruvia::ConnInfo>);
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
    const ruvia::detail::ContextServices contextServices;
    if (contextServices.requestBodySource().buffered() == nullptr ||
        contextServices.requestBodySource().lazy() != nullptr ||
        contextServices.requestBodySource().streaming() != nullptr ||
        contextServices.responseOutput().buffered() == nullptr ||
        contextServices.responseOutput().responseStream() != nullptr ||
        contextServices.responseOutput().webSocket() != nullptr ||
        contextServices.connInfo().plain() == nullptr ||
        contextServices.connInfo().tls() != nullptr) {
        return 8;
    }
    const auto tlsServices = contextServices.withTlsTransport(
        "198.51.100.9",
        "CN=package-client");
    const auto* tls = tlsServices.connInfo().tls();
    if (tlsServices.connInfo().plain() != nullptr ||
        tls == nullptr ||
        tlsServices.connInfo().remote().address() != "198.51.100.9" ||
        tls->clientCertificateSubject() != "CN=package-client") {
        return 9;
    }
    ruvia::app().setHttpListenPort(8080);
    return 0;
}
