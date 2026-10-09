#include <chrono>
#include <concepts>
#include <memory>
#include <stdexcept>
#include <string_view>

#include <asio/io_context.hpp>

#include "ruvia/core/connection_scanner.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/timer.h"
#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http2_types.h"

#include "context/context_services.h"
#include "http2/http2_sans_io_session.h"
#include "http2/http2_sans_io_session_lifecycle.h"
#include "http2_sansio_session_fixture.h"
#include "router/route_table.h"
#include "router/router.h"
#include "router/router_impl.h"
#include "sansio_driver_fixture.h"
#include "test_io_context.h"

// Sans-I/O HTTP/2 driver: connection setup, round trips, multiplexing and teardown.

RUVIA_TEST(http2_writer_completion_survives_reader_transition_order) {
    for (const bool write_failure : {false, true}) {
        for (const bool complete_before_stopping : {false, true}) {
            ruvia::detail::http2_sans_io_session_lifecycle lifecycle;
            RUVIA_CHECK(!lifecycle.writer_join_pending());
            RUVIA_CHECK(!lifecycle.writer_done());
            lifecycle.mark_writer_submitted();
            // Submission, not entry into the task body, establishes the join.
            RUVIA_CHECK(lifecycle.writer_join_pending());
            if (write_failure) {
                lifecycle.mark_write_failed();
            }
            if (complete_before_stopping) {
                lifecycle.mark_writer_done();
                RUVIA_CHECK(lifecycle.writer_done());
                RUVIA_CHECK(!lifecycle.stopping());
            }
            lifecycle.begin_stopping();
            if (!complete_before_stopping) {
                RUVIA_CHECK(lifecycle.writer_join_pending());
                lifecycle.mark_writer_done();
            }
            RUVIA_CHECK(lifecycle.stopping());
            RUVIA_CHECK(lifecycle.writer_done());
            RUVIA_CHECK(!lifecycle.writer_join_pending());
            RUVIA_CHECK(lifecycle.write_failed() == write_failure);
        }
    }
}

RUVIA_TEST(http2_writer_unsuccessful_submission_has_no_join_obligation) {
    ruvia::detail::http2_sans_io_session_lifecycle lifecycle;
    lifecycle.mark_writer_submitted();
    lifecycle.mark_writer_launch_failed();
    lifecycle.begin_stopping();
    RUVIA_CHECK(!lifecycle.writer_join_pending());
    RUVIA_CHECK(!lifecycle.writer_done());
    RUVIA_CHECK(lifecycle.stopping());
}

RUVIA_TEST(http2_writer_failure_is_retained_after_early_completion) {
    ruvia::detail::http2_sans_io_session_lifecycle lifecycle;
    lifecycle.mark_writer_submitted();
    lifecycle.record_writer_failure(std::make_exception_ptr(std::runtime_error("writer allocation failure")));
    lifecycle.mark_writer_done();
    lifecycle.begin_stopping();
    bool observed_failure = false;
    try {
        lifecycle.rethrow_writer_failure();
    } catch (const std::runtime_error& error) {
        observed_failure = std::string_view(error.what()) == "writer allocation failure";
    }
    RUVIA_CHECK(observed_failure);
    RUVIA_CHECK(lifecycle.writer_done());
    RUVIA_CHECK(!lifecycle.writer_join_pending());
}

RUVIA_TEST(sansio_driver_h2_inactivity_phase_counts_predispatch_runtime) {
    using phase_type = ruvia::connection_scanner::phase_type;
    RUVIA_CHECK(ruvia::detail::http2_sans_io_inactivity_phase(true, 0, false) ==
                phase_type::reading_initial);
    RUVIA_CHECK(ruvia::detail::http2_sans_io_inactivity_phase(false, 0, false) == phase_type::idle);
    RUVIA_CHECK(ruvia::detail::http2_sans_io_inactivity_phase(false, 1, false) ==
                phase_type::reading_payload);
    RUVIA_CHECK(ruvia::detail::http2_sans_io_inactivity_phase(false, 1, true) == phase_type::long_lived);
    RUVIA_CHECK(ruvia::detail::http2_sans_io_inactivity_phase(true, 1, true) ==
                phase_type::reading_initial);
}

RUVIA_TEST(sansio_driver_h2_session_context_owns_complete_wiring) {
    auto& io_context = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    ruvia::detail::http_server_options options;
    ruvia::connection_scanner::entry_type scanner_entry;
    auto worker_state = ruvia::detail::http_server_worker_state::running;
    const ruvia::stop_token stop_token;

    const ruvia::detail::http2_sans_io_session_context session_value(
        ruvia::detail::context_services(worker_value, stop_token)
            .with_tls_transport("192.0.2.1", "CN=test-client"),
        options, scanner_entry, worker_state);

    RUVIA_CHECK(&session_value.options() == &options);
    RUVIA_CHECK(&session_value.scanner_entry() == &scanner_entry);
    RUVIA_CHECK(session_value.worker_running());
    worker_state = ruvia::detail::http_server_worker_state::stopped;
    RUVIA_CHECK(!session_value.worker_running());
    const auto& services = session_value.services();
    RUVIA_CHECK(services.get_conn_info().remote().address() == "192.0.2.1");
    RUVIA_CHECK(services.get_conn_info().plain() == nullptr);
    RUVIA_CHECK(services.get_conn_info().tls() != nullptr);
    RUVIA_CHECK(services.get_conn_info().tls()->client_certificate_subject() == "CN=test-client");
}

RUVIA_TEST(sansio_driver_h2_real_dispatch_round_trip) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool got_response_head = false;
    bool got404_status = false;
    bool got_alt_svc = false;
    bool hpack_decode_succeeded = true;
    bool got_response_end = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory worker;
            ruvia::detail::route_table routes_value(worker.resource());  // empty -> 404
            // The sans-I/O unit supplies the same TLS listener response policy
            // that HttpServerSessionEntry attaches after a real handshake.
            co_await ruvia::as_awaitable(ruvia::test::run_bare_http2_sans_io_session_with(
                sock, routes_value, worker,
                [](ruvia::detail::context_services services) {
                    return services.with_tls_transport("127.0.0.1")
                        .with_automatic_alt_svc("h3=\":443\"; ma=86400");
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

            auto write_all = [&sock](std::string_view bytes_value) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(sock,
                    asio::buffer(bytes_value.data(), bytes_value.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto read_exact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await write_all(client_preface)) {
                co_return;
            }
            if (!co_await write_all(frame(0x4 /*SETTINGS*/, 0, 0, {}))) {
                co_return;
            }

            std::pmr::string header_block(std::pmr::get_default_resource());
            hpack_encoder::encode_header(header_block, ":method", "GET");
            hpack_encoder::encode_header(header_block, ":path", "/missing");
            hpack_encoder::encode_header(header_block, ":scheme", "http");
            hpack_encoder::encode_header(header_block, ":authority", "localhost");
            if (!co_await write_all(frame(0x1,
                    sansio_driver_test::flag_end_stream | sansio_driver_test::flag_end_headers, 1,
                    std::string_view(header_block.data(), header_block.size())))) {
                co_return;
            }

            ruvia::hpack_decoder decoder({.resource_ = std::pmr::get_default_resource()});
            for (;;) {
                char header_bytes[ruvia::http2_frame_header_bytes];
                if (!co_await read_exact(header_bytes, sizeof(header_bytes))) {
                    break;
                }
                const auto header_value = sansio_driver_test::parse_frame_header(
                    std::string_view(header_bytes, sizeof(header_bytes)));
                std::string payload_value(header_value.length_, '\0');
                if (header_value.length_ != 0 && !co_await read_exact(payload_value.data(), payload_value.size())) {
                    break;
                }
                if (header_value.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers) &&
                    header_value.stream_id_ == 1) {
                    got_response_head = true;
                    std::string status;
                    const auto decoded = decoder.decode(payload_value,
                        [&status, &got_alt_svc](std::string_view name, std::string_view value) {
                            if (name == ":status") {
                                status.assign(value);
                            } else if (name == "alt-svc" &&
                                       value == "h3=\":443\"; ma=86400") {
                                got_alt_svc = true;
                            }
                            return true;
                        });
                    hpack_decode_succeeded = decoded.decoded();
                    got404_status = hpack_decode_succeeded && status == "404";
                }
                if (header_value.stream_id_ == 1 &&
                    (header_value.flags_ & sansio_driver_test::flag_end_stream) != 0) {
                    got_response_end = true;
                    break;
                }
            }

            close_client_socket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(got_response_head);
    RUVIA_CHECK(hpack_decode_succeeded);
    RUVIA_CHECK(got404_status);
    RUVIA_CHECK(got_alt_svc);
    RUVIA_CHECK(got_response_end);
}

RUVIA_TEST(sansio_driver_h2_context_sends_origin_and_altsvc_and_observes_priority_update) {
    auto& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const auto port = acceptor.local_endpoint().port();
    bool origins_received = false, service_received = false, response_ended = false, priority_observed = false;
    std::exception_ptr failure;
    const auto completion = [&](std::exception_ptr error) {
        if (error && !failure) {
            failure = error;
        }
    };
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
        ruvia::worker_memory worker;
        ruvia::detail::router router;
        auto& implementation = ruvia::detail::router_impl::from(router);
        implementation.register_route(ruvia::http_known_method::get,
            std::pmr::string("/advertise", worker.resource()),
            ruvia::detail::route_handler_type(&priority_observed,
                +[](void* raw, ruvia::context& context_value) -> ruvia::task<ruvia::http_response> {
                    const std::array<std::string_view, 1> origins{"https://example.test"};
                    co_await context_value.advertise_origins(origins);
                    co_await context_value.advertise_alternative_service("h3=\":443\"; ma=60");
                    (void)co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(1));
                    const auto priority = context_value.req().priority();
                    *static_cast<bool*>(raw) = priority.urgency_ == 1 && priority.incremental_;
                    co_return context_value.text("advertised");
                }),
            ruvia::detail::request_body_mode::buffered,
            std::span<const ruvia::detail::controller_middleware_descriptor>{},
            std::span<const ruvia::detail::controller_middleware_descriptor>{});
        implementation.finalize();
        co_await ruvia::as_awaitable(ruvia::test::run_bare_tls_http2_sans_io_session(
            socket, implementation.route_table(), worker, "127.0.0.1")); }, completion);
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        tcp::socket socket(io);
        co_await socket.async_connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
        auto connection = ruvia::http2_connection::client({.receive_origin_advertisements_ = true});
        const std::array fields_value{ruvia::http_header_view("priority", "u=6")};
        const auto request = connection.submit_request_head(ruvia::http2_regular_request_head_view{
            .authority_ = ruvia::borrowed_text("example.test"), .target_ = "/advertise", .headers_ = fields_value});
        if (!request.submitted() || connection.submit_priority_update(request.submitted()->stream_id(),
                                        {.urgency_ = 1, .incremental_ = true}) != ruvia::http2_submit_status::accepted) {
            throw std::runtime_error("could not submit advertisement request and priority update");
        }
        const auto flush = [&]() -> asio::awaitable<void> {
            const auto output = connection.pending_output();
            if (!output.empty()) {
                co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
                (void)connection.consume_output(output.size());
            }
        };
        co_await flush();
        while (!response_ended) {
            std::array<char, 16384> input{};
            const auto size = co_await socket.async_read_some(asio::buffer(input), asio::use_awaitable);
            if (connection.feed(std::string_view(input.data(), size)) == ruvia::http2_feed_result::protocol_failure) {
                throw std::runtime_error("advertisement peer sent invalid HTTP/2 frames");
            }
            while (auto event = connection.next_event()) {
                if (const auto* origins = event->origin_advertisement()) {
                    origins_received = origins->origins_.size() == 1 && origins->origins_[0] == "https://example.test";
                }
                if (const auto* service = event->alternative_service_advertisement()) {
                    service_received = service->stream_id_ == 1 && service->origin_.empty() && service->field_value_ == "h3=\":443\"; ma=60";
                }
                if (const auto* end = event->message_end()) {
                    response_ended = end->stream_id() == 1;
                }
            }
            co_await flush();
        }
        close_client_socket(socket); }, completion);
    io.run();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK(origins_received && service_received && response_ended && priority_observed);
}

RUVIA_TEST(sansio_driver_h2_bodyless_response_survives_empty_accept_encoding_set) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool got_no_content_head = false;
    bool got_end_stream = false;
    bool saw_data = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory worker;
            ruvia::detail::router router;
            auto& impl = ruvia::detail::router_impl::from(router);
            impl.register_route(ruvia::http_known_method::get,
                std::pmr::string("/empty", std::pmr::get_default_resource()),
                ruvia::detail::route_handler_type(nullptr, &no_content_handler),
                ruvia::detail::request_body_mode::buffered,
                std::span<const ruvia::detail::controller_middleware_descriptor>{},
                std::span<const ruvia::detail::controller_middleware_descriptor>{});
            impl.finalize();
            co_await ruvia::as_awaitable(ruvia::test::run_bare_plain_http2_sans_io_session(
                sock, impl.route_table(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);

            auto write_all = [&sock](std::string_view bytes_value) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(sock,
                    asio::buffer(bytes_value.data(), bytes_value.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto read_exact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await write_all(client_preface)) {
                co_return;
            }
            if (!co_await write_all(frame(0x4 /*SETTINGS*/, 0, 0, {}))) {
                co_return;
            }

            std::pmr::string header_block(std::pmr::get_default_resource());
            hpack_encoder::encode_header(header_block, ":method", "GET");
            hpack_encoder::encode_header(header_block, ":path", "/empty");
            hpack_encoder::encode_header(header_block, ":scheme", "http");
            hpack_encoder::encode_header(header_block, ":authority", "localhost");
            hpack_encoder::encode_header(
                header_block, "accept-encoding", "identity;q=0, gzip;q=0, br;q=0, zstd;q=0");
            if (!co_await write_all(frame(0x1,
                    sansio_driver_test::flag_end_stream | sansio_driver_test::flag_end_headers, 1,
                    std::string_view(header_block.data(), header_block.size())))) {
                co_return;
            }

            ruvia::hpack_decoder decoder({.resource_ = std::pmr::get_default_resource()});
            for (;;) {
                char header_bytes[ruvia::http2_frame_header_bytes];
                if (!co_await read_exact(header_bytes, sizeof(header_bytes))) {
                    break;
                }
                const auto header_value = sansio_driver_test::parse_frame_header(
                    std::string_view(header_bytes, sizeof(header_bytes)));
                std::string payload_value(header_value.length_, '\0');
                if (header_value.length_ != 0 && !co_await read_exact(payload_value.data(), payload_value.size())) {
                    break;
                }
                if (header_value.stream_id_ != 1) {
                    continue;
                }
                if (header_value.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                    hpack_collect fields;
                    (void)decoder.decode(payload_value, [&fields](std::string_view name, std::string_view value) { return hpack_collect::on_header(&fields, name, value); });
                    got_no_content_head = (fields.joined_.find(":status=204;") != std::string_view::npos);
                } else if (header_value.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                    saw_data = true;
                }
                if ((header_value.flags_ & sansio_driver_test::flag_end_stream) != 0) {
                    got_end_stream = true;
                    break;
                }
            }

            close_client_socket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(got_no_content_head);
    RUVIA_CHECK(got_end_stream);
    RUVIA_CHECK(!saw_data);
}

RUVIA_TEST(sansio_driver_h2_post_echo_real_handler) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::string echoed;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory worker;
            ruvia::detail::router router;
            auto& impl = ruvia::detail::router_impl::from(router);
            impl.register_route(ruvia::http_known_method::post,
                std::pmr::string("/echo", std::pmr::get_default_resource()),
                ruvia::detail::route_handler_type(nullptr, &echo_handler),
                ruvia::detail::request_body_mode::buffered,
                std::span<const ruvia::detail::controller_middleware_descriptor>{},
                std::span<const ruvia::detail::controller_middleware_descriptor>{});
            impl.finalize();
            co_await ruvia::as_awaitable(ruvia::test::run_bare_plain_http2_sans_io_session(
                sock, impl.route_table(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);

            auto write_all = [&sock](std::string_view bytes_value) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(sock,
                    asio::buffer(bytes_value.data(), bytes_value.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto read_exact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await write_all(client_preface)) {
                co_return;
            }
            if (!co_await write_all(frame(0x4 /*SETTINGS*/, 0, 0, {}))) {
                co_return;
            }

            std::pmr::string header_block(std::pmr::get_default_resource());
            hpack_encoder::encode_header(header_block, ":method", "POST");
            hpack_encoder::encode_header(header_block, ":path", "/echo");
            hpack_encoder::encode_header(header_block, ":scheme", "http");
            hpack_encoder::encode_header(header_block, ":authority", "localhost");
            // HEADERS (END_HEADERS, body follows) then DATA "hello" (END_STREAM).
            if (!co_await write_all(frame(0x1, sansio_driver_test::flag_end_headers, 1,
                    std::string_view(header_block.data(), header_block.size())))) {
                co_return;
            }
            if (!co_await write_all(
                    frame(0x0 /*DATA*/, sansio_driver_test::flag_end_stream, 1, "hello"))) {
                co_return;
            }

            for (;;) {
                char header_bytes[ruvia::http2_frame_header_bytes];
                if (!co_await read_exact(header_bytes, sizeof(header_bytes))) {
                    break;
                }
                const auto header_value = sansio_driver_test::parse_frame_header(
                    std::string_view(header_bytes, sizeof(header_bytes)));
                std::string payload_value(header_value.length_, '\0');
                if (header_value.length_ != 0 && !co_await read_exact(payload_value.data(), payload_value.size())) {
                    break;
                }
                if (header_value.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) &&
                    header_value.stream_id_ == 1 && !payload_value.empty()) {
                    echoed = payload_value;
                    break;
                }
            }

            close_client_socket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(echoed == "handler-ran");
}

RUVIA_TEST(sansio_driver_h2_concurrent_streams_multiplex) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::vector<std::pair<std::uint32_t, std::string>> replies;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory worker;
            ruvia::detail::router router;
            auto& impl = ruvia::detail::router_impl::from(router);
            impl.register_route(ruvia::http_known_method::get,
                std::pmr::string("/slow", std::pmr::get_default_resource()),
                ruvia::detail::route_handler_type(&io, &slow_handler),
                ruvia::detail::request_body_mode::buffered,
                std::span<const ruvia::detail::controller_middleware_descriptor>{},
                std::span<const ruvia::detail::controller_middleware_descriptor>{});
            impl.register_route(ruvia::http_known_method::get,
                std::pmr::string("/fast", std::pmr::get_default_resource()),
                ruvia::detail::route_handler_type(nullptr, &fast_handler),
                ruvia::detail::request_body_mode::buffered,
                std::span<const ruvia::detail::controller_middleware_descriptor>{},
                std::span<const ruvia::detail::controller_middleware_descriptor>{});
            impl.finalize();
            co_await ruvia::as_awaitable(ruvia::test::run_bare_plain_http2_sans_io_session(
                sock, impl.route_table(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);

            auto write_all = [&sock](std::string_view bytes_value) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(sock,
                    asio::buffer(bytes_value.data(), bytes_value.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto read_exact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };
            auto request_on = [](std::uint32_t stream_id, std::string_view path) {
                std::pmr::string block(std::pmr::get_default_resource());
                hpack_encoder::encode_header(block, ":method", "GET");
                hpack_encoder::encode_header(block, ":path", path);
                hpack_encoder::encode_header(block, ":scheme", "http");
                hpack_encoder::encode_header(block, ":authority", "localhost");
                return frame(0x1,
                    sansio_driver_test::flag_end_stream | sansio_driver_test::flag_end_headers,
                    stream_id, std::string_view(block.data(), block.size()));
            };

            if (!co_await write_all(client_preface)) {
                co_return;
            }
            if (!co_await write_all(frame(0x4, 0, 0, {}))) {
                co_return;
            }
            if (!co_await write_all(request_on(1, "/slow"))) {
                co_return;  // slow first
            }
            if (!co_await write_all(request_on(3, "/fast"))) {
                co_return;  // fast second
            }

            while (replies.size() < 2) {
                char header_bytes[ruvia::http2_frame_header_bytes];
                if (!co_await read_exact(header_bytes, sizeof(header_bytes))) {
                    break;
                }
                const auto header_value = sansio_driver_test::parse_frame_header(
                    std::string_view(header_bytes, sizeof(header_bytes)));
                std::string payload_value(header_value.length_, '\0');
                if (header_value.length_ != 0 && !co_await read_exact(payload_value.data(), payload_value.size())) {
                    break;
                }
                if (header_value.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) &&
                    !payload_value.empty()) {
                    replies.emplace_back(header_value.stream_id_, payload_value);
                }
            }

            close_client_socket(sock);
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
    asio::io_context& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    terminated_body_observation observation_value{.io_ = &io};

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory worker;
            ruvia::detail::router router;
            auto& impl = ruvia::detail::router_impl::from(router);
            impl.register_route(ruvia::http_known_method::post,
                std::pmr::string("/upload", std::pmr::get_default_resource()),
                ruvia::detail::route_handler_type(&observation_value, &terminated_body_handler),
                ruvia::detail::request_body_mode::stream,
                std::span<const ruvia::detail::controller_middleware_descriptor>{},
                std::span<const ruvia::detail::controller_middleware_descriptor>{});
            impl.finalize();
            co_await ruvia::as_awaitable(ruvia::test::run_bare_plain_http2_sans_io_session(
                sock, impl.route_table(), worker, "127.0.0.1"));
            observation_value.session_returned_after_handler_ = observation_value.handler_finished_;
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
            const auto write_all = [&sock](std::string_view bytes_value) -> asio::awaitable<bool> {
                const auto [ec, count] = co_await asio::async_write(sock,
                    asio::buffer(bytes_value.data(), bytes_value.size()), asio::as_tuple(asio::use_awaitable));
                co_return !ec && count == bytes_value.size();
            };
            if (!co_await write_all(client_preface) || !co_await write_all(frame(0x4, 0, 0, {}))) {
                co_return;
            }
            std::pmr::string header_block(std::pmr::get_default_resource());
            hpack_encoder::encode_header(header_block, ":method", "POST");
            hpack_encoder::encode_header(header_block, ":path", "/upload");
            hpack_encoder::encode_header(header_block, ":scheme", "http");
            hpack_encoder::encode_header(header_block, ":authority", "localhost");
            if (!co_await write_all(frame(0x1, sansio_driver_test::flag_end_headers, 1,
                    std::string_view(header_block.data(), header_block.size())))) {
                co_return;
            }
            while (!observation_value.started_) {
                asio::steady_timer yield(io);
                yield.expires_after(std::chrono::milliseconds(1));
                (void)co_await yield.async_wait(asio::as_tuple(asio::use_awaitable));
            }
            close_client_socket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(observation_value.saw_transport_error_);
    RUVIA_CHECK(observation_value.handler_finished_);
    RUVIA_CHECK(observation_value.session_returned_after_handler_);
}

RUVIA_TEST(sansio_driver_h2_keepalive_requests_drains_connection) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool saw_goaway_no_error = false;
    bool saw_response_end = false;
    bool saw_refused_stream = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory worker;
            ruvia::detail::router router;
            auto& impl = ruvia::detail::router_impl::from(router);
            impl.register_route(ruvia::http_known_method::get,
                std::pmr::string("/ping", std::pmr::get_default_resource()),
                ruvia::detail::route_handler_type(nullptr, &echo_handler),
                ruvia::detail::request_body_mode::buffered,
                std::span<const ruvia::detail::controller_middleware_descriptor>{},
                std::span<const ruvia::detail::controller_middleware_descriptor>{});
            impl.finalize();
            ruvia::test::http2_sans_io_session_fixture fixture;
            auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 64});
            const auto worker_handle_value = attachment.loop().handle();
            fixture.options_.max_requests_per_connection_ = 1;
            co_await ruvia::as_awaitable(ruvia::detail::run_http2_sans_io_session(sock,
                impl.route_table(), worker,
                fixture.context(fixture.services(worker_handle_value).with_plain_transport("127.0.0.1"))));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);

            auto write_all = [&sock](std::string_view bytes_value) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(sock,
                    asio::buffer(bytes_value.data(), bytes_value.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto read_exact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };
            auto read_frame_into = [&read_exact](ruvia::http2_frame_header& header_value,
                                       std::string& payload_value) -> asio::awaitable<bool> {
                char header_bytes[ruvia::http2_frame_header_bytes];
                if (!co_await read_exact(header_bytes, sizeof(header_bytes))) {
                    co_return false;
                }
                header_value = sansio_driver_test::parse_frame_header(
                    std::string_view(header_bytes, sizeof(header_bytes)));
                payload_value.assign(header_value.length_, '\0');
                if (header_value.length_ != 0 && !co_await read_exact(payload_value.data(), payload_value.size())) {
                    co_return false;
                }
                co_return true;
            };
            auto send_get = [&write_all](std::uint32_t stream_id) -> asio::awaitable<bool> {
                std::pmr::string header_block(std::pmr::get_default_resource());
                hpack_encoder::encode_header(header_block, ":method", "GET");
                hpack_encoder::encode_header(header_block, ":path", "/ping");
                hpack_encoder::encode_header(header_block, ":scheme", "http");
                hpack_encoder::encode_header(header_block, ":authority", "localhost");
                co_return co_await write_all(frame(0x1 /*HEADERS*/,
                    sansio_driver_test::flag_end_stream | sansio_driver_test::flag_end_headers,
                    stream_id, std::string_view(header_block.data(), header_block.size())));
            };

            if (!co_await write_all(client_preface) ||
                !co_await write_all(frame(0x4 /*SETTINGS*/, 0, 0, {})) || !co_await send_get(1)) {
                co_return;
            }

            ruvia::http2_frame_header header_value{};
            std::string payload;
            while (!saw_goaway_no_error || !saw_response_end) {
                if (!co_await read_frame_into(header_value, payload)) {
                    co_return;
                }
                if (header_value.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway) &&
                    header_value.stream_id_ == 0 && payload.size() >= 8) {
                    const auto* bytes_value = reinterpret_cast<const unsigned char*>(payload.data());
                    const auto last_stream_id = sansio_driver_test::read_big_endian31(bytes_value);
                    const auto error_code = sansio_driver_test::read_big_endian32(bytes_value + 4);
                    saw_goaway_no_error = last_stream_id == 1 && error_code == 0;
                    continue;
                }
                if (header_value.stream_id_ == 1 &&
                    (header_value.flags_ & sansio_driver_test::flag_end_stream) != 0) {
                    saw_response_end = true;
                }
            }

            // The drained connection must refuse a stream above the advertised id.
            if (!co_await send_get(3)) {
                co_return;
            }
            while (!saw_refused_stream) {
                if (!co_await read_frame_into(header_value, payload)) {
                    co_return;
                }
                if (header_value.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream) &&
                    header_value.stream_id_ == 3 && payload.size() == 4) {
                    const auto* bytes_value = reinterpret_cast<const unsigned char*>(payload.data());
                    saw_refused_stream =
                        sansio_driver_test::read_big_endian32(bytes_value) ==
                        static_cast<std::uint32_t>(ruvia::http2_error_code::refused_stream);
                }
            }

            close_client_socket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(saw_goaway_no_error);
    RUVIA_CHECK(saw_response_end);
    RUVIA_CHECK(saw_refused_stream);
}
