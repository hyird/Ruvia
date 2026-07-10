#pragma once

// Per-stream async plumbing for the sans-I/O HTTP/2 session.
//
// Inbound bytes for a stream consumed asynchronously (a WebSocket tunnel or a
// streaming request body) live in the stream's own body-chunk queue -- the same
// Http2BodyQueue the coroutine session's readBodyChunk popped, so the core's
// backlog accounting (http2AccountDataBody's queued-bytes cap) keeps bounding a
// consumer that falls behind. The session's reader enqueues each kMessageBodyChunk
// and wakes the stream's Http2SansIoStreamSignal; the consumers here pop chunks,
// mirroring readBodyChunk's semantics exactly (reset/EOF -> end of stream).
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
#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/app/Task.h"
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
// queue (the coroutine's readBodyChunk), writeFrame submits DATA through the core (a
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
            if (stream == nullptr || stream->isReset()) {
                co_return false;
            }
            if (const auto chunk = http2PopStreamBodyChunk(*stream); !chunk.empty()) {
                buffer.append(chunk.data(), chunk.size());
                co_return true;
            }
            if (http2HasQueuedStreamBodyChunk(*stream)) {
                continue;
            }
            if (stream->bodyEnded() || signal_->ended) {
                co_return false;
            }
            co_await signal_->wait();
        }
    }

    [[nodiscard]] Task<std::error_code> writeFrame(
        std::string_view header,
        std::string_view payload,
        bool endStream) {
        auto result = connection_->submitData(streamId_, header, payload.empty() && endStream);
        if (!payload.empty()) {
            result = connection_->submitData(streamId_, payload, endStream);
        }
        writeSignal_->cancel();
        if (result == Http2SubmitResult::kClosed) {
            co_return std::make_error_code(std::errc::connection_reset);
        }
        // Backpressure: if the send window is closed, park until the core drains this
        // stream's blocked remainder (the reader wakes signal_ via takeUnblockedStreams).
        // Without this, a WS client that stops granting window while the app keeps
        // writing (broadcast/heartbeat) grows the core's pendingSends_ without bound.
        // WebSocketConnection serializes writers, so suspending here is safe.
        while (!signal_->ended && connection_->hasBlockedSend(streamId_)) {
            auto* stream = connection_->stream(streamId_);
            if (stream == nullptr || stream->isReset()) {
                co_return std::make_error_code(std::errc::connection_reset);
            }
            co_await signal_->wait();
        }
        co_return std::error_code{};
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
// request HEADERS, or an h2c-upgrade seeded body), so read never needs to wait.
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
            if (stream == nullptr || stream->isReset()) {
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
