#include <string>

#include <asio/io_context.hpp>

#include "ruvia/http/Hpack.h"
#include "ruvia/http/Http2Framing.h"
#include "ruvia/http/Http2Types.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/web/detail/http2/Http2SansIoSession.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/detail/router/RouterImpl.h"

#include "http2_sansio_session_fixture.h"
#include "sansio_driver_fixture.h"
#include "test_io_context.h"

RUVIA_TEST(sansio_driver_h2_unsupported_websocket_version_wire_is_bad_request) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool gotResponseHead = false;
    bool hpackDecodeSucceeded = true;
    bool gotBadRequest = false;
    bool gotSupportedVersion = false;
    bool gotAltSvc = false;
    bool sawForbiddenField = false;
    bool gotResponseEnd = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerWebSocketRoute(ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/ws", std::pmr::get_default_resource()),
                ruvia::detail::RouteStreamHandler(nullptr, &wsServerCloseHandler),
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::asAwaitable(ruvia::test::runBareHttp2SansIoSessionWith(
                sock, impl.routeTable(), worker,
                [](ruvia::detail::ContextServices services) {
                    return services.withTlsTransport("127.0.0.1")
                        .withAutomaticAltSvc("h3=\":443\"; ma=86400");
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
            if (!co_await writeAll(kClientPreface) ||
                !co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) {
                co_return;
            }
            std::pmr::string block(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(block, ":method", "CONNECT");
            HpackEncoder::encodeHeader(block, ":protocol", "websocket");
            HpackEncoder::encodeHeader(block, ":scheme", "https");
            HpackEncoder::encodeHeader(block, ":path", "/ws");
            HpackEncoder::encodeHeader(block, ":authority", "localhost");
            HpackEncoder::encodeHeader(block, "sec-websocket-version", "12");
            if (!co_await writeAll(frame(0x1, sansio_driver_test::kFlagEndHeaders, 1,
                    std::string_view(block.data(), block.size())))) {
                co_return;
            }

            ruvia::HpackDecoder decoder({.resource = std::pmr::get_default_resource()});
            for (;;) {
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
                    header.streamId == 1) {
                    gotResponseHead = true;
                    const auto decoded = decoder.decode(payload,
                        [&](std::string_view name, std::string_view value) {
                            if (name == ":status") {
                                gotBadRequest = value == "400";
                            } else if (name == "sec-websocket-version") {
                                gotSupportedVersion = value == "13";
                            } else if (name == "alt-svc") {
                                gotAltSvc = value == "h3=\":443\"; ma=86400";
                            } else if (name == "connection" || name == "upgrade" ||
                                       name == "sec-websocket-accept") {
                                sawForbiddenField = true;
                            }
                            return true;
                        });
                    hpackDecodeSucceeded = decoded.decoded();
                }
                if (header.streamId == 1 &&
                    (header.flags & sansio_driver_test::kFlagEndStream) != 0) {
                    gotResponseEnd = true;
                    break;
                }
            }
            closeClientSocket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(gotResponseHead);
    RUVIA_CHECK(hpackDecodeSucceeded);
    RUVIA_CHECK(gotBadRequest);
    RUVIA_CHECK(gotSupportedVersion);
    RUVIA_CHECK(gotAltSvc);
    RUVIA_CHECK(!sawForbiddenField);
    RUVIA_CHECK(gotResponseEnd);
}
