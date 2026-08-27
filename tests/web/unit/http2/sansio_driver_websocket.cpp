#include "sansio_driver_fixture.h"

// Sans-I/O HTTP/2 driver: WebSocket tunnels over HTTP/2 (RFC 8441).

RUVIA_TEST(sansio_driver_h2_websocket_echo) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool gotHandshake = false;
    std::string echoedFrame;  // reassembled ws frame bytes from stream-1 DATA
    bool gotCloseEndStream = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerWebSocketRoute(ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/ws", std::pmr::get_default_resource()),
                ruvia::detail::RouteStreamHandler(nullptr, &wsEchoHandler),
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::test::runBarePlainHttp2SansIoSession(
                sock, impl.routeTable(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);

            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(sock,
                    asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };
            auto readFrameInto = [&readExact](ruvia::detail::Http2FrameHeader& header,
                                     std::string& payload) -> asio::awaitable<bool> {
                char headerBytes[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) co_return false;
                header = ruvia::detail::http2ParseFrameHeader(
                    std::string_view(headerBytes, sizeof(headerBytes)));
                payload.assign(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) {
                    co_return false;
                }
                co_return true;
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) co_return;

            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "CONNECT");
            HpackEncoder::encodeHeader(headerBlock, ":protocol", "websocket");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/ws");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            HpackEncoder::encodeHeader(headerBlock, "sec-websocket-version", "13");
            // Extended CONNECT: END_HEADERS only -- the stream MUST stay open.
            if (!co_await writeAll(frame(0x1 /*HEADERS*/, ruvia::detail::kHttp2FlagEndHeaders, 1,
                    std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }

            // Wait for the 200 handshake HEADERS on stream 1.
            ruvia::detail::Http2FrameHeader header{};
            std::string payload;
            for (;;) {
                if (!co_await readFrameInto(header, payload)) co_return;
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kHeaders) &&
                    header.streamId == 1) {
                    gotHandshake = true;
                    break;
                }
            }

            // Send a masked text frame through the tunnel and reassemble the echo
            // (the transport may split the ws frame across DATA frames).
            if (!co_await writeAll(frame(0x0 /*DATA*/, 0, 1, maskedWsFrame(0x1, "hello"))))
                co_return;
            std::string tunnelBytes;
            while (echoedFrame.empty()) {
                if (!co_await readFrameInto(header, payload)) co_return;
                if (header.type != static_cast<std::uint8_t>(Http2FrameType::kData) ||
                    header.streamId != 1) {
                    continue;
                }
                tunnelBytes += payload;
                if (tunnelBytes.size() >= 2) {
                    const auto len = static_cast<std::size_t>(
                        static_cast<unsigned char>(tunnelBytes[1]) & 0x7FU);
                    if (tunnelBytes.size() >= 2 + len) {
                        echoedFrame = tunnelBytes.substr(0, 2 + len);
                    }
                }
            }

            // Close the tunnel: the client sends its masked Close and orderly
            // transport half-close together; the server echoes Close and ends its
            // half only after the protocol core observes that peer Close.
            if (!co_await writeAll(frame(0x0 /*DATA*/, ruvia::detail::kHttp2FlagEndStream, 1,
                    maskedWsFrame(0x8, std::string_view("\x03\xE8", 2))))) {
                co_return;
            }
            for (;;) {
                if (!co_await readFrameInto(header, payload)) break;
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kData) &&
                    header.streamId == 1 &&
                    (header.flags & ruvia::detail::kHttp2FlagEndStream) != 0) {
                    gotCloseEndStream = true;
                    break;
                }
            }

            closeClientSocket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(gotHandshake);
    // FIN|text, length 5, "hello" -- unmasked server frame, payload echoed intact.
    RUVIA_CHECK_EQ(echoedFrame.size(), static_cast<std::size_t>(7));
    RUVIA_CHECK_EQ(static_cast<unsigned char>(echoedFrame[0]), static_cast<unsigned char>(0x81));
    RUVIA_CHECK_EQ(static_cast<unsigned char>(echoedFrame[1]), static_cast<unsigned char>(5));
    RUVIA_CHECK(echoedFrame.substr(2) == "hello");
    RUVIA_CHECK(gotCloseEndStream);
}

RUVIA_TEST(sansio_driver_h2_websocket_success_ignores_accept_encoding_rejection) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::string handshakeFields;

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
            co_await ruvia::detail::taskAsAwaitable(ruvia::test::runBarePlainHttp2SansIoSession(
                sock, impl.routeTable(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);

            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                const auto [ec, n] = co_await asio::async_write(sock,
                    asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                const auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) co_return;

            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "CONNECT");
            HpackEncoder::encodeHeader(headerBlock, ":protocol", "websocket");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/ws");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            HpackEncoder::encodeHeader(headerBlock, "sec-websocket-version", "13");
            HpackEncoder::encodeHeader(
                headerBlock, "accept-encoding", "identity;q=0, gzip;q=0, br;q=0, zstd;q=0");
            if (!co_await writeAll(frame(0x1 /*HEADERS*/, ruvia::detail::kHttp2FlagEndHeaders, 1,
                    std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }

            ruvia::detail::HpackDecoder decoder({.resource = std::pmr::get_default_resource()});
            for (;;) {
                char headerBytes[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) {
                    break;
                }
                const auto header = ruvia::detail::http2ParseFrameHeader(
                    std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) {
                    break;
                }
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kHeaders) &&
                    header.streamId == 1) {
                    HpackCollect collect;
                    (void)decoder.decode(payload, &collect, &HpackCollect::onHeader);
                    handshakeFields = collect.joined;
                    break;
                }
            }

            closeClientSocket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(handshakeFields.find(":status=200;") != std::string::npos);
    RUVIA_CHECK(handshakeFields.find("content-encoding=") == std::string::npos);
}

RUVIA_TEST(sansio_driver_h2_server_close_waits_for_peer_close) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool gotHandshake = false;
    bool gotCloseWithoutEnd = false;
    bool gotTerminalEmptyEnd = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerWebSocketRoute(ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/server-close", std::pmr::get_default_resource()),
                ruvia::detail::RouteStreamHandler(nullptr, &wsServerCloseHandler),
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::test::runBarePlainHttp2SansIoSession(
                sock, impl.routeTable(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);

            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                const auto [ec, n] = co_await asio::async_write(sock,
                    asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                const auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };
            auto readFrameInto = [&readExact](ruvia::detail::Http2FrameHeader& header,
                                     std::string& payload) -> asio::awaitable<bool> {
                char headerBytes[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) {
                    co_return false;
                }
                header = ruvia::detail::http2ParseFrameHeader(
                    std::string_view(headerBytes, sizeof(headerBytes)));
                payload.assign(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) {
                    co_return false;
                }
                co_return true;
            };

            if (!co_await writeAll(kClientPreface) ||
                !co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) {
                co_return;
            }

            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "CONNECT");
            HpackEncoder::encodeHeader(headerBlock, ":protocol", "websocket");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/server-close");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            HpackEncoder::encodeHeader(headerBlock, "sec-websocket-version", "13");
            if (!co_await writeAll(frame(0x1 /*HEADERS*/, ruvia::detail::kHttp2FlagEndHeaders, 1,
                    std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }

            ruvia::detail::Http2FrameHeader header{};
            std::string payload;
            while (!gotCloseWithoutEnd) {
                if (!co_await readFrameInto(header, payload)) {
                    co_return;
                }
                if (header.streamId != 1) {
                    continue;
                }
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kHeaders)) {
                    gotHandshake = true;
                    continue;
                }
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kData) &&
                    payload.size() >= 4 && static_cast<unsigned char>(payload[0]) == 0x88U) {
                    gotCloseWithoutEnd = (header.flags & ruvia::detail::kHttp2FlagEndStream) == 0;
                }
            }

            if (!co_await writeAll(frame(0x0 /*DATA*/, ruvia::detail::kHttp2FlagEndStream, 1,
                    maskedWsFrame(0x8, std::string_view("\x03\xE8", 2))))) {
                co_return;
            }
            for (;;) {
                if (!co_await readFrameInto(header, payload)) {
                    break;
                }
                if (header.streamId == 1 &&
                    header.type == static_cast<std::uint8_t>(Http2FrameType::kData) &&
                    (header.flags & ruvia::detail::kHttp2FlagEndStream) != 0) {
                    gotTerminalEmptyEnd = payload.empty();
                    break;
                }
            }

            closeClientSocket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(gotHandshake);
    RUVIA_CHECK(gotCloseWithoutEnd);
    RUVIA_CHECK(gotTerminalEmptyEnd);
}

RUVIA_TEST(sansio_driver_h2_websocket_invalid_version_rejected) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool gotResponseHead = false;
    bool gotEndStream = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerWebSocketRoute(ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/ws", std::pmr::get_default_resource()),
                ruvia::detail::RouteStreamHandler(nullptr, &wsEchoHandler),
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::test::runBarePlainHttp2SansIoSession(
                sock, impl.routeTable(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);

            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(sock,
                    asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) co_return;

            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "CONNECT");
            HpackEncoder::encodeHeader(headerBlock, ":protocol", "websocket");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/ws");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            HpackEncoder::encodeHeader(headerBlock, "sec-websocket-version", "12");  // bad
            if (!co_await writeAll(frame(0x1, ruvia::detail::kHttp2FlagEndHeaders, 1,
                    std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }

            for (;;) {
                char headerBytes[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) break;
                const auto header = ruvia::detail::http2ParseFrameHeader(
                    std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size()))
                    break;
                if (header.streamId != 1) continue;
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kHeaders)) {
                    gotResponseHead = true;
                }
                if ((header.flags & ruvia::detail::kHttp2FlagEndStream) != 0) {
                    gotEndStream = true;
                    break;
                }
            }

            closeClientSocket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(gotResponseHead);
    RUVIA_CHECK(gotEndStream);
}

RUVIA_TEST(sansio_driver_h2_websocket_permessage_deflate) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::string handshakeFields;
    std::string echoedFrame;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerWebSocketRoute(ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/ws", std::pmr::get_default_resource()),
                ruvia::detail::RouteStreamHandler(nullptr, &wsEchoHandler),
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::test::runBarePlainHttp2SansIoSession(
                sock, impl.routeTable(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(sock,
                    asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4, 0, 0, {}))) co_return;
            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "CONNECT");
            HpackEncoder::encodeHeader(headerBlock, ":protocol", "websocket");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/ws");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            HpackEncoder::encodeHeader(headerBlock, "sec-websocket-version", "13");
            HpackEncoder::encodeHeader(headerBlock, "sec-websocket-extensions",
                "permessage-deflate; client_max_window_bits");
            if (!co_await writeAll(frame(0x1, ruvia::detail::kHttp2FlagEndHeaders, 1,
                    std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }

            // Handshake HEADERS: decode and capture the echoed extension.
            ruvia::detail::HpackDecoder decoder({.resource = std::pmr::get_default_resource()});
            ruvia::detail::Http2FrameHeader header{};
            for (;;) {
                char headerBytes[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) co_return;
                header = ruvia::detail::http2ParseFrameHeader(
                    std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size()))
                    co_return;
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kHeaders) &&
                    header.streamId == 1) {
                    HpackCollect collect;
                    (void)decoder.decode(payload, &collect, &HpackCollect::onHeader);
                    handshakeFields = collect.joined;
                    break;
                }
            }

            // Send a COMPRESSED masked text frame; the server must inflate + echo it.
            ruvia::detail::WebSocketDeflate clientCodec;
            std::pmr::string compressed(std::pmr::get_default_resource());
            if (!clientCodec.compress("hello-deflate", compressed)) co_return;
            if (!co_await writeAll(frame(0x0, 0, 1,
                    maskedWsFrame(0x1, std::string_view(compressed.data(), compressed.size()),
                        /*rsv1=*/true)))) {
                co_return;
            }
            std::string tunnelBytes;
            while (echoedFrame.empty()) {
                char headerBytes[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) co_return;
                header = ruvia::detail::http2ParseFrameHeader(
                    std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size()))
                    co_return;
                if (header.type != static_cast<std::uint8_t>(Http2FrameType::kData) ||
                    header.streamId != 1) {
                    continue;
                }
                tunnelBytes += payload;
                if (tunnelBytes.size() >= 2) {
                    const auto len = static_cast<std::size_t>(
                        static_cast<unsigned char>(tunnelBytes[1]) & 0x7FU);
                    if (tunnelBytes.size() >= 2 + len) {
                        echoedFrame = tunnelBytes.substr(0, 2 + len);
                    }
                }
            }
            closeClientSocket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(handshakeFields.find("sec-websocket-extensions=permessage-deflate") !=
                std::string_view::npos);
    // 13-byte echo does not shrink under deflate, so it comes back as a plain frame.
    RUVIA_CHECK_EQ(echoedFrame.size(), static_cast<std::size_t>(15));
    RUVIA_CHECK_EQ(static_cast<unsigned char>(echoedFrame[0]), static_cast<unsigned char>(0x81));
    RUVIA_CHECK(echoedFrame.substr(2) == "hello-deflate");
}

RUVIA_TEST(sansio_driver_h2_two_concurrent_ws_tunnels) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::string echo1;
    std::string echo3;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerWebSocketRoute(ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/ws", std::pmr::get_default_resource()),
                ruvia::detail::RouteStreamHandler(nullptr, &wsEchoHandler),
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::test::runBarePlainHttp2SansIoSession(
                sock, impl.routeTable(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(sock,
                    asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };
            auto openTunnel = [](std::uint32_t streamId) {
                std::pmr::string block(std::pmr::get_default_resource());
                HpackEncoder::encodeHeader(block, ":method", "CONNECT");
                HpackEncoder::encodeHeader(block, ":protocol", "websocket");
                HpackEncoder::encodeHeader(block, ":scheme", "http");
                HpackEncoder::encodeHeader(block, ":path", "/ws");
                HpackEncoder::encodeHeader(block, ":authority", "localhost");
                HpackEncoder::encodeHeader(block, "sec-websocket-version", "13");
                return frame(0x1, ruvia::detail::kHttp2FlagEndHeaders, streamId,
                    std::string_view(block.data(), block.size()));
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4, 0, 0, {}))) co_return;
            // Open BOTH tunnels (streams 1 and 3) before sending any frames.
            if (!co_await writeAll(openTunnel(1))) co_return;
            if (!co_await writeAll(openTunnel(3))) co_return;
            // A masked text frame down each tunnel.
            if (!co_await writeAll(frame(0x0, 0, 1, maskedWsFrame(0x1, "one")))) co_return;
            if (!co_await writeAll(frame(0x0, 0, 3, maskedWsFrame(0x1, "three")))) co_return;

            std::string tunnel1;
            std::string tunnel3;
            auto extractFrame = [](std::string& acc) -> std::string {
                if (acc.size() < 2) return {};
                const auto len =
                    static_cast<std::size_t>(static_cast<unsigned char>(acc[1]) & 0x7FU);
                if (acc.size() < 2 + len) return {};
                return acc.substr(0, 2 + len);
            };
            while (echo1.empty() || echo3.empty()) {
                char hb[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(hb, sizeof(hb))) co_return;
                const auto header =
                    ruvia::detail::http2ParseFrameHeader(std::string_view(hb, sizeof(hb)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size()))
                    co_return;
                if (header.type != static_cast<std::uint8_t>(Http2FrameType::kData)) continue;
                if (header.streamId == 1) {
                    tunnel1 += payload;
                    if (echo1.empty()) echo1 = extractFrame(tunnel1);
                } else if (header.streamId == 3) {
                    tunnel3 += payload;
                    if (echo3.empty()) echo3 = extractFrame(tunnel3);
                }
            }
            closeClientSocket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(echo1.size() == 5 && echo1.substr(2) == "one");    // FIN|text len3 "one"
    RUVIA_CHECK(echo3.size() == 7 && echo3.substr(2) == "three");  // FIN|text len5 "three"
}
