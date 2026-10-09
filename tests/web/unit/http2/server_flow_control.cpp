#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http_response.h"

#include "http2/http2_sans_io_session.h"
#include "http2_sansio_session_fixture.h"
#include "router/route_table.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

using asio::ip::tcp;
constexpr std::string_view client_preface = ruvia::http2_client_preface;
constexpr std::uint8_t flag_end_stream = 0x1;
constexpr std::uint8_t flag_end_headers = 0x4;
constexpr std::uint32_t connection_window_update_threshold = 512 * 1024;

std::uint32_t read31_bit_big_endian(const char* bytes_value) {
    const auto* value = reinterpret_cast<const unsigned char*>(bytes_value);
    return ((static_cast<std::uint32_t>(value[0]) & 0x7fU) << 24) |
           (static_cast<std::uint32_t>(value[1]) << 16) |
           (static_cast<std::uint32_t>(value[2]) << 8) |
           static_cast<std::uint32_t>(value[3]);
}

std::uint32_t read_big_endian32(const char* bytes_value) {
    const auto* value = reinterpret_cast<const unsigned char*>(bytes_value);
    return (static_cast<std::uint32_t>(value[0]) << 24) |
           (static_cast<std::uint32_t>(value[1]) << 16) |
           (static_cast<std::uint32_t>(value[2]) << 8) |
           static_cast<std::uint32_t>(value[3]);
}

std::string frame(
    std::uint8_t type, std::uint8_t flags, std::uint32_t stream_id, std::string_view payload_value) {
    std::string bytes_value(ruvia::http2_frame_header_bytes, '\0');
    if (!ruvia::encode_http2_frame_header(bytes_value, static_cast<std::uint32_t>(payload_value.size()),
            static_cast<ruvia::http2_frame_type>(type), flags, stream_id)) {
        throw std::length_error("invalid test HTTP/2 frame");
    }
    bytes_value.append(payload_value);
    return bytes_value;
}

// Drives the real sans-I/O h2 server session as the peer: completes the handshake, sends a
// body-less request on stream 1, then sends DATA on that now-ended stream. Those
// frames are dropped by the server (RFC 9113 6.9.1: they still count against the
// connection flow-control window). Returns every connection-level (stream 0)
// WINDOW_UPDATE increment emitted by the real session.
std::vector<std::uint32_t> collect_connection_window_updates_for_dropped_data(std::uint32_t data_bytes) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    std::vector<std::uint32_t> increments;

    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    // Server side: a real session with an empty route table (requests 404, which is
    // enough to open and finish the stream — the exact post-request stream state does
    // not matter, every drop branch must credit the connection window).
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory worker;
            ruvia::detail::route_table routes_value(worker.resource());
            co_await ruvia::as_awaitable(
                ruvia::test::run_bare_plain_http2_sans_io_session(sock, routes_value, worker, "127.0.0.1"));
        },
        asio::detached);

    // Client side: our synthetic HTTP/2 peer.
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

            // Preface + empty client SETTINGS.
            if (!co_await write_all(client_preface)) {
                co_return;
            }
            if (!co_await write_all(frame(0x4 /*SETTINGS*/, 0, 0, {}))) {
                co_return;
            }

            // A complete, body-less request on stream 1 (END_STREAM + END_HEADERS).
            std::pmr::string header_block(std::pmr::get_default_resource());
            ruvia::hpack_encoder::encode_header(header_block, ":method", "GET");
            ruvia::hpack_encoder::encode_header(header_block, ":path", "/");
            ruvia::hpack_encoder::encode_header(header_block, ":scheme", "http");
            ruvia::hpack_encoder::encode_header(header_block, ":authority", "localhost");
            if (!co_await write_all(
                    frame(0x1 /*HEADERS*/, flag_end_stream | flag_end_headers, 1,
                        std::string_view(header_block.data(), header_block.size())))) {
                co_return;
            }

            // DATA on the now-ended stream: every frame must be dropped yet still
            // join the connection credit batch. Keep each payload within the local
            // SETTINGS_MAX_FRAME_SIZE advertised by the server.
            std::string data(ruvia::http2_default_max_frame_size, 'x');
            auto remaining = data_bytes;
            while (remaining != 0) {
                const auto chunk_bytes =
                    static_cast<std::size_t>(remaining < data.size() ? remaining : data.size());
                if (!co_await write_all(
                        frame(0x0 /*DATA*/, 0, 1, std::string_view(data.data(), chunk_bytes)))) {
                    co_return;
                }
                remaining -= static_cast<std::uint32_t>(chunk_bytes);
            }

            // Half-close so the server's read loop reaches EOF and tears down.
            asio::error_code ignore;
            sock.shutdown(tcp::socket::shutdown_send, ignore);

            // Drain every frame the server emits, recording stream-0 WINDOW_UPDATEs.
            for (;;) {
                char header_bytes[ruvia::http2_frame_header_bytes];
                if (!co_await read_exact(header_bytes, sizeof(header_bytes))) {
                    break;
                }
                const auto parsed_header =
                    ruvia::parse_http2_frame_header(std::string_view(header_bytes, sizeof(header_bytes)));
                if (!parsed_header) {
                    throw std::runtime_error("invalid HTTP/2 frame header from server");
                }
                const auto& header_value = *parsed_header;
                std::string payload_value(header_value.length_, '\0');
                if (header_value.length_ != 0 && !co_await read_exact(payload_value.data(), payload_value.size())) {
                    break;
                }
                if (header_value.type_ == 0x8 /*WINDOW_UPDATE*/ && header_value.stream_id_ == 0) {
                    increments.push_back(
                        read31_bit_big_endian(payload_value.data()));
                }
            }
        },
        asio::detached);

    io.run();
    return increments;
}

// Drives a real server with a request whose content-length is nonzero but whose
// HEADERS frame carries END_STREAM (so no DATA can follow). Returns the RST_STREAM
// error code the server sends for stream 1, or std::nullopt if the stream was
// accepted instead of rejected.
std::optional<std::uint32_t> rst_error_for_bodyless_content_length_request() {
    asio::io_context& io = ruvia::test::new_test_io_context();
    std::optional<std::uint32_t> rst_error;

    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory worker;
            ruvia::detail::route_table routes_value(worker.resource());
            co_await ruvia::as_awaitable(
                ruvia::test::run_bare_plain_http2_sans_io_session(sock, routes_value, worker, "127.0.0.1"));
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

            // content-length: 5 with END_STREAM on HEADERS: no DATA can ever arrive,
            // so the declared length can never be satisfied -> malformed (RFC 9113
            // §8.1.1). The server must RST_STREAM(PROTOCOL_ERROR).
            std::pmr::string header_block(std::pmr::get_default_resource());
            ruvia::hpack_encoder::encode_header(header_block, ":method", "POST");
            ruvia::hpack_encoder::encode_header(header_block, ":path", "/");
            ruvia::hpack_encoder::encode_header(header_block, ":scheme", "http");
            ruvia::hpack_encoder::encode_header(header_block, ":authority", "localhost");
            ruvia::hpack_encoder::encode_header(header_block, "content-length", "5");
            if (!co_await write_all(
                    frame(0x1 /*HEADERS*/, flag_end_stream | flag_end_headers, 1,
                        std::string_view(header_block.data(), header_block.size())))) {
                co_return;
            }

            asio::error_code ignore;
            sock.shutdown(tcp::socket::shutdown_send, ignore);

            for (;;) {
                char header_bytes[ruvia::http2_frame_header_bytes];
                if (!co_await read_exact(header_bytes, sizeof(header_bytes))) {
                    break;
                }
                const auto parsed_header =
                    ruvia::parse_http2_frame_header(std::string_view(header_bytes, sizeof(header_bytes)));
                if (!parsed_header) {
                    throw std::runtime_error("invalid HTTP/2 frame header from server");
                }
                const auto& header_value = *parsed_header;
                std::string payload_value(header_value.length_, '\0');
                if (header_value.length_ != 0 && !co_await read_exact(payload_value.data(), payload_value.size())) {
                    break;
                }
                if (header_value.type_ == 0x3 /*RST_STREAM*/ && header_value.stream_id_ == 1 &&
                    payload_value.size() == 4) {
                    rst_error = read_big_endian32(payload_value.data());
                }
            }
        },
        asio::detached);

    io.run();
    return rst_error;
}

// A not-found handler whose response carries a header value large enough that the
// HPACK-encoded response header block exceeds the default 16384-byte max frame size,
// forcing the server onto its HEADERS + CONTINUATION path.
ruvia::task<ruvia::http_response> large_header_not_found_handler(ruvia::context& context_value) {
    (void)context_value;
    ruvia::http_response response({.resource_ = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::not_found);
    static const std::string big_value(40000, 'a');
    response.header("x-large", big_value);
    co_return response;
}

struct emitted_frame {
    std::uint8_t type_;
    std::uint32_t stream_id_;
    std::uint8_t flags_;
};

// Two requests (streams 1 and 3) arrive together; both 404 into the large-header
// handler, so both produce a multi-frame (HEADERS + CONTINUATION) response header
// block and their writes contend for the single write turn. Returns every frame the
// server emits, so the test can assert RFC 9113 6.10: a stream's HEADERS + CONTINUATION
// frames must be contiguous on the wire, never interleaved with another stream's frame.
std::vector<emitted_frame> frames_for_concurrent_large_header_responses() {
    asio::io_context& io = ruvia::test::new_test_io_context();
    std::vector<emitted_frame> frames;
    bool server_completed = false;
    bool client_completed = false;
    bool watchdog_completed = false;
    bool watchdog_cancelled = false;
    bool timed_out = false;
    std::exception_ptr server_failure;
    std::exception_ptr client_failure;

    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    tcp::socket server_socket(io);
    tcp::socket client_socket(io);
    asio::steady_timer watchdog(io);
    const auto finish_value = [&] {
        if (server_completed && client_completed) {
            if (!watchdog_cancelled) {
                watchdog_cancelled = true;
                watchdog.cancel();
            }
            if (watchdog_completed) {
                io.stop();
            }
        }
    };
    watchdog.expires_after(std::chrono::seconds(5));
    watchdog.async_wait([&](const asio::error_code& error) {
        watchdog_completed = true;
        if (!error) {
            timed_out = true;
            asio::error_code ignored;
            acceptor.close(ignored);
            client_socket.close(ignored);
            server_socket.close(ignored);
        }
        finish_value();
    });

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            if (timed_out) {
                co_return;
            }
            server_socket = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory worker;
            ruvia::detail::route_table routes_value(worker.resource());
            auto handler = &large_header_not_found_handler;
            routes_value.set_not_found_handler(
                ruvia::detail::callback_access::bind<ruvia::task<ruvia::http_response>(
                    ruvia::context&)>(handler));
            co_await ruvia::as_awaitable(
                ruvia::test::run_bare_plain_http2_sans_io_session(server_socket, routes_value, worker, "127.0.0.1"));
        },
        [&](std::exception_ptr failure) {
            server_failure = failure;
            server_completed = true;
            asio::error_code ignored;
            server_socket.close(ignored);
            finish_value();
        });

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            if (timed_out) {
                co_return;
            }
            auto& sock = client_socket;
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
            ruvia::hpack_encoder::encode_header(header_block, ":method", "GET");
            ruvia::hpack_encoder::encode_header(header_block, ":path", "/");
            ruvia::hpack_encoder::encode_header(header_block, ":scheme", "http");
            ruvia::hpack_encoder::encode_header(header_block, ":authority", "localhost");
            const auto req_view = std::string_view(header_block.data(), header_block.size());

            // Both requests in a single write, so the server dispatches them together.
            std::string both;
            both += frame(0x1 /*HEADERS*/, flag_end_stream | flag_end_headers, 1, req_view);
            both += frame(0x1 /*HEADERS*/, flag_end_stream | flag_end_headers, 3, req_view);
            if (!co_await write_all(both)) {
                co_return;
            }

            bool first_completed = false;
            bool second_completed = false;
            for (;;) {
                char header_bytes[ruvia::http2_frame_header_bytes];
                if (!co_await read_exact(header_bytes, sizeof(header_bytes))) {
                    break;
                }
                const auto parsed_header =
                    ruvia::parse_http2_frame_header(std::string_view(header_bytes, sizeof(header_bytes)));
                if (!parsed_header) {
                    throw std::runtime_error("invalid HTTP/2 frame header from server");
                }
                const auto& header_value = *parsed_header;
                std::string payload_value(header_value.length_, '\0');
                if (header_value.length_ != 0 && !co_await read_exact(payload_value.data(), payload_value.size())) {
                    break;
                }
                frames.push_back(emitted_frame{
                    static_cast<std::uint8_t>(header_value.type_), header_value.stream_id_, header_value.flags_});
                if ((header_value.type_ == 0x1 /*HEADERS*/ || header_value.type_ == 0x9 /*CONTINUATION*/) &&
                    (header_value.flags_ & flag_end_headers) != 0) {
                    first_completed |= header_value.stream_id_ == 1;
                    second_completed |= header_value.stream_id_ == 3;
                    if (first_completed && second_completed) {
                        break;
                    }
                }
            }
        },
        [&](std::exception_ptr failure) {
            client_failure = failure;
            client_completed = true;
            asio::error_code ignored;
            client_socket.close(ignored);
            acceptor.close(ignored);
            finish_value();
        });

    do {
        io.restart();
        io.run();
    } while (!server_completed || !client_completed || !watchdog_completed);
    if (timed_out) {
        throw std::runtime_error("HTTP/2 header fixture timed out");
    }
    if (client_failure) {
        std::rethrow_exception(client_failure);
    }
    if (server_failure) {
        std::rethrow_exception(server_failure);
    }
    return frames;
}

// The on-disk path of the short file the truncated-file-body handler serves. Set by
// rst_error_for_truncated_file_body() before the server runs; the single-threaded io_context
// makes this static safe.
std::string& truncated_file_body_path() {
    static std::string path;
    return path;
}

// A handler whose file-body response advertises a length far larger than the bytes
// actually on disk. writeFileBody streams the real (short) file, then hits EOF while
// it still owes body bytes -- the file-truncated/removed-mid-serve case -- so it can
// no longer honour the content-length it already sent.
ruvia::task<ruvia::http_response> truncated_file_body_handler(ruvia::context& context_value) {
    (void)context_value;
    ruvia::http_response response({.resource_ = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::ok);
    constexpr std::uint64_t declared_length = 40000;
    response.file_body(std::filesystem::path(truncated_file_body_path()), declared_length, 0,
        declared_length, ruvia::http_response_file_identity::unchecked());
    co_return response;
}

// The on-disk path (guaranteed absent) the missing-file-body handler points at.
std::string& missing_file_body_path() {
    static std::string path;
    return path;
}

// A handler whose file-body response references a file that does not exist on disk,
// so open_response_file_input fails after the response headers (with content-length) have
// already been sent -- the file-removed-before-serve case.
ruvia::task<ruvia::http_response> missing_file_body_handler(ruvia::context& context_value) {
    (void)context_value;
    ruvia::http_response response({.resource_ = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::ok);
    constexpr std::uint64_t declared_length = 40000;
    response.file_body(std::filesystem::path(missing_file_body_path()), declared_length, 0,
        declared_length, ruvia::http_response_file_identity::unchecked());
    co_return response;
}

// Drives the real sans-I/O h2 session over loopback whose not-found handler returns a
// file-body response that cannot be delivered (truncated on disk, or the file is
// missing). Returns the RST_STREAM error code the server sends for stream 1, or
// std::nullopt if it emitted no RST. The connection is deliberately left open (no
// shutdown_send) so the server must abort the stream while the connection is still
// live; a watchdog closes the socket if the RST never arrives, so the neutered-fix
// (mutation) case fails fast instead of blocking on the read forever.
std::optional<std::uint32_t> rst_error_for_file_body_handler(
    ruvia::task<ruvia::http_response> (*handler)(ruvia::context&)) {
    asio::io_context& io = ruvia::test::new_test_io_context();
    std::optional<std::uint32_t> rst_error;

    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            ruvia::worker_memory worker;
            ruvia::detail::route_table routes_value(worker.resource());
            routes_value.set_not_found_handler(
                ruvia::detail::callback_access::bind<ruvia::task<ruvia::http_response>(
                    ruvia::context&)>(handler));
            co_await ruvia::as_awaitable(
                ruvia::test::run_bare_plain_http2_sans_io_session(sock, routes_value, worker, "127.0.0.1"));
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
            ruvia::hpack_encoder::encode_header(header_block, ":method", "GET");
            ruvia::hpack_encoder::encode_header(header_block, ":path", "/");
            ruvia::hpack_encoder::encode_header(header_block, ":scheme", "http");
            ruvia::hpack_encoder::encode_header(header_block, ":authority", "localhost");
            if (!co_await write_all(
                    frame(0x1 /*HEADERS*/, flag_end_stream | flag_end_headers, 1,
                        std::string_view(header_block.data(), header_block.size())))) {
                co_return;
            }

            asio::steady_timer watchdog(io);
            watchdog.expires_after(std::chrono::seconds(5));
            watchdog.async_wait([&sock](const asio::error_code& ec) {
                if (!ec) {
                    asio::error_code ignore;
                    sock.close(ignore);
                }
            });

            for (;;) {
                char header_bytes[ruvia::http2_frame_header_bytes];
                if (!co_await read_exact(header_bytes, sizeof(header_bytes))) {
                    break;
                }
                const auto parsed_header =
                    ruvia::parse_http2_frame_header(std::string_view(header_bytes, sizeof(header_bytes)));
                if (!parsed_header) {
                    throw std::runtime_error("invalid HTTP/2 frame header from server");
                }
                const auto& header_value = *parsed_header;
                std::string payload_value(header_value.length_, '\0');
                if (header_value.length_ != 0 && !co_await read_exact(payload_value.data(), payload_value.size())) {
                    break;
                }
                if (header_value.type_ == 0x3 /*RST_STREAM*/ && header_value.stream_id_ == 1 &&
                    payload_value.size() == 4) {
                    rst_error = read_big_endian32(payload_value.data());
                    break;
                }
            }
            watchdog.cancel();
            asio::error_code ignore;
            sock.close(ignore);
        },
        asio::detached);

    io.run();
    return rst_error;
}

// Mid-body truncation: the served file is much shorter than the advertised length.
std::optional<std::uint32_t> rst_error_for_truncated_file_body() {
    const auto file_path =
        std::filesystem::temp_directory_path() / "ruvia_h2_truncated_body_test.bin";
    {
        std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
        out << "short";  // 5 bytes on disk vs 40000 advertised
    }
    truncated_file_body_path() = file_path.string();
    const auto rst_error = rst_error_for_file_body_handler(&truncated_file_body_handler);
    std::error_code remove_error;
    std::filesystem::remove(file_path, remove_error);
    return rst_error;
}

// File-open failure: the referenced file does not exist when the body is served.
std::optional<std::uint32_t> rst_error_for_missing_file_body() {
    const auto file_path = std::filesystem::temp_directory_path() / "ruvia_h2_missing_body_test.bin";
    std::error_code remove_error;
    std::filesystem::remove(file_path, remove_error);  // ensure absent
    missing_file_body_path() = file_path.string();
    return rst_error_for_file_body_handler(&missing_file_body_handler);
}
}  // namespace

RUVIA_TEST(http2_bodyless_headers_with_content_length_is_rejected) {
    // RFC 9113 §8.1.1: content-length must equal the sum of DATA payload lengths.
    // END_STREAM on HEADERS means zero DATA, so a nonzero content-length is
    // unsatisfiable and the request is malformed. The DATA path enforced this at its
    // own END_STREAM, but a body-less HEADERS reached dispatch unchecked until the
    // fix. Expect RST_STREAM(PROTOCOL_ERROR = 0x1).
    const auto rst_error = rst_error_for_bodyless_content_length_request();
    RUVIA_CHECK(rst_error.has_value());
    RUVIA_CHECK_EQ(rst_error.value_or(0), std::uint32_t{0x1});
}

RUVIA_TEST(http2_dropped_data_credits_connection_flow_window) {
    // DATA sent on an ended stream is dropped, but RFC 9113 6.9.1 still requires
    // its bytes to be returned at connection scope. The real session batches that
    // credit to avoid per-frame output amplification, then restores one exact
    // threshold when reached.
    constexpr auto threshold = connection_window_update_threshold;
    const auto increments = collect_connection_window_updates_for_dropped_data(threshold);
    bool credited = false;
    for (const auto increment : increments) {
        if (increment == threshold) {
            credited = true;
        }
    }
    RUVIA_CHECK(credited);
}

RUVIA_TEST(http2_headers_and_continuation_not_interleaved_across_streams) {
    const auto frames = frames_for_concurrent_large_header_responses();

    // The oversized response headers must have exercised the multi-frame path.
    bool saw_continuation = false;
    for (const auto& f : frames) {
        if (f.type_ == 0x9 /*CONTINUATION*/) {
            saw_continuation = true;
        }
    }
    RUVIA_CHECK(saw_continuation);
    bool first_completed = false;
    bool second_completed = false;
    for (const auto& emitted : frames) {
        if ((emitted.type_ == 0x1 || emitted.type_ == 0x9) &&
            (emitted.flags_ & flag_end_headers) != 0) {
            first_completed |= emitted.stream_id_ == 1;
            second_completed |= emitted.stream_id_ == 3;
        }
    }
    RUVIA_CHECK(first_completed && second_completed);

    // RFC 9113 6.10: once a HEADERS frame without END_HEADERS opens a header block,
    // every frame until END_HEADERS must be a CONTINUATION on the SAME stream -- no
    // other stream's frame may interleave.
    std::uint32_t open_stream = 0;
    bool interleaved = false;
    for (const auto& f : frames) {
        if (open_stream != 0) {
            if (f.type_ != 0x9 /*CONTINUATION*/ || f.stream_id_ != open_stream) {
                interleaved = true;
                break;
            }
            if ((f.flags_ & flag_end_headers) != 0) {
                open_stream = 0;
            }
        } else if (f.type_ == 0x1 /*HEADERS*/ &&
                   (f.flags_ & flag_end_headers) == 0) {
            open_stream = f.stream_id_;
        }
    }
    RUVIA_CHECK(!interleaved);
    RUVIA_CHECK_EQ(open_stream, std::uint32_t{0});
}

RUVIA_TEST(http2_truncated_file_body_aborts_stream_with_rst) {
    // The response advertises content-length 40000 but the file on disk is 5 bytes, so
    // writeFileBody hits a short read (read<=0) with body bytes still outstanding and
    // can no longer honour the advertised length. RFC 9113 §8.1: a committed response
    // that cannot finish must terminate the stream. Before the fix writeFileBody just
    // returned, leaving the stream open with no END_STREAM and no RST_STREAM, so the
    // peer hung until its own timeout. Expect RST_STREAM(INTERNAL_ERROR = 0x2).
    const auto rst_error = rst_error_for_truncated_file_body();
    RUVIA_CHECK(rst_error.has_value());
    RUVIA_CHECK_EQ(rst_error.value_or(0), std::uint32_t{0x2});
}

RUVIA_TEST(http2_missing_file_body_aborts_stream_with_rst) {
    // The response advertises content-length 40000 but the file is gone by the time the
    // body is served, so open_response_file_input fails after the headers are already on
    // the wire. Sending DATA(0, END_STREAM) would be a content-length mismatch (RFC 9113
    // §8.1.1) that a lenient peer accepts as a valid empty body -- silently turning a
    // failed serve into a 200 with no content. Before the fix writeFileBody did exactly
    // that; now it aborts the stream so the peer learns the response failed. Expect
    // RST_STREAM(INTERNAL_ERROR = 0x2), matching the mid-body truncation path.
    const auto rst_error = rst_error_for_missing_file_body();
    RUVIA_CHECK(rst_error.has_value());
    RUVIA_CHECK_EQ(rst_error.value_or(0), std::uint32_t{0x2});
}
