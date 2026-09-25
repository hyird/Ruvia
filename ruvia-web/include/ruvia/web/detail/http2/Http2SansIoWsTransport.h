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

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

#include "ruvia/core/Bytes.h"
#include "ruvia/core/PmrString.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/http/Http2Connection.h"
#include "ruvia/http/WebSocketServerProtocol.h"
#include "ruvia/web/detail/http2/Http2DataOutputBudget.h"
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
    Http2SansIoWsTransport(ruvia::Http2Connection& connection, std::uint32_t streamId,
        Http2SansIoBodyQueue& bodyQueue, Http2SansIoStreamSignal& signal, WorkerSignal& writeSignal,
        Http2DataOutputBudget& outputBudget, Executor executor) noexcept
        : connection_(connection),
          streamId_(streamId),
          bodyQueue_(bodyQueue),
          signal_(signal),
          writeSignal_(writeSignal),
          outputBudget_(&outputBudget),
          executor_(executor) {}

    Http2SansIoWsTransport(ruvia::Http2Connection& connection, std::uint32_t streamId,
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
            if (aborted_ || signal_.terminated()) {
                co_return WsTransportReadResult::makeFailure(aborted_
                                                                 ? std::make_error_code(std::errc::operation_canceled)
                                                                 : signal_.terminalError());
            }
            const auto receiveStatus = connection_.streamReceiveStatus(streamId_);
            if (receiveStatus == Http2StreamReceiveStatus::kClosed) {
                co_return WsTransportReadResult::makeFailure(
                    std::make_error_code(std::errc::connection_reset));
            }
            if (const auto chunk = bodyQueue_.pop(); !chunk.empty()) {
                buffer.append(chunk.data(), chunk.size());
                co_return WsTransportReadResult::makeData();
            }
            if (!bodyQueue_.empty()) {
                continue;
            }
            if (receiveStatus == Http2StreamReceiveStatus::kEnded) {
                co_return WsTransportReadResult::makeEnd();
            }
            if (aborted_ || signal_.terminated()) {
                co_return WsTransportReadResult::makeFailure(aborted_
                                                                 ? std::make_error_code(std::errc::operation_canceled)
                                                                 : signal_.terminalError());
            }
            co_await signal_.wait();
        }
    }

    [[nodiscard]] Task<std::error_code> writeBytes(
        std::string_view bytes, WebSocketServerTransportDisposition disposition) {
        const auto terminal = disposition == WebSocketServerTransportDisposition::kEndTransport
                                  ? Http2EndStream::kEndStream
                                  : Http2EndStream::kKeepOpen;
        constexpr std::size_t kSubmitChunkBytes = kHttp2DataOutputCreditBytes;
        std::size_t offset = 0;
        bool submittedEmptyTerminal = false;
        do {
            const auto count = bytes.empty() ? 0 : std::min(kSubmitChunkBytes, bytes.size() - offset);
            const auto chunk = bytes.substr(offset, count);
            const bool last = offset + count == bytes.size();
            const auto end = last ? terminal : Http2EndStream::kKeepOpen;
            for (;;) {
                if (aborted_ || signal_.terminated()) {
                    co_return aborted_ ? std::make_error_code(std::errc::operation_canceled)
                                       : signal_.terminalError();
                }
                if (chunk.empty() && end == Http2EndStream::kEndStream &&
                    connection_.hasQueuedData(streamId_)) {
                    const auto waitResult = co_await waitForSendWindow();
                    if (waitResult.aborted() != nullptr) {
                        co_return signal_.terminated() ? signal_.terminalError()
                                                       : std::make_error_code(std::errc::connection_reset);
                    }
                    continue;
                }
                if (outputBudget_ != nullptr && !chunk.empty()) {
                    for (;;) {
                        const auto window = connection_.sendWindowState(streamId_);
                        if (!window) {
                            co_return std::make_error_code(std::errc::connection_reset);
                        }
                        if (aborted_ || signal_.terminated()) {
                            co_return aborted_ ? std::make_error_code(std::errc::operation_canceled)
                                               : signal_.terminalError();
                        }
                        if (window->available != 0) {
                            break;
                        }
                        co_await outputBudget_->waitForChange();
                        if (aborted_ || signal_.terminated()) {
                            co_return aborted_ ? std::make_error_code(std::errc::operation_canceled)
                                               : signal_.terminalError();
                        }
                    }
                    if (!(co_await outputBudget_->acquire(streamId_, signal_)) || aborted_ ||
                        signal_.terminated()) {
                        co_return aborted_ ? std::make_error_code(std::errc::operation_canceled)
                                           : signal_.terminalError();
                    }
                }
                const auto result = connection_.submitData(streamId_, chunk, end);
                if (outputBudget_ != nullptr &&
                    (result == Http2DataSubmitStatus::kAccepted ||
                        result == Http2DataSubmitStatus::kQueued)) {
                    outputBudget_->noteDataSubmitted(streamId_, chunk.size());
                }
                wakeWriter();
                if (result != Http2DataSubmitStatus::kAccepted &&
                    result != Http2DataSubmitStatus::kQueued) {
                    if (outputBudget_ != nullptr && !chunk.empty()) {
                        outputBudget_->release(streamId_);
                    }
                }
                if (result == Http2DataSubmitStatus::kAccepted) {
                    break;
                }
                if (result == Http2DataSubmitStatus::kClosed) {
                    co_return std::make_error_code(std::errc::connection_reset);
                }
                if (result == Http2DataSubmitStatus::kInvalidState ||
                    result == Http2DataSubmitStatus::kContentLengthExceeded ||
                    result == Http2DataSubmitStatus::kContentLengthIncomplete) {
                    co_return std::make_error_code(std::errc::protocol_error);
                }
                const auto waitResult = co_await waitForSendWindow();
                if (waitResult.aborted() != nullptr) {
                    co_return signal_.terminated() ? signal_.terminalError()
                                                   : std::make_error_code(std::errc::connection_reset);
                }
                if (result == Http2DataSubmitStatus::kQueued) {
                    break;  // core owns this bounded chunk; wait before next submit
                }
            }
            offset += count;
            submittedEmptyTerminal = bytes.empty();
            if (!bytes.empty() && offset < bytes.size()) {
                const auto waitResult = co_await waitForSendWindow();
                if (waitResult.aborted() != nullptr) {
                    co_return signal_.terminated() ? signal_.terminalError()
                                                   : std::make_error_code(std::errc::connection_reset);
                }
            }
        } while (offset < bytes.size() || (!submittedEmptyTerminal && bytes.empty()));
        co_return std::error_code{};
    }

    void abort() noexcept {
        if (aborted_) {
            return;
        }
        aborted_ = true;
        if (outputBudget_ != nullptr) {
            outputBudget_->release(streamId_);
            outputBudget_->wake();
        }
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
    [[nodiscard]] Task<Http2SendWindowWaitResult> waitForSendWindow() {
        for (;;) {
            if (aborted_ || signal_.terminated() ||
                connection_.streamReceiveStatus(streamId_) == Http2StreamReceiveStatus::kClosed) {
                co_return Http2SendWindowWaitResult::makeAborted();
            }
            if (!connection_.hasQueuedData(streamId_)) {
                co_return Http2SendWindowWaitResult::makeReady();
            }
            co_await signal_.wait();
        }
    }

    void wakeWriter() noexcept {
        writeSignal_.notify();
    }

    ruvia::Http2Connection& connection_;
    std::uint32_t streamId_;
    Http2SansIoBodyQueue& bodyQueue_;
    Http2SansIoStreamSignal& signal_;
    WorkerSignal& writeSignal_;
    Http2DataOutputBudget* outputBudget_{nullptr};
    Executor executor_;
    bool aborted_{false};
};

// Streaming request-body reader for the sans-I/O session; the BodyReader facade wraps
// it for handler consumption. Chunk-for-chunk port of the coroutine readBodyChunk.
// Admission always binds the runtime-owned signal before this facade can exist.
class Http2SansIoRequestBodyReader final {
public:
    Http2SansIoRequestBodyReader(ruvia::Http2Connection& connection, std::uint32_t streamId,
        Http2SansIoBodyQueue& bodyQueue, Http2SansIoStreamSignal& signal) noexcept
        : connection_(connection),
          streamId_(streamId),
          bodyQueue_(bodyQueue),
          signal_(signal) {}

    [[nodiscard]] Task<std::optional<std::span<const std::byte>>> read() {
        for (;;) {
            if (signal_.terminated()) {
                throw std::system_error(signal_.terminalError());
            }
            const auto receiveStatus = connection_.streamReceiveStatus(streamId_);
            if (receiveStatus == Http2StreamReceiveStatus::kClosed) {
                throw std::system_error(std::make_error_code(std::errc::connection_reset));
            }
            if (const auto chunk = bodyQueue_.pop(); !chunk.empty()) {
                co_return ::ruvia::asBytes(chunk);
            }
            if (!bodyQueue_.empty()) {
                continue;
            }
            if (receiveStatus == Http2StreamReceiveStatus::kEnded) {
                co_return std::nullopt;
            }
            if (signal_.terminated()) {
                throw std::system_error(signal_.terminalError());
            }
            co_await signal_.wait();
        }
    }

private:
    ruvia::Http2Connection& connection_;
    std::uint32_t streamId_;
    Http2SansIoBodyQueue& bodyQueue_;
    Http2SansIoStreamSignal& signal_;
};

}  // namespace ruvia::detail
