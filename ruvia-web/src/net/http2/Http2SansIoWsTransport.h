#pragma once

// WebSocket transport for the sans-I/O HTTP/2 session (RFC 8441 Extended CONNECT).
//
// Mirrors Http2WebSocketTransport (the coroutine session's transport) but drives an
// Http2Connection through its submit* API: outbound WebSocket frames become DATA
// submits (the session's single writer flushes them), and inbound tunnel bytes arrive
// through a per-stream Http2WsInboundPipe that the session's reader fills from
// kRequestBodyChunk events. readMore suspends on the pipe's signal timer until the
// reader pushes bytes or ends the pipe (peer END_STREAM, RST, or connection teardown).
//
// The same-executor discipline of the session (reader, writer and handlers all run on
// the connection's executor) means pipe accesses never race: a wake via signal.cancel()
// while nothing is waiting is a no-op, and readMore always re-checks data before
// suspending, so no wakeup is lost.

#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <system_error>

#include <asio/steady_timer.hpp>

#include "net/http2/Http2Connection.h"
#include "runtime/AsioAwait.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/detail/PmrString.h"

namespace ruvia::detail {

// Inbound byte pipe for one WebSocket tunnel stream. Created by the session's reader
// when it admits the tunnel (so no DATA chunk can slip past before the handler runs)
// and destroyed by the handler task when the WebSocket session ends.
struct Http2WsInboundPipe final {
    template <typename Executor>
    Http2WsInboundPipe(Executor executor, std::pmr::memory_resource* resource)
        : data(resource), signal(executor) {}

    void push(std::string_view bytes) {
        data.append(bytes.data(), bytes.size());
        signal.cancel();
    }

    void end() noexcept {
        ended = true;
        signal.cancel();
    }

    std::pmr::string data;        // bytes not yet drained by readMore
    asio::steady_timer signal;    // wakes a suspended readMore (cancel() = wake)
    bool ended{false};            // peer END_STREAM / RST / connection teardown
};

template <typename Executor>
class Http2SansIoWsTransport final {
public:
    Http2SansIoWsTransport(
        Http2Connection& connection,
        std::uint32_t streamId,
        Http2WsInboundPipe& pipe,
        asio::steady_timer& writeSignal,
        Executor executor) noexcept
        : connection_(&connection),
          streamId_(streamId),
          pipe_(&pipe),
          writeSignal_(&writeSignal),
          executor_(executor) {}

    [[nodiscard]] Executor executor() const noexcept {
        return executor_;
    }

    [[nodiscard]] Task<bool> readMore(std::pmr::string& buffer) {
        for (;;) {
            if (!pipe_->data.empty()) {
                buffer.append(pipe_->data.data(), pipe_->data.size());
                pipe_->data.clear();
                co_return true;
            }
            if (pipe_->ended) {
                co_return false;
            }
            pipe_->signal.expires_at((asio::steady_timer::time_point::max)());
            co_await asyncError([this](auto handler) mutable {
                pipe_->signal.async_wait(std::move(handler));
            });
        }
    }

    [[nodiscard]] Task<std::error_code> writeFrame(
        std::string_view header,
        std::string_view payload,
        bool endStream) {
        // submitData only appends to the connection's outbound buffer (a window-blocked
        // remainder is queued in-order inside the core), so two submits keep the frame
        // contiguous on the wire; the writer is then woken to flush.
        auto result = connection_->submitData(streamId_, header, payload.empty() && endStream);
        if (!payload.empty()) {
            result = connection_->submitData(streamId_, payload, endStream);
        }
        writeSignal_->cancel();
        if (result == Http2SubmitResult::kClosed) {
            co_return std::make_error_code(std::errc::connection_reset);
        }
        co_return std::error_code{};
    }

private:
    Http2Connection* connection_;
    std::uint32_t streamId_;
    Http2WsInboundPipe* pipe_;
    asio::steady_timer* writeSignal_;
    Executor executor_;
};

}  // namespace ruvia::detail
