#pragma once

// Per-stream async plumbing for the sans-I/O HTTP/2 session.
//
// Inbound bytes consumed asynchronously (a WebSocket tunnel or streaming request
// body) live in a Web-owned queue. The HTTP core retains receive-window debt for
// each delivered DATA event; these consumers acknowledge it only after the queue
// drains, so a suspended handler naturally backpressures the peer.
//
// The same-executor discipline of the session (reader, writer and handlers all run
// on the connection's executor) means these never race: a signal wake
// while nothing is waiting is a no-op, and every consumer re-checks its condition
// before suspending, so no wakeup is lost.

#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

#include "ruvia/core/detail/worker/WorkerSignal.h"

#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/http/detail/websocket/WsConnection.h"
#include "ruvia/core/Task.h"
#include "ruvia/http/detail/util/PmrString.h"
#include "ruvia/web/detail/http2/Http2SansIoSendWindow.h"
#include "ruvia/web/detail/http2/Http2SansIoStreamRuntime.h"
#include "ruvia/web/detail/websocket/WsTransportReadResult.h"

namespace ruvia::detail {

// WebSocket transport (RFC 8441 Extended CONNECT) over the sans-I/O core. Mirrors the
// coroutine Http2WebSocketTransport: readMore pops tunnel DATA from the Web runtime's
// queue (the coroutine's readBodyChunk), writeBytes submits DATA through the core (a
// window-blocked remainder is queued in-order inside the core) and wakes the writer.
template <typename Executor>
class Http2SansIoWsTransport final {
public:
    Http2SansIoWsTransport(Http2Connection& connection, std::uint32_t streamId,
        Http2SansIoBodyQueue& bodyQueue, Http2SansIoStreamSignal& signal, WorkerSignal& writeSignal,
        Executor executor) noexcept
        : connection_(connection),
          streamId_(streamId),
          bodyQueue_(bodyQueue),
          signal_(signal),
          writeSignal_(writeSignal),
          executor_(executor) {}

    [[nodiscard]] Executor executor() const noexcept {
        return executor_;
    }

    [[nodiscard]] Task<WsTransportReadResult> readMore(std::pmr::string& buffer) {
        for (;;) {
            if (signal_.terminated()) {
                co_return WsTransportReadResult::makeFailure(signal_.terminalError());
            }
            auto* stream = connection_.stream(streamId_);
            if (stream == nullptr || stream->isAborted()) {
                co_return WsTransportReadResult::makeFailure(
                    std::make_error_code(std::errc::connection_reset));
            }
            if (const auto chunk = bodyQueue_.pop(); !chunk.empty()) {
                releaseCreditIfDrained();
                buffer.append(chunk.data(), chunk.size());
                co_return WsTransportReadResult::makeData();
            }
            if (!bodyQueue_.empty()) {
                continue;
            }
            if (stream->remoteReceive().endStream() != nullptr) {
                co_return WsTransportReadResult::makeEnd();
            }
            if (signal_.terminated()) {
                co_return WsTransportReadResult::makeFailure(signal_.terminalError());
            }
            co_await signal_.wait();
        }
    }

    [[nodiscard]] Task<std::error_code> writeBytes(
        std::string_view bytes, WsTransportDisposition disposition) {
        const auto terminal = disposition == WsTransportDisposition::kEndTransport
                                  ? Http2EndStream::kEndStream
                                  : Http2EndStream::kKeepOpen;
        for (;;) {
            if (signal_.terminated()) {
                co_return signal_.terminalError();
            }
            const auto result = connection_.submitData(streamId_, bytes, terminal);
            wakeWriter();
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
            const auto waitResult = co_await awaitHttp2SendWindow(connection_, streamId_, &signal_);
            if (waitResult.aborted() != nullptr) {
                co_return signal_.terminated() ? signal_.terminalError()
                                               : std::make_error_code(std::errc::connection_reset);
            }
            if (result == Http2DataSubmitStatus::kQueued) {
                co_return std::error_code{};
            }
        }
    }

    void abort() noexcept {
        try {
            (void)connection_.submitReset(streamId_, Http2ErrorCode::kCancel);
        } catch (...) {
            // abort() is best-effort teardown; callers cannot observe reset
            // serialization failure on this noexcept cleanup path.
        }
        signal_.wake();
        wakeWriter();
    }

private:
    void wakeWriter() noexcept {
        writeSignal_.notify();
    }

    void releaseCreditIfDrained() {
        if (!bodyQueue_.empty()) {
            return;
        }
        connection_.releaseAllReceivedData(streamId_);
        wakeWriter();
    }

    Http2Connection& connection_;
    std::uint32_t streamId_;
    Http2SansIoBodyQueue& bodyQueue_;
    Http2SansIoStreamSignal& signal_;
    WorkerSignal& writeSignal_;
    Executor executor_;
};

// Streaming request-body reader for the sans-I/O session; the BodyReader facade wraps
// it for handler consumption. Chunk-for-chunk port of the coroutine readBodyChunk.
// Admission always binds the runtime-owned signal before this facade can exist.
class Http2SansIoRequestBodyReader final {
public:
    Http2SansIoRequestBodyReader(Http2Connection& connection, std::uint32_t streamId,
        Http2SansIoBodyQueue& bodyQueue, Http2SansIoStreamSignal& signal,
        WorkerSignal& writeSignal) noexcept
        : connection_(connection),
          streamId_(streamId),
          bodyQueue_(bodyQueue),
          signal_(signal),
          writeSignal_(writeSignal) {}

    [[nodiscard]] Task<std::optional<std::string_view>> read() {
        for (;;) {
            if (signal_.terminated()) {
                throw std::system_error(signal_.terminalError());
            }
            auto* stream = connection_.stream(streamId_);
            if (stream == nullptr || stream->isAborted()) {
                throw std::system_error(std::make_error_code(std::errc::connection_reset));
            }
            if (const auto chunk = bodyQueue_.pop(); !chunk.empty()) {
                if (bodyQueue_.empty()) {
                    connection_.releaseAllReceivedData(streamId_);
                    writeSignal_.notify();
                }
                co_return chunk;
            }
            if (!bodyQueue_.empty()) {
                continue;
            }
            if (stream->remoteReceive().endStream() != nullptr) {
                co_return std::nullopt;
            }
            if (signal_.terminated()) {
                throw std::system_error(signal_.terminalError());
            }
            co_await signal_.wait();
        }
    }

private:
    Http2Connection& connection_;
    std::uint32_t streamId_;
    Http2SansIoBodyQueue& bodyQueue_;
    Http2SansIoStreamSignal& signal_;
    WorkerSignal& writeSignal_;
};

}  // namespace ruvia::detail
