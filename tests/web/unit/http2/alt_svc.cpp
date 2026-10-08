#include <array>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

#include <asio/io_context.hpp>

#include "ruvia/http/Hpack.h"
#include "ruvia/http/Http2Framing.h"
#include "ruvia/http/Http2Types.h"

#include "context/ContextServices.h"
#include "http2/Http2SansIoSession.h"
#include "http2_sansio_session_fixture.h"
#include "router/Router.h"
#include "router/RouterImpl.h"
#include "sansio_driver_fixture.h"
#include "test_io_context.h"

namespace {

constexpr std::string_view kAutomaticAltSvc = "h3=\":443\"; ma=86400";

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

struct ResponseHeaders final {
    std::string status;
    std::string altSvc;
    bool hasAltSvc{false};
    bool hasConnection{false};
    bool hasUpgrade{false};
    bool hasWebSocketAccept{false};
};

}  // namespace

RUVIA_TEST(sansio_driver_h2_emits_alt_svc_on_buffered_streaming_and_error_responses) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::map<std::uint32_t, ResponseHeaders> responses;
    std::map<std::uint32_t, std::string> bodies;
    bool hpackDecodeSucceeded = true;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
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
            impl.finalize();
            co_await ruvia::asAwaitable(ruvia::test::runBareHttp2SansIoSessionWith(
                sock, impl.routeTable(), worker,
                [](ruvia::detail::ContextServices services) {
                    return services.withTlsTransport("127.0.0.1")
                        .withAutomaticAltSvc(kAutomaticAltSvc);
                },
                std::string_view{}));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                const auto [ec, count] = co_await asio::async_write(sock,
                    asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                co_return !ec && count == bytes.size();
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                const auto [ec, count] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && count == size;
            };
            auto requestOn = [](std::uint32_t streamId, std::string_view path) {
                std::pmr::string block(std::pmr::get_default_resource());
                ruvia::HpackEncoder::encodeHeader(block, ":method", "GET");
                ruvia::HpackEncoder::encodeHeader(block, ":path", path);
                ruvia::HpackEncoder::encodeHeader(block, ":scheme", "https");
                ruvia::HpackEncoder::encodeHeader(block, ":authority", "localhost");
                return frame(0x1,
                    sansio_driver_test::kFlagEndStream | sansio_driver_test::kFlagEndHeaders,
                    streamId, std::string_view(block.data(), block.size()));
            };

            if (!co_await writeAll(kClientPreface) ||
                !co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) {
                co_return;
            }
            std::string requests;
            for (const auto& [streamId, path] : std::array{
                     std::pair{1U, std::string_view("/buffered")},
                     std::pair{3U, std::string_view("/override")},
                     std::pair{5U, std::string_view("/erase")},
                     std::pair{7U, std::string_view("/exception")},
                     std::pair{9U, std::string_view("/missing")},
                     std::pair{11U, std::string_view("/stream")}}) {
                requests += requestOn(streamId, path);
            }
            if (!co_await writeAll(requests)) {
                co_return;
            }

            ruvia::HpackDecoder decoder({.resource = std::pmr::get_default_resource()});
            std::set<std::uint32_t> pending{1, 3, 5, 7, 9, 11};
            while (!pending.empty()) {
                char headerBytes[ruvia::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) {
                    break;
                }
                const auto header = sansio_driver_test::parseFrameHeader(
                    std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) {
                    break;
                }
                if (header.type == static_cast<std::uint8_t>(ruvia::Http2FrameType::kHeaders) &&
                    pending.contains(header.streamId)) {
                    auto& fields = responses[header.streamId];
                    const auto decoded = decoder.decode(payload,
                        [&fields](std::string_view name, std::string_view value) {
                            if (name == ":status") {
                                fields.status.assign(value);
                            } else if (name == "alt-svc") {
                                fields.hasAltSvc = true;
                                fields.altSvc.assign(value);
                            } else if (name == "connection") {
                                fields.hasConnection = true;
                            } else if (name == "upgrade") {
                                fields.hasUpgrade = true;
                            } else if (name == "sec-websocket-accept") {
                                fields.hasWebSocketAccept = true;
                            }
                            return true;
                        });
                    hpackDecodeSucceeded = hpackDecodeSucceeded && decoded.decoded();
                } else if (header.type == static_cast<std::uint8_t>(ruvia::Http2FrameType::kData) &&
                           pending.contains(header.streamId)) {
                    bodies[header.streamId].append(payload);
                }
                if (pending.contains(header.streamId) &&
                    (header.flags & sansio_driver_test::kFlagEndStream) != 0) {
                    pending.erase(header.streamId);
                }
            }
            sansio_driver_test::closeClientSocket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(hpackDecodeSucceeded);
    for (const auto streamId : {1U, 7U, 9U, 11U}) {
        const auto& fields = responses[streamId];
        RUVIA_CHECK(fields.hasAltSvc);
        RUVIA_CHECK_EQ(fields.altSvc, std::string(kAutomaticAltSvc));
        RUVIA_CHECK(!fields.hasConnection);
        RUVIA_CHECK(!fields.hasUpgrade);
        RUVIA_CHECK(!fields.hasWebSocketAccept);
    }
    RUVIA_CHECK_EQ(responses[1].status, std::string("200"));
    RUVIA_CHECK_EQ(responses[7].status, std::string("500"));
    RUVIA_CHECK_EQ(responses[9].status, std::string("404"));
    RUVIA_CHECK_EQ(responses[11].status, std::string("200"));
    RUVIA_CHECK_EQ(responses[3].altSvc, std::string("h3=\":9443\"; ma=10"));
    RUVIA_CHECK_EQ(responses[5].status, std::string("200"));
    RUVIA_CHECK(!responses[5].hasAltSvc);
    RUVIA_CHECK_EQ(bodies[11], std::string("streamed"));
}
