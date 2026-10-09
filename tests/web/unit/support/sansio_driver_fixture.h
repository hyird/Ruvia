#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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
#include "ruvia/core/async.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/websocket_connection.h"
#include "ruvia/web/context.h"

#include "context/context_services.h"
#include "http2/http2_sans_io_session.h"
#include "http2_sansio_session_fixture.h"
#include "router/route_resolution.h"
#include "router/route_table.h"
#include "router/router.h"
#include "router/router_impl.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace sansio_driver_test {

using asio::ip::tcp;
using ruvia::hpack_encoder;
using ruvia::http2_frame_type;

inline constexpr std::uint8_t flag_end_stream = 0x1;
inline constexpr std::uint8_t flag_end_headers = 0x4;
inline ruvia::http2_frame_header parse_frame_header(std::string_view bytes_value) {
    const auto parsed_value = ruvia::parse_http2_frame_header(
        std::span<const char>(bytes_value.data(), bytes_value.size()));
    if (!parsed_value) {
        throw std::runtime_error("invalid/truncated HTTP/2 frame header in test client");
    }
    return *parsed_value;
}

inline void write_big_endian32(char* output, std::uint32_t value) noexcept {
    output[0] = static_cast<char>((value >> 24) & 0xffU);
    output[1] = static_cast<char>((value >> 16) & 0xffU);
    output[2] = static_cast<char>((value >> 8) & 0xffU);
    output[3] = static_cast<char>(value & 0xffU);
}

inline std::uint32_t read_big_endian32(const char* input) noexcept {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(input[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(input[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(input[2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(input[3]));
}

inline std::uint32_t read_big_endian32(const unsigned char* input) noexcept {
    return read_big_endian32(reinterpret_cast<const char*>(input));
}

inline std::uint32_t read_big_endian31(const char* input) noexcept {
    return read_big_endian32(input) & 0x7fffffffU;
}

inline std::uint32_t read_big_endian31(const unsigned char* input) noexcept {
    return read_big_endian32(input) & 0x7fffffffU;
}

constexpr std::string_view client_preface = ruvia::http2_client_preface;

// Tear down the synthetic client's transport once it has read the complete
// response. Uses a graceful shutdown (FIN) instead of a linger-0 abortive close
// (RST): on Windows IOCP an abortive RST teardown does not reliably complete the
// server session's pending overlapped async_read, so run_http2_sans_io_session's reader
// stays blocked and io.run() hangs forever (the whole ruvia_web_unit_tests
// binary then times out on the first such test). A FIN delivers a clean EOF that
// terminates the session on every platform -- the same teardown the HTTP/2
// server socket tests use.
inline void close_client_socket(tcp::socket& socket) noexcept {
    asio::error_code ignored;
    socket.shutdown(asio::socket_base::shutdown_both, ignored);
    socket.close(ignored);
}

// A real route handler: returns a distinctive body so the client can confirm the
// registered handler actually ran through the sans-I/O dispatch pipeline.
inline ruvia::task<ruvia::http_response> echo_handler(void*, ruvia::context& ctx) {
    co_return ctx.text("handler-ran");
}

// A slow handler: suspends on a timer (executor passed via the handler context) before
// responding, so a concurrently-dispatched fast handler can finish first.
inline ruvia::task<ruvia::http_response> slow_handler(void* context_value, ruvia::context& ctx) {
    auto* io = static_cast<asio::io_context*>(context_value);
    asio::steady_timer timer(*io);
    timer.expires_after(std::chrono::milliseconds(30));
    const auto wait_completion = co_await ruvia::async_asio(
        [&timer](auto handler) mutable { timer.async_wait(std::move(handler)); });
    (void)wait_completion.error_code();
    co_return ctx.text("slow");
}

inline ruvia::task<ruvia::http_response> fast_handler(void*, ruvia::context& ctx) {
    co_return ctx.text("fast");
}

inline ruvia::task<ruvia::http_response> no_content_handler(void*, ruvia::context& ctx) {
    ctx.status(ruvia::http_status::no_content);
    co_return ctx.body(nullptr);
}

inline ruvia::task<ruvia::http_response> buffered_status_handler(void*, ruvia::context& ctx) {
    ctx.status(ruvia::http_status::multi_status);
    co_return ctx.text("buffered-status");
}

inline ruvia::task<ruvia::http_response> invalid_http2_response_handler(void*, ruvia::context& ctx) {
    ctx.header("Connection", "close");
    co_return ctx.text("must-not-commit");
}

// Streaming request-body handler: drains the body reader, records the total bytes seen
// via the void* handler context, and replies with a fixed marker.
// Returns a large BUFFERED body (100 KiB) to exercise the buffered-response send-window
// pacing path (distinct from the file-body path).
constexpr std::size_t large_buffered_bytes = 100000;
inline ruvia::task<ruvia::http_response> large_buffered_handler(void*, ruvia::context&) {
    ruvia::http_response response({.resource_ = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::ok);
    std::string body(large_buffered_bytes, 'Q');
    response.body(body);
    co_return response;
}

inline ruvia::task<ruvia::http_response> stream_body_count_handler(void* ctx, ruvia::context& c) {
    auto* out = static_cast<std::size_t*>(ctx);
    std::size_t bytes_value = 0;
    auto& reader_value = c.req().get_body_reader();
    while (auto chunk = co_await reader_value.read()) {
        bytes_value += chunk->size();
    }
    *out = bytes_value;
    co_return c.text("upload-done");
}

struct terminated_body_observation final {
    asio::io_context* io_;
    bool started_{false};
    bool saw_transport_error_{false};
    bool handler_finished_{false};
    bool session_returned_after_handler_{false};
};

inline ruvia::task<ruvia::http_response> terminated_body_handler(void* raw, ruvia::context& context_value) {
    auto& observation_value = *static_cast<terminated_body_observation*>(raw);
    observation_value.started_ = true;
    try {
        (void)co_await context_value.req().get_body_reader().read();
    } catch (const std::system_error& error) {
        observation_value.saw_transport_error_ = static_cast<bool>(error.code());
    }
    asio::steady_timer completion_delay(*observation_value.io_);
    completion_delay.expires_after(std::chrono::milliseconds(5));
    (void)co_await ruvia::async_asio([&completion_delay](auto handler) mutable {
        completion_delay.async_wait(std::move(handler));
    });
    observation_value.handler_finished_ = true;
    co_return context_value.text("transport-ended");
}

// Path + size of the large temp file the pacing test serves (set in the test body).
inline std::string& large_file_path() {
    static std::string path;
    return path;
}
constexpr std::uint64_t large_file_bytes = 200000;  // > default send window (65535)

// A plain (buffered) route returning a FILE body larger than the send window: this is
// the path that had NO stream signal, so a window block could never be woken.
inline ruvia::task<ruvia::http_response> large_file_handler(void*, ruvia::context&) {
    ruvia::http_response response({.resource_ = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::ok);
    response.file_body(std::filesystem::path(large_file_path()), large_file_bytes, 0,
        large_file_bytes, ruvia::http_response_file_identity::unchecked());
    co_return response;
}

// A websocket echo handler: echoes each text message back and finishes when the peer
// closes (read returns nullopt).
inline ruvia::task<void> ws_echo_handler(void*, ruvia::context& ctx) {
    auto& ws = ctx.get_websocket();
    while (auto message = co_await ws.read()) {
        if (message->text()) {
            co_await ws.text(message->payload());
        }
    }
}

// Returns without waiting for peer input so session finalization initiates the
// server side of the closing handshake.
inline ruvia::task<void> ws_server_close_handler(void*, ruvia::context&) {
    co_return;
}

// Build a masked client->server websocket frame (RFC 6455 §5.1, short lengths only).
inline std::string masked_ws_frame(std::uint8_t opcode, std::string_view payload_value, bool rsv1 = false) {
    std::string f;
    f.push_back(static_cast<char>(0x80U | (rsv1 ? 0x40U : 0U) | opcode));  // FIN | RSV1? | opcode
    f.push_back(static_cast<char>(0x80U | static_cast<std::uint8_t>(payload_value.size())));
    const unsigned char mask[4] = {0x11, 0x22, 0x33, 0x44};
    f.append(reinterpret_cast<const char*>(mask), 4);
    for (std::size_t i = 0; i < payload_value.size(); ++i) {
        f.push_back(static_cast<char>(static_cast<unsigned char>(payload_value[i]) ^ mask[i % 4]));
    }
    return f;
}

inline std::string frame(
    std::uint8_t type, std::uint8_t flags, std::uint32_t stream_id, std::string_view payload_value) {
    std::string bytes_value(ruvia::http2_frame_header_bytes, '\0');
    if (!ruvia::encode_http2_frame_header(std::span<char>(bytes_value.data(), bytes_value.size()),
            static_cast<std::uint32_t>(payload_value.size()), static_cast<http2_frame_type>(type), flags,
            stream_id)) {
        throw std::runtime_error("invalid HTTP/2 frame values in test client");
    }
    bytes_value.append(payload_value);
    return bytes_value;
}

inline std::string window_update(std::uint32_t stream_id, std::uint32_t increment) {
    std::string payload_value(4, '\0');
    write_big_endian32(payload_value.data(), increment & 0x7fffffffU);
    return frame(static_cast<std::uint8_t>(ruvia::http2_frame_type::window_update), 0,
        stream_id, payload_value);
}

}  // namespace sansio_driver_test

// End-to-end proof that REAL framework dispatch runs over the sans-I/O core: onReadable
// builds an http_request from the stream (http2_request_builder), resolves it against a
// route_table, and runs the actual dispatch_buffered_response pipeline (which 404s an empty table),
// then submits the response. The client verifies a response HEADERS frame comes back --
// proving request-build -> resolve -> dispatch -> submit works with no coroutine session.

// End-to-end proof that a REAL registered handler runs over the sans-I/O core with a
// request body: a POST /echo route echoes the body; the buffered helper accumulates the
// DATA into the stream, dispatches to the handler, and submits the echoed response.

// Multiplexing proof: two concurrent requests -- stream 1 to a SLOW handler, stream 3
// to a FAST one -- must both complete, and the fast response must come back first even
// though its request arrived second. That out-of-order completion proves the handlers
// run concurrently rather than blocking the read/dispatch loop.

// End-to-end websocket over the sans-I/O session (RFC 8441 Extended CONNECT): the
// client opens a tunnel to a registered websocket echo route, sends a masked text
// frame as HTTP/2 DATA, and must get the unmasked echo back; a client Close is then
// answered with the server's Close carrying END_STREAM. Proves the per-stream inbound
// pipe + http2_sans_io_ws_transport + the shared session finalization over the core.

// A server-initiated RFC 6455 Close is not itself RFC 8441 transport EOF. The first
// DATA carries only the Close frame and keeps the h2 send half open; after the client
// replies with Close+END_STREAM, the server emits its separate empty DATA+END_STREAM.
// This pins the typed websocket_output_plan -> http2_end_stream mapping and prevents a runtime
// from reconstructing END_STREAM from "we sent a Close" again.

// An Extended CONNECT to a websocket route with a bad sec-websocket-version must be
// answered with a buffered error response (HEADERS then DATA+END_STREAM), mirroring
// the coroutine session's invalid-handshake 400 path.

namespace sansio_driver_test {

// Streaming handler that atomically ends with a trailer section: the h2 stream must
// end with trailing HEADERS (END_STREAM) instead of an empty DATA frame.
inline ruvia::task<void> stream_trailer_handler(void*, ruvia::context& c) {
    c.status(ruvia::http_status::multi_status);
    auto& stream = c.stream_text();
    co_await stream.write("body-part");
    const std::array<ruvia::http_header_view, 1> trailers{
        ruvia::http_header_view{"x-checksum", "abc123"}};
    co_await stream.end(trailers);
}

struct stream_access_observation final {
    std::size_t calls_{0};
    std::uint16_t status_{0};
    ruvia::http_protocol_version protocol_version_{ruvia::http_protocol_version::http11};

    void operator()(const ruvia::access_log_record& record) noexcept {
        ++calls_;
        status_ = record.status().value();
        protocol_version_ = record.protocol_version();
    }
};

// Streaming handler pushing one large chunk; used to exercise send-window pacing.
inline ruvia::task<void> stream_big_chunk_handler(void*, ruvia::context& c) {
    auto& stream = c.stream_text();
    co_await stream.write(std::string(64, 'z'));
}

struct hpack_collect {
    std::string joined_;
    static bool on_header(void* target, std::string_view name, std::string_view value) {
        auto* self = static_cast<hpack_collect*>(target);
        self->joined_.append(name).append("=").append(value).append(";");
        return true;
    }
};

}  // namespace sansio_driver_test

// Expect is one cross-version semantic contract. Stream 1 sends a legal repeated/
// empty-member 100-continue list and withholds DATA until the server's exact interim
// head arrives. Stream 3 sends an unknown extension and withholds DATA permanently;
// the Web product must answer 417 immediately instead of the HTTP core rejecting the
// field block or the buffered dispatcher deadlocking while it waits for content.

// Trailers over the sans-I/O h2 streaming path: HEAD(no END_STREAM), DATA body, then a
// trailing HEADERS frame carrying END_STREAM whose block decodes to the terminal section.

// permessage-deflate over h2 Extended CONNECT: the handshake echoes the negotiated
// extension, and a compressed (RSV1) client frame is inflated before reaching the
// handler, whose echo round-trips intact.

// Send-window pacing: with a tiny stream window the streaming sink must park until the
// client grants WINDOW_UPDATEs, and every byte must still arrive, ending the stream.

// P0 regression: a plain route returning a FILE body larger than the send window must
// pace on WINDOW_UPDATEs and deliver EVERY byte + END_STREAM. Before the fix, such
// streams got no http2_sans_io_stream_signal, so the first window-blocked file chunk could
// never be woken -- the response was silently truncated and the stream hung.

// P2 coverage: a streaming request body (request_body_mode::stream) flows through the
// live session to the handler's body reader chunk by chunk. Guards the signal-wake /
// body-queue handoff -- a regression there would hang a streaming upload forever.

// P2 coverage: a server-role request framed with trailers (DATA then a trailing
// HEADERS with END_STREAM, gRPC-style) must dispatch normally. Guards the
// process_trailer_headers server path (client-role trailers were the only coverage).

// #14 regression: a large BUFFERED response paced over a small send window must
// deliver every byte + END_STREAM (and the core never buffers more than one slice).

// #1 regression: TWO concurrent websocket tunnels (two Extended-CONNECT streams) on
// ONE h2 connection must both work -- each registers its own heartbeat slot on the
// shared scanner entry, and both echo. Before the per-tunnel heartbeat-slot fix they
// clobbered each other's registration; this proves multiplexed tunnels coexist.

// nginx keepalive_requests parity on h2 (the h1 side runs http1_request_sequence):
// after the configured number of request heads the session drains -- GOAWAY
// (NO_ERROR) advertising the last accepted stream, the in-flight request still
// completes, and a stream opened above the advertised id is refused.

using namespace sansio_driver_test;  // NOLINT(google-build-using-namespace)
