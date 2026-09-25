#include <concepts>
#include <memory>
#include <stdexcept>

#include <asio/io_context.hpp>

#include "ruvia/core/ConnectionScanner.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/http/Hpack.h"
#include "ruvia/http/Http2Framing.h"
#include "ruvia/http/Http2Types.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/web/detail/http2/Http2SansIoSession.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/detail/router/RouterImpl.h"

#include "http2_sansio_session_fixture.h"
#include "sansio_driver_fixture.h"
#include "test_io_context.h"

// Sans-I/O HTTP/2 driver: connection setup, round trips, multiplexing and teardown.

RUVIA_TEST(sansio_driver_h2_inactivity_phase_counts_predispatch_runtime) {
    using Phase = ruvia::ConnectionScanner::Phase;
    RUVIA_CHECK(ruvia::detail::http2SansIoInactivityPhase(true, 0) == Phase::kReadingInitial);
    RUVIA_CHECK(ruvia::detail::http2SansIoInactivityPhase(false, 0) == Phase::kIdle);
    RUVIA_CHECK(ruvia::detail::http2SansIoInactivityPhase(false, 1) == Phase::kReadingPayload);
}

RUVIA_TEST(sansio_driver_h2_session_context_owns_complete_wiring) {
    auto& ioContext = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(ioContext, {.mailboxCapacity = 8});
    const auto worker = attachment.loop().handle();
    ruvia::detail::HttpServerOptions options;
    ruvia::ConnectionScanner::Entry scannerEntry;
    auto workerState = ruvia::detail::HttpServerWorkerState::kRunning;
    const ruvia::StopToken stopToken;

    const ruvia::detail::Http2SansIoSessionContext session(
        ruvia::detail::ContextServices(worker, stopToken)
            .withTlsTransport("192.0.2.1", "CN=test-client"),
        options, scannerEntry, workerState);

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

RUVIA_TEST(sansio_driver_h2_real_dispatch_round_trip) {
    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool gotResponseHead = false;
    bool got404Status = false;
    bool hpackDecodeSucceeded = true;
    bool gotResponseEnd = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::RouteTable routes(worker.resource());  // empty -> 404
            // Test-owned defaults drive the production session's required wiring.
            co_await ruvia::asAwaitable(
                ruvia::test::runBarePlainHttp2SansIoSession(sock, routes, worker, "127.0.0.1"));
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

            if (!co_await writeAll(kClientPreface)) {
                co_return;
            }
            if (!co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) {
                co_return;
            }

            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "GET");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/missing");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            if (!co_await writeAll(frame(0x1,
                    sansio_driver_test::kFlagEndStream | sansio_driver_test::kFlagEndHeaders, 1,
                    std::string_view(headerBlock.data(), headerBlock.size())))) {
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
                    std::string status;
                    const auto decoded = decoder.decode(payload,
                        [&status](std::string_view name, std::string_view value) {
                            if (name == ":status") {
                                status.assign(value);
                            }
                            return true;
                        });
                    hpackDecodeSucceeded = decoded.decoded();
                    got404Status = hpackDecodeSucceeded && status == "404";
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
    RUVIA_CHECK(got404Status);
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
            impl.registerRoute(ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/empty", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(nullptr, &noContentHandler),
                ruvia::detail::RequestBodyMode::kBuffered,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::asAwaitable(ruvia::test::runBarePlainHttp2SansIoSession(
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

            if (!co_await writeAll(kClientPreface)) {
                co_return;
            }
            if (!co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) {
                co_return;
            }

            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "GET");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/empty");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            HpackEncoder::encodeHeader(
                headerBlock, "accept-encoding", "identity;q=0, gzip;q=0, br;q=0, zstd;q=0");
            if (!co_await writeAll(frame(0x1,
                    sansio_driver_test::kFlagEndStream | sansio_driver_test::kFlagEndHeaders, 1,
                    std::string_view(headerBlock.data(), headerBlock.size())))) {
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
                if (header.streamId != 1) {
                    continue;
                }
                if (header.type == static_cast<std::uint8_t>(ruvia::Http2FrameType::kHeaders)) {
                    HpackCollect fields;
                    (void)decoder.decode(payload, [&fields](std::string_view name, std::string_view value) { return HpackCollect::onHeader(&fields, name, value); });
                    gotNoContentHead = fields.joined.contains(":status=204;");
                } else if (header.type == static_cast<std::uint8_t>(ruvia::Http2FrameType::kData)) {
                    sawData = true;
                }
                if ((header.flags & sansio_driver_test::kFlagEndStream) != 0) {
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
            impl.registerRoute(ruvia::HttpKnownMethod::kPost,
                std::pmr::string("/echo", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(nullptr, &echoHandler),
                ruvia::detail::RequestBodyMode::kBuffered,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::asAwaitable(ruvia::test::runBarePlainHttp2SansIoSession(
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

            if (!co_await writeAll(kClientPreface)) {
                co_return;
            }
            if (!co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {}))) {
                co_return;
            }

            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "POST");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/echo");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "http");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            // HEADERS (END_HEADERS, body follows) then DATA "hello" (END_STREAM).
            if (!co_await writeAll(frame(0x1, sansio_driver_test::kFlagEndHeaders, 1,
                    std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }
            if (!co_await writeAll(
                    frame(0x0 /*DATA*/, sansio_driver_test::kFlagEndStream, 1, "hello"))) {
                co_return;
            }

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
                if (header.type == static_cast<std::uint8_t>(ruvia::Http2FrameType::kData) &&
                    header.streamId == 1 && !payload.empty()) {
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
            impl.registerRoute(ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/slow", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(&io, &slowHandler),
                ruvia::detail::RequestBodyMode::kBuffered,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.registerRoute(ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/fast", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(nullptr, &fastHandler),
                ruvia::detail::RequestBodyMode::kBuffered,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::asAwaitable(ruvia::test::runBarePlainHttp2SansIoSession(
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
            auto requestOn = [](std::uint32_t streamId, std::string_view path) {
                std::pmr::string block(std::pmr::get_default_resource());
                HpackEncoder::encodeHeader(block, ":method", "GET");
                HpackEncoder::encodeHeader(block, ":path", path);
                HpackEncoder::encodeHeader(block, ":scheme", "http");
                HpackEncoder::encodeHeader(block, ":authority", "localhost");
                return frame(0x1,
                    sansio_driver_test::kFlagEndStream | sansio_driver_test::kFlagEndHeaders,
                    streamId, std::string_view(block.data(), block.size()));
            };

            if (!co_await writeAll(kClientPreface)) {
                co_return;
            }
            if (!co_await writeAll(frame(0x4, 0, 0, {}))) {
                co_return;
            }
            if (!co_await writeAll(requestOn(1, "/slow"))) {
                co_return;  // slow first
            }
            if (!co_await writeAll(requestOn(3, "/fast"))) {
                co_return;  // fast second
            }

            while (replies.size() < 2) {
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
                if (header.type == static_cast<std::uint8_t>(ruvia::Http2FrameType::kData) &&
                    !payload.empty()) {
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
            impl.registerRoute(ruvia::HttpKnownMethod::kPost,
                std::pmr::string("/upload", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(&observation, &terminatedBodyHandler),
                ruvia::detail::RequestBodyMode::kStream,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::asAwaitable(ruvia::test::runBarePlainHttp2SansIoSession(
                sock, impl.routeTable(), worker, "127.0.0.1"));
            observation.sessionReturnedAfterHandler = observation.handlerFinished;
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
            const auto writeAll = [&sock](std::string_view bytes) -> asio::awaitable<bool> {
                const auto [ec, count] = co_await asio::async_write(sock,
                    asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
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
            if (!co_await writeAll(frame(0x1, sansio_driver_test::kFlagEndHeaders, 1,
                    std::string_view(headerBlock.data(), headerBlock.size())))) {
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
            impl.registerRoute(ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/ping", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(nullptr, &echoHandler),
                ruvia::detail::RequestBodyMode::kBuffered,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            ruvia::test::Http2SansIoSessionFixture fixture;
            auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 64});
            const auto workerHandle = attachment.loop().handle();
            fixture.options.maxRequestsPerConnection = 1;
            co_await ruvia::asAwaitable(ruvia::detail::runHttp2SansIoSession(sock,
                impl.routeTable(), worker,
                fixture.context(fixture.services(workerHandle).withPlainTransport("127.0.0.1"))));
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
            auto readFrameInto = [&readExact](ruvia::Http2FrameHeader& header,
                                     std::string& payload) -> asio::awaitable<bool> {
                char headerBytes[ruvia::kHttp2FrameHeaderBytes];
                if (!co_await readExact(headerBytes, sizeof(headerBytes))) {
                    co_return false;
                }
                header = sansio_driver_test::parseFrameHeader(
                    std::string_view(headerBytes, sizeof(headerBytes)));
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
                co_return co_await writeAll(frame(0x1 /*HEADERS*/,
                    sansio_driver_test::kFlagEndStream | sansio_driver_test::kFlagEndHeaders,
                    streamId, std::string_view(headerBlock.data(), headerBlock.size())));
            };

            if (!co_await writeAll(kClientPreface) ||
                !co_await writeAll(frame(0x4 /*SETTINGS*/, 0, 0, {})) || !co_await sendGet(1)) {
                co_return;
            }

            ruvia::Http2FrameHeader header{};
            std::string payload;
            while (!sawGoawayNoError || !sawResponseEnd) {
                if (!co_await readFrameInto(header, payload)) {
                    co_return;
                }
                if (header.type == static_cast<std::uint8_t>(ruvia::Http2FrameType::kGoaway) &&
                    header.streamId == 0 && payload.size() >= 8) {
                    const auto* bytes = reinterpret_cast<const unsigned char*>(payload.data());
                    const auto lastStreamId = sansio_driver_test::readBigEndian31(bytes);
                    const auto errorCode = sansio_driver_test::readBigEndian32(bytes + 4);
                    sawGoawayNoError = lastStreamId == 1 && errorCode == 0;
                    continue;
                }
                if (header.streamId == 1 &&
                    (header.flags & sansio_driver_test::kFlagEndStream) != 0) {
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
                if (header.type == static_cast<std::uint8_t>(ruvia::Http2FrameType::kRstStream) &&
                    header.streamId == 3 && payload.size() == 4) {
                    const auto* bytes = reinterpret_cast<const unsigned char*>(payload.data());
                    sawRefusedStream =
                        sansio_driver_test::readBigEndian32(bytes) ==
                        static_cast<std::uint32_t>(ruvia::Http2ErrorCode::kRefusedStream);
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
