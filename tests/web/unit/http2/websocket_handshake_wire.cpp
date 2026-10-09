#include <string>

#include <asio/io_context.hpp>

#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http2_types.h"

#include "context/context_services.h"
#include "http2/http2_sans_io_session.h"
#include "http2_sansio_session_fixture.h"
#include "router/router.h"
#include "router/router_impl.h"
#include "sansio_driver_fixture.h"
#include "test_io_context.h"

RUVIA_TEST(sansio_driver_h2_unsupported_websocket_version_wire_is_bad_request) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool got_response_head = false;
    bool hpack_decode_succeeded = true;
    bool got_bad_request = false;
    bool got_supported_version = false;
    bool got_alt_svc = false;
    bool saw_forbidden_field = false;
    bool got_response_end = false;

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
            co_await ruvia::as_awaitable(ruvia::test::run_bare_http2_sans_io_session_with(
                sock, impl.route_table(), worker,
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
                const auto [ec, count] = co_await asio::async_write(sock,
                    asio::buffer(bytes_value.data(), bytes_value.size()), asio::as_tuple(asio::use_awaitable));
                co_return !ec && count == bytes_value.size();
            };
            auto read_exact = [&sock](void* data, std::size_t size) -> asio::awaitable<bool> {
                const auto [ec, count] = co_await asio::async_read(
                    sock, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && count == size;
            };
            if (!co_await write_all(client_preface) ||
                !co_await write_all(frame(0x4 /*SETTINGS*/, 0, 0, {}))) {
                co_return;
            }
            std::pmr::string block(std::pmr::get_default_resource());
            hpack_encoder::encode_header(block, ":method", "CONNECT");
            hpack_encoder::encode_header(block, ":protocol", "websocket");
            hpack_encoder::encode_header(block, ":scheme", "https");
            hpack_encoder::encode_header(block, ":path", "/ws");
            hpack_encoder::encode_header(block, ":authority", "localhost");
            hpack_encoder::encode_header(block, "sec-websocket-version", "12");
            if (!co_await write_all(frame(0x1, sansio_driver_test::flag_end_headers, 1,
                    std::string_view(block.data(), block.size())))) {
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
                    const auto decoded = decoder.decode(payload_value,
                        [&](std::string_view name, std::string_view value) {
                            if (name == ":status") {
                                got_bad_request = value == "400";
                            } else if (name == "sec-websocket-version") {
                                got_supported_version = value == "13";
                            } else if (name == "alt-svc") {
                                got_alt_svc = value == "h3=\":443\"; ma=86400";
                            } else if (name == "connection" || name == "upgrade" ||
                                       name == "sec-websocket-accept") {
                                saw_forbidden_field = true;
                            }
                            return true;
                        });
                    hpack_decode_succeeded = decoded.decoded();
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
    RUVIA_CHECK(got_bad_request);
    RUVIA_CHECK(got_supported_version);
    RUVIA_CHECK(got_alt_svc);
    RUVIA_CHECK(!saw_forbidden_field);
    RUVIA_CHECK(got_response_end);
}
