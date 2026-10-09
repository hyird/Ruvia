#include <array>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/use_future.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http1_server_semantics.h"
#include "ruvia/http/http_byte_range.h"
#include "ruvia/http/http_multipart_byte_range_plan.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/web/error.h"

#include "context_services_fixture.h"
#include "router/router.h"
#include "router/router_impl.h"
#include "server/http1_request_sequence.h"
#include "server/http1_route_dispatch.h"
#include "server/http_response_writer.h"
#include "server/http_server_response_stream_route.h"
#include "server/http_server_websocket_route.h"
#include "test_harness.h"
#include "test_io_context.h"
#include "websocket/websocket_response_headers.h"

namespace {

using asio::ip::tcp;

constexpr std::string_view automatic_alt_svc = "h3=\":443\"; ma=86400";
constexpr std::string_view masked_peer_close_frame{"\x88\x82\x11\x22\x33\x44\x12\xCA", 8};
constexpr std::string_view identity_request =
    "GET /buffered HTTP/1.1\r\nHost: example.test\r\nAccept-Encoding: identity\r\n\r\n";

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

ruvia::task<void> websocket_drain_handler(void*, ruvia::context&) {
    co_return;
}

void register_alt_svc_routes(ruvia::detail::router_impl& impl) {
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
    impl.register_websocket_route(ruvia::http_known_method::get,
        std::pmr::string("/ws", std::pmr::get_default_resource()),
        ruvia::detail::route_stream_handler_type(nullptr, &websocket_drain_handler),
        no_middlewares, no_middlewares);
}

template <typename emit_type>
std::string capture_http1_wire(emit_type emit) {
    auto& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const auto endpoint = acceptor.local_endpoint();
    std::string bytes;

    auto server = asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            socket.non_blocking(true);
            co_await ruvia::as_awaitable(emit(socket));
            std::error_code ignored;
            socket.shutdown(tcp::socket::shutdown_send, ignored);
        },
        asio::use_future);
    auto client = asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket socket(io);
            co_await socket.async_connect(endpoint, asio::use_awaitable);
            std::array<char, 2048> buffer{};
            for (;;) {
                const auto [error, count] = co_await socket.async_read_some(
                    asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));
                bytes.append(buffer.data(), count);
                if (error == asio::error::eof) {
                    break;
                }
                if (error) {
                    throw std::system_error(error);
                }
            }
        },
        asio::use_future);

    io.run();
    server.get();
    client.get();
    return bytes;
}

void parse_http1_request(std::string_view wire, ruvia::http1_server_request_parse_state& parsed_value,
    ruvia::request_memory& memory) {
    ruvia::http1_server_request_parser parser;
    parser.parse_head(wire, parsed_value, 0, memory.resource());
    if (parsed_value.head_ready() == nullptr) {
        throw std::runtime_error("invalid HTTP/1 request in Alt-Svc test");
    }
}

ruvia::task<void> write_buffered_response(tcp::socket& socket, ruvia::worker_memory& worker_value,
    const ruvia::http_request& request, ruvia::http_response& response,
    ruvia::http1_request_connection_plan connection_plan) {
    ruvia::http_response_head_buffer head(worker_value.allocator<char>());
    std::pmr::string file_chunk(worker_value.allocator<char>());
    const auto write_plan = ruvia::get_http1_buffered_response_plan(
        ruvia::plan_buffered_http_response_write(request.known_method(), response), connection_plan);
    const auto result_value = co_await ruvia::detail::write_response(
        socket, worker_value, &head, &file_chunk, response, write_plan);
    if (result_value.completed() == nullptr) {
        throw std::runtime_error("HTTP/1 test response write failed");
    }
}

ruvia::detail::context_services tls_services() {
    return ruvia::test::test_context_services()
        .with_tls_transport("127.0.0.1")
        .with_automatic_alt_svc(automatic_alt_svc);
}

ruvia::task<void> emit_buffered_route(tcp::socket& socket, const ruvia::detail::route_table& routes_value,
    std::string_view path, bool tls) {
    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    std::string request_wire("GET ");
    request_wire.append(path).append(
        " HTTP/1.1\r\nHost: example.test\r\n"
        "Accept-Encoding: identity\r\n\r\n");
    ruvia::http1_server_request_parse_state parsed;
    parse_http1_request(request_wire, parsed, memory);
    const auto services = tls ? tls_services()
                              : ruvia::test::test_context_services().with_plain_transport("127.0.0.1");
    const auto resolution = routes_value.resolve(parsed.request_);
    auto response = co_await routes_value.dispatch_buffered_response(parsed.request_, resolution, memory,
        ruvia::detail::document_root_binding::none(), services);
    const auto connection_plan = ruvia::detail::require_http1_final_response_commit(
        response, parsed.connection_plan_);
    co_await write_buffered_response(socket, worker, parsed.request_, response, connection_plan);
}

ruvia::task<void> emit_protocol_error(tcp::socket& socket, const ruvia::detail::route_table& routes_value) {
    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    ruvia::http1_server_request_parse_state parsed;
    parse_http1_request(identity_request, parsed, memory);
    auto response = co_await routes_value.handle_error(parsed.request_, memory,
        ruvia::http_error_info({.status_ = ruvia::http_status::bad_request,
            .message_ = "invalid protocol request"}),
        tls_services());
    const auto connection_plan = ruvia::detail::require_http1_final_response_commit(
        response, parsed.connection_plan_);
    co_await write_buffered_response(socket, worker, parsed.request_, response, connection_plan);
}

ruvia::task<void> emit_streaming_route(tcp::socket& socket, const ruvia::detail::route_table& routes_value) {
    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    constexpr std::string_view request_wire =
        "GET /stream HTTP/1.1\r\nHost: example.test\r\nAccept-Encoding: identity\r\n\r\n";
    ruvia::http1_server_request_parse_state parsed;
    parse_http1_request(request_wire, parsed, memory);
    auto resolution = routes_value.resolve(parsed.request_);
    const auto* resolved = resolution.resolved();
    const auto* request_head = parsed.head_ready();
    if (resolved == nullptr || request_head == nullptr) {
        throw std::runtime_error("HTTP/1 streaming route did not resolve");
    }
    auto coding = parsed.response_coding_selection();
    if (coding.selected() == nullptr) {
        throw std::runtime_error("HTTP/1 test request has no response coding");
    }
    ruvia::connection_scanner::entry_type scanner_entry;
    ruvia::detail::http_server_options options;
    ruvia::detail::http1_request_sequence request_sequence(std::nullopt);
    ruvia::http_response response({.resource_ = memory.resource()});
    ruvia::http_response_head_buffer response_head(worker.allocator<char>());
    ruvia::detail::http1_route_dispatch<tcp::socket> dispatch{.stream_ = socket,
        .memory_ = worker,
        .scanner_entry_ = scanner_entry,
        .parsed_ = parsed,
        .response_coding_ = *coding.selected(),
        .response_coding_availability_ = ruvia::detail::http_response_coding_availability::identity_only,
        .routes_ = routes_value,
        .request_memory_ = memory,
        .base_route_services_ = tls_services(),
        .options_ = options,
        .response_ = response,
        .request_sequence_ = request_sequence};
    static_cast<void>(co_await ruvia::detail::dispatch_http_response_stream_route(
        dispatch, response_head, *request_head, *resolved, {}, ruvia::protocol_byte_limit::limited(ruvia::default_max_buffered_body_bytes)));
}

ruvia::task<void> emit_websocket_route(tcp::socket& socket,
    const ruvia::detail::route_table& routes_value, std::string_view version) {
    auto attachment = ruvia::attach_event_loop(
        static_cast<asio::io_context&>(socket.get_executor().context()), {.queue_capacity_ = 64});
    const auto worker_handle_value = attachment.loop().handle();
    ruvia::stop_token stop_token;
    const auto services = ruvia::detail::context_services(worker_handle_value, stop_token)
                              .with_tls_transport("127.0.0.1")
                              .with_automatic_alt_svc(automatic_alt_svc);
    const std::string pending_frames(masked_peer_close_frame);
    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    std::string request_wire =
        "GET /ws HTTP/1.1\r\nHost: example.test\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: ";
    request_wire.append(version).append("\r\nAccept-Encoding: identity\r\n\r\n");
    ruvia::http1_server_request_parse_state parsed;
    parse_http1_request(request_wire, parsed, memory);
    auto resolution = routes_value.resolve(parsed.request_);
    const auto* resolved = resolution.resolved();
    if (resolved == nullptr) {
        throw std::runtime_error("HTTP/1 WebSocket route did not resolve");
    }
    auto coding = parsed.response_coding_selection();
    if (coding.selected() == nullptr) {
        throw std::runtime_error("HTTP/1 WebSocket test request has no response coding");
    }
    ruvia::connection_scanner::entry_type scanner_entry;
    ruvia::detail::http_server_options options;
    ruvia::detail::http1_request_sequence request_sequence(std::nullopt);
    ruvia::http_response response({.resource_ = memory.resource()});
    ruvia::detail::http1_route_dispatch<tcp::socket> dispatch{.stream_ = socket,
        .memory_ = worker,
        .scanner_entry_ = scanner_entry,
        .parsed_ = parsed,
        .response_coding_ = *coding.selected(),
        .response_coding_availability_ = ruvia::detail::http_response_coding_availability::identity_only,
        .routes_ = routes_value,
        .request_memory_ = memory,
        .base_route_services_ = services,
        .options_ = options,
        .response_ = response,
        .request_sequence_ = request_sequence};
    const auto completion = co_await ruvia::detail::dispatch_http_websocket_route(
        dispatch, *resolved, pending_frames);
    if (version == "13") {
        if (completion.has_value()) {
            throw std::runtime_error("valid HTTP/1 WebSocket request was not upgraded");
        }
    } else {
        if (!completion.has_value() || completion->buffered_response() == nullptr) {
            throw std::runtime_error("unsupported HTTP/1 WebSocket version was not rejected");
        }
        co_await write_buffered_response(
            socket, worker, parsed.request_, response, completion->connection_plan());
    }
}

std::string_view first_response_head(std::string_view wire) {
    const auto end = wire.find("\r\n\r\n");
    return end == std::string_view::npos ? wire : wire.substr(0, end + 4);
}

}  // namespace

RUVIA_TEST(http1_tls_wire_responses_emit_alt_svc_and_honor_application_headers) {
    ruvia::detail::router router;
    auto& impl = ruvia::detail::router_impl::from(router);
    register_alt_svc_routes(impl);
    impl.finalize();
    const auto& routes_value = impl.route_table();

    const auto buffered = capture_http1_wire([&](tcp::socket& socket) {
        return emit_buffered_route(socket, routes_value, "/buffered", true);
    });
    RUVIA_CHECK((first_response_head(buffered).find("Alt-Svc: h3=\":443\"; ma=86400\r\n") != std::string_view::npos));
    RUVIA_CHECK(first_response_head(buffered).starts_with("HTTP/1.1 200"));

    const auto override = capture_http1_wire([&](tcp::socket& socket) {
        return emit_buffered_route(socket, routes_value, "/override", true);
    });
    RUVIA_CHECK((first_response_head(override).find("Alt-Svc: h3=\":9443\"; ma=10\r\n") != std::string_view::npos));
    RUVIA_CHECK(first_response_head(override).find(automatic_alt_svc) == std::string_view::npos);

    const auto erased = capture_http1_wire([&](tcp::socket& socket) {
        return emit_buffered_route(socket, routes_value, "/erase", true);
    });
    RUVIA_CHECK(!(first_response_head(erased).find("Alt-Svc:") != std::string_view::npos));

    const auto plain = capture_http1_wire([&](tcp::socket& socket) {
        return emit_buffered_route(socket, routes_value, "/buffered", false);
    });
    RUVIA_CHECK(!(first_response_head(plain).find("Alt-Svc:") != std::string_view::npos));

    const auto protocol_error = capture_http1_wire([&](tcp::socket& socket) {
        return emit_protocol_error(socket, routes_value);
    });
    RUVIA_CHECK(first_response_head(protocol_error).starts_with("HTTP/1.1 400"));
    RUVIA_CHECK((first_response_head(protocol_error).find("Alt-Svc: h3=\":443\"; ma=86400\r\n") != std::string_view::npos));

    const auto not_found = capture_http1_wire([&](tcp::socket& socket) {
        return emit_buffered_route(socket, routes_value, "/missing", true);
    });
    RUVIA_CHECK(first_response_head(not_found).starts_with("HTTP/1.1 404"));
    RUVIA_CHECK((first_response_head(not_found).find("Alt-Svc: h3=\":443\"; ma=86400\r\n") != std::string_view::npos));

    const auto exception = capture_http1_wire([&](tcp::socket& socket) {
        return emit_buffered_route(socket, routes_value, "/exception", true);
    });
    RUVIA_CHECK(first_response_head(exception).starts_with("HTTP/1.1 500"));
    RUVIA_CHECK((first_response_head(exception).find("Alt-Svc: h3=\":443\"; ma=86400\r\n") != std::string_view::npos));
}

RUVIA_TEST(http1_tls_streaming_and_websocket_wire_responses_emit_alt_svc) {
    ruvia::detail::router router;
    auto& impl = ruvia::detail::router_impl::from(router);
    register_alt_svc_routes(impl);
    impl.finalize();
    const auto& routes_value = impl.route_table();

    const auto streaming = capture_http1_wire([&](tcp::socket& socket) {
        return emit_streaming_route(socket, routes_value);
    });
    RUVIA_CHECK(first_response_head(streaming).starts_with("HTTP/1.1 200"));
    RUVIA_CHECK((first_response_head(streaming).find("Alt-Svc: h3=\":443\"; ma=86400\r\n") != std::string_view::npos));
    RUVIA_CHECK((first_response_head(streaming).find("Transfer-Encoding: chunked\r\n") != std::string_view::npos));
    RUVIA_CHECK((streaming.find("streamed") != std::string_view::npos));

    const auto websocket_value = capture_http1_wire([&](tcp::socket& socket) {
        return emit_websocket_route(socket, routes_value, "13");
    });
    const auto handshake = first_response_head(websocket_value);
    RUVIA_CHECK(handshake.starts_with("HTTP/1.1 101"));
    RUVIA_CHECK((handshake.find("alt-svc: h3=\":443\"; ma=86400\r\n") != std::string_view::npos));
}

RUVIA_TEST(http1_buffered_response_writer_sends_multipart_file_slices_with_exact_length) {
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() / "ruvia-http1-multipart-writer.bin";
    const std::string content(50000, 'h');
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << content;
    }

    const auto wire = capture_http1_wire([&](tcp::socket& socket) -> ruvia::task<void> {
        ruvia::worker_memory worker;
        ruvia::request_memory memory(worker);
        ruvia::http1_server_request_parse_state parsed;
        constexpr std::string_view request_wire =
            "GET /multipart HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n";
        parse_http1_request(request_wire, parsed, memory);
        const auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-19999,30000-49999", content.size());
        auto multipart = ruvia::make_http_multipart_byte_range_plan(
            ranges, content.size(), "text/plain", "h1_writer_boundary", {}, memory.resource());
        ruvia::http_response response({.resource_ = memory.resource()});
        response.status(ruvia::http_status::partial_content);
        response.header("Content-Type", multipart.content_type());
        response.multipart_file_body(path, content.size(),
            ruvia::http_response_file_identity::unchecked(), std::move(multipart));
        const auto connection_plan = ruvia::detail::require_http1_final_response_commit(
            response, parsed.connection_plan_);
        co_await write_buffered_response(socket, worker, parsed.request_, response, connection_plan);
    });

    const auto head = first_response_head(wire);
    RUVIA_CHECK(head.starts_with("HTTP/1.1 206"));
    const auto length_start = head.find("Content-Length: ");
    RUVIA_CHECK(length_start != std::string_view::npos);
    const auto length_end = head.find("\r\n", length_start);
    const auto body = wire.substr(head.size());
    const std::string expected =
        "--h1_writer_boundary\r\nContent-Type: text/plain\r\nContent-Range: bytes 0-19999/50000\r\n\r\n" +
        content.substr(0, 20000) + "\r\n--h1_writer_boundary\r\nContent-Type: text/plain\r\n" +
        "Content-Range: bytes 30000-49999/50000\r\n\r\n" + content.substr(30000) +
        "\r\n--h1_writer_boundary--\r\n";
    RUVIA_CHECK_EQ(body, expected);
    if (length_start != std::string_view::npos && length_end != std::string_view::npos) {
        const auto declared = head.substr(length_start + 16, length_end - length_start - 16);
        RUVIA_CHECK_EQ(declared, std::to_string(expected.size()));
    }

    bool identity_failed_after_commit = false;
    const auto identity_failure_wire = capture_http1_wire([&](tcp::socket& socket) -> ruvia::task<void> {
        ruvia::worker_memory worker;
        ruvia::request_memory memory(worker);
        ruvia::http1_server_request_parse_state parsed;
        constexpr std::string_view request_wire =
            "GET /multipart HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n";
        parse_http1_request(request_wire, parsed, memory);
        const auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-1,10-11", content.size());
        auto multipart = ruvia::make_http_multipart_byte_range_plan(
            ranges, content.size(), "text/plain", "h1_identity_boundary", {}, memory.resource());
        ruvia::http_response response({.resource_ = memory.resource()});
        response.status(ruvia::http_status::partial_content);
        response.header("Content-Type", multipart.content_type());
        response.multipart_file_body(path, content.size(),
            ruvia::http_response_file_identity::checked({}), std::move(multipart));
        const auto write_plan = ruvia::get_http1_buffered_response_plan(
            ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response),
            parsed.connection_plan_);
        ruvia::http_response_head_buffer response_head(worker.allocator<char>());
        std::pmr::string file_chunk(worker.allocator<char>());
        const auto result_value = co_await ruvia::detail::write_response(
            socket, worker, &response_head, &file_chunk, response, write_plan);
        identity_failed_after_commit = result_value.failed_after_commit() != nullptr;
    });
    RUVIA_CHECK(identity_failed_after_commit);
    RUVIA_CHECK(first_response_head(identity_failure_wire).starts_with("HTTP/1.1 206"));
    const auto failed_head = first_response_head(identity_failure_wire);
    RUVIA_CHECK(identity_failure_wire.substr(failed_head.size()).find("hh") == std::string_view::npos);
    fs::remove(path);
}

RUVIA_TEST(http1_tls_unsupported_websocket_version_wire_is_400_without_upgrade_fields) {
    ruvia::detail::router router;
    auto& impl = ruvia::detail::router_impl::from(router);
    register_alt_svc_routes(impl);
    impl.finalize();
    const auto& routes_value = impl.route_table();

    const auto wire = capture_http1_wire([&](tcp::socket& socket) {
        return emit_websocket_route(socket, routes_value, "12");
    });
    const auto head = first_response_head(wire);
    RUVIA_CHECK(head.starts_with("HTTP/1.1 400"));
    RUVIA_CHECK((head.find("Sec-WebSocket-Version: 13\r\n") != std::string_view::npos));
    RUVIA_CHECK((head.find("Alt-Svc: h3=\":443\"; ma=86400\r\n") != std::string_view::npos));
    RUVIA_CHECK(!(head.find("Upgrade:") != std::string_view::npos));
    RUVIA_CHECK(!(head.find("Connection: Upgrade") != std::string_view::npos));
}
