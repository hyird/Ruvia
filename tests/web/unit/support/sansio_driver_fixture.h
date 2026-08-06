#pragma once

#include "test_io_context.h"
#include "test_harness.h"
#include "http2_sansio_session_fixture.h"

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

#include <array>
#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <filesystem>
#include <fstream>

#include "ruvia/http/detail/response/HttpResponseFileAccess.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/http/detail/http2/frame/Http2FrameCodec.h"
#include "ruvia/http/detail/http2/hpack/Http2Hpack.h"
#include "ruvia/http/detail/http2/message/Http2RequestBuilder.h"
#include "ruvia/http/detail/http2/flow/Http2WindowUpdate.h"
#include "ruvia/http/detail/websocket/message/HttpWebSocketPermessageDeflate.h"
#include "ruvia/web/detail/http2/Http2SansIoSession.h"
#include "ruvia/web/detail/router/RouteResolution.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/io/SansIoDriver.h"
#include "ruvia/web/Context.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/detail/router/Router.h"

namespace sansio_driver_test {

inline ruvia::WorkerHandle testWorker(asio::io_context& io) {
    return ruvia::detail::WorkerHandleAccess::make(std::make_shared<ruvia::detail::WorkerDispatcher>(io, 64));
}

using asio::ip::tcp;
using ruvia::detail::HpackEncoder;
using ruvia::detail::Http2Connection;
using ruvia::detail::Http2DataSubmitStatus;
using ruvia::detail::Http2EndStream;
using ruvia::detail::Http2FrameType;

constexpr std::string_view kClientPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

// Tear down the synthetic client's transport once it has read the complete
// response. Uses a graceful shutdown (FIN) instead of a linger-0 abortive close
// (RST): on Windows IOCP an abortive RST teardown does not reliably complete the
// server session's pending overlapped async_read, so runHttp2SansIoSession's reader
// stays blocked and io.run() hangs forever (the whole ruvia_web_unit_tests
// binary then times out on the first such test). A FIN delivers a clean EOF that
// terminates the session on every platform -- the same teardown the HTTP/2
// server socket tests use.
inline void closeClientSocket(tcp::socket& socket) noexcept {
    asio::error_code ignored;
    socket.shutdown(asio::socket_base::shutdown_both, ignored);
    socket.close(ignored);
}

// A real route handler: returns a distinctive body so the client can confirm the
// registered handler actually ran through the sans-I/O dispatch pipeline.
inline ruvia::Task<ruvia::HttpResponse> echoHandler(void*, ruvia::Context& ctx) {
    co_return ctx.text("handler-ran");
}

// A slow handler: suspends on a timer (executor passed via the handler context) before
// responding, so a concurrently-dispatched fast handler can finish first.
inline ruvia::Task<ruvia::HttpResponse> slowHandler(void* context, ruvia::Context& ctx) {
    auto* io = static_cast<asio::io_context*>(context);
    asio::steady_timer timer(*io);
    timer.expires_after(std::chrono::milliseconds(30));
    const auto waitCompletion = co_await ruvia::detail::asyncAsio([&timer](auto handler) mutable { timer.async_wait(std::move(handler)); });
    (void)waitCompletion.errorCode();
    co_return ctx.text("slow");
}

inline ruvia::Task<ruvia::HttpResponse> fastHandler(void*, ruvia::Context& ctx) {
    co_return ctx.text("fast");
}

inline ruvia::Task<ruvia::HttpResponse> bufferedStatusHandler(void*, ruvia::Context& ctx) {
    ctx.status(ruvia::http_status::kMultiStatus);
    co_return ctx.text("buffered-status");
}

inline ruvia::Task<ruvia::HttpResponse> invalidHttp2ResponseHandler(void*, ruvia::Context& ctx) {
    ctx.header("Connection", "close");
    co_return ctx.text("must-not-commit");
}

// Streaming request-body handler: drains the body reader, records the total bytes seen
// via the void* handler context, and replies with a fixed marker.
// Returns a large BUFFERED body (100 KiB) to exercise the buffered-response send-window
// pacing path (distinct from the file-body path).
constexpr std::size_t kLargeBufferedBytes = 100000;
inline ruvia::Task<ruvia::HttpResponse> largeBufferedHandler(void*, ruvia::Context&) {
    ruvia::HttpResponse response(std::pmr::get_default_resource());
    response.status(ruvia::http_status::kOk);
    std::string body(kLargeBufferedBytes, 'Q');
    response.body(body);
    co_return response;
}

inline ruvia::Task<ruvia::HttpResponse> streamBodyCountHandler(void* ctx, ruvia::Context& c) {
    auto* out = static_cast<std::size_t*>(ctx);
    std::size_t bytes = 0;
    auto& reader = c.req().bodyReader();
    while (auto chunk = co_await reader.read()) {
        bytes += chunk->size();
    }
    *out = bytes;
    co_return c.text("upload-done");
}

struct TerminatedBodyObservation final {
    asio::io_context* io;
    bool started{false};
    bool sawTransportError{false};
    bool handlerFinished{false};
    bool sessionReturnedAfterHandler{false};
};

inline ruvia::Task<ruvia::HttpResponse> terminatedBodyHandler(void* raw, ruvia::Context& context) {
    auto& observation = *static_cast<TerminatedBodyObservation*>(raw);
    observation.started = true;
    try {
        (void)co_await context.req().bodyReader().read();
    } catch (const std::system_error& error) {
        observation.sawTransportError = static_cast<bool>(error.code());
    }
    asio::steady_timer completionDelay(*observation.io);
    completionDelay.expires_after(std::chrono::milliseconds(5));
    (void)co_await ruvia::detail::asyncAsio([&completionDelay](auto handler) mutable { completionDelay.async_wait(std::move(handler)); });
    observation.handlerFinished = true;
    co_return context.text("transport-ended");
}

// Path + size of the large temp file the pacing test serves (set in the test body).
inline std::string& largeFilePath() {
    static std::string path;
    return path;
}
constexpr std::uint64_t kLargeFileBytes = 200000;  // > default send window (65535)

// A plain (buffered) route returning a FILE body larger than the send window: this is
// the path that had NO stream signal, so a window block could never be woken.
inline ruvia::Task<ruvia::HttpResponse> largeFileHandler(void*, ruvia::Context&) {
    ruvia::HttpResponse response(std::pmr::get_default_resource());
    response.status(ruvia::http_status::kOk);
    ruvia::detail::setResponseFileBody(response, std::filesystem::path(largeFilePath()), kLargeFileBytes, 0, kLargeFileBytes);
    co_return response;
}

// A WebSocket echo handler: echoes each text message back and finishes when the peer
// closes (read returns nullopt).
inline ruvia::Task<void> wsEchoHandler(void*, ruvia::Context& ctx) {
    auto& ws = ctx.webSocket();
    while (auto message = co_await ws.read()) {
        if (message->text()) {
            co_await ws.text(message->payload());
        }
    }
}

// Returns without waiting for peer input so session finalization initiates the
// server side of the closing handshake.
inline ruvia::Task<void> wsServerCloseHandler(void*, ruvia::Context&) {
    co_return;
}

// Build a masked client->server WebSocket frame (RFC 6455 §5.1, short lengths only).
inline std::string maskedWsFrame(std::uint8_t opcode, std::string_view payload, bool rsv1 = false) {
    std::string f;
    f.push_back(static_cast<char>(0x80U | (rsv1 ? 0x40U : 0U) | opcode));  // FIN | RSV1? | opcode
    f.push_back(static_cast<char>(0x80U | static_cast<std::uint8_t>(payload.size())));
    const unsigned char mask[4] = {0x11, 0x22, 0x33, 0x44};
    f.append(reinterpret_cast<const char*>(mask), 4);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        f.push_back(static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i % 4]));
    }
    return f;
}

inline std::string frame(std::uint8_t type, std::uint8_t flags, std::uint32_t streamId, std::string_view payload) {
    std::string bytes(ruvia::detail::kHttp2FrameHeaderBytes, '\0');
    ruvia::detail::http2WriteFrameHeader(bytes.data(), static_cast<std::uint32_t>(payload.size()), static_cast<Http2FrameType>(type), flags, streamId);
    bytes.append(payload);
    return bytes;
}

}  // namespace sansio_driver_test

// End-to-end proof that the generic sans-I/O driver (ruvia-core) can back a real
// HTTP/2 server over a real socket using ONLY the Http2Connection core: a synthetic
// client sends a GET; the pump feeds the core, the onReadable callback dispatches a
// 200 "pong" response, and the pump flushes it back. Validates the driver contract and
// the core's external usability with zero coroutine sessions.

// End-to-end proof that REAL framework dispatch runs over the sans-I/O core: onReadable
// builds an HttpRequest from the stream (Http2RequestBuilder), resolves it against a
// RouteTable, and runs the actual dispatchBufferedResponse pipeline (which 404s an empty table),
// then submits the response. The client verifies a response HEADERS frame comes back --
// proving request-build -> resolve -> dispatch -> submit works with no coroutine session.

// End-to-end proof that a REAL registered handler runs over the sans-I/O core with a
// request body: a POST /echo route echoes the body; the buffered helper accumulates the
// DATA into the stream, dispatches to the handler, and submits the echoed response.

// Multiplexing proof: two concurrent requests -- stream 1 to a SLOW handler, stream 3
// to a FAST one -- must both complete, and the fast response must come back first even
// though its request arrived second. That out-of-order completion proves the handlers
// run concurrently rather than blocking the read/dispatch loop.

// End-to-end WebSocket over the sans-I/O session (RFC 8441 Extended CONNECT): the
// client opens a tunnel to a registered WebSocket echo route, sends a masked text
// frame as HTTP/2 DATA, and must get the unmasked echo back; a client Close is then
// answered with the server's Close carrying END_STREAM. Proves the per-stream inbound
// pipe + Http2SansIoWsTransport + the shared session finalization over the core.

// A server-initiated RFC 6455 Close is not itself RFC 8441 transport EOF. The first
// DATA carries only the Close frame and keeps the h2 send half open; after the client
// replies with Close+END_STREAM, the server emits its separate empty DATA+END_STREAM.
// This pins the typed WsOutputPlan -> Http2EndStream mapping and prevents a runtime
// from reconstructing END_STREAM from "we sent a Close" again.

// An Extended CONNECT to a WebSocket route with a bad sec-websocket-version must be
// answered with a buffered error response (HEADERS then DATA+END_STREAM), mirroring
// the coroutine session's invalid-handshake 400 path.

namespace sansio_driver_test {

// Streaming handler that atomically ends with a trailer section: the h2 stream must
// end with trailing HEADERS (END_STREAM) instead of an empty DATA frame.
inline ruvia::Task<void> streamTrailerHandler(void*, ruvia::Context& c) {
    c.status(ruvia::http_status::kMultiStatus);
    auto& stream = c.streamText();
    co_await stream.write("body-part");
    const std::array<ruvia::HttpHeaderView, 1> trailers{ruvia::HttpHeaderView{"x-checksum", "abc123"}};
    co_await stream.end(trailers);
}

struct StreamAccessObservation final {
    std::size_t calls{0};
    std::uint16_t status{0};
    ruvia::HttpProtocolVersion protocolVersion{ruvia::HttpProtocolVersion::kHttp11};

    void operator()(const ruvia::AccessLogRecord& record) noexcept {
        ++calls;
        status = record.status().value();
        protocolVersion = record.protocolVersion();
    }
};

// Streaming handler pushing one large chunk; used to exercise send-window pacing.
inline ruvia::Task<void> streamBigChunkHandler(void*, ruvia::Context& c) {
    auto& stream = c.streamText();
    co_await stream.write(std::string(64, 'z'));
}

struct HpackCollect {
    std::string joined;
    static bool onHeader(void* target, std::string_view name, std::string_view value) {
        auto* self = static_cast<HpackCollect*>(target);
        self->joined.append(name).append("=").append(value).append(";");
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
// streams got no Http2SansIoStreamSignal, so the first window-blocked file chunk could
// never be woken -- the response was silently truncated and the stream hung.

// P2 coverage: a streaming request body (RequestBodyMode::kStream) flows through the
// live session to the handler's body reader chunk by chunk. Guards the signal-wake /
// body-queue handoff -- a regression there would hang a streaming upload forever.

// P2 coverage: a server-role request framed with trailers (DATA then a trailing
// HEADERS with END_STREAM, gRPC-style) must dispatch normally. Guards the
// processTrailerHeaders server path (client-role trailers were the only coverage).

// #14 regression: a large BUFFERED response paced over a small send window must
// deliver every byte + END_STREAM (and the core never buffers more than one slice).

// #1 regression: TWO concurrent WebSocket tunnels (two Extended-CONNECT streams) on
// ONE h2 connection must both work -- each registers its own heartbeat slot on the
// shared scanner entry, and both echo. Before the per-tunnel heartbeat-slot fix they
// clobbered each other's registration; this proves multiplexed tunnels coexist.

// nginx keepalive_requests parity on h2 (the h1 side runs Http1RequestSequence):
// after the configured number of request heads the session drains -- GOAWAY
// (NO_ERROR) advertising the last accepted stream, the in-flight request still
// completes, and a stream opened above the advertised id is refused.

using namespace sansio_driver_test;  // NOLINT(google-build-using-namespace)
