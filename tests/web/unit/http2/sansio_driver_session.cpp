#include "sansio_driver_fixture.h"

// Sans-I/O HTTP/2 driver: connection setup, round trips, multiplexing and teardown.

RUVIA_TEST(sansio_driver_h2_inactivity_phase_counts_predispatch_runtime) {
    using Phase = ruvia::detail::ConnectionScanner::Phase;
    RUVIA_CHECK(ruvia::detail::http2SansIoInactivityPhase(true, 0) == Phase::kReadingInitial);
    RUVIA_CHECK(ruvia::detail::http2SansIoInactivityPhase(false, 0) == Phase::kIdle);
    RUVIA_CHECK(ruvia::detail::http2SansIoInactivityPhase(false, 1) == Phase::kReadingPayload);
}

RUVIA_TEST(sansio_driver_h2_session_context_owns_complete_wiring) {
    ruvia::detail::HttpServerOptions options;
    ruvia::detail::ConnectionScanner::Entry scannerEntry;
    auto workerState = ruvia::detail::HttpServerWorkerState::kRunning;
    const ruvia::detail::Http2SansIoSessionContext session(ruvia::detail::ContextServices{}.withTlsTransport("192.0.2.1", "CN=test-client"), options, scannerEntry, workerState);

    RUVIA_CHECK(&session.options() == &options);
    RUVIA_CHECK(&session.scannerEntry() == &scannerEntry);
    RUVIA_CHECK(session.workerRunning());
    workerState = ruvia::detail::HttpServerWorkerState::kStopped;
    RUVIA_CHECK(!session.workerRunning());
    const auto& services = session.services();
    RUVIA_CHECK(services.connInfo().remote().address() == "192.0.2.1");
    RUVIA_CHECK(services.connInfo().plain() == nullptr);
    RUVIA_CHECK(services.connInfo().tls() != nullptr);
    RUVIA_CHECK(services.connInfo().tls()->clientCertificateSubject() == "CN=test-client");
}

RUVIA_TEST(sansio_driver_h2_get_round_trip) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool gotPong = false;

    // Server: drive Http2Connection with the generic pump. onReadable answers each
    // completed request with a fixed 200 "pong".
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            std::pmr::monotonic_buffer_resource resource;
            Http2Connection conn(&resource);
            conn.beginConnection();

            auto onReadable = [&resource, &ruvia_ctx](Http2Connection& c) -> ruvia::Task<void> {
                for (;;) {
                    const auto event = c.nextEvent();
                    if (!event.has_value()) {
                        break;
                    }
                    if (const auto* messageEnd = event->messageEnd()) {
                        const auto streamId = messageEnd->streamId();
                        ruvia::HttpResponse response(&resource);
                        response.status(ruvia::http_status::kOk);
                        response.body("pong");
                        const auto* stream = c.stream(streamId);
                        RUVIA_CHECK(stream != nullptr);
                        const auto submittedHead = c.submitResponseHead(streamId, response, ruvia::detail::httpBufferedResponseWritePlan(stream == nullptr ? ruvia::HttpKnownMethod::kUnknown : stream->requestKnownMethod(), response));
                        RUVIA_CHECK(submittedHead.submitted() != nullptr);
                        RUVIA_CHECK(c.submitData(streamId, "pong", Http2EndStream::kEndStream) == Http2DataSubmitStatus::kAccepted);
                    }
                }
                co_return;
            };
            co_await ruvia::detail::taskAsAwaitable(ruvia::detail::pumpSansIoConnection(conn, sock, [](const Http2Connection& connection) noexcept { return connection.connectionError().has_value(); }, onReadable));
        },
        asio::detached);

    // Client: a synthetic HTTP/2 peer that GETs / and looks for the DATA reply.
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);

            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) co_return;

            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "GET");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            if (!co_await writeAll(frame(0x1 /*HEADERS*/, ruvia::detail::kHttp2FlagEndStream | ruvia::detail::kHttp2FlagEndHeaders, 1, std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }

            // Drain frames until the stream-1 DATA reply arrives.
            for (;;) {
                char headerBytes[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) break;
                const auto header = ruvia::detail::http2ParseFrameHeader(std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) break;
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kData) && header.streamId == 1) {
                    gotPong = (std::string_view(payload) == "pong");
                    break;
                }
            }

            closeClientSocket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(gotPong);
}

RUVIA_TEST(sansio_driver_h2_real_dispatch_round_trip) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool gotResponseHead = false;
    bool gotResponseEnd = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::RouteTable routes(worker.resource());  // empty -> 404
            // Test-owned defaults drive the production session's required wiring.
            co_await ruvia::detail::taskAsAwaitable(ruvia::test::runBarePlainHttp2SansIoSession(sock, routes, worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);

            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) co_return;

            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "GET");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/missing");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            if (!co_await writeAll(frame(0x1, ruvia::detail::kHttp2FlagEndStream | ruvia::detail::kHttp2FlagEndHeaders, 1, std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }

            for (;;) {
                char headerBytes[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) break;
                const auto header = ruvia::detail::http2ParseFrameHeader(std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) break;
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kHeaders) && header.streamId == 1) {
                    gotResponseHead = true;
                }
                if (header.streamId == 1 && (header.flags & ruvia::detail::kHttp2FlagEndStream) != 0) {
                    gotResponseEnd = true;
                    break;
                }
            }

            closeClientSocket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(gotResponseHead);
    RUVIA_CHECK(gotResponseEnd);
}

RUVIA_TEST(sansio_driver_h2_bodyless_response_survives_empty_accept_encoding_set) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool gotNoContentHead = false;
    bool gotEndStream = false;
    bool sawData = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string("/empty", std::pmr::get_default_resource()), ruvia::detail::RouteHandler(nullptr, &noContentHandler), ruvia::detail::RequestBodyMode::kBuffered, std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{}, std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::test::runBarePlainHttp2SansIoSession(sock, impl.routeTable(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);

            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) co_return;

            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "GET");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/empty");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            HpackEncoder::encodeHeader(headerBlock, "accept-encoding", "identity;q=0, gzip;q=0, br;q=0, zstd;q=0");
            if (!co_await writeAll(frame(0x1, ruvia::detail::kHttp2FlagEndStream | ruvia::detail::kHttp2FlagEndHeaders, 1, std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }

            ruvia::detail::HpackDecoder decoder(std::pmr::get_default_resource());
            for (;;) {
                char headerBytes[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) break;
                const auto header = ruvia::detail::http2ParseFrameHeader(std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) break;
                if (header.streamId != 1) {
                    continue;
                }
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kHeaders)) {
                    HpackCollect fields;
                    (void)decoder.decode(payload, &fields, &HpackCollect::onHeader);
                    gotNoContentHead = fields.joined.find(":status=204;") != std::string::npos;
                } else if (header.type == static_cast<std::uint8_t>(Http2FrameType::kData)) {
                    sawData = true;
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
    RUVIA_CHECK(gotNoContentHead);
    RUVIA_CHECK(gotEndStream);
    RUVIA_CHECK(!sawData);
}

RUVIA_TEST(sansio_driver_h2_post_echo_real_handler) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::string echoed;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerRoute(ruvia::HttpKnownMethod::kPost, std::pmr::string("/echo", std::pmr::get_default_resource()), ruvia::detail::RouteHandler(nullptr, &echoHandler), ruvia::detail::RequestBodyMode::kBuffered, std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{}, std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::test::runBarePlainHttp2SansIoSession(sock, impl.routeTable(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);

            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) co_return;

            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "POST");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/echo");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            // HEADERS (END_HEADERS, body follows) then DATA "hello" (END_STREAM).
            if (!co_await writeAll(frame(0x1, ruvia::detail::kHttp2FlagEndHeaders, 1, std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }
            if (!co_await writeAll(frame(0x0 /*DATA*/, ruvia::detail::kHttp2FlagEndStream, 1, "hello"))) {
                co_return;
            }

            for (;;) {
                char headerBytes[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) break;
                const auto header = ruvia::detail::http2ParseFrameHeader(std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) break;
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kData) && header.streamId == 1 && !payload.empty()) {
                    echoed = payload;
                    break;
                }
            }

            closeClientSocket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(echoed == "handler-ran");
}

RUVIA_TEST(sansio_driver_h2_concurrent_streams_multiplex) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::vector<std::pair<std::uint32_t, std::string>> replies;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string("/slow", std::pmr::get_default_resource()), ruvia::detail::RouteHandler(&io, &slowHandler), ruvia::detail::RequestBodyMode::kBuffered, std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{}, std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.registerRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string("/fast", std::pmr::get_default_resource()), ruvia::detail::RouteHandler(nullptr, &fastHandler), ruvia::detail::RequestBodyMode::kBuffered, std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{}, std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::test::runBarePlainHttp2SansIoSession(sock, impl.routeTable(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);

            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };
            auto requestOn = [](std::uint32_t streamId, std::string_view path) {
                std::pmr::string block(std::pmr::get_default_resource());
                HpackEncoder::encodeHeader(block, ":method", "GET");
                HpackEncoder::encodeHeader(block, ":path", path);
                HpackEncoder::encodeHeader(block, ":scheme", "http");
                HpackEncoder::encodeHeader(block, ":authority", "localhost");
                return frame(0x1, ruvia::detail::kHttp2FlagEndStream | ruvia::detail::kHttp2FlagEndHeaders, streamId, std::string_view(block.data(), block.size()));
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4, 0, 0, {}))) co_return;
            if (!co_await writeAll(requestOn(1, "/slow"))) co_return;  // slow first
            if (!co_await writeAll(requestOn(3, "/fast"))) co_return;  // fast second

            while (replies.size() < 2) {
                char headerBytes[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) break;
                const auto header = ruvia::detail::http2ParseFrameHeader(std::string_view(headerBytes, sizeof(headerBytes)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) break;
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kData) && !payload.empty()) {
                    replies.emplace_back(header.streamId, payload);
                }
            }

            closeClientSocket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK_EQ(replies.size(), static_cast<std::size_t>(2));
    // The fast handler (stream 3, requested second) completes and replies first.
    RUVIA_CHECK_EQ(replies[0].first, static_cast<std::uint32_t>(3));
    RUVIA_CHECK(replies[0].second == "fast");
    RUVIA_CHECK_EQ(replies[1].first, static_cast<std::uint32_t>(1));
    RUVIA_CHECK(replies[1].second == "slow");
}

RUVIA_TEST(sansio_driver_h2_transport_end_is_error_and_joins_handler) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    TerminatedBodyObservation observation{.io = &io};

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerRoute(ruvia::HttpKnownMethod::kPost, std::pmr::string("/upload", std::pmr::get_default_resource()), ruvia::detail::RouteHandler(&observation, &terminatedBodyHandler), ruvia::detail::RequestBodyMode::kStream, std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{}, std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::test::runBarePlainHttp2SansIoSession(sock, impl.routeTable(), worker, "127.0.0.1"));
            observation.sessionReturnedAfterHandler = observation.handlerFinished;
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
            const auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                const auto [ec, count] = co_await asio::async_write(sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                co_return !ec && count == bytes.size();
            };
            if (!co_await writeAll(kClientPreface) || !co_await writeAll(frame(0x4, 0, 0, {}))) {
                co_return;
            }
            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "POST");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/upload");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            if (!co_await writeAll(frame(0x1, ruvia::detail::kHttp2FlagEndHeaders, 1, std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }
            while (!observation.started) {
                asio::steady_timer yield(io);
                yield.expires_after(std::chrono::milliseconds(1));
                (void)co_await yield.async_wait(asio::as_tuple(asio::use_awaitable));
            }
            closeClientSocket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(observation.sawTransportError);
    RUVIA_CHECK(observation.handlerFinished);
    RUVIA_CHECK(observation.sessionReturnedAfterHandler);
}

RUVIA_TEST(sansio_driver_h2_keepalive_requests_drains_connection) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool sawGoawayNoError = false;
    bool sawResponseEnd = false;
    bool sawRefusedStream = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerRoute(ruvia::HttpKnownMethod::kGet, std::pmr::string("/ping", std::pmr::get_default_resource()), ruvia::detail::RouteHandler(nullptr, &echoHandler), ruvia::detail::RequestBodyMode::kBuffered, std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{}, std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            ruvia::test::Http2SansIoSessionFixture fixture;
            const auto workerHandle = testWorker(io);
            fixture.options.keepaliveRequests = 1;
            co_await ruvia::detail::taskAsAwaitable(ruvia::detail::runHttp2SansIoSession(sock, impl.routeTable(), worker, fixture.context(ruvia::detail::ContextServices{}.withPlainTransport("127.0.0.1").withWorker(workerHandle))));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);

            auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(sock, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };
            auto readFrameInto = [&readExact](ruvia::detail::Http2FrameHeader& header, std::string& payload) -> asio::awaitable<bool> {
                char headerBytes[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) {
                    co_return false;
                }
                header = ruvia::detail::http2ParseFrameHeader(std::string_view(headerBytes, sizeof(headerBytes)));
                payload.assign(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) {
                    co_return false;
                }
                co_return true;
            };
            auto sendGet = [&writeAll](std::uint32_t streamId) -> asio::awaitable<bool> {
                std::pmr::string headerBlock(std::pmr::get_default_resource());
                HpackEncoder::encodeHeader(headerBlock, ":method", "GET");
                HpackEncoder::encodeHeader(headerBlock, ":path", "/ping");
                HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
                HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
                co_return co_await writeAll(frame(0x1 /*HEADERS*/, ruvia::detail::kHttp2FlagEndStream | ruvia::detail::kHttp2FlagEndHeaders, streamId, std::string_view(headerBlock.data(), headerBlock.size())));
            };

            if (!co_await writeAll(kClientPreface) || !co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {})) || !co_await sendGet(1)) {
                co_return;
            }

            ruvia::detail::Http2FrameHeader header{};
            std::string payload;
            while (!sawGoawayNoError || !sawResponseEnd) {
                if (!co_await readFrameInto(header, payload)) {
                    co_return;
                }
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kGoaway) && header.streamId == 0 && payload.size() >= 8) {
                    const auto* bytes = reinterpret_cast<const unsigned char*>(payload.data());
                    const auto lastStreamId = ruvia::detail::http2Read31(bytes);
                    const auto errorCode = ruvia::detail::http2Read32(bytes + 4);
                    sawGoawayNoError = lastStreamId == 1 && errorCode == 0;
                    continue;
                }
                if (header.streamId == 1 && (header.flags & ruvia::detail::kHttp2FlagEndStream) != 0) {
                    sawResponseEnd = true;
                }
            }

            // The drained connection must refuse a stream above the advertised id.
            if (!co_await sendGet(3)) {
                co_return;
            }
            while (!sawRefusedStream) {
                if (!co_await readFrameInto(header, payload)) {
                    co_return;
                }
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kRstStream) && header.streamId == 3 && payload.size() == 4) {
                    const auto* bytes = reinterpret_cast<const unsigned char*>(payload.data());
                    sawRefusedStream = ruvia::detail::http2Read32(bytes) == static_cast<std::uint32_t>(ruvia::detail::Http2ErrorCode::kRefusedStream);
                }
            }

            closeClientSocket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(sawGoawayNoError);
    RUVIA_CHECK(sawResponseEnd);
    RUVIA_CHECK(sawRefusedStream);
}
