#include <string_view>

#include "ruvia/core/event_loop_attachment.h"

#include "context/context_services.h"
#include "http2/http2_sans_io_session.h"
#include "http2_sansio_session_fixture.h"
#include "router/route_table.h"
#include "router/router.h"
#include "router/router_impl.h"
#include "sansio_driver_fixture.h"

// Sans-I/O HTTP/2 driver: request and response bodies: pacing, streaming and trailers.

RUVIA_TEST(sansio_driver_h2_expectation_decision_precedes_request_content) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool got_continue = false;
    bool continue_ended_stream = false;
    bool got_unsupported_final = false;
    bool got_supported_final = false;
    std::string supported_body;
    stream_access_observation access_observation;

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
            ruvia::test::http2_sans_io_session_fixture fixture;
            auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 64});
            const auto worker_handle_value = attachment.loop().handle();
            fixture.options_.access_log_.callback_ =
                ruvia::detail::callback_access::bind<void(const ruvia::access_log_record&) noexcept>(
                    access_observation);
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
                const auto [ec, n] = co_await asio::async_write(sock,
                    asio::buffer(bytes_value.data(), bytes_value.size()), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == bytes_value.size();
            };
            auto read_exact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                const auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await write_all(client_preface) ||
                !co_await write_all(frame(0x4 /*SETTINGS*/, 0, 0, {}))) {
                co_return;
            }

            const auto make_head = [](std::string_view expect) {
                std::pmr::string block(std::pmr::get_default_resource());
                hpack_encoder::encode_header(block, ":method", "POST");
                hpack_encoder::encode_header(block, ":path", "/echo");
                hpack_encoder::encode_header(block, ":scheme", "http");
                hpack_encoder::encode_header(block, ":authority", "localhost");
                hpack_encoder::encode_header(block, "content-length", "5");
                hpack_encoder::encode_header(block, "expect", expect);
                return block;
            };
            const auto continue_head = make_head(", 100-continue,");
            const auto unsupported_head = make_head("custom-feature");
            if (!co_await write_all(frame(0x1 /*HEADERS*/, sansio_driver_test::flag_end_headers, 1,
                    std::string_view(continue_head.data(), continue_head.size()))) ||
                !co_await write_all(frame(0x1 /*HEADERS*/, sansio_driver_test::flag_end_headers, 3,
                    std::string_view(unsupported_head.data(), unsupported_head.size())))) {
                co_return;
            }

            ruvia::hpack_decoder decoder({.resource_ = std::pmr::get_default_resource()});
            bool content_sent = false;
            bool supported_ended = false;
            while (!(got_continue && got_unsupported_final && supported_ended)) {
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

                if (header_value.type_ == static_cast<std::uint8_t>(http2_frame_type::headers) &&
                    (header_value.stream_id_ == 1 || header_value.stream_id_ == 3)) {
                    hpack_collect fields;
                    const auto decoded = decoder.decode(payload_value, [&fields](std::string_view name, std::string_view value) { return hpack_collect::on_header(&fields, name, value); });
                    RUVIA_CHECK(decoded.decoded());
                    if (!decoded.decoded()) {
                        break;
                    }
                    if (header_value.stream_id_ == 1 &&
                        (fields.joined_.find(":status=100;") != std::string_view::npos)) {
                        got_continue = true;
                        continue_ended_stream =
                            (header_value.flags_ & sansio_driver_test::flag_end_stream) != 0;
                        RUVIA_CHECK(fields.joined_ == ":status=100;");
                        if (!content_sent) {
                            content_sent = co_await write_all(frame(
                                0x0 /*DATA*/, sansio_driver_test::flag_end_stream, 1, "hello"));
                            if (!content_sent) {
                                break;
                            }
                        }
                    } else if (header_value.stream_id_ == 1 &&
                               (fields.joined_.find(":status=200;") != std::string_view::npos)) {
                        got_supported_final = true;
                    } else if (header_value.stream_id_ == 3 &&
                               (fields.joined_.find(":status=417;") != std::string_view::npos)) {
                        got_unsupported_final = true;
                    }
                } else if (header_value.type_ == static_cast<std::uint8_t>(http2_frame_type::data) &&
                           header_value.stream_id_ == 1 && got_supported_final) {
                    supported_body.append(payload_value);
                    supported_ended = (header_value.flags_ & sansio_driver_test::flag_end_stream) != 0;
                }
            }

            close_client_socket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(got_continue);
    RUVIA_CHECK(!continue_ended_stream);
    RUVIA_CHECK(got_unsupported_final);
    RUVIA_CHECK(got_supported_final);
    RUVIA_CHECK(supported_body == "handler-ran");
    RUVIA_CHECK_EQ(access_observation.calls_, std::size_t{2});
    RUVIA_CHECK(access_observation.protocol_version_ == ruvia::http_protocol_version::http2);
}

RUVIA_TEST(sansio_driver_h2_buffered_access_uses_only_committed_plan_status) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool got_status = false;
    bool got_body_end = false;
    bool got_invalid_reset = false;
    stream_access_observation access_observation;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory worker;
            ruvia::detail::router router;
            auto& impl = ruvia::detail::router_impl::from(router);
            const auto no_middleware =
                std::span<const ruvia::detail::controller_middleware_descriptor>{};
            impl.register_route(ruvia::http_known_method::get,
                std::pmr::string("/buffered-status", std::pmr::get_default_resource()),
                ruvia::detail::route_handler_type(nullptr, &buffered_status_handler),
                ruvia::detail::request_body_mode::buffered, no_middleware, no_middleware);
            impl.register_route(ruvia::http_known_method::get,
                std::pmr::string("/invalid-response", std::pmr::get_default_resource()),
                ruvia::detail::route_handler_type(nullptr, &invalid_http2_response_handler),
                ruvia::detail::request_body_mode::buffered, no_middleware, no_middleware);
            impl.finalize();

            ruvia::test::http2_sans_io_session_fixture fixture;
            auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 64});
            const auto worker_handle_value = attachment.loop().handle();
            fixture.options_.access_log_.callback_ =
                ruvia::detail::callback_access::bind<void(const ruvia::access_log_record&) noexcept>(
                    access_observation);
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
            const auto write_all = [&sock](std::string_view bytes_value) -> asio::awaitable<bool> {
                const auto [ec, n] = co_await asio::async_write(sock,
                    asio::buffer(bytes_value.data(), bytes_value.size()), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == bytes_value.size();
            };
            const auto read_exact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                const auto [ec, n] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };
            const auto request_head = [](std::uint32_t stream_id, std::string_view path) {
                std::pmr::string block(std::pmr::get_default_resource());
                hpack_encoder::encode_header(block, ":method", "GET");
                hpack_encoder::encode_header(block, ":path", path);
                hpack_encoder::encode_header(block, ":scheme", "http");
                hpack_encoder::encode_header(block, ":authority", "localhost");
                return frame(0x1,
                    sansio_driver_test::flag_end_stream | sansio_driver_test::flag_end_headers,
                    stream_id, std::string_view(block.data(), block.size()));
            };

            if (!co_await write_all(client_preface) || !co_await write_all(frame(0x4, 0, 0, {})) ||
                !co_await write_all(request_head(1, "/buffered-status")) ||
                !co_await write_all(request_head(3, "/invalid-response"))) {
                co_return;
            }

            ruvia::hpack_decoder decoder({.resource_ = std::pmr::get_default_resource()});
            while (!(got_body_end && got_invalid_reset)) {
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
                if (header_value.stream_id_ == 1 &&
                    header_value.type_ == static_cast<std::uint8_t>(http2_frame_type::headers)) {
                    hpack_collect fields;
                    const auto decoded = decoder.decode(payload_value, [&fields](std::string_view name, std::string_view value) { return hpack_collect::on_header(&fields, name, value); });
                    RUVIA_CHECK(decoded.decoded());
                    got_status = decoded.decoded() &&
                                 (fields.joined_.find(":status=207;") != std::string_view::npos);
                } else if (header_value.stream_id_ == 1 &&
                           header_value.type_ == static_cast<std::uint8_t>(http2_frame_type::data)) {
                    got_body_end = (header_value.flags_ & sansio_driver_test::flag_end_stream) != 0;
                } else if (header_value.stream_id_ == 3 &&
                           header_value.type_ == static_cast<std::uint8_t>(http2_frame_type::rst_stream)) {
                    got_invalid_reset = true;
                }
            }

            close_client_socket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(got_status);
    RUVIA_CHECK(got_body_end);
    RUVIA_CHECK(got_invalid_reset);
    RUVIA_CHECK_EQ(access_observation.calls_, std::size_t{1});
    RUVIA_CHECK_EQ(access_observation.status_, std::uint16_t{207});
    RUVIA_CHECK(access_observation.protocol_version_ == ruvia::http_protocol_version::http2);
}

RUVIA_TEST(sansio_driver_h2_buffered_peer_abort_before_commit_has_no_status) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    stream_access_observation access_observation;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory worker;
            ruvia::detail::router router;
            auto& impl = ruvia::detail::router_impl::from(router);
            impl.register_route(ruvia::http_known_method::get,
                std::pmr::string("/peer-abort", std::pmr::get_default_resource()),
                ruvia::detail::route_handler_type(&io, &slow_handler),
                ruvia::detail::request_body_mode::buffered,
                std::span<const ruvia::detail::controller_middleware_descriptor>{},
                std::span<const ruvia::detail::controller_middleware_descriptor>{});
            impl.finalize();
            ruvia::test::http2_sans_io_session_fixture fixture;
            auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 64});
            const auto worker_handle_value = attachment.loop().handle();
            fixture.options_.access_log_.callback_ =
                ruvia::detail::callback_access::bind<void(const ruvia::access_log_record&) noexcept>(
                    access_observation);
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
            const auto write_all = [&sock](std::string_view bytes_value) -> asio::awaitable<bool> {
                const auto [ec, n] = co_await asio::async_write(sock,
                    asio::buffer(bytes_value.data(), bytes_value.size()), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == bytes_value.size();
            };

            std::pmr::string block(std::pmr::get_default_resource());
            hpack_encoder::encode_header(block, ":method", "GET");
            hpack_encoder::encode_header(block, ":path", "/peer-abort");
            hpack_encoder::encode_header(block, ":scheme", "http");
            hpack_encoder::encode_header(block, ":authority", "localhost");
            const std::string cancel_payload("\0\0\0\x08", 4);
            if (!co_await write_all(client_preface) || !co_await write_all(frame(0x4, 0, 0, {})) ||
                !co_await write_all(frame(0x1,
                    sansio_driver_test::flag_end_stream | sansio_driver_test::flag_end_headers, 1,
                    std::string_view(block.data(), block.size()))) ||
                !co_await write_all(frame(0x3 /*RST_STREAM*/, 0, 1, cancel_payload))) {
                co_return;
            }

            asio::steady_timer settle(io);
            settle.expires_after(std::chrono::milliseconds(150));
            const auto [wait_ec] = co_await settle.async_wait(asio::as_tuple(asio::use_awaitable));
            (void)wait_ec;
            close_client_socket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK_EQ(access_observation.calls_, std::size_t{0});
}

RUVIA_TEST(sansio_driver_h2_stream_trailers_emitted) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::string body;
    std::string head_fields;
    std::string trailer_fields;
    bool trailer_end_stream = false;
    stream_access_observation access_observation;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory worker;
            ruvia::detail::router router;
            auto& impl = ruvia::detail::router_impl::from(router);
            impl.register_response_stream_route(ruvia::http_known_method::get,
                std::pmr::string("/trail", std::pmr::get_default_resource()),
                ruvia::detail::route_stream_handler_type(nullptr, &stream_trailer_handler),
                std::span<const ruvia::detail::controller_middleware_descriptor>{},
                std::span<const ruvia::detail::controller_middleware_descriptor>{});
            impl.finalize();
            ruvia::test::http2_sans_io_session_fixture fixture;
            auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 64});
            const auto worker_handle_value = attachment.loop().handle();
            fixture.options_.access_log_.callback_ =
                ruvia::detail::callback_access::bind<void(const ruvia::access_log_record&) noexcept>(
                    access_observation);
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

            if (!co_await write_all(client_preface)) {
                co_return;
            }
            if (!co_await write_all(frame(0x4, 0, 0, {}))) {
                co_return;
            }
            std::pmr::string header_block(std::pmr::get_default_resource());
            hpack_encoder::encode_header(header_block, ":method", "GET");
            hpack_encoder::encode_header(header_block, ":path", "/trail");
            hpack_encoder::encode_header(header_block, ":scheme", "http");
            hpack_encoder::encode_header(header_block, ":authority", "localhost");
            if (!co_await write_all(frame(0x1,
                    sansio_driver_test::flag_end_stream | sansio_driver_test::flag_end_headers, 1,
                    std::string_view(header_block.data(), header_block.size())))) {
                co_return;
            }

            ruvia::hpack_decoder decoder({.resource_ = std::pmr::get_default_resource()});
            bool saw_head = false;
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
                if (header_value.type_ == static_cast<std::uint8_t>(http2_frame_type::headers)) {
                    hpack_collect collect;
                    (void)decoder.decode(payload_value, [&collect](std::string_view name, std::string_view value) { return hpack_collect::on_header(&collect, name, value); });
                    if (!saw_head) {
                        saw_head = true;
                        head_fields = collect.joined_;
                    } else {
                        trailer_fields = collect.joined_;  // the trailing HEADERS block
                        trailer_end_stream = (header_value.flags_ & sansio_driver_test::flag_end_stream) != 0;
                        break;
                    }
                } else if (header_value.type_ == static_cast<std::uint8_t>(http2_frame_type::data)) {
                    body += payload_value;
                    if ((header_value.flags_ & sansio_driver_test::flag_end_stream) != 0) {
                        break;
                    }
                }
            }
            close_client_socket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(body == "body-part");
    RUVIA_CHECK((head_fields.find(":status=207;") != std::string_view::npos));
    RUVIA_CHECK(trailer_fields == "x-checksum=abc123;");
    RUVIA_CHECK(trailer_end_stream);
    RUVIA_CHECK_EQ(access_observation.calls_, std::size_t{1});
    RUVIA_CHECK_EQ(access_observation.status_, std::uint16_t{207});
    RUVIA_CHECK(access_observation.protocol_version_ == ruvia::http_protocol_version::http2);
}

RUVIA_TEST(sansio_driver_h2_stream_send_window_pacing) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::size_t received_value = 0;
    bool saw_end = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory worker;
            ruvia::detail::router router;
            auto& impl = ruvia::detail::router_impl::from(router);
            impl.register_response_stream_route(ruvia::http_known_method::get,
                std::pmr::string("/big", std::pmr::get_default_resource()),
                ruvia::detail::route_stream_handler_type(nullptr, &stream_big_chunk_handler),
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
            // SETTINGS_INITIAL_WINDOW_SIZE = 8: the 64-byte body must be granted 8 at a time.
            const char settings_payload[6] = {0x00, 0x04, 0x00, 0x00, 0x00, 0x08};
            if (!co_await write_all(frame(0x4, 0, 0, std::string_view(settings_payload, 6)))) {
                co_return;
            }
            std::pmr::string header_block(std::pmr::get_default_resource());
            hpack_encoder::encode_header(header_block, ":method", "GET");
            hpack_encoder::encode_header(header_block, ":path", "/big");
            hpack_encoder::encode_header(header_block, ":scheme", "http");
            hpack_encoder::encode_header(header_block, ":authority", "localhost");
            if (!co_await write_all(frame(0x1,
                    sansio_driver_test::flag_end_stream | sansio_driver_test::flag_end_headers, 1,
                    std::string_view(header_block.data(), header_block.size())))) {
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
                if (header_value.stream_id_ != 1) {
                    continue;
                }
                if (header_value.type_ == static_cast<std::uint8_t>(http2_frame_type::data)) {
                    received_value += payload_value.size();
                    if ((header_value.flags_ & sansio_driver_test::flag_end_stream) != 0) {
                        saw_end = true;
                        break;
                    }
                    if (!payload_value.empty()) {
                        // Grant the next window slice (connection + stream scoped).
                        const auto updates = sansio_driver_test::window_update(
                                                 0, static_cast<std::uint32_t>(payload_value.size())) +
                                             sansio_driver_test::window_update(
                                                 1, static_cast<std::uint32_t>(payload_value.size()));
                        if (!co_await write_all(updates)) {
                            break;
                        }
                    }
                }
            }
            close_client_socket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK_EQ(received_value, static_cast<std::size_t>(64));
    RUVIA_CHECK(saw_end);
}

RUVIA_TEST(sansio_driver_h2_large_file_body_paces_and_completes) {
    // Write the temp file (large_file_bytes of a repeating pattern).
    const auto path =
        (std::filesystem::temp_directory_path() / "ruvia_sansio_large_file_test.bin").string();
    large_file_path() = path;
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        std::string block(4096, '\0');
        for (std::size_t i = 0; i < block.size(); ++i) {
            block[i] = static_cast<char>('A' + (i % 26));
        }
        std::uint64_t written = 0;
        while (written < large_file_bytes) {
            const auto n = static_cast<std::size_t>(
                std::min<std::uint64_t>(block.size(), large_file_bytes - written));
            out.write(block.data(), static_cast<std::streamsize>(n));
            written += n;
        }
    }

    asio::io_context& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::uint64_t received_value = 0;
    bool saw_end = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory worker;
            ruvia::detail::router router;
            auto& impl = ruvia::detail::router_impl::from(router);
            impl.register_route(ruvia::http_known_method::get,
                std::pmr::string("/file", std::pmr::get_default_resource()),
                ruvia::detail::route_handler_type(nullptr, &large_file_handler),
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
            // Small initial window (16) so the file body blocks almost immediately and
            // only completes if WINDOW_UPDATE-driven pacing wakes it repeatedly.
            const char settings_payload[6] = {0x00, 0x04, 0x00, 0x00, 0x00, 0x10};
            if (!co_await write_all(frame(0x4, 0, 0, std::string_view(settings_payload, 6)))) {
                co_return;
            }
            std::pmr::string header_block(std::pmr::get_default_resource());
            hpack_encoder::encode_header(header_block, ":method", "GET");
            hpack_encoder::encode_header(header_block, ":path", "/file");
            hpack_encoder::encode_header(header_block, ":scheme", "http");
            hpack_encoder::encode_header(header_block, ":authority", "localhost");
            if (!co_await write_all(frame(0x1,
                    sansio_driver_test::flag_end_stream | sansio_driver_test::flag_end_headers, 1,
                    std::string_view(header_block.data(), header_block.size())))) {
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
                if (header_value.stream_id_ != 1 ||
                    header_value.type_ != static_cast<std::uint8_t>(http2_frame_type::data)) {
                    continue;
                }
                received_value += payload_value.size();
                if ((header_value.flags_ & sansio_driver_test::flag_end_stream) != 0) {
                    saw_end = true;
                    break;
                }
                if (!payload_value.empty()) {
                    const auto updates = sansio_driver_test::window_update(
                                             0, static_cast<std::uint32_t>(payload_value.size())) +
                                         sansio_driver_test::window_update(
                                             1, static_cast<std::uint32_t>(payload_value.size()));
                    if (!co_await write_all(updates)) {
                        break;
                    }
                }
            }
            close_client_socket(sock);
        },
        asio::detached);

    io.run();
    std::filesystem::remove(path);
    RUVIA_CHECK_EQ(received_value, large_file_bytes);  // every byte delivered, not truncated
    RUVIA_CHECK(saw_end);
}

RUVIA_TEST(sansio_driver_h2_streaming_request_body) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::size_t received_bytes = 0;
    std::string body;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory worker;
            ruvia::detail::router router;
            auto& impl = ruvia::detail::router_impl::from(router);
            impl.register_route(ruvia::http_known_method::post,
                std::pmr::string("/upload", std::pmr::get_default_resource()),
                ruvia::detail::route_handler_type(&received_bytes, &stream_body_count_handler),
                ruvia::detail::request_body_mode::stream,
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
            auto yield = [&io]() -> asio::awaitable<void> {
                asio::steady_timer t(io, std::chrono::milliseconds(5));
                co_await t.async_wait(asio::as_tuple(asio::use_awaitable));
            };

            if (!co_await write_all(client_preface)) {
                co_return;
            }
            if (!co_await write_all(frame(0x4, 0, 0, {}))) {
                co_return;
            }
            std::pmr::string header_block(std::pmr::get_default_resource());
            hpack_encoder::encode_header(header_block, ":method", "POST");
            hpack_encoder::encode_header(header_block, ":path", "/upload");
            hpack_encoder::encode_header(header_block, ":scheme", "http");
            hpack_encoder::encode_header(header_block, ":authority", "localhost");
            // HEADERS with NO END_STREAM -> body follows in separate DATA frames.
            if (!co_await write_all(frame(0x1, sansio_driver_test::flag_end_headers, 1,
                    std::string_view(header_block.data(), header_block.size())))) {
                co_return;
            }
            co_await yield();
            if (!co_await write_all(frame(0x0, 0, 1, "aaa"))) {
                co_return;  // 3 bytes
            }
            co_await yield();
            if (!co_await write_all(frame(0x0, 0, 1, "bb"))) {
                co_return;  // 2 bytes
            }
            co_await yield();
            if (!co_await write_all(frame(0x0, sansio_driver_test::flag_end_stream, 1, {}))) {
                co_return;
            }

            for (;;) {
                char hb[ruvia::http2_frame_header_bytes];
                if (!co_await read_exact(hb, sizeof(hb))) {
                    break;
                }
                const auto header_value =
                    sansio_driver_test::parse_frame_header(std::string_view(hb, sizeof(hb)));
                std::string payload_value(header_value.length_, '\0');
                if (header_value.length_ != 0 && !co_await read_exact(payload_value.data(), payload_value.size())) {
                    break;
                }
                if (header_value.type_ == static_cast<std::uint8_t>(http2_frame_type::data) &&
                    header_value.stream_id_ == 1 && !payload_value.empty()) {
                    body = payload_value;
                    break;
                }
            }
            close_client_socket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK_EQ(received_bytes, static_cast<std::size_t>(5));  // "aaa" + "bb"
    RUVIA_CHECK(body == "upload-done");
}

RUVIA_TEST(sansio_driver_h2_server_request_trailers_dispatch) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::string body;
    bool trailers_observed = false;
    bool early_hints_observed = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory worker;
            ruvia::detail::router router;
            auto& impl = ruvia::detail::router_impl::from(router);
            impl.register_route(ruvia::http_known_method::post,
                std::pmr::string("/echo", std::pmr::get_default_resource()),
                ruvia::detail::route_handler_type(&trailers_observed, [](void* raw, ruvia::context& context_value) -> ruvia::task<ruvia::http_response> {
                    *static_cast<bool*>(raw) = context_value.req().trailer("x-checksum") == "abc" && !context_value.req().header("x-checksum") && context_value.req().trailers().size() == 1;
                    const std::array fields_value{ruvia::http_header_view("Link", std::string_view("</style.css>; rel=preload"))};
                    co_await context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints, fields_value));
                    co_return context_value.text("handler-ran");
                }),
                ruvia::detail::request_body_mode::buffered, std::span<const ruvia::detail::controller_middleware_descriptor>{}, std::span<const ruvia::detail::controller_middleware_descriptor>{});
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
            if (!co_await write_all(frame(0x4, 0, 0, {}))) {
                co_return;
            }
            std::pmr::string header_block(std::pmr::get_default_resource());
            hpack_encoder::encode_header(header_block, ":method", "POST");
            hpack_encoder::encode_header(header_block, ":path", "/echo");
            hpack_encoder::encode_header(header_block, ":scheme", "http");
            hpack_encoder::encode_header(header_block, ":authority", "localhost");
            if (!co_await write_all(frame(0x1, sansio_driver_test::flag_end_headers, 1,
                    std::string_view(header_block.data(), header_block.size())))) {
                co_return;
            }
            // Body, then a trailing HEADERS block carrying END_STREAM.
            if (!co_await write_all(frame(0x0, 0, 1, "hi"))) {
                co_return;
            }
            std::pmr::string trailer_block(std::pmr::get_default_resource());
            hpack_encoder::encode_header(trailer_block, "x-checksum", "abc");
            if (!co_await write_all(frame(0x1,
                    sansio_driver_test::flag_end_headers | sansio_driver_test::flag_end_stream, 1,
                    std::string_view(trailer_block.data(), trailer_block.size())))) {
                co_return;
            }

            ruvia::hpack_decoder decoder({.resource_ = std::pmr::get_default_resource()});
            for (;;) {
                char hb[ruvia::http2_frame_header_bytes];
                if (!co_await read_exact(hb, sizeof(hb))) {
                    break;
                }
                const auto header_value =
                    sansio_driver_test::parse_frame_header(std::string_view(hb, sizeof(hb)));
                std::string payload_value(header_value.length_, '\0');
                if (header_value.length_ != 0 && !co_await read_exact(payload_value.data(), payload_value.size())) {
                    break;
                }
                if (header_value.type_ == static_cast<std::uint8_t>(http2_frame_type::headers) && header_value.stream_id_ == 1) {
                    hpack_collect fields;
                    const auto decoded = decoder.decode(payload_value, [&fields](std::string_view name, std::string_view value) { return hpack_collect::on_header(&fields, name, value); });
                    RUVIA_CHECK(decoded.decoded());
                    if ((fields.joined_.find(":status=103;") != std::string_view::npos)) {
                        early_hints_observed = (fields.joined_.find("link=</style.css>; rel=preload;") != std::string_view::npos) && (header_value.flags_ & sansio_driver_test::flag_end_stream) == 0;
                    }
                }
                if (header_value.type_ == static_cast<std::uint8_t>(http2_frame_type::data) &&
                    header_value.stream_id_ == 1 && !payload_value.empty()) {
                    body = payload_value;
                    break;
                }
            }
            close_client_socket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(body == "handler-ran");  // trailers ended the request; handler dispatched
    RUVIA_CHECK(trailers_observed);
    RUVIA_CHECK(early_hints_observed);
}

RUVIA_TEST(sansio_driver_h2_large_buffered_body_paces_and_completes) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::uint64_t received_value = 0;
    bool saw_end = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory worker;
            ruvia::detail::router router;
            auto& impl = ruvia::detail::router_impl::from(router);
            impl.register_route(ruvia::http_known_method::get,
                std::pmr::string("/big", std::pmr::get_default_resource()),
                ruvia::detail::route_handler_type(nullptr, &large_buffered_handler),
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
            const char settings_payload[6] = {0x00, 0x04, 0x00, 0x00, 0x00, 0x20};  // window 32
            if (!co_await write_all(frame(0x4, 0, 0, std::string_view(settings_payload, 6)))) {
                co_return;
            }
            std::pmr::string header_block(std::pmr::get_default_resource());
            hpack_encoder::encode_header(header_block, ":method", "GET");
            hpack_encoder::encode_header(header_block, ":path", "/big");
            hpack_encoder::encode_header(header_block, ":scheme", "http");
            hpack_encoder::encode_header(header_block, ":authority", "localhost");
            if (!co_await write_all(frame(0x1,
                    sansio_driver_test::flag_end_stream | sansio_driver_test::flag_end_headers, 1,
                    std::string_view(header_block.data(), header_block.size())))) {
                co_return;
            }

            for (;;) {
                char hb[ruvia::http2_frame_header_bytes];
                if (!co_await read_exact(hb, sizeof(hb))) {
                    break;
                }
                const auto header_value =
                    sansio_driver_test::parse_frame_header(std::string_view(hb, sizeof(hb)));
                std::string payload_value(header_value.length_, '\0');
                if (header_value.length_ != 0 && !co_await read_exact(payload_value.data(), payload_value.size())) {
                    break;
                }
                if (header_value.stream_id_ != 1 ||
                    header_value.type_ != static_cast<std::uint8_t>(http2_frame_type::data)) {
                    continue;
                }
                received_value += payload_value.size();
                if ((header_value.flags_ & sansio_driver_test::flag_end_stream) != 0) {
                    saw_end = true;
                    break;
                }
                if (!payload_value.empty()) {
                    const auto updates = sansio_driver_test::window_update(
                                             0, static_cast<std::uint32_t>(payload_value.size())) +
                                         sansio_driver_test::window_update(
                                             1, static_cast<std::uint32_t>(payload_value.size()));
                    if (!co_await write_all(updates)) {
                        break;
                    }
                }
            }
            close_client_socket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK_EQ(received_value, static_cast<std::uint64_t>(large_buffered_bytes));
    RUVIA_CHECK(saw_end);
}
