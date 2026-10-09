#include <array>
#include <map>
#include <set>
#include <stdexcept>
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

namespace {

constexpr std::string_view automatic_alt_svc = "h3=\":443\"; ma=86400";

ruvia::task<ruvia::http_response> alt_svc_buffered_handler(void*, ruvia::context& context_value) {
    co_return context_value.text("buffered");
}

ruvia::task<ruvia::http_response> alt_svc_override_handler(void*, ruvia::context& context_value) {
    context_value.header("Alt-Svc", "h3=\":9443\"; ma=10");
    co_return context_value.text("overridden");
}

ruvia::task<ruvia::http_response> alt_svc_erase_handler(void*, ruvia::context& context_value) {
    context_value.remove_header("Alt-Svc");
    co_return context_value.text("removed");
}

ruvia::task<ruvia::http_response> alt_svc_exception_handler(void*, ruvia::context&) {
    throw std::runtime_error("test exception");
    co_return ruvia::http_response{};
}

ruvia::task<void> alt_svc_stream_handler(void*, ruvia::context& context_value) {
    auto& stream = context_value.stream_text();
    co_await stream.write("streamed");
    co_await stream.end();
}

struct response_headers final {
    std::string status_;
    std::string alt_svc_;
    bool has_alt_svc_{false};
    bool has_connection_{false};
    bool has_upgrade_{false};
    bool has_websocket_accept_{false};
};

}  // namespace

RUVIA_TEST(sansio_driver_h2_emits_alt_svc_on_buffered_streaming_and_error_responses) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    std::map<std::uint32_t, response_headers> responses;
    std::map<std::uint32_t, std::string> bodies;
    bool hpack_decode_succeeded = true;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory worker;
            ruvia::detail::router router;
            auto& impl = ruvia::detail::router_impl::from(router);
            const auto no_middlewares = std::span<const ruvia::detail::controller_middleware_descriptor>{};
            impl.register_route(ruvia::http_known_method::get,
                std::pmr::string("/buffered", std::pmr::get_default_resource()),
                ruvia::detail::route_handler_type(nullptr, &alt_svc_buffered_handler),
                ruvia::detail::request_body_mode::buffered, no_middlewares, no_middlewares);
            impl.register_route(ruvia::http_known_method::get,
                std::pmr::string("/override", std::pmr::get_default_resource()),
                ruvia::detail::route_handler_type(nullptr, &alt_svc_override_handler),
                ruvia::detail::request_body_mode::buffered, no_middlewares, no_middlewares);
            impl.register_route(ruvia::http_known_method::get,
                std::pmr::string("/erase", std::pmr::get_default_resource()),
                ruvia::detail::route_handler_type(nullptr, &alt_svc_erase_handler),
                ruvia::detail::request_body_mode::buffered, no_middlewares, no_middlewares);
            impl.register_route(ruvia::http_known_method::get,
                std::pmr::string("/exception", std::pmr::get_default_resource()),
                ruvia::detail::route_handler_type(nullptr, &alt_svc_exception_handler),
                ruvia::detail::request_body_mode::buffered, no_middlewares, no_middlewares);
            impl.register_response_stream_route(ruvia::http_known_method::get,
                std::pmr::string("/stream", std::pmr::get_default_resource()),
                ruvia::detail::route_stream_handler_type(nullptr, &alt_svc_stream_handler),
                no_middlewares, no_middlewares);
            impl.finalize();
            co_await ruvia::as_awaitable(ruvia::test::run_bare_http2_sans_io_session_with(
                sock, impl.route_table(), worker,
                [](ruvia::detail::context_services services) {
                    return services.with_tls_transport("127.0.0.1")
                        .with_automatic_alt_svc(automatic_alt_svc);
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
            auto request_on = [](std::uint32_t stream_id, std::string_view path) {
                std::pmr::string block(std::pmr::get_default_resource());
                ruvia::hpack_encoder::encode_header(block, ":method", "GET");
                ruvia::hpack_encoder::encode_header(block, ":path", path);
                ruvia::hpack_encoder::encode_header(block, ":scheme", "https");
                ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
                return frame(0x1,
                    sansio_driver_test::flag_end_stream | sansio_driver_test::flag_end_headers,
                    stream_id, std::string_view(block.data(), block.size()));
            };

            if (!co_await write_all(client_preface) ||
                !co_await write_all(frame(0x4 /*SETTINGS*/, 0, 0, {}))) {
                co_return;
            }
            std::string requests;
            for (const auto& [stream_id, path] : std::array{
                     std::pair{1U, std::string_view("/buffered")},
                     std::pair{3U, std::string_view("/override")},
                     std::pair{5U, std::string_view("/erase")},
                     std::pair{7U, std::string_view("/exception")},
                     std::pair{9U, std::string_view("/missing")},
                     std::pair{11U, std::string_view("/stream")}}) {
                requests += request_on(stream_id, path);
            }
            if (!co_await write_all(requests)) {
                co_return;
            }

            ruvia::hpack_decoder decoder({.resource_ = std::pmr::get_default_resource()});
            std::set<std::uint32_t> pending{1, 3, 5, 7, 9, 11};
            while (!pending.empty()) {
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
                    pending.contains(header_value.stream_id_)) {
                    auto& fields_value = responses[header_value.stream_id_];
                    const auto decoded = decoder.decode(payload_value,
                        [&fields_value](std::string_view name, std::string_view value) {
                            if (name == ":status") {
                                fields_value.status_.assign(value);
                            } else if (name == "alt-svc") {
                                fields_value.has_alt_svc_ = true;
                                fields_value.alt_svc_.assign(value);
                            } else if (name == "connection") {
                                fields_value.has_connection_ = true;
                            } else if (name == "upgrade") {
                                fields_value.has_upgrade_ = true;
                            } else if (name == "sec-websocket-accept") {
                                fields_value.has_websocket_accept_ = true;
                            }
                            return true;
                        });
                    hpack_decode_succeeded = hpack_decode_succeeded && decoded.decoded();
                } else if (header_value.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) &&
                           pending.contains(header_value.stream_id_)) {
                    bodies[header_value.stream_id_].append(payload_value);
                }
                if (pending.contains(header_value.stream_id_) &&
                    (header_value.flags_ & sansio_driver_test::flag_end_stream) != 0) {
                    pending.erase(header_value.stream_id_);
                }
            }
            sansio_driver_test::close_client_socket(sock);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(hpack_decode_succeeded);
    for (const auto stream_id : {1U, 7U, 9U, 11U}) {
        const auto& fields_value = responses[stream_id];
        RUVIA_CHECK(fields_value.has_alt_svc_);
        RUVIA_CHECK_EQ(fields_value.alt_svc_, std::string(automatic_alt_svc));
        RUVIA_CHECK(!fields_value.has_connection_);
        RUVIA_CHECK(!fields_value.has_upgrade_);
        RUVIA_CHECK(!fields_value.has_websocket_accept_);
    }
    RUVIA_CHECK_EQ(responses[1].status_, std::string("200"));
    RUVIA_CHECK_EQ(responses[7].status_, std::string("500"));
    RUVIA_CHECK_EQ(responses[9].status_, std::string("404"));
    RUVIA_CHECK_EQ(responses[11].status_, std::string("200"));
    RUVIA_CHECK_EQ(responses[3].alt_svc_, std::string("h3=\":9443\"; ma=10"));
    RUVIA_CHECK_EQ(responses[5].status_, std::string("200"));
    RUVIA_CHECK(!responses[5].has_alt_svc_);
    RUVIA_CHECK_EQ(bodies[11], std::string("streamed"));
}
