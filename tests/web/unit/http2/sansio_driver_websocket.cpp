#include <chrono>
#include <string_view>
#include <utility>

#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>

#include "ruvia/core/connection_scanner.h"
#include "ruvia/core/event_loop_attachment.h"

#include "context/context_services.h"
#include "http2/http2_sans_io_session.h"
#include "http2_sansio_session_fixture.h"
#include "router/route_table.h"
#include "router/router.h"
#include "router/router_impl.h"
#include "sansio_driver_fixture.h"

// Sans-I/O HTTP/2 driver: websocket tunnels over HTTP/2 (RFC 8441).

namespace {

struct websocket_operation_memory final {
    std::size_t messages_{0};
    bool request_arena_stable_{true};
    bool retained_data_stable_{true};
};

ruvia::task<void> echo_with_operation_memory(void* target, ruvia::context& context_value) {
    auto& observed_value = *static_cast<websocket_operation_memory*>(target);
    const std::string expected_handshake(512, 'h');
    const std::string expected_retained(512, 'r');
    const std::pmr::string handshake(expected_handshake, context_value.arena());
    const std::pmr::string retained(expected_retained, context_value.pool());
    auto& socket = context_value.get_websocket();
    for (;;) {
        auto* before = static_cast<std::byte*>(context_value.arena()->allocate(1, 1));
        auto message = co_await socket.read();
        if (message && message->text()) {
            co_await socket.text(message->payload());
            ++observed_value.messages_;
        }
        auto* after = static_cast<std::byte*>(context_value.arena()->allocate(1, 1));
        observed_value.request_arena_stable_ = observed_value.request_arena_stable_ && after == before + 1;
        observed_value.retained_data_stable_ = observed_value.retained_data_stable_ &&
                                               handshake == std::string_view(expected_handshake) &&
                                               retained == std::string_view(expected_retained);
        if (!message) {
            co_return;
        }
    }
}

}  // namespace

RUVIA_TEST(sansio_driver_h2_websocket_echo) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool got_handshake = false;
    bool got_automatic_alt_svc = false;
    bool hpack_decode_succeeded = true;
    bool saw_forbidden_handshake_field = false;
    std::string echoed_frame;  // reassembled ws frame bytes from stream-1 DATA
    bool got_close_end_stream = false;
    websocket_operation_memory operation_memory;
    constexpr std::size_t message_count = 32;
    const std::string message_payload(80, 'e');
    std::size_t echoes = 0;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory worker;
            ruvia::detail::router router;
            auto& impl = ruvia::detail::router_impl::from(router);
            impl.register_websocket_route(ruvia::http_known_method::get,
                std::pmr::string("/ws", std::pmr::get_default_resource()),
                ruvia::detail::route_stream_handler_type(&operation_memory, &echo_with_operation_memory),
                std::span<const ruvia::detail::controller_middleware_descriptor>{},
                std::span<const ruvia::detail::controller_middleware_descriptor>{});
            impl.finalize();
            auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 64});
            const auto worker_handle_value = attachment.loop().handle();
            ruvia::test::http2_sans_io_session_fixture fixture;
            fixture.options_.request_body_timeout_ = std::chrono::milliseconds(100);
            ruvia::connection_scanner scanner(worker_handle_value,
                {.scan_interval_ = std::chrono::milliseconds(5),
                    .payload_read_timeout_ = fixture.options_.request_body_timeout_});
            ruvia::connection_scanner::guard_type scanner_guard(
                &scanner, fixture.scanner_entry_, sock);
            scanner.start();
            auto services = fixture.services(worker_handle_value)
                                .with_tls_transport("127.0.0.1")
                                .with_automatic_alt_svc("h3=\":443\"; ma=86400");
            co_await ruvia::as_awaitable(ruvia::detail::run_http2_sans_io_session(
                sock, impl.route_table(), worker,
                fixture.context(std::move(services)), std::string_view{}));
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

            if (!co_await write_all(client_preface)) {
                co_return;
            }
            if (!co_await write_all(frame(0x4 /*SETTINGS*/, 0, 0, {}))) {
                co_return;
            }

            std::pmr::string header_block(std::pmr::get_default_resource());
            hpack_encoder::encode_header(header_block, ":method", "CONNECT");
            hpack_encoder::encode_header(header_block, ":protocol", "websocket");
            hpack_encoder::encode_header(header_block, ":scheme", "http");
            hpack_encoder::encode_header(header_block, ":path", "/ws");
            hpack_encoder::encode_header(header_block, ":authority", "localhost");
            hpack_encoder::encode_header(header_block, "sec-websocket-version", "13");
            // Extended CONNECT: END_HEADERS only -- the stream MUST stay open.
            if (!co_await write_all(frame(0x1 /*HEADERS*/, sansio_driver_test::flag_end_headers, 1,
                    std::string_view(header_block.data(), header_block.size())))) {
                co_return;
            }

            // Wait for the 200 handshake HEADERS on stream 1.
            ruvia::hpack_decoder decoder({.resource_ = std::pmr::get_default_resource()});
            ruvia::http2_frame_header header_value{};
            std::string payload;
            for (;;) {
                if (!co_await read_frame_into(header_value, payload)) {
                    co_return;
                }
                if (header_value.type_ == static_cast<std::uint8_t>(http2_frame_type::headers) &&
                    header_value.stream_id_ == 1) {
                    got_handshake = true;
                    hpack_collect fields;
                    const auto decoded = decoder.decode(payload,
                        [&fields](std::string_view name, std::string_view value) {
                            return hpack_collect::on_header(&fields, name, value);
                        });
                    hpack_decode_succeeded = decoded.decoded();
                    got_automatic_alt_svc =
                        (fields.joined_.find("alt-svc=h3=\":443\"; ma=86400;") != std::string_view::npos);
                    saw_forbidden_handshake_field = (fields.joined_.find("connection=") != std::string_view::npos) ||
                                                    (fields.joined_.find("upgrade=") != std::string_view::npos) ||
                                                    (fields.joined_.find("sec-websocket-accept=") != std::string_view::npos);
                    break;
                }
            }

            // An upgraded websocket is long-lived, not an HTTP request body.
            // Its idle period exceeds request_body_timeout and must not kill the tunnel.
            asio::steady_timer idle_delay(io);
            idle_delay.expires_after(std::chrono::milliseconds(180));
            co_await idle_delay.async_wait(asio::use_awaitable);

            // Repeated reads and writes must reuse operation memory while the
            // handshake and explicitly retained data stay alive.
            for (std::size_t index = 0; index != message_count; ++index) {
                if (!co_await write_all(frame(0x0 /*DATA*/, 0, 1, masked_ws_frame(0x1, message_payload)))) {
                    co_return;
                }
                echoed_frame.clear();
                std::string tunnel_bytes;
                while (echoed_frame.empty()) {
                    if (!co_await read_frame_into(header_value, payload)) {
                        co_return;
                    }
                    if (header_value.type_ != static_cast<std::uint8_t>(http2_frame_type::data) ||
                        header_value.stream_id_ != 1) {
                        continue;
                    }
                    tunnel_bytes += payload;
                    if (tunnel_bytes.size() >= 2) {
                        const auto len = static_cast<std::size_t>(
                            static_cast<unsigned char>(tunnel_bytes[1]) & 0x7FU);
                        if (tunnel_bytes.size() >= 2 + len) {
                            echoed_frame = tunnel_bytes.substr(0, 2 + len);
                        }
                    }
                }
                if (echoed_frame.substr(2) != message_payload) {
                    close_client_socket(sock);
                    co_return;
                }
                ++echoes;
            }

            // Close the tunnel: the client sends its masked Close and orderly
            // transport half-close together; the server echoes Close and ends its
            // half only after the protocol core observes that peer Close.
            if (!co_await write_all(frame(0x0 /*DATA*/, sansio_driver_test::flag_end_stream, 1,
                    masked_ws_frame(0x8, std::string_view("\x03\xE8", 2))))) {
                co_return;
            }
            for (;;) {
                if (!co_await read_frame_into(header_value, payload)) {
                    break;
                }
                if (header_value.type_ == static_cast<std::uint8_t>(http2_frame_type::data) &&
                    header_value.stream_id_ == 1 &&
                    (header_value.flags_ & sansio_driver_test::flag_end_stream) != 0) {
                    got_close_end_stream = true;
                    break;
                }
            }

            close_client_socket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(got_handshake);
    RUVIA_CHECK(hpack_decode_succeeded);
    RUVIA_CHECK(got_automatic_alt_svc);
    RUVIA_CHECK(!saw_forbidden_handshake_field);
    RUVIA_CHECK_EQ(echoes, message_count);
    RUVIA_CHECK_EQ(operation_memory.messages_, message_count);
    RUVIA_CHECK(operation_memory.request_arena_stable_);
    RUVIA_CHECK(operation_memory.retained_data_stable_);
    // FIN|text -- unmasked server frame, payload echoed intact.
    RUVIA_CHECK_EQ(echoed_frame.size(), message_payload.size() + 2);
    RUVIA_CHECK_EQ(static_cast<unsigned char>(echoed_frame[0]), static_cast<unsigned char>(0x81));
    RUVIA_CHECK_EQ(static_cast<unsigned char>(echoed_frame[1]), static_cast<unsigned char>(message_payload.size()));
    RUVIA_CHECK(echoed_frame.substr(2) == message_payload);
    RUVIA_CHECK(got_close_end_stream);
}

RUVIA_TEST(sansio_driver_h2_websocket_success_ignores_accept_encoding_rejection) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::string handshake_fields;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory worker;
            ruvia::detail::router router;
            auto& impl = ruvia::detail::router_impl::from(router);
            impl.register_websocket_route(ruvia::http_known_method::get,
                std::pmr::string("/ws", std::pmr::get_default_resource()),
                ruvia::detail::route_stream_handler_type(nullptr, &ws_server_close_handler),
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
                const auto [ec, n] = co_await asio::async_write(sock,
                    asio::buffer(bytes_value.data(), bytes_value.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto read_exact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                const auto [ec, n] = co_await asio::async_read(
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
            hpack_encoder::encode_header(header_block, ":method", "CONNECT");
            hpack_encoder::encode_header(header_block, ":protocol", "websocket");
            hpack_encoder::encode_header(header_block, ":scheme", "http");
            hpack_encoder::encode_header(header_block, ":path", "/ws");
            hpack_encoder::encode_header(header_block, ":authority", "localhost");
            hpack_encoder::encode_header(header_block, "sec-websocket-version", "13");
            hpack_encoder::encode_header(
                header_block, "accept-encoding", "identity;q=0, gzip;q=0, br;q=0, zstd;q=0");
            if (!co_await write_all(frame(0x1 /*HEADERS*/, sansio_driver_test::flag_end_headers, 1,
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
                if (header_value.type_ == static_cast<std::uint8_t>(http2_frame_type::headers) &&
                    header_value.stream_id_ == 1) {
                    hpack_collect collect;
                    (void)decoder.decode(payload_value, [&collect](std::string_view name, std::string_view value) { return hpack_collect::on_header(&collect, name, value); });
                    handshake_fields = collect.joined_;
                    break;
                }
            }

            close_client_socket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK((handshake_fields.find(":status=200;") != std::string_view::npos));
    RUVIA_CHECK(!(handshake_fields.find("content-encoding=") != std::string_view::npos));
}

RUVIA_TEST(sansio_driver_h2_server_close_waits_for_peer_close) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool got_handshake = false;
    bool got_close_without_end = false;
    bool got_terminal_empty_end = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory worker;
            ruvia::detail::router router;
            auto& impl = ruvia::detail::router_impl::from(router);
            impl.register_websocket_route(ruvia::http_known_method::get,
                std::pmr::string("/server-close", std::pmr::get_default_resource()),
                ruvia::detail::route_stream_handler_type(nullptr, &ws_server_close_handler),
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
                const auto [ec, n] = co_await asio::async_write(sock,
                    asio::buffer(bytes_value.data(), bytes_value.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto read_exact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                const auto [ec, n] = co_await asio::async_read(
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

            if (!co_await write_all(client_preface) ||
                !co_await write_all(frame(0x4 /*SETTINGS*/, 0, 0, {}))) {
                co_return;
            }

            std::pmr::string header_block(std::pmr::get_default_resource());
            hpack_encoder::encode_header(header_block, ":method", "CONNECT");
            hpack_encoder::encode_header(header_block, ":protocol", "websocket");
            hpack_encoder::encode_header(header_block, ":scheme", "http");
            hpack_encoder::encode_header(header_block, ":path", "/server-close");
            hpack_encoder::encode_header(header_block, ":authority", "localhost");
            hpack_encoder::encode_header(header_block, "sec-websocket-version", "13");
            if (!co_await write_all(frame(0x1 /*HEADERS*/, sansio_driver_test::flag_end_headers, 1,
                    std::string_view(header_block.data(), header_block.size())))) {
                co_return;
            }

            ruvia::http2_frame_header header_value{};
            std::string payload;
            while (!got_close_without_end) {
                if (!co_await read_frame_into(header_value, payload)) {
                    co_return;
                }
                if (header_value.stream_id_ != 1) {
                    continue;
                }
                if (header_value.type_ == static_cast<std::uint8_t>(http2_frame_type::headers)) {
                    got_handshake = true;
                    continue;
                }
                if (header_value.type_ == static_cast<std::uint8_t>(http2_frame_type::data) &&
                    payload.size() >= 4 && static_cast<unsigned char>(payload[0]) == 0x88U) {
                    got_close_without_end = (header_value.flags_ & sansio_driver_test::flag_end_stream) == 0;
                }
            }

            if (!co_await write_all(frame(0x0 /*DATA*/, sansio_driver_test::flag_end_stream, 1,
                    masked_ws_frame(0x8, std::string_view("\x03\xE8", 2))))) {
                co_return;
            }
            for (;;) {
                if (!co_await read_frame_into(header_value, payload)) {
                    break;
                }
                if (header_value.stream_id_ == 1 &&
                    header_value.type_ == static_cast<std::uint8_t>(http2_frame_type::data) &&
                    (header_value.flags_ & sansio_driver_test::flag_end_stream) != 0) {
                    got_terminal_empty_end = payload.empty();
                    break;
                }
            }

            close_client_socket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(got_handshake);
    RUVIA_CHECK(got_close_without_end);
    RUVIA_CHECK(got_terminal_empty_end);
}

RUVIA_TEST(sansio_driver_h2_websocket_invalid_version_rejected) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool got_response_head = false;
    bool got_bad_request = false;
    bool got_supported_version = false;
    bool got_end_stream = false;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory worker;
            ruvia::detail::router router;
            auto& impl = ruvia::detail::router_impl::from(router);
            impl.register_websocket_route(ruvia::http_known_method::get,
                std::pmr::string("/ws", std::pmr::get_default_resource()),
                ruvia::detail::route_stream_handler_type(nullptr, &ws_echo_handler),
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
            hpack_encoder::encode_header(header_block, ":method", "CONNECT");
            hpack_encoder::encode_header(header_block, ":protocol", "websocket");
            hpack_encoder::encode_header(header_block, ":scheme", "http");
            hpack_encoder::encode_header(header_block, ":path", "/ws");
            hpack_encoder::encode_header(header_block, ":authority", "localhost");
            hpack_encoder::encode_header(header_block, "sec-websocket-version", "12");  // bad
            if (!co_await write_all(frame(0x1, sansio_driver_test::flag_end_headers, 1,
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
                if (header_value.type_ == static_cast<std::uint8_t>(http2_frame_type::headers)) {
                    got_response_head = true;
                    const auto decoded = decoder.decode(payload_value,
                        [&got_bad_request, &got_supported_version](
                            std::string_view name, std::string_view value) {
                            got_bad_request = got_bad_request ||
                                              (name == ":status" && value == "400");
                            got_supported_version = got_supported_version ||
                                                    (name == "sec-websocket-version" &&
                                                        value == "13");
                            return true;
                        });
                    if (!decoded.decoded()) {
                        close_client_socket(sock);
                        co_return;
                    }
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
    RUVIA_CHECK(got_response_head);
    RUVIA_CHECK(got_bad_request);
    RUVIA_CHECK(got_supported_version);
    RUVIA_CHECK(got_end_stream);
}

RUVIA_TEST(sansio_driver_h2_websocket_permessage_deflate) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::string handshake_fields;
    std::string echoed_frame;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory worker;
            ruvia::detail::router router;
            auto& impl = ruvia::detail::router_impl::from(router);
            impl.register_websocket_route(ruvia::http_known_method::get,
                std::pmr::string("/ws", std::pmr::get_default_resource()),
                ruvia::detail::route_stream_handler_type(nullptr, &ws_echo_handler),
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
            if (!co_await write_all(frame(0x4, 0, 0, {}))) {
                co_return;
            }
            std::pmr::string header_block(std::pmr::get_default_resource());
            hpack_encoder::encode_header(header_block, ":method", "CONNECT");
            hpack_encoder::encode_header(header_block, ":protocol", "websocket");
            hpack_encoder::encode_header(header_block, ":scheme", "http");
            hpack_encoder::encode_header(header_block, ":path", "/ws");
            hpack_encoder::encode_header(header_block, ":authority", "localhost");
            hpack_encoder::encode_header(header_block, "sec-websocket-version", "13");
            hpack_encoder::encode_header(header_block, "sec-websocket-extensions",
                "permessage-deflate; client_max_window_bits");
            if (!co_await write_all(frame(0x1, sansio_driver_test::flag_end_headers, 1,
                    std::string_view(header_block.data(), header_block.size())))) {
                co_return;
            }

            // Handshake HEADERS: decode and capture the echoed extension.
            ruvia::hpack_decoder decoder({.resource_ = std::pmr::get_default_resource()});
            ruvia::http2_frame_header header_value{};
            for (;;) {
                char header_bytes[ruvia::http2_frame_header_bytes];
                if (!co_await read_exact(header_bytes, sizeof(header_bytes))) {
                    co_return;
                }
                header_value = sansio_driver_test::parse_frame_header(
                    std::string_view(header_bytes, sizeof(header_bytes)));
                std::string payload_value(header_value.length_, '\0');
                if (header_value.length_ != 0 && !co_await read_exact(payload_value.data(), payload_value.size())) {
                    co_return;
                }
                if (header_value.type_ == static_cast<std::uint8_t>(http2_frame_type::headers) &&
                    header_value.stream_id_ == 1) {
                    hpack_collect collect;
                    (void)decoder.decode(payload_value, [&collect](std::string_view name, std::string_view value) { return hpack_collect::on_header(&collect, name, value); });
                    handshake_fields = collect.joined_;
                    break;
                }
            }

            // Use the public client-role codec to make a masked compressed frame.
            const auto fixed_mask = [](void*, ruvia::websocket_mask_key_type& key) noexcept {
                key = {char{0x11}, char{0x22}, char{0x33}, char{0x44}};
                return true;
            };
            ruvia::websocket_connection client_codec({
                .resource_ = std::pmr::get_default_resource(),
                .compression_ = (ruvia::websocket_compression{.enabled_ = true}),
                .role_ = ruvia::websocket_connection_role::client,
                .mask_key_generator_ = fixed_mask,
            });
            if (client_codec.submit_frame(ruvia::websocket_opcode::text, "hello-deflate") !=
                ruvia::websocket_frame_submit_status::accepted) {
                co_return;
            }
            const std::string compressed_frame(client_codec.output_plan().bytes());
            if (!co_await write_all(frame(0x0, 0, 1, compressed_frame))) {
                co_return;
            }
            std::string tunnel_bytes;
            while (echoed_frame.empty()) {
                char header_bytes[ruvia::http2_frame_header_bytes];
                if (!co_await read_exact(header_bytes, sizeof(header_bytes))) {
                    co_return;
                }
                header_value = sansio_driver_test::parse_frame_header(
                    std::string_view(header_bytes, sizeof(header_bytes)));
                std::string payload_value(header_value.length_, '\0');
                if (header_value.length_ != 0 && !co_await read_exact(payload_value.data(), payload_value.size())) {
                    co_return;
                }
                if (header_value.type_ != static_cast<std::uint8_t>(http2_frame_type::data) ||
                    header_value.stream_id_ != 1) {
                    continue;
                }
                tunnel_bytes += payload_value;
                if (tunnel_bytes.size() >= 2) {
                    const auto len = static_cast<std::size_t>(
                        static_cast<unsigned char>(tunnel_bytes[1]) & 0x7FU);
                    if (tunnel_bytes.size() >= 2 + len) {
                        echoed_frame = tunnel_bytes.substr(0, 2 + len);
                    }
                }
            }
            close_client_socket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(handshake_fields.find("sec-websocket-extensions=permessage-deflate") !=
                std::string_view::npos);
    // 13-byte echo does not shrink under deflate, so it comes back as a plain frame.
    RUVIA_CHECK_EQ(echoed_frame.size(), static_cast<std::size_t>(15));
    RUVIA_CHECK_EQ(static_cast<unsigned char>(echoed_frame[0]), static_cast<unsigned char>(0x81));
    RUVIA_CHECK(echoed_frame.substr(2) == "hello-deflate");
}

RUVIA_TEST(sansio_driver_h2_two_concurrent_ws_tunnels) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::string echo1;
    std::string echo3;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory worker;
            ruvia::detail::router router;
            auto& impl = ruvia::detail::router_impl::from(router);
            impl.register_websocket_route(ruvia::http_known_method::get,
                std::pmr::string("/ws", std::pmr::get_default_resource()),
                ruvia::detail::route_stream_handler_type(nullptr, &ws_echo_handler),
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
            auto open_tunnel = [](std::uint32_t stream_id) {
                std::pmr::string block(std::pmr::get_default_resource());
                hpack_encoder::encode_header(block, ":method", "CONNECT");
                hpack_encoder::encode_header(block, ":protocol", "websocket");
                hpack_encoder::encode_header(block, ":scheme", "http");
                hpack_encoder::encode_header(block, ":path", "/ws");
                hpack_encoder::encode_header(block, ":authority", "localhost");
                hpack_encoder::encode_header(block, "sec-websocket-version", "13");
                return frame(0x1, sansio_driver_test::flag_end_headers, stream_id,
                    std::string_view(block.data(), block.size()));
            };

            if (!co_await write_all(client_preface)) {
                co_return;
            }
            if (!co_await write_all(frame(0x4, 0, 0, {}))) {
                co_return;
            }
            // Open BOTH tunnels (streams 1 and 3) before sending any frames.
            if (!co_await write_all(open_tunnel(1))) {
                co_return;
            }
            if (!co_await write_all(open_tunnel(3))) {
                co_return;
            }
            // A masked text frame down each tunnel.
            if (!co_await write_all(frame(0x0, 0, 1, masked_ws_frame(0x1, "one")))) {
                co_return;
            }
            if (!co_await write_all(frame(0x0, 0, 3, masked_ws_frame(0x1, "three")))) {
                co_return;
            }

            std::string tunnel1;
            std::string tunnel3;
            auto extract_frame = [](std::string& acc) -> std::string {
                if (acc.size() < 2) {
                    return {};
                }
                const auto len =
                    static_cast<std::size_t>(static_cast<unsigned char>(acc[1]) & 0x7FU);
                if (acc.size() < 2 + len) {
                    return {};
                }
                return acc.substr(0, 2 + len);
            };
            while (echo1.empty() || echo3.empty()) {
                char hb[ruvia::http2_frame_header_bytes];
                if (!co_await read_exact(hb, sizeof(hb))) {
                    co_return;
                }
                const auto header_value =
                    sansio_driver_test::parse_frame_header(std::string_view(hb, sizeof(hb)));
                std::string payload_value(header_value.length_, '\0');
                if (header_value.length_ != 0 && !co_await read_exact(payload_value.data(), payload_value.size())) {
                    co_return;
                }
                if (header_value.type_ != static_cast<std::uint8_t>(http2_frame_type::data)) {
                    continue;
                }
                if (header_value.stream_id_ == 1) {
                    tunnel1 += payload_value;
                    if (echo1.empty()) {
                        echo1 = extract_frame(tunnel1);
                    }
                } else if (header_value.stream_id_ == 3) {
                    tunnel3 += payload_value;
                    if (echo3.empty()) {
                        echo3 = extract_frame(tunnel3);
                    }
                }
            }
            close_client_socket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(echo1.size() == 5 && echo1.substr(2) == "one");    // FIN|text len3 "one"
    RUVIA_CHECK(echo3.size() == 7 && echo3.substr(2) == "three");  // FIN|text len5 "three"
}
