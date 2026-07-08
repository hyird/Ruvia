#pragma once

// Streaming response sink for the sans-I/O HTTP/2 session (ruvia-web).
//
// Mirrors the coroutine Http2ResponseStreamSink but drives an Http2Connection through
// its submit* API instead of a socket: commit() emits the streaming HEADERS (no
// Content-Length) via submitStreamingResponseHead, write() streams DATA via submitData,
// and end() closes the stream. The shared dispatchResponseStreamWith machinery calls
// these via the responseStream*Thunk<Sink> function pointers, so the method set matches.
//
// v1 scope: trailers are validated but not yet emitted on the sans-I/O path (rare on
// streams); backpressure is handled by the core (submitData buffers a blocked remainder
// and drains it on WINDOW_UPDATE).

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <asio/steady_timer.hpp>

#include "net/http2/Http2Connection.h"
#include "net/server/HttpResponseStreamHead.h"
#include "net/server/HttpResponseStreamState.h"
#include "runtime/AsioAwait.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/http/detail/PmrString.h"

namespace ruvia {
class Context;  // only forwarded as Context* via the type-erased bindContext thunk
}

namespace ruvia::detail {

template <typename Executor>
class Http2SansIoResponseStreamSink final {
public:
    Http2SansIoResponseStreamSink(
        Http2Connection& connection,
        std::uint32_t streamId,
        ResponseBodyMode mode,
        std::pmr::memory_resource* resource,
        Executor executor) noexcept
        : connection_(connection),
          streamId_(streamId),
          mode_(mode),
          scratch_(resource),
          executor_(executor) {}

    [[nodiscard]] bool committed() const noexcept { return state_.committed(); }

    [[nodiscard]] bool aborted() const noexcept {
        auto* stream = connection_.stream(streamId_);
        return stream == nullptr || stream->isReset();
    }

    void bindContext(Context* context, ResponseStreamState::StreamingHeadThunk streamingHead) noexcept {
        state_.bindContext(context, streamingHead);
    }

    [[nodiscard]] std::pmr::string& scratch() noexcept {
        clearPmrStringRetainingSmall(scratch_);
        return scratch_;
    }

    Task<void> write(std::string_view chunk) {
        if (chunk.empty()) {
            co_return;
        }
        co_await commit();
        state_.ensureBodyAllowed();
        (void)connection_.submitData(streamId_, chunk, /*endStream=*/false);
    }

    Task<void> sleep(std::chrono::milliseconds duration) {
        asio::steady_timer timer(executor_, duration);
        const auto ec = co_await asyncError([&timer](auto handler) mutable {
            timer.async_wait(std::move(handler));
        });
        if (ec) {
            throw std::system_error(ec);
        }
    }

    void addTrailer(std::string_view name, std::string_view value) {
        // v1: validate but do not emit (sans-I/O trailer HEADERS frame is a follow-up).
        state_.ensureTrailerAllowed(name, value);
    }

    Task<void> end() {
        if (state_.ended()) {
            co_return;
        }
        co_await commit();
        if (state_.bodyForbidden()) {
            state_.markEnded();
            co_return;
        }
        (void)connection_.submitData(streamId_, {}, /*endStream=*/true);
        state_.markEnded();
    }

private:
    Task<void> commit() {
        if (state_.committed()) {
            co_return;
        }
        auto streamHead = prepareResponseStreamHead(
            state_.streamingHead(), mode_, ResponseStreamFraming::kHttp2DataFrames);
        state_.markCommitted(streamHead.bodyForbidden());
        connection_.submitStreamingResponseHead(
            streamId_, streamHead.response(), streamHead.bodyForbidden());
        if (state_.bodyForbidden()) {
            state_.markEnded();
        }
    }

    Http2Connection& connection_;
    std::uint32_t streamId_;
    ResponseBodyMode mode_;
    ResponseStreamState state_;
    std::pmr::string scratch_;
    Executor executor_;
};

}  // namespace ruvia::detail
