#include <chrono>
#include <concepts>
#include <memory>
#include <stdexcept>

#include <asio/io_context.hpp>

#include "ruvia/core/ConnectionScanner.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/Timer.h"
#include "ruvia/http/Hpack.h"
#include "ruvia/http/Http2Framing.h"
#include "ruvia/http/Http2Types.h"

#include "context/ContextServices.h"
#include "http2/Http2SansIoSession.h"
#include "http2/Http2SansIoSessionLifecycle.h"
#include "http2_sansio_session_fixture.h"
#include "router/RouteTable.h"
#include "router/Router.h"
#include "router/RouterImpl.h"
#include "sansio_driver_fixture.h"
#include "test_io_context.h"

// Sans-I/O HTTP/2 driver: connection setup, round trips, multiplexing and teardown.

RUVIA_TEST(http2_writer_completion_survives_reader_transition_order) {
    for (const bool write_failure : {false, true}) {
        for (const bool complete_before_stopping : {false, true}) {
            ruvia::detail::Http2SansIoSessionLifecycle lifecycle;
            RUVIA_CHECK(!lifecycle.writer_join_pending());
            RUVIA_CHECK(!lifecycle.writerDone());
            lifecycle.mark_writer_submitted();
            // Submission, not entry into the task body, establishes the join.
            RUVIA_CHECK(lifecycle.writer_join_pending());
            if (write_failure) {
                lifecycle.markWriteFailed();
            }
            if (complete_before_stopping) {
                lifecycle.markWriterDone();
                RUVIA_CHECK(lifecycle.writerDone());
                RUVIA_CHECK(!lifecycle.stopping());
            }
            lifecycle.beginStopping();
            if (!complete_before_stopping) {
                RUVIA_CHECK(lifecycle.writer_join_pending());
                lifecycle.markWriterDone();
            }
            RUVIA_CHECK(lifecycle.stopping());
            RUVIA_CHECK(lifecycle.writerDone());
            RUVIA_CHECK(!lifecycle.writer_join_pending());
            RUVIA_CHECK(lifecycle.writeFailed() == write_failure);
        }
    }
}

RUVIA_TEST(http2_writer_unsuccessful_submission_has_no_join_obligation) {
    ruvia::detail::Http2SansIoSessionLifecycle lifecycle;
    lifecycle.mark_writer_submitted();
    lifecycle.mark_writer_launch_failed();
    lifecycle.beginStopping();
    RUVIA_CHECK(!lifecycle.writer_join_pending());
    RUVIA_CHECK(!lifecycle.writerDone());
    RUVIA_CHECK(lifecycle.stopping());
}

RUVIA_TEST(http2_writer_failure_is_retained_after_early_completion) {
    ruvia::detail::Http2SansIoSessionLifecycle lifecycle;
    lifecycle.mark_writer_submitted();
    lifecycle.recordWriterFailure(std::make_exception_ptr(std::runtime_error("writer allocation failure")));
    lifecycle.markWriterDone();
    lifecycle.beginStopping();
    bool observed_failure = false;
    try {
        lifecycle.rethrowWriterFailure();
    } catch (const std::runtime_error& error) {
        observed_failure = std::string_view(error.what()) == "writer allocation failure";
    }
    RUVIA_CHECK(observed_failure);
    RUVIA_CHECK(lifecycle.writerDone());
    RUVIA_CHECK(!lifecycle.writer_join_pending());
}

RUVIA_TEST(sansio_driver_h2_inactivity_phase_counts_predispatch_runtime) {
    using Phase = ruvia::ConnectionScanner::Phase;
    RUVIA_CHECK(ruvia::detail::http2SansIoInactivityPhase(true, 0, false) ==
                Phase::kReadingInitial);
    RUVIA_CHECK(ruvia::detail::http2SansIoInactivityPhase(false, 0, false) == Phase::kIdle);
    RUVIA_CHECK(ruvia::detail::http2SansIoInactivityPhase(false, 1, false) ==
                Phase::kReadingPayload);
    RUVIA_CHECK(ruvia::detail::http2SansIoInactivityPhase(false, 1, true) == Phase::kLongLived);
    RUVIA_CHECK(ruvia::detail::http2SansIoInactivityPhase(true, 1, true) ==
                Phase::kReadingInitial);
}

RUVIA_TEST(sansio_driver_h2_session_context_owns_complete_wiring) {
    auto& ioContext = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(ioContext, {.queue_capacity = 8});
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
    bool gotAltSvc = false;
    bool hpackDecodeSucceeded = true;
    bool gotResponseEnd = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::WorkerMemory worker;
            ruvia::detail::RouteTable routes(worker.resource());  // empty -> 404
            // The sans-I/O unit supplies the same TLS listener response policy
            // that HttpServerSessionEntry attaches after a real handshake.
            co_await ruvia::asAwaitable(ruvia::test::runBareHttp2SansIoSessionWith(
                sock, routes, worker,
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
                        [&status, &gotAltSvc](std::string_view name, std::string_view value) {
                            if (name == ":status") {
                                status.assign(value);
                            } else if (name == "alt-svc" &&
                                       value == "h3=\":443\"; ma=86400") {
                                gotAltSvc = true;
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
    RUVIA_CHECK(gotAltSvc);
    RUVIA_CHECK(gotResponseEnd);
}

RUVIA_TEST(sansio_driver_h2_context_sends_origin_and_altsvc_and_observes_priority_update) {
    auto& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const auto port = acceptor.local_endpoint().port();
    bool originsReceived = false, serviceReceived = false, responseEnded = false, priorityObserved = false;
    std::exception_ptr failure;
    const auto completion = [&](std::exception_ptr error) {
        if (error && !failure) {
            failure = error;
        }
    };
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
        ruvia::WorkerMemory worker;
        ruvia::detail::Router router;
        auto& implementation = ruvia::detail::RouterImpl::from(router);
        implementation.registerRoute(ruvia::HttpKnownMethod::kGet,
            std::pmr::string("/advertise", worker.resource()),
            ruvia::detail::RouteHandler(&priorityObserved,
                +[](void* raw, ruvia::Context& context) -> ruvia::Task<ruvia::HttpResponse> {
                    const std::array<std::string_view, 1> origins{"https://example.test"};
                    co_await context.advertiseOrigins(origins);
                    co_await context.advertiseAlternativeService("h3=\":443\"; ma=60");
                    (void)co_await ruvia::sleepFor(context.worker(), std::chrono::milliseconds(1));
                    const auto priority = context.req().priority();
                    *static_cast<bool*>(raw) = priority.urgency == 1 && priority.incremental;
                    co_return context.text("advertised");
                }),
            ruvia::detail::RequestBodyMode::kBuffered,
            std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
            std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
        implementation.finalize();
        co_await ruvia::asAwaitable(ruvia::test::runBareTlsHttp2SansIoSession(
            socket, implementation.routeTable(), worker, "127.0.0.1")); }, completion);
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        tcp::socket socket(io);
        co_await socket.async_connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
        auto connection = ruvia::Http2Connection::client({.receiveOriginAdvertisements = true});
        const std::array fields{ruvia::HttpHeaderView("priority", "u=6")};
        const auto request = connection.submitRequestHead(ruvia::Http2RegularRequestHeadView{
            .authority = ruvia::BorrowedText("example.test"), .target = "/advertise", .headers = fields});
        if (!request.submitted() || connection.submitPriorityUpdate(request.submitted()->streamId(),
                                        {.urgency = 1, .incremental = true}) != ruvia::Http2SubmitStatus::kAccepted) {
            throw std::runtime_error("could not submit advertisement request and priority update");
        }
        const auto flush = [&]() -> asio::awaitable<void> {
            const auto output = connection.pendingOutput();
            if (!output.empty()) {
                co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
                (void)connection.consumeOutput(output.size());
            }
        };
        co_await flush();
        while (!responseEnded) {
            std::array<char, 16384> input{};
            const auto size = co_await socket.async_read_some(asio::buffer(input), asio::use_awaitable);
            if (connection.feed(std::string_view(input.data(), size)) == ruvia::Http2FeedResult::kProtocolFailure) {
                throw std::runtime_error("advertisement peer sent invalid HTTP/2 frames");
            }
            while (auto event = connection.nextEvent()) {
                if (const auto* origins = event->originAdvertisement()) {
                    originsReceived = origins->origins.size() == 1 && origins->origins[0] == "https://example.test";
                }
                if (const auto* service = event->alternativeServiceAdvertisement()) {
                    serviceReceived = service->streamId == 1 && service->origin.empty() && service->fieldValue == "h3=\":443\"; ma=60";
                }
                if (const auto* end = event->messageEnd()) {
                    responseEnded = end->streamId() == 1;
                }
            }
            co_await flush();
        }
        closeClientSocket(socket); }, completion);
    io.run();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK(originsReceived && serviceReceived && responseEnded && priorityObserved);
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
            auto attachment = ruvia::attachEventLoop(io, {.queue_capacity = 64});
            const auto workerHandle = attachment.loop().handle();
            fixture.options.max_requests_per_connection = 1;
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
