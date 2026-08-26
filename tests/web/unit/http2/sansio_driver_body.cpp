#include "sansio_driver_fixture.h"

// Sans-I/O HTTP/2 driver: request and response bodies: pacing, streaming and trailers.

RUVIA_TEST(sansio_driver_h2_expectation_decision_precedes_request_content) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool gotContinue = false;
    bool continueEndedStream = false;
    bool gotUnsupportedFinal = false;
    bool gotSupportedFinal = false;
    std::string supportedBody;
    StreamAccessObservation accessObservation;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerRoute(ruvia::HttpKnownMethod::kPost,
                std::pmr::string("/echo", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(nullptr, &echoHandler),
                ruvia::detail::RequestBodyMode::kBuffered,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            ruvia::test::Http2SansIoSessionFixture fixture;
            const auto workerHandle = testWorker(io);
            fixture.options.accessLog.callback =
                ruvia::detail::CallbackAccess::bind<void(const ruvia::AccessLogRecord&) noexcept>(
                    accessObservation);
            co_await ruvia::detail::taskAsAwaitable(
                ruvia::detail::runHttp2SansIoSession(sock, impl.routeTable(), worker,
                    fixture.context(ruvia::detail::ContextServices{}
                            .withPlainTransport("127.0.0.1")
                            .withWorker(workerHandle))));
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
                co_return !ec && n == bytes.size();
            };
            auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                const auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await writeAll(kClientPreface) ||
                !co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) {
                co_return;
            }

            const auto makeHead = [](std::string_view expect) {
                std::pmr::string block(std::pmr::get_default_resource());
                HpackEncoder::encodeHeader(block, ":method", "POST");
                HpackEncoder::encodeHeader(block, ":path", "/echo");
                HpackEncoder::encodeHeader(block, ":scheme", "http");
                HpackEncoder::encodeHeader(block, ":authority", "localhost");
                HpackEncoder::encodeHeader(block, "content-length", "5");
                HpackEncoder::encodeHeader(block, "expect", expect);
                return block;
            };
            const auto continueHead = makeHead(", 100-continue,");
            const auto unsupportedHead = makeHead("custom-feature");
            if (!co_await writeAll(frame(0x1 /*HEADERS*/, ruvia::detail::kHttp2FlagEndHeaders, 1,
                    std::string_view(continueHead.data(), continueHead.size()))) ||
                !co_await writeAll(frame(0x1 /*HEADERS*/, ruvia::detail::kHttp2FlagEndHeaders, 3,
                    std::string_view(unsupportedHead.data(), unsupportedHead.size())))) {
                co_return;
            }

            ruvia::detail::HpackDecoder decoder({.resource = std::pmr::get_default_resource()});
            bool contentSent = false;
            bool supportedEnded = false;
            while (!(gotContinue && gotUnsupportedFinal && supportedEnded)) {
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
                    (header.streamId == 1 || header.streamId == 3)) {
                    HpackCollect fields;
                    const auto decoded = decoder.decode(payload, &fields, &HpackCollect::onHeader);
                    RUVIA_CHECK(decoded.decoded() != nullptr);
                    if (decoded.failure() != nullptr) {
                        break;
                    }
                    if (header.streamId == 1 &&
                        fields.joined.find(":status=100;") != std::string_view::npos) {
                        gotContinue = true;
                        continueEndedStream =
                            (header.flags & ruvia::detail::kHttp2FlagEndStream) != 0;
                        RUVIA_CHECK(fields.joined == ":status=100;");
                        if (!contentSent) {
                            contentSent = co_await writeAll(frame(
                                0x0 /*DATA*/, ruvia::detail::kHttp2FlagEndStream, 1, "hello"));
                            if (!contentSent) {
                                break;
                            }
                        }
                    } else if (header.streamId == 1 &&
                               fields.joined.find(":status=200;") != std::string::npos) {
                        gotSupportedFinal = true;
                    } else if (header.streamId == 3 &&
                               fields.joined.find(":status=417;") != std::string::npos) {
                        gotUnsupportedFinal = true;
                    }
                } else if (header.type == static_cast<std::uint8_t>(Http2FrameType::kData) &&
                           header.streamId == 1 && gotSupportedFinal) {
                    supportedBody.append(payload);
                    supportedEnded = (header.flags & ruvia::detail::kHttp2FlagEndStream) != 0;
                }
            }

            closeClientSocket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(gotContinue);
    RUVIA_CHECK(!continueEndedStream);
    RUVIA_CHECK(gotUnsupportedFinal);
    RUVIA_CHECK(gotSupportedFinal);
    RUVIA_CHECK(supportedBody == "handler-ran");
    RUVIA_CHECK_EQ(accessObservation.calls, std::size_t{2});
    RUVIA_CHECK(accessObservation.protocolVersion == ruvia::HttpProtocolVersion::kHttp2);
}

RUVIA_TEST(sansio_driver_h2_buffered_access_uses_only_committed_plan_status) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool gotStatus = false;
    bool gotBodyEnd = false;
    bool gotInvalidReset = false;
    StreamAccessObservation accessObservation;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            const auto noMiddleware =
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{};
            impl.registerRoute(ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/buffered-status", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(nullptr, &bufferedStatusHandler),
                ruvia::detail::RequestBodyMode::kBuffered, noMiddleware, noMiddleware);
            impl.registerRoute(ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/invalid-response", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(nullptr, &invalidHttp2ResponseHandler),
                ruvia::detail::RequestBodyMode::kBuffered, noMiddleware, noMiddleware);
            impl.finalize();

            ruvia::test::Http2SansIoSessionFixture fixture;
            const auto workerHandle = testWorker(io);
            fixture.options.accessLog.callback =
                ruvia::detail::CallbackAccess::bind<void(const ruvia::AccessLogRecord&) noexcept>(
                    accessObservation);
            co_await ruvia::detail::taskAsAwaitable(
                ruvia::detail::runHttp2SansIoSession(sock, impl.routeTable(), worker,
                    fixture.context(ruvia::detail::ContextServices{}
                            .withPlainTransport("127.0.0.1")
                            .withWorker(workerHandle))));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
            const auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                const auto [ec, n] = co_await asio::async_write(sock,
                    asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == bytes.size();
            };
            const auto readExact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                const auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };
            const auto requestHead = [](std::uint32_t streamId, std::string_view path) {
                std::pmr::string block(std::pmr::get_default_resource());
                HpackEncoder::encodeHeader(block, ":method", "GET");
                HpackEncoder::encodeHeader(block, ":path", path);
                HpackEncoder::encodeHeader(block, ":scheme", "http");
                HpackEncoder::encodeHeader(block, ":authority", "localhost");
                return frame(0x1,
                    ruvia::detail::kHttp2FlagEndStream | ruvia::detail::kHttp2FlagEndHeaders,
                    streamId, std::string_view(block.data(), block.size()));
            };

            if (!co_await writeAll(kClientPreface) || !co_await writeAll(frame(0x4, 0, 0, {})) ||
                !co_await writeAll(requestHead(1, "/buffered-status")) ||
                !co_await writeAll(requestHead(3, "/invalid-response"))) {
                co_return;
            }

            ruvia::detail::HpackDecoder decoder({.resource = std::pmr::get_default_resource()});
            while (!(gotBodyEnd && gotInvalidReset)) {
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
                if (header.streamId == 1 &&
                    header.type == static_cast<std::uint8_t>(Http2FrameType::kHeaders)) {
                    HpackCollect fields;
                    const auto decoded = decoder.decode(payload, &fields, &HpackCollect::onHeader);
                    RUVIA_CHECK(decoded.decoded() != nullptr);
                    gotStatus = decoded.decoded() != nullptr &&
                                fields.joined.find(":status=207;") != std::string::npos;
                } else if (header.streamId == 1 &&
                           header.type == static_cast<std::uint8_t>(Http2FrameType::kData)) {
                    gotBodyEnd = (header.flags & ruvia::detail::kHttp2FlagEndStream) != 0;
                } else if (header.streamId == 3 &&
                           header.type == static_cast<std::uint8_t>(Http2FrameType::kRstStream)) {
                    gotInvalidReset = true;
                }
            }

            closeClientSocket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(gotStatus);
    RUVIA_CHECK(gotBodyEnd);
    RUVIA_CHECK(gotInvalidReset);
    RUVIA_CHECK_EQ(accessObservation.calls, std::size_t{1});
    RUVIA_CHECK_EQ(accessObservation.status, std::uint16_t{207});
    RUVIA_CHECK(accessObservation.protocolVersion == ruvia::HttpProtocolVersion::kHttp2);
}

RUVIA_TEST(sansio_driver_h2_buffered_peer_abort_before_commit_has_no_status) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    StreamAccessObservation accessObservation;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerRoute(ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/peer-abort", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(&io, &slowHandler),
                ruvia::detail::RequestBodyMode::kBuffered,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            ruvia::test::Http2SansIoSessionFixture fixture;
            const auto workerHandle = testWorker(io);
            fixture.options.accessLog.callback =
                ruvia::detail::CallbackAccess::bind<void(const ruvia::AccessLogRecord&) noexcept>(
                    accessObservation);
            co_await ruvia::detail::taskAsAwaitable(
                ruvia::detail::runHttp2SansIoSession(sock, impl.routeTable(), worker,
                    fixture.context(ruvia::detail::ContextServices{}
                            .withPlainTransport("127.0.0.1")
                            .withWorker(workerHandle))));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
            const auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                const auto [ec, n] = co_await asio::async_write(sock,
                    asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == bytes.size();
            };

            std::pmr::string block(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(block, ":method", "GET");
            HpackEncoder::encodeHeader(block, ":path", "/peer-abort");
            HpackEncoder::encodeHeader(block, ":scheme", "http");
            HpackEncoder::encodeHeader(block, ":authority", "localhost");
            const std::string cancelPayload("\0\0\0\x08", 4);
            if (!co_await writeAll(kClientPreface) || !co_await writeAll(frame(0x4, 0, 0, {})) ||
                !co_await writeAll(frame(0x1,
                    ruvia::detail::kHttp2FlagEndStream | ruvia::detail::kHttp2FlagEndHeaders, 1,
                    std::string_view(block.data(), block.size()))) ||
                !co_await writeAll(frame(0x3 /*RST_STREAM*/, 0, 1, cancelPayload))) {
                co_return;
            }

            asio::steady_timer settle(io);
            settle.expires_after(std::chrono::milliseconds(150));
            const auto [waitEc] = co_await settle.async_wait(asio::as_tuple(asio::use_awaitable));
            (void)waitEc;
            closeClientSocket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK_EQ(accessObservation.calls, std::size_t{0});
}

RUVIA_TEST(sansio_driver_h2_stream_trailers_emitted) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::string body;
    std::string headFields;
    std::string trailerFields;
    bool trailerEndStream = false;
    StreamAccessObservation accessObservation;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerResponseStreamRoute(ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/trail", std::pmr::get_default_resource()),
                ruvia::detail::RouteStreamHandler(nullptr, &streamTrailerHandler),
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            ruvia::test::Http2SansIoSessionFixture fixture;
            const auto workerHandle = testWorker(io);
            fixture.options.accessLog.callback =
                ruvia::detail::CallbackAccess::bind<void(const ruvia::AccessLogRecord&) noexcept>(
                    accessObservation);
            co_await ruvia::detail::taskAsAwaitable(
                ruvia::detail::runHttp2SansIoSession(sock, impl.routeTable(), worker,
                    fixture.context(ruvia::detail::ContextServices{}
                            .withPlainTransport("127.0.0.1")
                            .withWorker(workerHandle))));
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
            HpackEncoder::encodeHeader(headerBlock, ":method", "GET");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/trail");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            if (!co_await writeAll(frame(0x1,
                    ruvia::detail::kHttp2FlagEndStream | ruvia::detail::kHttp2FlagEndHeaders, 1,
                    std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }

            ruvia::detail::HpackDecoder decoder({.resource = std::pmr::get_default_resource()});
            bool sawHead = false;
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
                    HpackCollect collect;
                    (void)decoder.decode(payload, &collect, &HpackCollect::onHeader);
                    if (!sawHead) {
                        sawHead = true;
                        headFields = collect.joined;
                    } else {
                        trailerFields = collect.joined;  // the trailing HEADERS block
                        trailerEndStream = (header.flags & ruvia::detail::kHttp2FlagEndStream) != 0;
                        break;
                    }
                } else if (header.type == static_cast<std::uint8_t>(Http2FrameType::kData)) {
                    body += payload;
                    if ((header.flags & ruvia::detail::kHttp2FlagEndStream) != 0) break;
                }
            }
            closeClientSocket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(body == "body-part");
    RUVIA_CHECK(headFields.find(":status=207;") != std::string_view::npos);
    RUVIA_CHECK(trailerFields == "x-checksum=abc123;");
    RUVIA_CHECK(trailerEndStream);
    RUVIA_CHECK_EQ(accessObservation.calls, std::size_t{1});
    RUVIA_CHECK_EQ(accessObservation.status, std::uint16_t{207});
    RUVIA_CHECK(accessObservation.protocolVersion == ruvia::HttpProtocolVersion::kHttp2);
}

RUVIA_TEST(sansio_driver_h2_stream_send_window_pacing) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::size_t received = 0;
    bool sawEnd = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerResponseStreamRoute(ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/big", std::pmr::get_default_resource()),
                ruvia::detail::RouteStreamHandler(nullptr, &streamBigChunkHandler),
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
            // SETTINGS_INITIAL_WINDOW_SIZE = 8: the 64-byte body must be granted 8 at a time.
            const char settingsPayload[6] = {0x00, 0x04, 0x00, 0x00, 0x00, 0x08};
            if (!co_await writeAll(frame(0x4, 0, 0, std::string_view(settingsPayload, 6))))
                co_return;
            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "GET");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/big");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            if (!co_await writeAll(frame(0x1,
                    ruvia::detail::kHttp2FlagEndStream | ruvia::detail::kHttp2FlagEndHeaders, 1,
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
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kData)) {
                    received += payload.size();
                    if ((header.flags & ruvia::detail::kHttp2FlagEndStream) != 0) {
                        sawEnd = true;
                        break;
                    }
                    if (!payload.empty()) {
                        // Grant the next window slice (connection + stream scoped).
                        char updates[2 * (ruvia::detail::kHttp2FrameHeaderBytes + 4)];
                        char* out = ruvia::detail::http2WriteWindowUpdate(
                            updates, 0, static_cast<std::uint32_t>(payload.size()));
                        out = ruvia::detail::http2WriteWindowUpdate(
                            out, 1, static_cast<std::uint32_t>(payload.size()));
                        if (!co_await writeAll(std::string_view(
                                updates, static_cast<std::size_t>(out - updates)))) {
                            break;
                        }
                    }
                }
            }
            closeClientSocket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK_EQ(received, static_cast<std::size_t>(64));
    RUVIA_CHECK(sawEnd);
}

RUVIA_TEST(sansio_driver_h2_large_file_body_paces_and_completes) {
    // Write the temp file (kLargeFileBytes of a repeating pattern).
    const auto path =
        (std::filesystem::temp_directory_path() / "ruvia_sansio_large_file_test.bin").string();
    largeFilePath() = path;
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        std::string block(4096, '\0');
        for (std::size_t i = 0; i < block.size(); ++i) {
            block[i] = static_cast<char>('A' + (i % 26));
        }
        std::uint64_t written = 0;
        while (written < kLargeFileBytes) {
            const auto n = static_cast<std::size_t>(
                std::min<std::uint64_t>(block.size(), kLargeFileBytes - written));
            out.write(block.data(), static_cast<std::streamsize>(n));
            written += n;
        }
    }

    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::uint64_t received = 0;
    bool sawEnd = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerRoute(ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/file", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(nullptr, &largeFileHandler),
                ruvia::detail::RequestBodyMode::kBuffered,
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
            // Small initial window (16) so the file body blocks almost immediately and
            // only completes if WINDOW_UPDATE-driven pacing wakes it repeatedly.
            const char settingsPayload[6] = {0x00, 0x04, 0x00, 0x00, 0x00, 0x10};
            if (!co_await writeAll(frame(0x4, 0, 0, std::string_view(settingsPayload, 6))))
                co_return;
            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "GET");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/file");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            if (!co_await writeAll(frame(0x1,
                    ruvia::detail::kHttp2FlagEndStream | ruvia::detail::kHttp2FlagEndHeaders, 1,
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
                if (header.streamId != 1 ||
                    header.type != static_cast<std::uint8_t>(Http2FrameType::kData)) {
                    continue;
                }
                received += payload.size();
                if ((header.flags & ruvia::detail::kHttp2FlagEndStream) != 0) {
                    sawEnd = true;
                    break;
                }
                if (!payload.empty()) {
                    char updates[2 * (ruvia::detail::kHttp2FrameHeaderBytes + 4)];
                    char* out = ruvia::detail::http2WriteWindowUpdate(
                        updates, 0, static_cast<std::uint32_t>(payload.size()));
                    out = ruvia::detail::http2WriteWindowUpdate(
                        out, 1, static_cast<std::uint32_t>(payload.size()));
                    if (!co_await writeAll(
                            std::string_view(updates, static_cast<std::size_t>(out - updates)))) {
                        break;
                    }
                }
            }
            closeClientSocket(sock);
        },
        asio::detached);

    io.run();
    std::filesystem::remove(path);
    RUVIA_CHECK_EQ(received, kLargeFileBytes);  // every byte delivered, not truncated
    RUVIA_CHECK(sawEnd);
}

RUVIA_TEST(sansio_driver_h2_streaming_request_body) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::size_t receivedBytes = 0;
    std::string body;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerRoute(ruvia::HttpKnownMethod::kPost,
                std::pmr::string("/upload", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(&receivedBytes, &streamBodyCountHandler),
                ruvia::detail::RequestBodyMode::kStream,
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
            auto yield = [&io]() -> asio::awaitable<void> {
                asio::steady_timer t(io, std::chrono::milliseconds(5));
                co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4, 0, 0, {}))) co_return;
            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "POST");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/upload");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            // HEADERS with NO END_STREAM -> body follows in separate DATA frames.
            if (!co_await writeAll(frame(0x1, ruvia::detail::kHttp2FlagEndHeaders, 1,
                    std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }
            co_await yield();
            if (!co_await writeAll(frame(0x0, 0, 1, "aaa"))) co_return;  // 3 bytes
            co_await yield();
            if (!co_await writeAll(frame(0x0, 0, 1, "bb"))) co_return;  // 2 bytes
            co_await yield();
            if (!co_await writeAll(frame(0x0, ruvia::detail::kHttp2FlagEndStream, 1, {})))
                co_return;

            for (;;) {
                char hb[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(hb, sizeof(hb))) break;
                const auto header =
                    ruvia::detail::http2ParseFrameHeader(std::string_view(hb, sizeof(hb)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size()))
                    break;
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kData) &&
                    header.streamId == 1 && !payload.empty()) {
                    body = payload;
                    break;
                }
            }
            closeClientSocket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK_EQ(receivedBytes, static_cast<std::size_t>(5));  // "aaa" + "bb"
    RUVIA_CHECK(body == "upload-done");
}

RUVIA_TEST(sansio_driver_h2_server_request_trailers_dispatch) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::string body;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerRoute(ruvia::HttpKnownMethod::kPost,
                std::pmr::string("/echo", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(nullptr, &echoHandler),
                ruvia::detail::RequestBodyMode::kBuffered,
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
            HpackEncoder::encodeHeader(headerBlock, ":method", "POST");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/echo");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            if (!co_await writeAll(frame(0x1, ruvia::detail::kHttp2FlagEndHeaders, 1,
                    std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }
            // Body, then a trailing HEADERS block carrying END_STREAM.
            if (!co_await writeAll(frame(0x0, 0, 1, "hi"))) co_return;
            std::pmr::string trailerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(trailerBlock, "x-checksum", "abc");
            if (!co_await writeAll(frame(0x1,
                    ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream, 1,
                    std::string_view(trailerBlock.data(), trailerBlock.size())))) {
                co_return;
            }

            for (;;) {
                char hb[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(hb, sizeof(hb))) break;
                const auto header =
                    ruvia::detail::http2ParseFrameHeader(std::string_view(hb, sizeof(hb)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size()))
                    break;
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kData) &&
                    header.streamId == 1 && !payload.empty()) {
                    body = payload;
                    break;
                }
            }
            closeClientSocket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(body == "handler-ran");  // trailers ended the request; handler dispatched
}

RUVIA_TEST(sansio_driver_h2_large_buffered_body_paces_and_completes) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::uint64_t received = 0;
    bool sawEnd = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerRoute(ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/big", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(nullptr, &largeBufferedHandler),
                ruvia::detail::RequestBodyMode::kBuffered,
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
            const char settingsPayload[6] = {0x00, 0x04, 0x00, 0x00, 0x00, 0x20};  // window 32
            if (!co_await writeAll(frame(0x4, 0, 0, std::string_view(settingsPayload, 6))))
                co_return;
            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "GET");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/big");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            if (!co_await writeAll(frame(0x1,
                    ruvia::detail::kHttp2FlagEndStream | ruvia::detail::kHttp2FlagEndHeaders, 1,
                    std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }

            for (;;) {
                char hb[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(hb, sizeof(hb))) break;
                const auto header =
                    ruvia::detail::http2ParseFrameHeader(std::string_view(hb, sizeof(hb)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size()))
                    break;
                if (header.streamId != 1 ||
                    header.type != static_cast<std::uint8_t>(Http2FrameType::kData)) {
                    continue;
                }
                received += payload.size();
                if ((header.flags & ruvia::detail::kHttp2FlagEndStream) != 0) {
                    sawEnd = true;
                    break;
                }
                if (!payload.empty()) {
                    char updates[2 * (ruvia::detail::kHttp2FrameHeaderBytes + 4)];
                    char* out = ruvia::detail::http2WriteWindowUpdate(
                        updates, 0, static_cast<std::uint32_t>(payload.size()));
                    out = ruvia::detail::http2WriteWindowUpdate(
                        out, 1, static_cast<std::uint32_t>(payload.size()));
                    if (!co_await writeAll(
                            std::string_view(updates, static_cast<std::size_t>(out - updates)))) {
                        break;
                    }
                }
            }
            closeClientSocket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK_EQ(received, static_cast<std::uint64_t>(kLargeBufferedBytes));
    RUVIA_CHECK(sawEnd);
}
