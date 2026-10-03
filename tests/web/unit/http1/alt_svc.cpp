#include <array>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/use_future.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/http/Http1ServerRequestParser.h"
#include "ruvia/http/Http1ServerSemantics.h"
#include "ruvia/http/HttpByteRange.h"
#include "ruvia/http/HttpResponseServer.h"
#include "ruvia/http/http_multipart_byte_range_plan.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/server/http1/Http1RequestSequence.h"
#include "ruvia/web/detail/server/response/HttpResponseWriter.h"
#include "ruvia/web/detail/server/route/Http1RouteDispatch.h"
#include "ruvia/web/detail/server/route/HttpServerWebSocketRoute.h"
#include "ruvia/web/detail/server/stream/HttpServerResponseStreamRoute.h"
#include "ruvia/web/detail/websocket/WebSocketResponseHeaders.h"

#include "context_services_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

using asio::ip::tcp;

constexpr std::string_view kAutomaticAltSvc = "h3=\":443\"; ma=86400";
constexpr std::string_view kMaskedPeerCloseFrame{"\x88\x82\x11\x22\x33\x44\x12\xCA", 8};
constexpr std::string_view kIdentityRequest =
    "GET /buffered HTTP/1.1\r\nHost: example.test\r\nAccept-Encoding: identity\r\n\r\n";

ruvia::Task<ruvia::HttpResponse> altSvcBufferedHandler(void*, ruvia::Context& context) {
    co_return context.text("buffered");
}

ruvia::Task<ruvia::HttpResponse> altSvcOverrideHandler(void*, ruvia::Context& context) {
    context.header("Alt-Svc", "h3=\":9443\"; ma=10");
    co_return context.text("overridden");
}

ruvia::Task<ruvia::HttpResponse> altSvcEraseHandler(void*, ruvia::Context& context) {
    context.removeHeader("Alt-Svc");
    co_return context.text("removed");
}

ruvia::Task<ruvia::HttpResponse> altSvcExceptionHandler(void*, ruvia::Context&) {
    throw std::runtime_error("test exception");
    co_return ruvia::HttpResponse{};
}

ruvia::Task<void> altSvcStreamHandler(void*, ruvia::Context& context) {
    auto& stream = context.streamText();
    co_await stream.write("streamed");
    co_await stream.end();
}

ruvia::Task<void> webSocketDrainHandler(void*, ruvia::Context&) {
    co_return;
}

void registerAltSvcRoutes(ruvia::detail::RouterImpl& impl) {
    const auto noMiddlewares = std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{};
    impl.registerRoute(ruvia::HttpKnownMethod::kGet,
        std::pmr::string("/buffered", std::pmr::get_default_resource()),
        ruvia::detail::RouteHandler(nullptr, &altSvcBufferedHandler),
        ruvia::detail::RequestBodyMode::kBuffered, noMiddlewares, noMiddlewares);
    impl.registerRoute(ruvia::HttpKnownMethod::kGet,
        std::pmr::string("/override", std::pmr::get_default_resource()),
        ruvia::detail::RouteHandler(nullptr, &altSvcOverrideHandler),
        ruvia::detail::RequestBodyMode::kBuffered, noMiddlewares, noMiddlewares);
    impl.registerRoute(ruvia::HttpKnownMethod::kGet,
        std::pmr::string("/erase", std::pmr::get_default_resource()),
        ruvia::detail::RouteHandler(nullptr, &altSvcEraseHandler),
        ruvia::detail::RequestBodyMode::kBuffered, noMiddlewares, noMiddlewares);
    impl.registerRoute(ruvia::HttpKnownMethod::kGet,
        std::pmr::string("/exception", std::pmr::get_default_resource()),
        ruvia::detail::RouteHandler(nullptr, &altSvcExceptionHandler),
        ruvia::detail::RequestBodyMode::kBuffered, noMiddlewares, noMiddlewares);
    impl.registerResponseStreamRoute(ruvia::HttpKnownMethod::kGet,
        std::pmr::string("/stream", std::pmr::get_default_resource()),
        ruvia::detail::RouteStreamHandler(nullptr, &altSvcStreamHandler),
        noMiddlewares, noMiddlewares);
    impl.registerWebSocketRoute(ruvia::HttpKnownMethod::kGet,
        std::pmr::string("/ws", std::pmr::get_default_resource()),
        ruvia::detail::RouteStreamHandler(nullptr, &webSocketDrainHandler),
        noMiddlewares, noMiddlewares);
}

template <typename Emit>
std::string captureHttp1Wire(Emit emit) {
    auto& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const auto endpoint = acceptor.local_endpoint();
    std::string bytes;

    auto server = asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            socket.non_blocking(true);
            co_await ruvia::asAwaitable(emit(socket));
            std::error_code ignored;
            socket.shutdown(tcp::socket::shutdown_send, ignored);
        },
        asio::use_future);
    auto client = asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket socket(io);
            co_await socket.async_connect(endpoint, asio::use_awaitable);
            std::array<char, 2048> buffer{};
            for (;;) {
                const auto [error, count] = co_await socket.async_read_some(
                    asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));
                bytes.append(buffer.data(), count);
                if (error == asio::error::eof) {
                    break;
                }
                if (error) {
                    throw std::system_error(error);
                }
            }
        },
        asio::use_future);

    io.run();
    server.get();
    client.get();
    return bytes;
}

void parseHttp1Request(std::string_view wire, ruvia::Http1ServerRequestParseState& parsed,
    ruvia::RequestMemory& memory) {
    ruvia::Http1ServerRequestParser parser;
    parser.parseHead(wire, parsed, 0, memory.resource());
    if (parsed.headReady() == nullptr) {
        throw std::runtime_error("invalid HTTP/1 request in Alt-Svc test");
    }
}

ruvia::Task<void> writeBufferedResponse(tcp::socket& socket, ruvia::WorkerMemory& worker,
    const ruvia::HttpRequest& request, ruvia::HttpResponse& response,
    ruvia::Http1RequestConnectionPlan connectionPlan) {
    ruvia::HttpResponseHeadBuffer head(worker.allocator<char>());
    std::pmr::string fileChunk(worker.allocator<char>());
    const auto writePlan = ruvia::http1BufferedResponsePlan(
        ruvia::planBufferedHttpResponseWrite(request.knownMethod(), response), connectionPlan);
    const auto result = co_await ruvia::detail::writeResponse(
        socket, worker, &head, &fileChunk, response, writePlan);
    if (result.completed() == nullptr) {
        throw std::runtime_error("HTTP/1 test response write failed");
    }
}

ruvia::detail::ContextServices tlsServices() {
    return ruvia::test::testContextServices()
        .withTlsTransport("127.0.0.1")
        .withAutomaticAltSvc(kAutomaticAltSvc);
}

ruvia::Task<void> emitBufferedRoute(tcp::socket& socket, const ruvia::detail::RouteTable& routes,
    std::string_view path, bool tls) {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    std::string requestWire("GET ");
    requestWire.append(path).append(
        " HTTP/1.1\r\nHost: example.test\r\n"
        "Accept-Encoding: identity\r\n\r\n");
    ruvia::Http1ServerRequestParseState parsed;
    parseHttp1Request(requestWire, parsed, memory);
    const auto services = tls ? tlsServices()
                              : ruvia::test::testContextServices().withPlainTransport("127.0.0.1");
    const auto resolution = routes.resolve(parsed.request);
    auto response = co_await routes.dispatchBufferedResponse(parsed.request, resolution, memory,
        ruvia::detail::DocumentRootBinding::none(), services);
    const auto connectionPlan = ruvia::detail::requireHttp1FinalResponseCommit(
        response, parsed.connectionPlan);
    co_await writeBufferedResponse(socket, worker, parsed.request, response, connectionPlan);
}

ruvia::Task<void> emitProtocolError(tcp::socket& socket, const ruvia::detail::RouteTable& routes) {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::Http1ServerRequestParseState parsed;
    parseHttp1Request(kIdentityRequest, parsed, memory);
    auto response = co_await routes.handleError(parsed.request, memory,
        ruvia::HttpErrorInfo({.status = ruvia::http_status::kBadRequest,
            .message = "invalid protocol request"}),
        tlsServices());
    const auto connectionPlan = ruvia::detail::requireHttp1FinalResponseCommit(
        response, parsed.connectionPlan);
    co_await writeBufferedResponse(socket, worker, parsed.request, response, connectionPlan);
}

ruvia::Task<void> emitStreamingRoute(tcp::socket& socket, const ruvia::detail::RouteTable& routes) {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    constexpr std::string_view requestWire =
        "GET /stream HTTP/1.1\r\nHost: example.test\r\nAccept-Encoding: identity\r\n\r\n";
    ruvia::Http1ServerRequestParseState parsed;
    parseHttp1Request(requestWire, parsed, memory);
    auto resolution = routes.resolve(parsed.request);
    const auto* resolved = resolution.resolved();
    const auto* requestHead = parsed.headReady();
    if (resolved == nullptr || requestHead == nullptr) {
        throw std::runtime_error("HTTP/1 streaming route did not resolve");
    }
    auto coding = parsed.responseCodingSelection();
    if (coding.selected() == nullptr) {
        throw std::runtime_error("HTTP/1 test request has no response coding");
    }
    ruvia::ConnectionScanner::Entry scannerEntry;
    ruvia::detail::HttpServerOptions options;
    ruvia::detail::Http1RequestSequence requestSequence(std::nullopt);
    ruvia::HttpResponse response({.resource = memory.resource()});
    ruvia::HttpResponseHeadBuffer responseHead(worker.allocator<char>());
    ruvia::detail::Http1RouteDispatch<tcp::socket> dispatch{.stream = socket,
        .memory = worker,
        .scannerEntry = scannerEntry,
        .parsed = parsed,
        .responseCoding = *coding.selected(),
        .responseCodingAvailability = ruvia::detail::HttpResponseCodingAvailability::kIdentityOnly,
        .routes = routes,
        .requestMemory = memory,
        .baseRouteServices = tlsServices(),
        .options = options,
        .response = response,
        .requestSequence = requestSequence};
    static_cast<void>(co_await ruvia::detail::dispatchHttpResponseStreamRoute(
        dispatch, responseHead, *requestHead, *resolved, {}, ruvia::ProtocolByteLimit::limited(ruvia::kDefaultMaxBufferedBodyBytes)));
}

ruvia::Task<void> emitWebSocketRoute(tcp::socket& socket,
    const ruvia::detail::RouteTable& routes, std::string_view version) {
    auto attachment = ruvia::attachEventLoop(
        static_cast<asio::io_context&>(socket.get_executor().context()), {.mailboxCapacity = 64});
    const auto workerHandle = attachment.loop().handle();
    ruvia::StopToken stopToken;
    const auto services = ruvia::detail::ContextServices(workerHandle, stopToken)
                              .withTlsTransport("127.0.0.1")
                              .withAutomaticAltSvc(kAutomaticAltSvc);
    const std::string pendingFrames(kMaskedPeerCloseFrame);
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    std::string requestWire =
        "GET /ws HTTP/1.1\r\nHost: example.test\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: ";
    requestWire.append(version).append("\r\nAccept-Encoding: identity\r\n\r\n");
    ruvia::Http1ServerRequestParseState parsed;
    parseHttp1Request(requestWire, parsed, memory);
    auto resolution = routes.resolve(parsed.request);
    const auto* resolved = resolution.resolved();
    if (resolved == nullptr) {
        throw std::runtime_error("HTTP/1 WebSocket route did not resolve");
    }
    auto coding = parsed.responseCodingSelection();
    if (coding.selected() == nullptr) {
        throw std::runtime_error("HTTP/1 WebSocket test request has no response coding");
    }
    ruvia::ConnectionScanner::Entry scannerEntry;
    ruvia::detail::HttpServerOptions options;
    ruvia::detail::Http1RequestSequence requestSequence(std::nullopt);
    ruvia::HttpResponse response({.resource = memory.resource()});
    ruvia::detail::Http1RouteDispatch<tcp::socket> dispatch{.stream = socket,
        .memory = worker,
        .scannerEntry = scannerEntry,
        .parsed = parsed,
        .responseCoding = *coding.selected(),
        .responseCodingAvailability = ruvia::detail::HttpResponseCodingAvailability::kIdentityOnly,
        .routes = routes,
        .requestMemory = memory,
        .baseRouteServices = services,
        .options = options,
        .response = response,
        .requestSequence = requestSequence};
    const auto completion = co_await ruvia::detail::dispatchHttpWebSocketRoute(
        dispatch, *resolved, pendingFrames);
    if (version == "13") {
        if (completion.has_value()) {
            throw std::runtime_error("valid HTTP/1 WebSocket request was not upgraded");
        }
    } else {
        if (!completion.has_value() || completion->bufferedResponse() == nullptr) {
            throw std::runtime_error("unsupported HTTP/1 WebSocket version was not rejected");
        }
        co_await writeBufferedResponse(
            socket, worker, parsed.request, response, completion->connectionPlan());
    }
}

std::string_view firstResponseHead(std::string_view wire) {
    const auto end = wire.find("\r\n\r\n");
    return end == std::string_view::npos ? wire : wire.substr(0, end + 4);
}

}  // namespace

RUVIA_TEST(http1_tls_wire_responses_emit_alt_svc_and_honor_application_headers) {
    ruvia::detail::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    registerAltSvcRoutes(impl);
    impl.finalize();
    const auto& routes = impl.routeTable();

    const auto buffered = captureHttp1Wire([&](tcp::socket& socket) {
        return emitBufferedRoute(socket, routes, "/buffered", true);
    });
    RUVIA_CHECK(firstResponseHead(buffered).contains("Alt-Svc: h3=\":443\"; ma=86400\r\n"));
    RUVIA_CHECK(firstResponseHead(buffered).starts_with("HTTP/1.1 200"));

    const auto override = captureHttp1Wire([&](tcp::socket& socket) {
        return emitBufferedRoute(socket, routes, "/override", true);
    });
    RUVIA_CHECK(firstResponseHead(override).contains("Alt-Svc: h3=\":9443\"; ma=10\r\n"));
    RUVIA_CHECK(!firstResponseHead(override).contains(kAutomaticAltSvc));

    const auto erased = captureHttp1Wire([&](tcp::socket& socket) {
        return emitBufferedRoute(socket, routes, "/erase", true);
    });
    RUVIA_CHECK(!firstResponseHead(erased).contains("Alt-Svc:"));

    const auto plain = captureHttp1Wire([&](tcp::socket& socket) {
        return emitBufferedRoute(socket, routes, "/buffered", false);
    });
    RUVIA_CHECK(!firstResponseHead(plain).contains("Alt-Svc:"));

    const auto protocolError = captureHttp1Wire([&](tcp::socket& socket) {
        return emitProtocolError(socket, routes);
    });
    RUVIA_CHECK(firstResponseHead(protocolError).starts_with("HTTP/1.1 400"));
    RUVIA_CHECK(firstResponseHead(protocolError).contains("Alt-Svc: h3=\":443\"; ma=86400\r\n"));

    const auto notFound = captureHttp1Wire([&](tcp::socket& socket) {
        return emitBufferedRoute(socket, routes, "/missing", true);
    });
    RUVIA_CHECK(firstResponseHead(notFound).starts_with("HTTP/1.1 404"));
    RUVIA_CHECK(firstResponseHead(notFound).contains("Alt-Svc: h3=\":443\"; ma=86400\r\n"));

    const auto exception = captureHttp1Wire([&](tcp::socket& socket) {
        return emitBufferedRoute(socket, routes, "/exception", true);
    });
    RUVIA_CHECK(firstResponseHead(exception).starts_with("HTTP/1.1 500"));
    RUVIA_CHECK(firstResponseHead(exception).contains("Alt-Svc: h3=\":443\"; ma=86400\r\n"));
}

RUVIA_TEST(http1_tls_streaming_and_websocket_wire_responses_emit_alt_svc) {
    ruvia::detail::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    registerAltSvcRoutes(impl);
    impl.finalize();
    const auto& routes = impl.routeTable();

    const auto streaming = captureHttp1Wire([&](tcp::socket& socket) {
        return emitStreamingRoute(socket, routes);
    });
    RUVIA_CHECK(firstResponseHead(streaming).starts_with("HTTP/1.1 200"));
    RUVIA_CHECK(firstResponseHead(streaming).contains("Alt-Svc: h3=\":443\"; ma=86400\r\n"));
    RUVIA_CHECK(firstResponseHead(streaming).contains("Transfer-Encoding: chunked\r\n"));
    RUVIA_CHECK(streaming.contains("streamed"));

    const auto webSocket = captureHttp1Wire([&](tcp::socket& socket) {
        return emitWebSocketRoute(socket, routes, "13");
    });
    const auto handshake = firstResponseHead(webSocket);
    RUVIA_CHECK(handshake.starts_with("HTTP/1.1 101"));
    RUVIA_CHECK(handshake.contains("alt-svc: h3=\":443\"; ma=86400\r\n"));
}

RUVIA_TEST(http1BufferedResponseWriterSendsMultipartFileSlicesWithExactLength) {
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() / "ruvia-http1-multipart-writer.bin";
    const std::string content(50000, 'h');
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << content;
    }

    const auto wire = captureHttp1Wire([&](tcp::socket& socket) -> ruvia::Task<void> {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::Http1ServerRequestParseState parsed;
        constexpr std::string_view requestWire =
            "GET /multipart HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n";
        parseHttp1Request(requestWire, parsed, memory);
        const auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-19999,30000-49999", content.size());
        auto multipart = ruvia::make_http_multipart_byte_range_plan(
            ranges, content.size(), "text/plain", "h1_writer_boundary", {}, memory.resource());
        ruvia::HttpResponse response({.resource = memory.resource()});
        response.status(ruvia::http_status::kPartialContent);
        response.header("Content-Type", multipart.content_type());
        response.multipart_file_body(path, content.size(),
            ruvia::HttpResponseFileIdentity::unchecked(), std::move(multipart));
        const auto connectionPlan = ruvia::detail::requireHttp1FinalResponseCommit(
            response, parsed.connectionPlan);
        co_await writeBufferedResponse(socket, worker, parsed.request, response, connectionPlan);
    });

    const auto head = firstResponseHead(wire);
    RUVIA_CHECK(head.starts_with("HTTP/1.1 206"));
    const auto lengthStart = head.find("Content-Length: ");
    RUVIA_CHECK(lengthStart != std::string_view::npos);
    const auto lengthEnd = head.find("\r\n", lengthStart);
    const auto body = wire.substr(head.size());
    const std::string expected =
        "--h1_writer_boundary\r\nContent-Type: text/plain\r\nContent-Range: bytes 0-19999/50000\r\n\r\n" +
        content.substr(0, 20000) + "\r\n--h1_writer_boundary\r\nContent-Type: text/plain\r\n" +
        "Content-Range: bytes 30000-49999/50000\r\n\r\n" + content.substr(30000) +
        "\r\n--h1_writer_boundary--\r\n";
    RUVIA_CHECK_EQ(body, expected);
    if (lengthStart != std::string_view::npos && lengthEnd != std::string_view::npos) {
        const auto declared = head.substr(lengthStart + 16, lengthEnd - lengthStart - 16);
        RUVIA_CHECK_EQ(declared, std::to_string(expected.size()));
    }

    bool identityFailedAfterCommit = false;
    const auto identityFailureWire = captureHttp1Wire([&](tcp::socket& socket) -> ruvia::Task<void> {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::Http1ServerRequestParseState parsed;
        constexpr std::string_view requestWire =
            "GET /multipart HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n";
        parseHttp1Request(requestWire, parsed, memory);
        const auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-1,10-11", content.size());
        auto multipart = ruvia::make_http_multipart_byte_range_plan(
            ranges, content.size(), "text/plain", "h1_identity_boundary", {}, memory.resource());
        ruvia::HttpResponse response({.resource = memory.resource()});
        response.status(ruvia::http_status::kPartialContent);
        response.header("Content-Type", multipart.content_type());
        response.multipart_file_body(path, content.size(),
            ruvia::HttpResponseFileIdentity::checked({}), std::move(multipart));
        const auto writePlan = ruvia::http1BufferedResponsePlan(
            ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response),
            parsed.connectionPlan);
        ruvia::HttpResponseHeadBuffer responseHead(worker.allocator<char>());
        std::pmr::string fileChunk(worker.allocator<char>());
        const auto result = co_await ruvia::detail::writeResponse(
            socket, worker, &responseHead, &fileChunk, response, writePlan);
        identityFailedAfterCommit = result.failedAfterCommit() != nullptr;
    });
    RUVIA_CHECK(identityFailedAfterCommit);
    RUVIA_CHECK(firstResponseHead(identityFailureWire).starts_with("HTTP/1.1 206"));
    const auto failedHead = firstResponseHead(identityFailureWire);
    RUVIA_CHECK(identityFailureWire.substr(failedHead.size()).find("hh") == std::string_view::npos);
    fs::remove(path);
}

RUVIA_TEST(http1_tls_unsupported_websocket_version_wire_is_400_without_upgrade_fields) {
    ruvia::detail::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    registerAltSvcRoutes(impl);
    impl.finalize();
    const auto& routes = impl.routeTable();

    const auto wire = captureHttp1Wire([&](tcp::socket& socket) {
        return emitWebSocketRoute(socket, routes, "12");
    });
    const auto head = firstResponseHead(wire);
    RUVIA_CHECK(head.starts_with("HTTP/1.1 400"));
    RUVIA_CHECK(head.contains("Sec-WebSocket-Version: 13\r\n"));
    RUVIA_CHECK(head.contains("Alt-Svc: h3=\":443\"; ma=86400\r\n"));
    RUVIA_CHECK(!head.contains("Upgrade:"));
    RUVIA_CHECK(!head.contains("Connection: Upgrade"));
}
