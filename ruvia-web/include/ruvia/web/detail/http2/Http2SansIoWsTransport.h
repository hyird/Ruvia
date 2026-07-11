#pragma once

// Per-stream async plumbing for the sans-I/O HTTP/2 session.
//
// Inbound bytes for a stream consumed asynchronously (a WebSocket tunnel or a
// streaming request body) live in the stream's own body-chunk queue -- the same
// Http2BodyQueue the coroutine session's readBodyChunk popped, so the core's
// backlog accounting (http2AccountDataBody's queued-bytes cap) keeps bounding a
// consumer that falls behind. The session's reader enqueues kTunnelData for an
// accepted CONNECT tunnel (or kMessageBodyChunk for a streaming HTTP request) and
// wakes the stream's Http2SansIoStreamSignal; the consumers here pop chunks,
// mirroring readBodyChunk's semantics exactly (reset/peer half-close -> EOF).
//
// The same-executor discipline of the session (reader, writer and handlers all run
// on the connection's executor) means these never race: a wake via signal.cancel()
// while nothing is waiting is a no-op, and every consumer re-checks its condition
// before suspending, so no wakeup is lost.

#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

#include <asio/steady_timer.hpp>

#include "ruvia/http/detail/http2/Http2BodyQueue.h"
#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/http/detail/websocket/WsConnection.h"
#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/Task.h"
#include "ruvia/http/detail/PmrString.h"

namespace ruvia::detail {

// Wake signal for one dispatched stream: readers of the stream's body queue (and
// window-blocked response-body writers) suspend on it; the session's reader wakes it
// on new chunks / body end / RST / send-window reopen, and ends it at teardown.
struct Http2SansIoStreamSignal final {
    template <typename Executor>
    explicit Http2SansIoStreamSignal(Executor executor) : signal(executor) {}

    void wake() {
        signal.cancel();
    }

    void end() noexcept {
        ended = true;
        signal.cancel();
    }

    [[nodiscard]] Task<void> wait() {
        signal.expires_at((asio::steady_timer::time_point::max)());
        (void)co_await asyncError([this](auto handler) mutable {
            signal.async_wait(std::move(handler));
        });
    }

    asio::steady_timer signal;
    bool ended{false};  // session teardown: no more events will arrive
};

// WebSocket transport (RFC 8441 Extended CONNECT) over the sans-I/O core. Mirrors the
// coroutine Http2WebSocketTransport: readMore pops tunnel DATA from the stream's body
// queue (the coroutine's readBodyChunk), writeBytes submits DATA through the core (a
// window-blocked remainder is queued in-order inside the core) and wakes the writer.
template <typename Executor>
class Http2SansIoWsTransport final {
public:
    Http2SansIoWsTransport(
        Http2Connection& connection,
        std::uint32_t streamId,
        Http2SansIoStreamSignal& signal,
        asio::steady_timer& writeSignal,
        Executor executor) noexcept
        : connection_(&connection),
          streamId_(streamId),
          signal_(&signal),
          writeSignal_(&writeSignal),
          executor_(executor) {}

    [[nodiscard]] Executor executor() const noexcept {
        return executor_;
    }

    [[nodiscard]] Task<bool> readMore(std::pmr::string& buffer) {
        for (;;) {
            auto* stream = connection_->stream(streamId_);
            if (stream == nullptr || stream->isAborted()) {
                co_return false;
            }
            if (const auto chunk = http2PopStreamBodyChunk(*stream); !chunk.empty()) {
                buffer.append(chunk.data(), chunk.size());
                co_return true;
            }
            if (http2HasQueuedStreamBodyChunk(*stream)) {
                continue;
            }
            if (stream->peerEndStream() || signal_->ended) {
                co_return false;
            }
            co_await signal_->wait();
        }
    }

    [[nodiscard]] Task<std::error_code> writeBytes(
        std::string_view bytes,
        WsTransportDisposition disposition) {
        const auto terminal = disposition == WsTransportDisposition::kEndTransport
            ? Http2EndStream::kEndStream
            : Http2EndStream::kKeepOpen;
        for (;;) {
            const auto result = connection_->submitData(streamId_, bytes, terminal);
            writeSignal_->cancel();
            if (result == Http2DataSubmitStatus::kAccepted) {
                co_return std::error_code{};
            }
            if (result == Http2DataSubmitStatus::kClosed) {
                co_return std::make_error_code(std::errc::connection_reset);
            }
            if (result == Http2DataSubmitStatus::kInvalidState) {
                co_return std::make_error_code(std::errc::protocol_error);
            }
            if (result == Http2DataSubmitStatus::kContentLengthExceeded ||
                result == Http2DataSubmitStatus::kContentLengthIncomplete) {
                // Tunnel DATA is unbounded; observing a response-length verdict here
                // means the stream was configured with the wrong local message mode.
                co_return std::make_error_code(std::errc::protocol_error);
            }

            // kQueued means this input is already core-owned; wait for it to drain,
            // then return without resubmitting. kBackpressured accepted no bytes, so
            // wait for the older queued input and retry this exact view.
            while (connection_->hasQueuedData(streamId_)) {
                auto* stream = connection_->stream(streamId_);
                if (stream == nullptr || stream->isAborted() || signal_->ended) {
                    co_return std::make_error_code(std::errc::connection_reset);
                }
                co_await signal_->wait();
            }
            if (result == Http2DataSubmitStatus::kQueued) {
                co_return std::error_code{};
            }
            auto* stream = connection_->stream(streamId_);
            if (stream == nullptr || stream->isAborted() || signal_->ended) {
                co_return std::make_error_code(std::errc::connection_reset);
            }
        }
    }

    void abort() noexcept {
        (void)connection_->submitReset(streamId_, Http2ErrorCode::kCancel);
        signal_->end();
        writeSignal_->cancel();
    }

private:
    Http2Connection* connection_;
    std::uint32_t streamId_;
    Http2SansIoStreamSignal* signal_;
    asio::steady_timer* writeSignal_;
    Executor executor_;
};

// Streaming request-body reader for the sans-I/O session; the BodyReader facade wraps
// it for handler consumption. Chunk-for-chunk port of the coroutine readBodyChunk. A
// null signal means the body already ended when dispatch started (END_STREAM on the
// request HEADERS), so read never needs to wait.
class Http2SansIoRequestBodyReader final {
public:
    Http2SansIoRequestBodyReader(
        Http2Connection& connection,
        std::uint32_t streamId,
        Http2SansIoStreamSignal* signal) noexcept
        : connection_(&connection), streamId_(streamId), signal_(signal) {}

    [[nodiscard]] Task<std::optional<std::string_view>> read() {
        for (;;) {
            auto* stream = connection_->stream(streamId_);
            if (stream == nullptr || stream->isAborted()) {
                co_return std::nullopt;
            }
            if (const auto chunk = http2PopStreamBodyChunk(*stream); !chunk.empty()) {
                co_return chunk;
            }
            if (http2HasQueuedStreamBodyChunk(*stream)) {
                continue;
            }
            if (stream->bodyEnded() || signal_ == nullptr || signal_->ended) {
                co_return std::nullopt;
            }
            co_await signal_->wait();
        }
    }

private:
    Http2Connection* connection_;
    std::uint32_t streamId_;
    Http2SansIoStreamSignal* signal_;
};

}  // namespace ruvia::detail
